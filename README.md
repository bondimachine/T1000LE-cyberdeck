# T1000LE cyberdeck: keep the LCD and keyboard, add a Raspberry Pi 4

```
Raspberry Pi 4 ──GPIO──► 2× 74AHCT245 ──► PJ7  LCD (640x400 dual-scan STN)
      │                  (5V, switched)
      │                  + switched 5V, VEE supply, VEE failsafe
      └──USB──► Pico ──► PJ8  keyboard matrix (11 scan x 8 return)
```

- `lcd/`: `t1kfb`, a userspace driver that bit-bangs the panel from Pi GPIO,
  the same way [hzeller/rpi-rgb-led-matrix](https://github.com/hzeller/rpi-rgb-led-matrix)
  drives LED panels. It mirrors `/dev/fb0` to the LCD and has test patterns.
- `keyboard/`: Pico firmware (pico-sdk + TinyUSB). The Pico shows up as a
  standard USB boot keyboard, plus a serial port that logs matrix events.
- `tools/map_keys.py`: press each key when asked; it writes `keyboard/src/keymap.h`.

> **Unverified so far:** the panel timing details (bit order, FP position),
> what `PVBL` does, and the VEE voltage and polarity. The driver has flags for
> each of these (see bring-up below). If you can find the panel's part number
> (sticker on the back of the LCD module), its datasheet answers all of them.

---

## 1. LCD hardware

### Signals (PJ7)

| PJ7 | Signal | Meaning (best guess) | Pi GPIO (header pin) |
|---|---|---|---|
| 1 | PVBL | unknown: AC "M" signal or panel enable | 27 (13) |
| 2, 6, 10, 17, 22 | GND | | GND |
| 3 | CBLON (active low) | backlight on | 5 (29) |
| 4 | CBLHI (active low) | backlight bright | 6 (31) |
| 5 | N/C | | |
| 7 | FP | frame pulse | 26 (37) |
| 8 | LP | line latch | 25 (22) |
| 9 | SCP | shift clock | 24 (18) |
| 11–14 | UD0–3 | upper-half data | 16, 17, 18, 19 (36, 11, 12, 35) |
| 15 | LCD5V | panel logic 5V | via 5V switch, enable GPIO 13 (33) |
| 16 | LCDVEE | panel bias voltage | via VEE supply, enable GPIO 12 (32) |
| 18–21 | LD0–3 | lower-half data | 20, 21, 22, 23 (38, 40, 15, 16) |

Pins are defined in `lcd/pins.h`. All must stay in GPIO 0–31.

### Adapter board

1. **Level shifting.** The panel is 5V logic, and 3.3V may not register as
   high. Use **two 74AHCT245** (DIR = high, /OE = low), or 74AHCT541. Power
   them from the **switched** LCD 5V rail so nothing drives the panel while it
   is off. Use AHCT rather than HCT: AHCT inputs tolerate 3.3V even when the
   chip is unpowered, so the Pi can't back-power anything.
2. **Switched 5V.** P-MOSFET high-side switch (for example AO3401) with a 10k
   gate pull-up to 5V and an NPN (2N3904, 1k base resistor) from GPIO 13.
   GPIO high turns the panel on.
3. **VEE.** The mainboard generated this, so you need your own supply. For
   STN panels it is usually **negative, roughly −15 to −25 V**. Before
   building anything, confirm the polarity and value: measure PJ7-16 on the
   original board if it still powers up, or check the panel datasheet.
   - Use an adjustable inverting DC-DC (MC34063-style or a small module) with
     an enable input.
   - Start at the lowest magnitude and raise it slowly: this is the contrast
     control.
4. **VEE failsafe (strongly recommended).** STN panels are damaged by DC
   across the liquid crystal. That is what happens if VEE stays on while the
   clocks stop (for example, the Pi hangs or the driver crashes).
   - Feed FP (5V side of the buffer) into a **74HC123** retriggerable one-shot
     (B input; A = GND; /CLR = 5V). R = 100k and C = 1µF gives about 45 ms.
   - AND its Q output with GPIO 12 (74HCT08, or two transistors in series) to
     drive the VEE enable.
   - At 70 Hz, FP arrives every 14 ms. If it stops for more than about 45 ms,
     VEE is cut in hardware.
5. **Backlight.** CBLON/CBLHI are only control lines. Find out where the
   backlight inverter is (lid or mainboard) and how it is powered. If the
   inverter is on the dead mainboard, you'll need a replacement inverter
   (EL or CCFL, depending on the lamp).

## 2. Keyboard hardware (PJ8 → Pico)

| PJ8 | Signal | Pico GPIO |
|---|---|---|
| 1–11 | KBSC0–10 | GP0–GP10 |
| 12–19 | KBRTN0–7 | GP11–GP18 |
| 20 | N/C | |

The matrix is passive, so no level shifting is needed. You'll want an FFC/FPC
breakout that matches the ribbon's pitch (measure it, probably 1.0 or 1.25 mm).
Pins are defined in `keyboard/src/matrix_pins.h`.

## 3. Raspberry Pi setup

Add to the end of the single line in `/boot/firmware/cmdline.txt`:

```
isolcpus=3 nohz_full=3 rcu_nocbs=3 video=HDMI-A-1:640x400@70D
```

- `isolcpus` and friends keep Linux off core 3, so the scan thread runs
  undisturbed.
- `video=...D` forces a 640×400 framebuffer even with no monitor attached.
  That gives an 80×25 text console with the default 8×16 font. Check the
  result with `fbset`. If you end up with a different size, `--fb-offset`
  and `--fb-scale` pick the region to show.

Build and run on the Pi:

```bash
cd lcd && make && sudo ./t1kfb --help
```

`/dev/fb0` shows the text console. It also works for X11 using the `fbdev`
driver (`xserver-xorg-video-fbdev`). Wayland compositors render outside
`/dev/fb0`, so they won't appear yet. That would need a DRM capture source.

Run at boot once everything works: `sudo make install && sudo systemctl enable --now t1kfb`.

## 4. LCD bring-up procedure

Go step by step. Stop at the first thing that looks wrong.

1. **Scope only, no panel.** Run `sudo ./t1kfb --no-vee --stats` and check:
   - SCP, about 2.2 MHz in bursts
   - LP, 14 kHz
   - FP, 70 Hz
   - "overruns 0" in the stats

   If SCP edges look bad through the buffer, raise `--slowdown`.
2. **Panel connected, VEE supply disconnected.** Check that the logic current
   on LCD5V is sane: a few tens of mA.
3. **Connect VEE at minimal magnitude.** Run `sudo ./t1kfb --source test:calib`
   and slowly turn the VEE magnitude up until the image appears.
4. **Tune with the calib pattern** (border, solid top-left block, centre
   cross, thin diagonals):

   | Symptom | Fix |
   |---|---|
   | Diagonals look jagged or broken every 4 pixels | `--lsb-first` |
   | Image is shifted vertically by a line or wrapped | `--fp-line 1` (or 199) |
   | Dark and clear are swapped | `--invert` |
   | Upper and lower halves look swapped | `test:halves` shows which half is which; swap UD/LD in `pins.h` |
   | Flicker | raise `--fps`, try `--rt` |
   | Panel shows nothing but timing is right | try `--pvbl high`, `--pvbl low`, `--pvbl line` |

   `--pvbl frame` (the default) toggles PVBL every frame. That is correct if
   PVBL is the AC drive signal, and only causes flicker if it's an enable.
   Don't leave the panel running for long in a mode you're unsure of.
5. **Real output.** Run `sudo ./t1kfb --source fb --dither`.

Other test patterns: `checker`, `border`, `halves`, `hlines`, `vlines`,
`diag`, `all`, `none`. `--source pbm:image.pbm` shows a 1-bit image.

## 5. Keyboard firmware

```bash
export PICO_SDK_PATH=~/pico-sdk          # git clone --recursive https://github.com/raspberrypi/pico-sdk
cd keyboard && mkdir -p build && cd build
cmake ..                                 # add -DPICO_BOARD=pico2 for a Pico 2
make
```

Hold BOOTSEL, plug in the Pico, and copy `t1k_keyboard.uf2` to it. Then map
the keys:

```bash
pip install pyserial
python3 tools/map_keys.py /dev/cu.usbmodem*     # or /dev/ttyACM0 on Linux
```

Rebuild and flash again. Fn combos go in layer 1 of `keymap.h`, edited by
hand.

Any serial terminal on that port shows `down SC3 RTN5`-style events, which
helps when debugging the wiring. Keyboards without diodes can produce
"ghost" keys. The firmware ignores ambiguous 3-key rectangles instead of
reporting phantom keys.
