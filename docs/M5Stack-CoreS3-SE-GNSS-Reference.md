# M5Stack CoreS3 SE + Module GNSS — Firmware Development Reference

Hardware-verified reference for the stack on this bench.
Everything marked **[measured]** was confirmed by probing the actual device.
Items marked **[library]** or **[datasheet]** come from M5Unified source or vendor
docs and were not independently verified.

Generated 2026-08-25. Device-specific identifiers (MAC, unique ID) redacted for publication.

---

## 1. Device identity

| Property | Value | Source |
|---|---|---|
| Host board | **M5Stack CoreS3 SE** | [measured] `M5.getBoard()` → `17` = `board_M5StackCoreS3SE` |
| SoC | ESP32-S3, QFN56, revision **v0.2** | [measured] esptool |
| Cores | Dual Xtensa LX7 + LP core @ 240 MHz | [measured] |
| Radios | Wi-Fi b/g/n, Bluetooth 5 (LE) | [measured] |
| Flash | **16 MB** external quad-SPI, 3.3 V (XMC, `0x46`/`0x4018`) | [measured] |
| PSRAM | Enabled at boot (`psramInit(): PSRAM enabled`); 8 MB per spec | [measured] / [datasheet] |
| Base MAC | `xx:xx:xx:xx:xx:xx` *(redacted)* | [measured] |
| Unique ID | `<redacted>` | [measured] |
| USB | Native USB-Serial/JTAG, VID `0x303A` PID `0x1001` | [measured] |
| Secure Boot | **Disabled** | [measured] |
| Flash Encryption | **Disabled** | [measured] |
| eFuses | All key blocks empty, nothing read/write protected | [measured] |

### Stack composition

```
+---------------------------+
|  M5Stack CoreS3 SE        |  <- host, ESP32-S3
+---------------------------+
|  M5Stack Module GNSS      |  <- NEO-M9N + BMI270 + BMM150 + BMP280
+---------------------------+
```

> **Important:** the CoreS3 **SE** has **no onboard IMU, camera, magnetometer or
> proximity sensor** — M5Stack removed them from the standard CoreS3. Every
> motion/environmental sensor in this stack lives on the **GNSS module**.

---

## 2. I2C bus map

### Internal bus — `SDA = GPIO12`, `SCL = GPIO11` [measured]

| Addr | Device | Location | Notes |
|---|---|---|---|
| `0x34` | AXP2101 | CoreS3 SE | PMIC, battery/charge, rail control |
| `0x36` | AW88298 | CoreS3 SE | Speaker amplifier |
| `0x38` | FT6336 | CoreS3 SE | Capacitive touch controller |
| `0x40` | ES7210 | CoreS3 SE | Microphone ADC |
| `0x51` | BM8563 | CoreS3 SE | Real-time clock |
| `0x58` | AW9523 | CoreS3 SE | GPIO expander |
| `0x69` | **BMI270** | **GNSS module** | 6-axis IMU. `CHIP_ID = 0x24` verified. Address selected by on-module slide switch (`0x68`/`0x69`), currently `0x69` |
| `0x76` | **BMP280** | **GNSS module** | Barometer. `CHIP_ID = 0x58` verified |
| `0x10` | BMM150 | GNSS module | Magnetometer — **not directly addressable**. Sits behind the BMI270's auxiliary interface; reach it through the BMI270 |

### Port A (external Grove I2C) — `SDA = GPIO2`, `SCL = GPIO1` [measured]

Empty. Available for your own peripherals.

---

## 3. GNSS — u-blox NEO-M9N-00B-00

| Property | Value |
|---|---|
| Interface | **UART** |
| Host RX (module TX) | **GPIO18** [measured] |
| Host TX (module RX) | **GPIO17** [measured] |
| Baud | **38400**, 8N1 (NEO-M9N default) [measured] |
| Output | NMEA 0183 — `GNRMC`, `GNGGA`, `GNGSA`, `GxGSV`, `GNVTG` |
| Rate | ~12 sentences/sec observed |
| I2C (DDC) | Not exposed — `0x42` absent from both buses |
| Backup battery | Coin cell fitted (warm-start almanac retention) |
| Antenna | U.FL / IPEX connector |

