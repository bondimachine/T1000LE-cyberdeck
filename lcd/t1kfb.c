/*
 * t1kfb - drive the Toshiba T1000LE dual-scan STN panel from Raspberry Pi GPIO.
 *
 * Same idea as hzeller/rpi-rgb-led-matrix: the GPIO registers are mmap()ed
 * from /dev/gpiomem and a busy-looping thread, pinned to an isolated CPU core,
 * clocks the image out forever. No kernel driver, no PIO.
 *
 * Panel model (from the maintenance manual pinout, PJ7):
 *   - 640x400, split in an upper and a lower half (200 rows each) that are
 *     refreshed at the same time.
 *   - Each SCP clock shifts 4 pixels of the upper half (UD0-3) and 4 pixels of
 *     the lower half (LD0-3): 160 clocks per line.
 *   - LP latches the shifted line and advances the row driver.
 *   - FP marks the first line of a frame.
 *
 * Works on Pi 1-4 / Zero (BCM283x/BCM2711). NOT on Pi 5 (GPIO lives on RP1).
 *
 * Build: make      Run: sudo ./t1kfb --help
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pins.h"

#define BIT(p) (1u << (p))
#define MAX_W 1024
#define MAX_H 1024

_Static_assert(PIN_UD0 < 32 && PIN_UD1 < 32 && PIN_UD2 < 32 && PIN_UD3 < 32 &&
                   PIN_LD0 < 32 && PIN_LD1 < 32 && PIN_LD2 < 32 && PIN_LD3 < 32 &&
                   PIN_SCP < 32 && PIN_LP < 32 && PIN_FP < 32 && PIN_PVBL < 32 &&
                   PIN_CBLON < 32 && PIN_CBLHI < 32 && PIN_VEE_EN < 32 && PIN_LCD5V_EN < 32,
               "all pins must be in GPIO bank 0");

/* Data bit i of a packed byte goes to data_pins[i]: bits 0-3 upper, 4-7 lower. */
static const int data_pins[8] = {PIN_UD0, PIN_UD1, PIN_UD2, PIN_UD3,
                                 PIN_LD0, PIN_LD1, PIN_LD2, PIN_LD3};

enum pvbl_mode { PVBL_LOW, PVBL_HIGH, PVBL_FRAME, PVBL_LINE };
enum backlight { BL_OFF, BL_ON, BL_HIGH };

static struct {
    int width, height;
    int fps;
    int slowdown;     /* each GPIO write is repeated this many times */
    int lp_width;     /* LP pulse width, in GPIO writes */
    int fp_line;      /* which line's LP pulse gets FP asserted */
    bool lsb_first;   /* first pixel of a 4-pixel group on D0 instead of D3 */
    bool invert;      /* swap dark/clear */
    enum pvbl_mode pvbl;
    enum backlight backlight;
    int cpu;
    bool rt;
    bool no_vee;
    int vee_delay_frames;
    const char *source;
    bool dither;
    int threshold;
    int fb_xoff, fb_yoff, fb_scale;
    int conv_hz;
    bool stats;
} cfg = {
    .width = 640,
    .height = 400,
    .fps = 70,
    .slowdown = 4,
    .lp_width = 8,
    .fp_line = 0,
    .pvbl = PVBL_FRAME,
    .backlight = BL_ON,
    .cpu = 3,
    .vee_delay_frames = 10,
    .source = "test:calib",
    .threshold = 128,
    .fb_scale = 1,
    .conv_hz = 30,
};

static int half, clocks; /* rows per half, SCP clocks per line */

/* ------------------------------------------------------------------ GPIO */

static volatile uint32_t *gpio;
#define REG_SET 7  /* GPSET0 at 0x1c */
#define REG_CLR 10 /* GPCLR0 at 0x28 */

static uint32_t data_mask, lut_set[256], lut_clr[256];

static void gpio_fsel(int pin, int mode) {
    int reg = pin / 10, sh = (pin % 10) * 3;
    uint32_t v = gpio[reg];
    v &= ~(7u << sh);
    v |= (uint32_t)(mode & 7) << sh;
    gpio[reg] = v;
}
#define FSEL_IN 0
#define FSEL_OUT 1

static inline void gset(uint32_t m) {
    for (int k = 0; k < cfg.slowdown; k++) gpio[REG_SET] = m;
}
static inline void gclr(uint32_t m) {
    for (int k = 0; k < cfg.slowdown; k++) gpio[REG_CLR] = m;
}

