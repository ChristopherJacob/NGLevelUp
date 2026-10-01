# NGLevelUp

A van levelling instrument on an **M5Stack CoreS3 SE + Module GNSS**, built in ESPHome —
grown into a small aviation-style instrument panel.

Successor to [LevelUp](https://github.com/ChristopherJacob/LevelUp), which did the levelling
job in ~8,000 lines of ESP-IDF C on different hardware. The levelling mathematics is carried
over unchanged; Wi-Fi, web UI, OTA, Home Assistant integration and the display are now
ESPHome's problem instead of ours.

---

## What it does

Seven pages, swipe left/right between them.

| Page | Shows |
|---|---|
| **LEVEL** | Bubble vial, per-corner lift in inches (blocks) or ramp height (ramps), hold-to-zero |
| **GNSS** | Position, satellites, HDOP, speed, course, and raw NMEA health counters |
| **COMPASS** | Course over ground on a dial, with the needle hidden when stationary |
| **ALTIMETER** | Two-pointer barometric altimeter with a settable datum and set-from-GPS |
| **ATTITUDE** | Artificial horizon with a slip/skid ball |
| **RATES** | Vertical speed and a G-meter |
| **SYSTEM** | Network, battery, temperatures, odometer, pin map |

Plus:

- **Adaptive beeper** — 1200 ms down to 120 ms as you approach level, suppressed while driving
- **Sleep mode** — ten minutes after the ignition-switched USB loses power, the screen and LED
  bar go off and LVGL pauses. Wakes on power returning or a screen touch
- **Trip odometer** — GNSS distance, persisted across reboots
- **Night dimming** — backlight follows the local clock between configurable day/night levels
- **Home Assistant** over the native API — around fifty entities
- **A standalone web UI** on the device, so it stays configurable when HA is unreachable

## Hardware

| Part | Notes |
|---|---|
| M5Stack **CoreS3 SE** | ESP32-S3, 16 MB flash, quad PSRAM, 320×240 ILI9342C |
| M5Stack **Module GNSS** | u-blox NEO-M9N, BMI270 IMU, BMP280, BMM150 |
| M5GO base | battery + a 10-LED bar (see caveats) |
| **External GNSS antenna** | not optional in practice — see below |

`docs/M5Stack-CoreS3-SE-GNSS-Reference.md` has the full pin map, M-Bus table and DIP switch
configuration, measured from the actual hardware.

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill it in.
2. Set the GNSS module's DIP switches to **RXD 4 ON, TXD 4 ON** (module TX → G18,
   module RX → G17). Only one switch per block may be on.
3. Keep `leveling.h` and `instruments.h` beside the YAML — `esphome: includes:` resolves
   relative to the config file, so every build host needs them.
4. Flash. Then set Track Width and Wheelbase, get the vehicle genuinely level **by an
   independent reference**, and press **HOLD TO ZERO**.

Step 4's wording is deliberate. Zeroing captures the current attitude as "level", so it
conflates the mounting offset (which you want to cancel) with the vehicle's attitude (which
you want to measure). Zero on a slope and the device will read level on that slope forever.

## Things that cost real time to work out

Most of this is documented nowhere else, which is the main reason this repo exists.

**The ESP32-S3 is 2.4 GHz only.** 802.11 b/g/n — it cannot see a 5 GHz SSID. And ESPHome
sorts candidate networks by `priority` *first*, then signal strength, so two networks at the
default priority means the loudest wins regardless of which you meant.

**The IMU is not aligned with the screen.** The BMI270 sits rotated 180° about the screen's
vertical axis, corrected with `axis_map: {x: -x, y: y, z: -z}`. Separately, LevelUp's QMI8658
sat 90° from it, so its "pitch" was the front/back axis and ours is not — handled by swapping
the first two arguments to `leveling_compute()` at the call site, leaving `leveling.h`
byte-identical to the original.