### DIP switch configuration — CRITICAL

The module does **not** hardwire its UART. Three DIP blocks (`PPS`, `TXD`, `RXD`)
select which M-Bus pin each signal reaches. **Labels are from the HOST's
perspective**: `RXD` = the pin the CoreS3 receives on = where the GNSS transmits.

**Current working configuration: RXD switch 4 = ON, TXD switch 4 = ON.**

| Switch | Bus pin | CoreS3 SE pin | Safe? |
|---|---|---|---|
| **RXD 4** | mbus15 | **G18** | ✅ **in use** |
| RXD 3 | mbus22 | G7 | ✅ free GPIO |
| RXD 2 | mbus26 | G14 | ❌ collides with I2S DIN (mic) |
| RXD 1 | mbus2 | G10 | ⚠️ shared with PPS |
| **TXD 4** | mbus16 | **G17** | ✅ **in use** |
| TXD 3 | mbus23 | G13 | ❌ collides with I2S DOUT (speaker) |
| TXD 2 | mbus21 | G6 | ✅ free GPIO |
| TXD 1 | mbus24 | G0 | ❌ boot strap pin |

Only **one** switch per block may be ON.

> **Port C is consumed.** G17/G18 are the CoreS3's Grove Port C UART. With the
> GNSS module fitted, **Port C is unavailable** for anything else. If you need
> Port C back, move GNSS to RXD 3 / TXD 2 (G7 / G6).

---

## 4. CoreS3 SE pin assignments

### Onboard subsystems [library — M5Unified]

| Function | Pins |
|---|---|
| Internal I2C | SDA `GPIO12`, SCL `GPIO11` |
| External I2C (Port A) | SDA `GPIO2`, SCL `GPIO1` |
| Port B | `GPIO8`, `GPIO9` |
| Port C (UART) | RX `GPIO18`, TX `GPIO17` — **taken by GNSS** |
| I2S shared | BCK `GPIO34`, WS/LRCK `GPIO33`, MCLK `GPIO0` |
| Microphone (ES7210) | data in `GPIO14` |
| Speaker (AW88298) | data out `GPIO13` |
| SD card | CLK `GPIO36`, CMD `GPIO37`, D0 `GPIO35`, CS `GPIO4` |
| UART0 console | TX `GPIO43`, RX `GPIO44` |

### Full M-Bus map [library — M5Unified `_pin_table_mbus`]

| Bus pin | CoreS3 SE | Bus pin | CoreS3 SE |
|---|---|---|---|
| 1 | GND | 2 | `GPIO10` |
| 3 | GND | 4 | `GPIO8` |
| 5 | — | 6 | — |
| 7 | `GPIO37` | 8 | `GPIO5` |
| 9 | `GPIO35` | 10 | `GPIO9` |
| 11 | `GPIO36` | 12 | — |
| 13 | 3V3 | 14 | `GPIO43` (UART0 TX) |
| 15 | `GPIO18` | 16 | `GPIO17` |
| 17 | `GPIO12` (SDA) | 18 | `GPIO11` (SCL) |
| 19 | `GPIO2` | 20 | `GPIO1` |
| 21 | `GPIO6` | 22 | `GPIO7` |
| 23 | `GPIO13` | 24 | `GPIO0` |
| 25 | HPWR | 26 | `GPIO14` |
| 27 | HPWR | 28 | 5V |
| 29 | HPWR | 30 | BAT |

Note: bus pin 13/14 differ between the M5Unified GPIO table and the module's
silkscreen (which marks 13 as 3V3). Signal pins above are authoritative.

---

## 5. Build setup

### `platformio.ini` — known good

```ini
[env:cores3]
platform = espressif32          ; 6.10.0 tested
board = m5stack-cores3
framework = arduino
lib_deps =
    m5stack/M5Unified
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1 ; REQUIRED for Serial over native USB
    -DARDUINO_USB_MODE=1
    -DCORE_DEBUG_LEVEL=3
upload_port  = /dev/cu.usbmodem101
monitor_port = /dev/cu.usbmodem101
monitor_speed = 115200
```

### Minimal known-good sketch