static uint32_t all_pins_mask(void) {
    return data_mask | BIT(PIN_SCP) | BIT(PIN_LP) | BIT(PIN_FP) | BIT(PIN_PVBL) |
           BIT(PIN_CBLON) | BIT(PIN_CBLHI) | BIT(PIN_VEE_EN) | BIT(PIN_LCD5V_EN);
}

static void check_board(void) {
    char model[128] = {0};
    FILE *f = fopen("/proc/device-tree/model", "r");
    if (f) {
        if (!fgets(model, sizeof model, f)) model[0] = 0;
        fclose(f);
    }
    if (strstr(model, "Raspberry Pi 5")) {
        fprintf(stderr, "This is a Pi 5: its GPIO is on RP1 and can't be bit-banged this way.\n");
        exit(1);
    }
    if (model[0] && !strstr(model, "Raspberry Pi"))
        fprintf(stderr, "warning: unknown board '%s'\n", model);
}

static void gpio_init(void) {
    check_board();
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/gpiomem (run as root or add user to the gpio group)");
        exit(1);
    }
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap /dev/gpiomem");
        exit(1);
    }
    gpio = p;

    data_mask = 0;
    for (int i = 0; i < 8; i++) data_mask |= BIT(data_pins[i]);
    for (int b = 0; b < 256; b++) {
        uint32_t s = 0;
        for (int i = 0; i < 8; i++)
            if (b & (1 << i)) s |= BIT(data_pins[i]);
        lut_set[b] = s;
        lut_clr[b] = data_mask & ~s;
    }

    /* Safe idle state BEFORE switching to outputs: everything low, backlight off. */
    gpio[REG_CLR] = all_pins_mask();
    if (CBL_ACTIVE_LOW) gpio[REG_SET] = BIT(PIN_CBLON) | BIT(PIN_CBLHI);
    uint32_t m = all_pins_mask();
    for (int pin = 0; pin < 32; pin++)
        if (m & BIT(pin)) gpio_fsel(pin, FSEL_OUT);
}

static void set_backlight(enum backlight bl) {
    bool on = bl != BL_OFF, hi = bl == BL_HIGH;
    uint32_t on_bit = BIT(PIN_CBLON), hi_bit = BIT(PIN_CBLHI);
    if (CBL_ACTIVE_LOW) {
        gpio[on ? REG_CLR : REG_SET] = on_bit;
        gpio[hi ? REG_CLR : REG_SET] = hi_bit;
    } else {
        gpio[on ? REG_SET : REG_CLR] = on_bit;
        gpio[hi ? REG_SET : REG_CLR] = hi_bit;
    }
}

static void gpio_release(void) {
    gpio[REG_CLR] = BIT(PIN_VEE_EN);
    usleep(50000);
    gpio[REG_CLR] = all_pins_mask();
    uint32_t m = all_pins_mask();
    for (int pin = 0; pin < 32; pin++)
        if (m & BIT(pin)) gpio_fsel(pin, FSEL_IN);
}

/* ------------------------------------------------- lock-free triple buffer */

static uint8_t *frames[3]; /* packed: half rows * clocks bytes */
#define TB_DIRTY 4u
static _Atomic unsigned tb_pending = 1; /* reader starts on 0, writer on 2 */
static int tb_writer = 2;

static void tb_publish(void) {
    tb_writer = (int)(atomic_exchange(&tb_pending, (unsigned)tb_writer | TB_DIRTY) & 3);
}

/* ---------------------------------------------------------- scan thread */

static atomic_bool stop_requested;
static _Atomic uint64_t st_frames, st_overruns, st_max_late_ns;

static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline void shift_line(const uint8_t *row) {
    const uint32_t scp = BIT(PIN_SCP);
    for (int c = 0; c < clocks; c++) {
        uint8_t b = row[c];
        gclr(scp);        /* falling edge: panel latches previous data */
        gclr(lut_clr[b]); /* change data while SCP is low            */
        gset(lut_set[b]);
        gset(scp); /* rising edge */
    }
    gclr(scp); /* falling edge for the last group */
}

static void set_pin(int pin, bool v) { (v ? gset : gclr)(BIT(pin)); }

