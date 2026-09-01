# NGLevelUp

A van levelling instrument on an **M5Stack CoreS3 SE + Module GNSS**, built in ESPHome.

Successor to [LevelUp](https://github.com/ChristopherJacob/LevelUp), which did the same job
in ~8,000 lines of ESP-IDF C on a Waveshare AMOLED board. The levelling mathematics is
carried over unchanged; everything else — Wi-Fi, web UI, OTA, Home Assistant integration,
display — is now ESPHome's problem instead of ours.

Measures roll and pitch, converts them to per-corner lift in inches for the vehicle's actual
track width and wheelbase, and tells you which corner to raise and by how much. Adds GNSS:
position, speed and course over ground, published to Home Assistant.

---

## What it does

- **Bubble level** with per-corner lift in inches (blocks mode) or ramp height (ramps mode)
- **Adaptive beeper** — cadence from 1200 ms down to 120 ms as you approach level, muted
  automatically while driving
- **GNSS** — latitude, longitude, altitude, speed, satellites, HDOP, and a compass page
  showing course over ground
- **Home Assistant** over the native API — 40-odd entities including the four corner lifts
  and a plain-text guidance string
- **Standalone web UI** on the device itself, so it stays configurable when HA is unreachable

Four pages, swipe left/right: **LEVEL → GNSS → COMPASS → SYSTEM**.

## Hardware

| Part | Notes |
|---|---|
| M5Stack **CoreS3 SE** | ESP32-S3, 16 MB flash, quad PSRAM, 320×240 ILI9342C |
| M5Stack **Module GNSS** | u-blox NEO-M9N, BMI270 IMU, BMP280, BMM150 |
| M5GO **Bottom2** base | optional; battery + LED bar (see caveats) |
| **External GNSS antenna** | not optional in practice — see below |

`docs/M5Stack-CoreS3-SE-GNSS-Reference.md` has the full pin map, M-Bus table and DIP switch
configuration, measured from the actual hardware.

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill it in.
2. Set the GNSS module's DIP switches to **RXD 4 ON, TXD 4 ON** (module TX → G18,
   module RX → G17). Only one switch per block may be on.
3. Keep `leveling.h` next to the YAML — `esphome: includes:` resolves relative to the config.
4. Flash. Then set Track Width and Wheelbase, place the device level, and press
   **HOLD TO ZERO**.

## Things that cost real time to work out

Most of these are not documented anywhere, which is the main reason this repo exists.

**The ESP32-S3 is 2.4 GHz only.** 802.11 b/g/n — it cannot see a 5 GHz SSID. And ESPHome
sorts candidate networks by `priority` *first*, then signal strength, so with two networks at
the default priority the loudest one wins regardless of which you meant.

**The IMU is not aligned with the screen.** On this stack the BMI270 sits rotated 180° about
the screen's vertical axis, corrected with `axis_map: {x: -x, y: y, z: -z}`. Separately,
LevelUp's QMI8658 sat 90° from it, so its "pitch" was the front/back axis and ours is not —
handled by swapping the first two arguments to `leveling_compute()` at the call site, which
leaves `leveling.h` byte-identical to the original.

**Clear the IMU calibration before measuring orientation.** `motion.calibrate_level` writes a
persisted matrix to NVS, and every reading afterwards passes through it. A stray zeroing at an
odd attitude produces convincing, entirely fictitious axis behaviour.

**The touchscreen needs help.** ESPHome's `ft63x6` releases reset and reads the chip ID with
no delay; the FT6336 needs ~300 ms and returns 0, logging a failure without marking the
component failed — so the follow-up config writes are silently lost too. Reset is done here
from an `on_boot` handler at priority 700 instead. The touchscreen must also be **polled**;
copying `update_interval: never` from an M5Stack config that has a real interrupt pin leaves
it silently dead.

**GNSS course is course over ground**, derived from the velocity solution — not a compass.
It is the direction of travel, not where the vehicle points, and it is meaningless when
stationary. The compass page hides the needle rather than showing a stale heading.

**The antenna decides whether GNSS works at all.** Indoors the receiver streams NMEA happily
while reporting no fix. The GNSS page shows characters processed and checksum failures so you
can tell "DIP switches wrong" from "no sky".

**The LED bar is a power trap.** Undriven, its data line floats and latches the LEDs at full
white — around 600 mA / 3 W, which ran the stack ~13 °C hotter and drained the battery while
plugged in. The config declares the strip purely to hold GPIO5 low. **Do not remove that
block**: it lights nothing, and deleting it brings the heat back.

## Known limitations

- **The LED bar cannot be driven.** Pin (G5) and chipset (SK6812) are confirmed from
  M5Stack's own pin-map tool, RMT allocates cleanly, and both 3- and 4-byte framing were
  tried; nothing ever lights. Unresolved.
- **The BMM150 magnetometer is unused.** It sits behind the BMI270's auxiliary interface,
  which ESPHome has no driver for. There is therefore no true heading while parked.
- **PPS and UBX** are unused. The `gps` component is NMEA-only and read-only, so navigation
  rate and constellation cannot be configured from here.

## Levelling maths

`leveling.h` is vendored from LevelUp, header-only so ESPHome can pull it in via
`esphome: includes:`. The mathematics is unchanged; the only edits were mechanical (C99
compound literals rewritten for C++, bodies made `static inline`) plus two extra output
fields. It still passes LevelUp's own test suite unmodified:

```sh
cc  -std=c99   -I. test/test_leveling.c -o t && ./t
c++ -std=c++17 -I. -x c++ test/test_leveling.c -o t && ./t
```

## Licence

CC0 1.0 Universal (public domain), matching LevelUp.