```cpp
#include <M5Unified.h>
#include <Wire.h>

HardwareSerial GNSS(1);

void setup() {
  auto cfg = M5.config();
  cfg.external_imu = true;              // IMU is on the module, not the host
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Power.setExtOutput(true);

  Wire1.begin(12, 11, 100000);          // use Wire1, NOT Wire — see gotchas
  GNSS.begin(38400, SERIAL_8N1, 18, 17);
}

void loop() {
  M5.Imu.update();
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);       // BMI270 via M5Unified

  while (GNSS.available()) Serial.write(GNSS.read());   // raw NMEA
}
```

---

## 6. Gotchas — hard-won, all [measured]

1. **`Wire` (I2C port 0) is unusable.** M5Unified claims port 0 for its internal
   bus; calling `Wire.begin()` silently fails to scan. **Use `Wire1`**, or
   M5Unified's own `M5.In_I2C` / `M5.Ex_I2C`. To change pins on `Wire1`, call
   `Wire1.end()` before `Wire1.begin(sda, scl)` — otherwise pin changes are ignored.

2. **`TX` and `RX` are reserved Arduino macros.** Naming a variable or array `TX`
   or `RX` gives `conflicting declaration` errors. Use `TXP`/`RXP` or similar.

3. **`ARDUINO_USB_CDC_ON_BOOT=1` is mandatory** for `Serial` output over the
   native USB-Serial/JTAG port. Without it the port enumerates but stays silent.

4. **esptool: don't raise the baud.** `--baud 921600` corrupts the stream on
   native USB-JTAG (baud is meaningless there). Use the default. Flash reads run
   at roughly 92 kbit/s — a full 7 MB app partition takes about 12 minutes.

5. **`cfg.external_imu = true`** is required, or M5Unified won't find the
   module's BMI270 (the SE has no internal IMU to fall back on).

6. **Cold start needs sky.** The NEO-M9N streams NMEA immediately, but reports
   `fixQ=0`, `mode=1`, `hdop=99.99` and `00` satellites indoors. Allow several
   minutes with clear sky view for first fix. NMEA flowing ≠ fix acquired.

7. **macOS: no `timeout` in this shell.** Use
   `perl -e 'alarm 30; exec "cat /dev/cu.usbmodem101"'` to capture serial with a
   time limit.

---

## 7. Flash layout

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data/nvs | `0x9000` | 20 KB |
| `otadata` | data/ota | `0xe000` | 8 KB |
| `factory` | app/factory | `0x10000` | 7 MB |

Single-app scheme — **no OTA slots, no filesystem partition**. Add a custom
`partitions.csv` if you need OTA or SPIFFS/LittleFS.

NVS notably contains M5Stack's factory production-line Wi-Fi credentials
(SSID `M5Stack-Production`) and M5GFX's `AUTODETECT` board-ID cache.

### Factory firmware backup

The original M5Stack CoreS3 demo was overwritten during this work. A complete
backup of all allocated flash (`0x0`–`0x710000`) lives in the session scratchpad:

```
head64k.bin   0x00000000 – 0x00010000   bootloader + partition table + nvs
full.bin      0x00010000 – 0x00710000   factory app
```

Restore with:

```sh
esptool --port /dev/cu.usbmodem101 write-flash 0x0 head64k.bin 0x10000 full.bin
```

The demo is also re-downloadable via M5Burner.

---

## 8. Verified working state

```
GPS   : NMEA on GPIO18 @38400 — 1272 sentences / 105 s, no dropouts
IMU   : BMI270 @0x69 — accel z = -1.00 g (gravity), gyro live
BARO  : BMP280 @0x76 — present, CHIP_ID 0x58
MAG   : BMM150 — present behind BMI270 aux interface, untested
```

## 9. Not yet exercised

- **BMP280** pressure/temperature reads (present, no driver wired up)
- **BMM150** magnetometer via the BMI270 auxiliary interface
- **PPS** timing signal from the GNSS (third DIP block, currently unused)
- **UBX binary protocol** — for configuring rate, constellations, nav mode.
  Requires TXD (G17), which is already switched on
- Speaker, microphone, SD card, touch — all present but untested here