static void *scan_thread(void *arg) {
    (void)arg;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cfg.cpu, &cs);
    if (pthread_setaffinity_np(pthread_self(), sizeof cs, &cs))
        fprintf(stderr, "warning: could not pin scan thread to CPU %d\n", cfg.cpu);
    if (cfg.rt) {
        struct sched_param sp = {.sched_priority = 90};
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
            fprintf(stderr, "warning: SCHED_FIFO failed (not root?)\n");
    }

    const uint64_t line_ns = 1000000000ull / ((uint64_t)cfg.fps * (uint64_t)half);
    const uint32_t lp = BIT(PIN_LP), fp = BIT(PIN_FP);
    unsigned disp = 0;
    uint64_t frame = 0, stop_at = 0;
    bool vee_on = false, m = false, stopping = false;
    set_pin(PIN_PVBL, cfg.pvbl == PVBL_HIGH);
    uint64_t next = now_ns();

    for (;;) {
        if (atomic_load_explicit(&tb_pending, memory_order_acquire) & TB_DIRTY)
            disp = atomic_exchange(&tb_pending, disp) & 3;
        const uint8_t *fb = frames[disp];

        if (cfg.pvbl == PVBL_FRAME) set_pin(PIN_PVBL, m = !m);

        for (int line = 0; line < half; line++) {
            shift_line(fb + (size_t)line * clocks);

            /* Pace on the latch so every row is driven for the same time. */
            next += line_ns;
            uint64_t t = now_ns();
            if (t > next) {
                uint64_t late = t - next;
                if (late > atomic_load_explicit(&st_max_late_ns, memory_order_relaxed))
                    atomic_store_explicit(&st_max_late_ns, late, memory_order_relaxed);
                if (late > line_ns) {
                    atomic_fetch_add_explicit(&st_overruns, 1, memory_order_relaxed);
                    next = t;
                }
            } else {
                while (now_ns() < next) {
                }
            }

            bool is_fp = line == cfg.fp_line;
            if (is_fp) gset(fp);
            gset(lp);
            for (int k = 1; k < cfg.lp_width; k++) gset(lp);
            gclr(lp);
            if (is_fp) gclr(fp);
            if (cfg.pvbl == PVBL_LINE) set_pin(PIN_PVBL, m = !m);
        }
        frame++;
        atomic_store_explicit(&st_frames, frame, memory_order_relaxed);

        /* VEE only while we are clocking the panel: on after a few good
         * frames, off first on shutdown, then keep scanning while it decays. */
        if (!stopping && atomic_load(&stop_requested)) {
            stopping = true;
            gpio[REG_CLR] = BIT(PIN_VEE_EN);
            vee_on = false;
            stop_at = frame + (uint64_t)cfg.fps / 5 + 1; /* ~200 ms */
        }
        if (stopping && frame >= stop_at) break;
        if (!stopping && !vee_on && !cfg.no_vee && frame >= (uint64_t)cfg.vee_delay_frames) {
            gpio[REG_SET] = BIT(PIN_VEE_EN);
            vee_on = true;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- sources */

static uint8_t *img; /* width*height, 1 = dark ("ink") pixel */

static void pack(uint8_t *out) {
    const int W = cfg.width;
    for (int r = 0; r < half; r++) {
        const uint8_t *up = img + (size_t)r * W, *lo = img + (size_t)(r + half) * W;
        for (int c = 0; c < clocks; c++) {
            uint8_t b = 0;
            for (int k = 0; k < 4; k++) {
                int bit = cfg.lsb_first ? k : 3 - k;
                int x = 4 * c + k;
                b |= (uint8_t)(((up[x] ^ cfg.invert) & 1) << bit);
                b |= (uint8_t)(((lo[x] ^ cfg.invert) & 1) << (bit + 4));
            }
            out[(size_t)r * clocks + c] = b;
        }
    }
}

static const char *patterns[] = {"calib", "checker", "border", "halves", "hlines",
                                 "vlines", "diag",    "all",    "none",   NULL};

static bool render_test(const char *name) {
    const int W = cfg.width, H = cfg.height;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool v;
            bool edge = x == 0 || y == 0 || x == W - 1 || y == H - 1;
            if (!strcmp(name, "checker"))
                v = ((x / 8) + (y / 8)) & 1;
            else if (!strcmp(name, "border"))
                v = edge || x == 4 || y == 4 || x == W - 5 || y == H - 5;
            else if (!strcmp(name, "halves")) /* only the upper half dark */
                v = y < H / 2;
            else if (!strcmp(name, "hlines"))
                v = y % 4 == 0;
            else if (!strcmp(name, "vlines"))
                v = x % 4 == 0;
            else if (!strcmp(name, "diag"))
                v = (x + y) % 16 == 0;
            else if (!strcmp(name, "all"))
                v = true;
            else if (!strcmp(name, "none"))
                v = false;
            else if (!strcmp(name, "calib"))
                /* border + solid top-left block (orientation) + thin diagonals
                 * (smooth if the nibble bit order is right, jagged if not)
                 * + a line exactly on the half boundary. */
                v = edge || (x < 32 && y < 32) || ((x - y + 4096) % 32 == 0) ||
                    y == H / 2 || x == W / 2;
            else
                return false;
            img[(size_t)y * W + x] = v;
        }
    return true;
}

static bool load_pbm(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    int vals[2], n = 0, c;
    char magic[3] = {0};
    if (fread(magic, 1, 2, f) != 2 || strcmp(magic, "P4")) {
        fprintf(stderr, "%s: only binary PBM (P4) is supported\n", path);
        fclose(f);
        return false;
    }
    while (n < 2 && (c = fgetc(f)) != EOF) {
        if (c == '#')
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
        else if (c >= '0' && c <= '9') {
            ungetc(c, f);
            if (fscanf(f, "%d", &vals[n++]) != 1) break;
        }
    }
    fgetc(f); /* single whitespace after height */
    if (n != 2 || vals[0] <= 0 || vals[1] <= 0) {
        fprintf(stderr, "%s: bad header\n", path);
        fclose(f);
        return false;
    }
    int w = vals[0], h = vals[1], stride = (w + 7) / 8;
    uint8_t *row = malloc((size_t)stride);
    memset(img, 0, (size_t)cfg.width * cfg.height);
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        if (y >= cfg.height) continue;
        for (int x = 0; x < w && x < cfg.width; x++)
            img[(size_t)y * cfg.width + x] = (row[x / 8] >> (7 - x % 8)) & 1;
    }
    free(row);
    fclose(f);
    return true;
}