**Clear the IMU calibration before measuring orientation.** `motion.calibrate_level` persists
a 3×3 matrix to NVS and every later reading passes through it. A stray zeroing at an odd
attitude produces convincing, entirely fictitious axis behaviour — two full rounds of tilt
measurement were spent chasing a phantom rotation that was pure software artifact.

**The touchscreen needs help, twice.** ESPHome's `ft63x6` releases reset and reads the chip ID
with no delay; the FT6336 needs ~300 ms and returns 0, logging a failure *without* marking the
component failed — so the follow-up config writes are silently lost too. Reset is done here
from an `on_boot` handler at priority 700. The touchscreen must also be **polled**: copying
`update_interval: never` from an M5Stack config that has a real interrupt pin leaves it
silently dead.

**The LED bar is a power trap, and the chipset matters.** Undriven, its data line floats and
latches the LEDs at full white — about 600 mA / 3 W, which ran the stack ~13 °C hotter and
drained the battery while plugged in. Declaring the strip at all fixes that by holding the pin
low. Driving it needs **WS2812** timing on **GPIO5**; the same pin with SK6812 timing never
lit on the previous base.

**GNSS course is course over ground**, derived from the velocity solution — not a compass. It
is the direction of travel, not where the vehicle points, and it is meaningless when
stationary, so the compass hides its needle rather than showing a stale heading.

**The altimeter is barometric, not GNSS.** Pressure from the BMP280 plus a settable datum, the
way the real instrument works. GNSS altitude sits beside it to set *against* — there is a
one-press "set from GPS" — because a barometric needle is smooth and immediate where GNSS
altitude is the noisiest axis of a fix. At the 29.92 standard setting the gauge shows
*pressure* altitude, so being a few hundred feet from your true elevation is correct
behaviour, not a bug.

**The antenna decides whether GNSS works at all.** Indoors the receiver streams NMEA happily
while reporting no fix. The GNSS page shows characters processed and checksum failures so you
can tell "DIP switches wrong" from "no sky".

**The odometer's real constraint is flash wear.** Writing the total to NVS on every fix would
be thousands of writes an hour, so it accumulates in RAM and folds into the persisted total
every five minutes *and on ignition loss* — the power sensor tells us exactly when a trip
ended. Distance is computed in double: at road speeds consecutive fixes are metres apart and
float latitudes lose a short step in the subtraction.

## Known limitations

- **The BMM150 magnetometer is unused.** It sits behind the BMI270's auxiliary interface,
  which ESPHome has no driver for, so there is no true heading while parked.
- **PPS and UBX** are unused. The `gps` component is NMEA-only and read-only, so navigation
  rate and constellation cannot be configured from here.
- **Seven pages means up to six swipes** to reach one. A page-jump — a long-press menu, or an
  HA `select` driving `lvgl.page.show` — is the sensible next addition.
- **Night lighting is backlight dimming only.** A proper amber-on-black panel palette would
  mean converting every page to LVGL styles; not done.
- **Sleep keeps Wi-Fi, the API and GNSS alive**, so Home Assistant keeps the entity and trip
  logging continues. It is not a low-power deep sleep.

## The maths

Two header-only files, both host-testable, neither depending on ESPHome:

- **`leveling.h`** — vendored from LevelUp. The mathematics is unchanged; edits were
  mechanical (C99 compound literals rewritten for C++, bodies made `static inline`) plus two
  extra output fields and a block of shared device state. It still passes LevelUp's own test
  suite unmodified.
- **`instruments.h`** — new, and deliberately state-free so it stays testable: barometric
  altitude and its inverse, hPa/inHg, km/h→mph, m→ft, altimeter needle wrap, a smoothed
  rate-of-change for the VSI, G magnitude, and haversine distance.

```sh
cc  -std=c99   -Wall -I. test/test_leveling.c    -o t && ./t
cc  -std=c99   -Wall -I. test/test_instruments.c -o t && ./t
c++ -std=c++17 -Wall -I. -x c++ test/test_instruments.c -o t && ./t
```

## Licence

CC0 1.0 Universal (public domain), matching LevelUp.
