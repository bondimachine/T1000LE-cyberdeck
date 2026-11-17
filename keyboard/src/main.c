/*
 * T1000LE keyboard -> USB HID on a Raspberry Pi Pico (RP2040/RP2350).
 *
 * Scans the 11x8 matrix on PJ8: one KBSC line is driven low at a time, the
 * others are left floating (so pressing several keys can't short two driven
 * outputs), and the KBRTN lines are read with pull-ups.
 *
 * Every press/release is also logged on the USB serial port as
 *     down SC3 RTN5
 *     up SC3 RTN5
 * which is what tools/map_keys.py uses to build keymap.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "keycodes.h"
#include "keymap.h"
#include "matrix_pins.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define SCAN_INTERVAL_US 1000
#define SETTLE_US 10
#define DEBOUNCE_SCANS 5

static uint8_t raw_prev[N_SCAN], stable_cnt[N_SCAN], state[N_SCAN];
static uint8_t pressed_code[N_SCAN][N_RTN]; /* code chosen at press time */
static bool report_dirty;

static void cdc_printf(const char *fmt, ...) {
    if (!tud_cdc_connected()) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        tud_cdc_write(buf, (uint32_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
        tud_cdc_write_flush();
    }
}

static void matrix_init(void) {
    for (int s = 0; s < N_SCAN; s++) {
        gpio_init(scan_pins[s]);
        gpio_put(scan_pins[s], 0); /* output value used when switched to output */
        gpio_set_dir(scan_pins[s], GPIO_IN);
        gpio_pull_up(scan_pins[s]);
    }
    for (int r = 0; r < N_RTN; r++) {
        gpio_init(rtn_pins[r]);
        gpio_set_dir(rtn_pins[r], GPIO_IN);
        gpio_pull_up(rtn_pins[r]);
    }
}

static uint8_t scan_line(int s) {
    gpio_set_dir(scan_pins[s], GPIO_OUT);
    busy_wait_us_32(SETTLE_US);
    uint32_t all = gpio_get_all();
    gpio_set_dir(scan_pins[s], GPIO_IN);
    uint8_t v = 0;
    for (int r = 0; r < N_RTN; r++)
        if (!(all & (1u << rtn_pins[r]))) v |= (uint8_t)(1u << r);
    return v;
}

/* Without diodes, three keys on the corners of a rectangle make the fourth
 * corner look pressed. Ignore changes to a line that might be a ghost. */
static bool maybe_ghost(int s, const uint8_t *m) {
    if (__builtin_popcount(m[s]) < 2) return false;
    for (int j = 0; j < N_SCAN; j++)
        if (j != s && (m[j] & m[s])) return true;
    return false;
}

static bool fn_held(void) {
    for (int s = 0; s < N_SCAN; s++)
        for (int r = 0; r < N_RTN; r++)
            if ((state[s] & (1u << r)) && keymap[0][s][r] == KC_FN) return true;
    return false;
}

static void key_event(int s, int r, bool down) {
    cdc_printf("%s SC%d RTN%d\r\n", down ? "down" : "up", s, r);
    if (down) {
        uint8_t code = keymap[0][s][r];
        if (code != KC_FN && fn_held()) {
            uint8_t alt = keymap[1][s][r];
            if (alt != KC_TRNS) code = alt;
        }
        pressed_code[s][r] = code;
    } else {
        pressed_code[s][r] = KC_NO;
    }
    report_dirty = true;
}

static void matrix_task(void) {
    uint8_t cand[N_SCAN];
    for (int s = 0; s < N_SCAN; s++) {
        uint8_t v = scan_line(s);
        if (v != raw_prev[s]) {
            raw_prev[s] = v;
            stable_cnt[s] = 0;
        } else if (stable_cnt[s] < DEBOUNCE_SCANS) {
            stable_cnt[s]++;
        }
        cand[s] = stable_cnt[s] >= DEBOUNCE_SCANS ? raw_prev[s] : state[s];
    }
    for (int s = 0; s < N_SCAN; s++) {
        uint8_t changed = cand[s] ^ state[s];
        if (!changed || maybe_ghost(s, cand)) continue;
        /* Apply releases before presses so Fn-layer lookups see current state. */
        for (int r = 0; r < N_RTN; r++)
            if ((changed & (1u << r)) && !(cand[s] & (1u << r))) {
                state[s] &= (uint8_t) ~(1u << r);
                key_event(s, r, false);
            }
        for (int r = 0; r < N_RTN; r++)
            if ((changed & (1u << r)) && (cand[s] & (1u << r))) {
                state[s] |= (uint8_t)(1u << r);
                key_event(s, r, true);
            }
    }
}

static void hid_task(void) {
    if (!report_dirty) return;
    if (tud_suspended()) {
        tud_remote_wakeup();
        return;
    }
    if (!tud_hid_ready()) return;

    uint8_t mods = 0, keys[6] = {0};
    int n = 0;
    for (int s = 0; s < N_SCAN; s++)
        for (int r = 0; r < N_RTN; r++) {
            uint8_t c = pressed_code[s][r];
            if (c == KC_NO || c == KC_TRNS || c == KC_FN) continue;
            if (c >= HID_KEY_CONTROL_LEFT && c <= HID_KEY_GUI_RIGHT)
                mods |= (uint8_t)(1u << (c - HID_KEY_CONTROL_LEFT));
            else if (n < 6)
                keys[n++] = c;
            else
                memset(keys, HID_KEY_NONE + 1, sizeof keys); /* ErrorRollOver */
        }
    if (tud_hid_keyboard_report(0, mods, keys)) report_dirty = false;
}

int main(void) {
    matrix_init();
    tusb_init();
    uint64_t next = time_us_64();
    bool was_connected = false;
    for (;;) {
        tud_task();
        if (time_us_64() >= next) {
            next += SCAN_INTERVAL_US;
            matrix_task();
            hid_task();
        }
        bool c = tud_cdc_connected();
        if (c && !was_connected)
            cdc_printf("T1000LE keyboard ready: %d scan x %d return lines\r\n", N_SCAN, N_RTN);
        was_connected = c;
    }
}

/* Required HID callbacks. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance, (void)report_id, (void)type, (void)buffer, (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize) {
    /* Caps/Num lock LEDs arrive here; the T1000LE keyboard connector has none. */
    (void)instance, (void)report_id, (void)type, (void)buffer, (void)bufsize;
}