/* Linux framebuffer (fbcon console, or X11 on the fbdev driver). */
static struct {
    uint8_t *mem;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
} fbs;

static bool fb_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return false;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &fbs.v) || ioctl(fd, FBIOGET_FSCREENINFO, &fbs.f)) {
        perror("FBIOGET_*SCREENINFO");
        close(fd);
        return false;
    }
    fbs.mem = mmap(NULL, fbs.f.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (fbs.mem == MAP_FAILED) {
        perror("mmap framebuffer");
        return false;
    }
    if (fbs.v.bits_per_pixel != 16 && fbs.v.bits_per_pixel != 24 && fbs.v.bits_per_pixel != 32) {
        fprintf(stderr, "%s: unsupported %u bpp\n", path, fbs.v.bits_per_pixel);
        return false;
    }
    fprintf(stderr, "%s: %ux%u %ubpp\n", path, fbs.v.xres, fbs.v.yres, fbs.v.bits_per_pixel);
    if (fbs.v.xres < (unsigned)(cfg.width * cfg.fb_scale) ||
        fbs.v.yres < (unsigned)(cfg.height * cfg.fb_scale))
        fprintf(stderr, "warning: framebuffer is smaller than the panel; the rest stays blank\n");
    return true;
}

static inline unsigned chan8(uint32_t px, const struct fb_bitfield *bf) {
    if (!bf->length) return 0;
    unsigned v = (px >> bf->offset) & ((1u << bf->length) - 1);
    return bf->length >= 8 ? v >> (bf->length - 8) : (v * 255) / ((1u << bf->length) - 1);
}

static inline unsigned fb_luma(unsigned sx, unsigned sy) {
    if (sx >= fbs.v.xres || sy >= fbs.v.yres) return 255;
    unsigned bpp = fbs.v.bits_per_pixel / 8;
    const uint8_t *p = fbs.mem + (size_t)(sy + fbs.v.yoffset) * fbs.f.line_length +
                       (size_t)(sx + fbs.v.xoffset) * bpp;
    uint32_t px = 0;
    for (unsigned i = 0; i < bpp; i++) px |= (uint32_t)p[i] << (8 * i);
    return (chan8(px, &fbs.v.red) * 77 + chan8(px, &fbs.v.green) * 150 +
            chan8(px, &fbs.v.blue) * 29) >> 8;
}

static void fb_render(void) {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int s = cfg.fb_scale;
    for (int y = 0; y < cfg.height; y++)
        for (int x = 0; x < cfg.width; x++) {
            unsigned sum = 0;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++)
                    sum += fb_luma((unsigned)(cfg.fb_xoff + x * s + dx),
                                   (unsigned)(cfg.fb_yoff + y * s + dy));
            unsigned l = sum / (unsigned)(s * s);
            unsigned th = cfg.dither ? bayer[y & 3][x & 3] * 16u + 8u : (unsigned)cfg.threshold;
            img[(size_t)y * cfg.width + x] = l < th; /* dark source pixel -> ink */
        }
}

/* --------------------------------------------------------------- main */

static void on_signal(int sig) {
    (void)sig;
    atomic_store(&stop_requested, true);
}

static void usage(void) {
    printf(
        "usage: t1kfb [options]\n"
        "  --source SRC        test:NAME | fb[:/dev/fb0] | pbm:FILE   (default test:calib)\n"
        "                      test patterns:");
    for (int i = 0; patterns[i]; i++) printf(" %s", patterns[i]);
    printf(
        "\n"
        "  --size WxH          panel size (default 640x400)\n"
        "  --fps N             refresh rate (default 70)\n"
        "  --slowdown N        repeat each GPIO write N times, lowers SCP speed (default 4)\n"
        "  --lp-width N        LP pulse width in GPIO writes (default 8)\n"
        "  --fp-line N         line whose latch carries FP (default 0)\n"
        "  --lsb-first         first pixel of each group on D0 (default D3)\n"
        "  --invert            swap dark and clear pixels\n"
        "  --pvbl MODE         low | high | frame | line  (toggle per frame/line; default frame)\n"
        "  --backlight MODE    off | on | high (default on)\n"
        "  --no-vee            never enable VEE (scope/timing tests)\n"
        "  --vee-delay N       frames before VEE is enabled (default 10)\n"
        "  --cpu N             core for the scan thread (default 3, isolate it with isolcpus=3)\n"
        "  --rt                use SCHED_FIFO (set kernel.sched_rt_runtime_us=-1 too)\n"
        "  --dither            ordered dithering for fb source (default: threshold)\n"
        "  --threshold N       luma threshold 0-255 for fb source (default 128)\n"
        "  --fb-offset X,Y     top-left of the region read from the framebuffer\n"
        "  --fb-scale N        downscale the framebuffer by N (box filter)\n"
        "  --conv-hz N         framebuffer sampling rate (default 30)\n"
        "  --stats             print refresh statistics every 5 s\n");
}

static int parse_enum(const char *s, const char *const *names) {
    for (int i = 0; names[i]; i++)
        if (!strcmp(s, names[i])) return i;
    fprintf(stderr, "bad value '%s'\n", s);
    exit(2);
}

int main(int argc, char **argv) {
    static const char *pvbl_names[] = {"low", "high", "frame", "line", NULL};
    static const char *bl_names[] = {"off", "on", "high", NULL};
    static const struct option opts[] = {
        {"source", 1, 0, 's'},    {"size", 1, 0, 'S'},      {"fps", 1, 0, 'f'},
        {"slowdown", 1, 0, 'w'},  {"lp-width", 1, 0, 'l'},  {"fp-line", 1, 0, 'F'},
        {"lsb-first", 0, 0, 'L'}, {"invert", 0, 0, 'i'},    {"pvbl", 1, 0, 'p'},
        {"backlight", 1, 0, 'b'}, {"no-vee", 0, 0, 'n'},    {"vee-delay", 1, 0, 'v'},
        {"cpu", 1, 0, 'c'},       {"rt", 0, 0, 'r'},        {"dither", 0, 0, 'd'},
        {"threshold", 1, 0, 't'}, {"fb-offset", 1, 0, 'o'}, {"fb-scale", 1, 0, 'x'},
        {"conv-hz", 1, 0, 'z'},   {"stats", 0, 0, 'T'},     {"help", 0, 0, 'h'},
        {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (o) {
        case 's': cfg.source = optarg; break;
        case 'S':
            if (sscanf(optarg, "%dx%d", &cfg.width, &cfg.height) != 2) return usage(), 2;
            break;
        case 'f': cfg.fps = atoi(optarg); break;
        case 'w': cfg.slowdown = atoi(optarg); break;
        case 'l': cfg.lp_width = atoi(optarg); break;
        case 'F': cfg.fp_line = atoi(optarg); break;
        case 'L': cfg.lsb_first = true; break;
        case 'i': cfg.invert = true; break;
        case 'p': cfg.pvbl = parse_enum(optarg, pvbl_names); break;
        case 'b': cfg.backlight = parse_enum(optarg, bl_names); break;
        case 'n': cfg.no_vee = true; break;
        case 'v': cfg.vee_delay_frames = atoi(optarg); break;
        case 'c': cfg.cpu = atoi(optarg); break;
        case 'r': cfg.rt = true; break;
        case 'd': cfg.dither = true; break;
        case 't': cfg.threshold = atoi(optarg); break;
        case 'o':
            if (sscanf(optarg, "%d,%d", &cfg.fb_xoff, &cfg.fb_yoff) != 2) return usage(), 2;
            break;
        case 'x': cfg.fb_scale = atoi(optarg); break;
        case 'z': cfg.conv_hz = atoi(optarg); break;
        case 'T': cfg.stats = true; break;
        default: usage(); return o == 'h' ? 0 : 2;
        }
    }
    if (cfg.width % 4 || cfg.height % 2 || cfg.width > MAX_W || cfg.height > MAX_H ||
        cfg.fps < 1 || cfg.slowdown < 1 || cfg.lp_width < 1 || cfg.fb_scale < 1 ||
        cfg.conv_hz < 1) {
        fprintf(stderr, "invalid option values\n");
        return 2;
    }
    half = cfg.height / 2;
    clocks = cfg.width / 4;
    if (cfg.fp_line < 0 || cfg.fp_line >= half) {
        fprintf(stderr, "--fp-line must be 0..%d\n", half - 1);
        return 2;
    }
    if (cfg.cpu < 0 || cfg.cpu >= sysconf(_SC_NPROCESSORS_CONF)) cfg.cpu = 0;

    img = calloc((size_t)cfg.width * cfg.height, 1);
    for (int i = 0; i < 3; i++) frames[i] = calloc((size_t)half * clocks, 1);

    /* Prepare the first image before touching the hardware. */
    enum { SRC_STATIC, SRC_FB } kind = SRC_STATIC;
    if (!strncmp(cfg.source, "test:", 5)) {
        if (!render_test(cfg.source + 5)) {
            fprintf(stderr, "unknown pattern '%s'\n", cfg.source + 5);
            return 2;
        }
    } else if (!strncmp(cfg.source, "pbm:", 4)) {
        if (!load_pbm(cfg.source + 4)) return 1;
    } else if (!strcmp(cfg.source, "fb") || !strncmp(cfg.source, "fb:", 3)) {
        if (!fb_open(cfg.source[2] ? cfg.source + 3 : "/dev/fb0")) return 1;
        kind = SRC_FB;
        fb_render();
    } else {
        fprintf(stderr, "unknown source '%s'\n", cfg.source);
        return 2;
    }
    pack(frames[tb_writer]);
    tb_publish();

    if (mlockall(MCL_CURRENT | MCL_FUTURE)) perror("warning: mlockall");
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    gpio_init();
    gpio[REG_SET] = BIT(PIN_LCD5V_EN); /* logic power first */
    usleep(20000);
    set_backlight(cfg.backlight);

    pthread_t th;
    if (pthread_create(&th, NULL, scan_thread, NULL)) {
        perror("pthread_create");
        gpio_release();
        return 1;
    }
    fprintf(stderr, "scanning %dx%d @ %d Hz on CPU %d, VEE %s. Ctrl-C to stop.\n", cfg.width,
            cfg.height, cfg.fps, cfg.cpu, cfg.no_vee ? "disabled" : "enabled after warm-up");

    uint64_t last_stats = now_ns(), last_frames = 0;
    while (!atomic_load(&stop_requested)) {
        if (kind == SRC_FB) {
            fb_render();
            pack(frames[tb_writer]);
            tb_publish();
        }
        usleep(1000000 / (unsigned)cfg.conv_hz);
        if (cfg.stats && now_ns() - last_stats >= 5000000000ull) {
            uint64_t t = now_ns(), fr = atomic_load(&st_frames);
            fprintf(stderr, "%.1f fps, overruns %llu, max late %.1f us\n",
                    (double)(fr - last_frames) * 1e9 / (double)(t - last_stats),
                    (unsigned long long)atomic_load(&st_overruns),
                    (double)atomic_exchange(&st_max_late_ns, 0) / 1000.0);
            last_stats = t;
            last_frames = fr;
        }
    }

    pthread_join(th, NULL); /* turns VEE off and keeps scanning ~200 ms */
    set_backlight(BL_OFF);
    gpio_release();
    fprintf(stderr, "stopped\n");
    return 0;
}
