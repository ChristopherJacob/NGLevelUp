# Instrument Panel Additions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn NGLevelUp into an aviation-style instrument panel: a speed-unit switch, ignition-power sensing with a sleep mode, and four new instrument pages (altimeter, attitude indicator with slip/skid ball, vertical speed with G-meter, trip odometer) plus night dimming — without changing or removing any existing page.

**Architecture:** New pure maths (pressure→altitude, unit conversions, altimeter needle wrap) goes in a new header-only `instruments.h` with host unit tests, mirroring how `leveling.h` is already vendored and tested. Device state (sleep flag, cached altitude) joins the existing `inline` shared-state block in `leveling.h`, because ESPHome emits `includes:` *after* `globals:` and so a `globals:` entry can never use an included type. Everything user-visible is additive: one new switch, one new binary sensor, one new page, three new config entities.

**Tech Stack:** ESPHome 2026.9.x (ESP-IDF), LVGL 9.x via ESPHome's `lvgl` component, C99/C++17 header-only maths, host tests with plain `cc`/`c++` and `assert`.

---

## Scope

Three independent features, small enough for one plan. They touch mostly disjoint parts of the config:

| Feature | Risk | Needs isolating? |
|---|---|---|
| Speed unit switch | low — display strings only | no |
| Ignition sense + sleep | medium — power, LVGL pause | yes, flash alone |
| Altimeter page | low — new page, new maths | no |
| Attitude indicator | medium — relies on `clip_corner` clipping a rotated child to a circle, unproven on this panel | yes, flash alone |
| VSI + G-meter page | low — new page, new maths | no |
| Trip odometer | medium — persists to NVS, so flash-wear matters | no |
| Night dimming | low as scoped (backlight only) | no |

The sleep work is the one to flash on its own. It can black the screen and pause LVGL, so if it misbehaves it looks identical to a crash — don't bundle it with anything.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `instruments.h` | Pure maths, grown across several tasks: barometric altitude and its inverse, hPa/inHg, km/h→mph, m→ft, altimeter needle wrap, smoothed rate-of-change for the VSI, G magnitude, haversine distance for the odometer. No state, no hardware, no ESPHome types. | **Create** |
| `test/test_instruments.c` | Host unit tests for `instruments.h`. Compiles as C99 and C++17. | **Create** |
| `leveling.h` | Unchanged maths. Gains three `inline` state variables in the existing shared-state block at the bottom. | Modify |
| `m5stack-cores3-gnss-lvgl.yaml` | The switch, the binary sensor, the sleep scripts, the altimeter page and its entities. | Modify |
| `docs/superpowers/plans/2026-09-30-instrument-panel.md` | This plan. | Created |

`instruments.h` is deliberately separate from `leveling.h`: `leveling.h`'s maths is vendored from LevelUp and still passes LevelUp's own test suite unmodified. That property is worth keeping, so new maths goes in a new file with its own tests.

## Conventions used throughout

- **Validate before compiling, compile before flashing, and never sync an unvalidated file.** Check for the literal string `Configuration is valid`; a bare exit code is not enough.
- **Grep two different warning patterns.** `^WARNING` catches ESPHome-level problems; `warning:` catches compiler ones. They are different, and checking only the first hides real issues.
- `$EH` below means the ESPHome binary in use, e.g. `~/esphome-venv/bin/esphome`. Substitute whatever is current.
- Secrets live on the build host. `esphome config` needs a `secrets.yaml` beside the YAML.

---

## Task 1: Put the working directory under git

The project directory is not a git repository, so there is nothing to commit to. The GitHub repo `ChristopherJacob/NGLevelUp` exists and is behind the local files. This task makes the local directory a working copy **without** overwriting local work and **without** pushing.

**Files:**
- Modify: `/Users/cjacob/Documents/Claude/Projects/NGLU/` (adds `.git/`)
- Create: `.gitignore`

- [ ] **Step 1: Initialise and attach the remote without touching the working tree**

```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
git init -q -b main
git remote add origin https://github.com/ChristopherJacob/NGLevelUp.git
git fetch -q origin
# mixed reset: index becomes origin/main, working tree is left completely alone
git reset -q origin/main
```

- [ ] **Step 2: Confirm nothing was overwritten**

Run:
```bash
git status --short
grep -c "chipset: WS2812" m5stack-cores3-gnss-lvgl.yaml
grep -c "g_led_bucket" leveling.h
```
Expected: `git status` lists modified/untracked files (the local work since Sept 1). The two greps print `1` and `3` — proving the WS2812 LED fix and the bucket variable are still present. **If either prints 0, stop:** the reset overwrote local work and you must recover from the newest local backup.

- [ ] **Step 3: Add a .gitignore**

```bash
cat > .gitignore <<'EOF'
secrets.yaml
.esphome/
*.bin
*.elf
.DS_Store
lvgl_ui_project/
OriginalTest.yaml
EOF
```

- [ ] **Step 4: Commit the current device state as the baseline**

```bash
git add -A
git commit -m "chore: local baseline — LED bar on WS2812, Roll/Pitch inches sensors

Brings the working copy up to what is actually flashed and verified on the
device: the LED bar driven as WS2812 on GPIO5 (the newer base needs that
timing, not SK6812), the level indicator restored behind its switch, and
Roll (in) / Pitch (in) for LevelUp parity."
```

Expected: commit succeeds. Nothing is pushed — `git log origin/main..HEAD` shows one local commit.

---

## Task 2: instruments.h and its host tests

Pure maths, test-first. Everything here is deterministic and runs on the host, so this is the one part of the plan that gets real TDD.

**Files:**
- Create: `instruments.h`
- Create: `test/test_instruments.c`

- [ ] **Step 1: Write the failing test**

Create `test/test_instruments.c`:

```c
// Host unit tests for instruments.h. Compiles as C99 and as C++17.
#include "instruments.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int close_to(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static void test_altitude_zero_when_pressure_equals_setting(void)
{
    // If station pressure equals the altimeter setting, you are at the
    // datum the setting refers to — zero feet.
    assert(close_to(inst_press_to_alt_ft(1013.25f, 1013.25f), 0.0f, 0.01f));
    assert(close_to(inst_press_to_alt_ft(1000.00f, 1000.00f), 0.0f, 0.01f));
}

static void test_altitude_increases_as_pressure_falls(void)
{
    float low  = inst_press_to_alt_ft(1000.0f, 1013.25f);
    float high = inst_press_to_alt_ft(900.0f, 1013.25f);
    assert(high > low);
    assert(low > 0.0f);           // 1000 hPa is above the 1013.25 datum
}

static void test_altitude_negative_below_datum(void)
{
    // Pressure higher than the setting means you are below the datum.
    assert(inst_press_to_alt_ft(1030.0f, 1013.25f) < 0.0f);
}

static void test_qnh_round_trip(void)
{
    // Deriving the setting from a known altitude must invert cleanly.
    const float p = 880.0f;
    const float q = 1011.3f;
    float alt = inst_press_to_alt_ft(p, q);
    float back = inst_alt_to_qnh_hpa(p, alt);
    assert(close_to(back, q, 0.05f));
}

static void test_invalid_inputs_return_nan(void)
{
    assert(isnan(inst_press_to_alt_ft(0.0f, 1013.25f)));
    assert(isnan(inst_press_to_alt_ft(-5.0f, 1013.25f)));
    assert(isnan(inst_press_to_alt_ft(1013.25f, 0.0f)));
    assert(isnan(inst_alt_to_qnh_hpa(0.0f, 1000.0f)));
}

static void test_pressure_unit_conversions(void)
{
    assert(close_to(inst_hpa_to_inhg(1013.25f), 29.9213f, 0.001f));
    assert(close_to(inst_inhg_to_hpa(29.92f), 1013.21f, 0.05f));
    // Round trip.
    assert(close_to(inst_inhg_to_hpa(inst_hpa_to_inhg(987.6f)), 987.6f, 0.01f));
}

static void test_speed_and_length_conversions(void)
{
    assert(close_to(inst_kmh_to_mph(100.0f), 62.1371f, 0.001f));
    assert(close_to(inst_kmh_to_mph(0.0f), 0.0f, 0.0001f));
    assert(close_to(inst_m_to_ft(1000.0f), 3280.84f, 0.01f));
}

static void test_needle_wrap(void)
{
    // Hundreds pointer: one revolution per 1000 ft, dial reads 0..10.
    assert(close_to(inst_alt_needle(0.0f, 1000.0f), 0.0f, 0.001f));
    assert(close_to(inst_alt_needle(500.0f, 1000.0f), 5.0f, 0.001f));
    assert(close_to(inst_alt_needle(1000.0f, 1000.0f), 0.0f, 0.001f));
    assert(close_to(inst_alt_needle(2500.0f, 1000.0f), 5.0f, 0.001f));

    // Thousands pointer: one revolution per 10000 ft.
    assert(close_to(inst_alt_needle(2500.0f, 10000.0f), 2.5f, 0.001f));
    assert(close_to(inst_alt_needle(12500.0f, 10000.0f), 2.5f, 0.001f));

    // Below sea level must wrap forward, not produce a negative needle.
    float n = inst_alt_needle(-200.0f, 1000.0f);
    assert(n >= 0.0f && n <= 10.0f);
    assert(close_to(n, 8.0f, 0.001f));

    assert(close_to(inst_alt_needle(500.0f, 0.0f), 0.0f, 0.001f));  // guard
}

int main(void)
{
    test_altitude_zero_when_pressure_equals_setting();
    test_altitude_increases_as_pressure_falls();
    test_altitude_negative_below_datum();
    test_qnh_round_trip();
    test_invalid_inputs_return_nan();
    test_pressure_unit_conversions();
    test_speed_and_length_conversions();
    test_needle_wrap();
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run it and confirm it fails to even compile**

Run:
```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti -lm
```
Expected: FAIL — `fatal error: 'instruments.h' file not found`.

- [ ] **Step 3: Write instruments.h**

Create `instruments.h`:

```c
// =============================================================================
//  instruments.h — pure maths for the NGLevelUp instrument pages
// =============================================================================
//  Header-only so ESPHome can pull it in with `esphome: includes:`. Compiles as
//  both C99 and C++ so test/test_instruments.c builds either way.
//
//  Deliberately contains NO state and NO hardware access, which is what makes
//  it testable on the host. Shared device state lives in leveling.h instead.
//
//  Kept separate from leveling.h because that file's maths is vendored from
//  LevelUp and still passes LevelUp's own test suite unmodified — a property
//  worth not disturbing.
// =============================================================================
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INST_STD_QNH_HPA    1013.25f     /* ISA sea-level pressure          */
#define INST_HPA_PER_INHG   33.863886f   /* exact by definition             */
#define INST_MPH_PER_KMH    0.6213712f
#define INST_FT_PER_M       3.2808399f

/* Exponent and scale of the standard hypsometric equation, in feet. These are
   the same constants a mechanical altimeter's aneroid linkage approximates. */
#define INST_ALT_SCALE_FT   145366.45f
#define INST_ALT_EXP        0.190284f

/* Barometric altitude in feet, from station pressure and the altimeter setting
   (QNH). Returns NAN for non-physical input rather than +/-inf, so callers can
   test with isnan() and display a dash. */
static inline float inst_press_to_alt_ft(float p_hpa, float qnh_hpa)
{
    if (!(p_hpa > 0.0f) || !(qnh_hpa > 0.0f)) return NAN;
    return INST_ALT_SCALE_FT * (1.0f - powf(p_hpa / qnh_hpa, INST_ALT_EXP));
}

/* Inverse: the altimeter setting that would make p_hpa read known_alt_ft.
   This is what "set the altimeter to field elevation" does. */
static inline float inst_alt_to_qnh_hpa(float p_hpa, float known_alt_ft)
{
    float ratio;
    if (!(p_hpa > 0.0f)) return NAN;
    ratio = 1.0f - known_alt_ft / INST_ALT_SCALE_FT;
    if (!(ratio > 0.0f)) return NAN;
    return p_hpa / powf(ratio, 1.0f / INST_ALT_EXP);
}

static inline float inst_hpa_to_inhg(float hpa)  { return hpa / INST_HPA_PER_INHG; }
static inline float inst_inhg_to_hpa(float inhg) { return inhg * INST_HPA_PER_INHG; }
static inline float inst_kmh_to_mph(float kmh)   { return kmh * INST_MPH_PER_KMH; }
static inline float inst_m_to_ft(float m)        { return m * INST_FT_PER_M; }

/* Position of a classic wrapping altimeter pointer on a 0..10 dial.
   ft_per_rev is how much altitude one full turn covers: 1000 for the hundreds
   pointer, 10000 for the thousands pointer. Negative altitudes wrap forward so
   the needle never jumps to the wrong side below sea level. */
static inline float inst_alt_needle(float alt_ft, float ft_per_rev)
{
    float w;
    if (!(ft_per_rev > 0.0f)) return 0.0f;
    w = fmodf(alt_ft, ft_per_rev);
    if (w < 0.0f) w += ft_per_rev;
    return w / (ft_per_rev / 10.0f);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif
```

- [ ] **Step 4: Run the tests as C99 and as C++17**

Run:
```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti_c  -lm && /tmp/ti_c
cp test/test_instruments.c /tmp/ti.cpp
c++ -std=c++17 -Wall -I. /tmp/ti.cpp -o /tmp/ti_cpp -lm && /tmp/ti_cpp
```
Expected: `ALL TESTS PASSED` twice, with no compiler warnings.

- [ ] **Step 5: Confirm leveling.h's own tests still pass**

`instruments.h` must not have disturbed anything. Run:
```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
curl -sL -o /tmp/test_leveling.c \
  https://raw.githubusercontent.com/ChristopherJacob/LevelUp/master/test/leveling/test_leveling.c
cc -std=c99 -Wall -I. /tmp/test_leveling.c -o /tmp/tl -lm && /tmp/tl
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add instruments.h test/test_instruments.c
git commit -m "feat: add instruments.h with barometric altitude and unit maths

Header-only and state-free so it unit-tests on the host, matching how
leveling.h is handled. Covers pressure->altitude, its inverse for setting
the altimeter from a known elevation, hPa/inHg, km/h->mph, m->ft, and the
wrapping needle position for a classic altimeter dial."
```

---

## Task 3: Speed unit switch

Adds a switch and makes the two places that print speed honour it. Nothing is removed.

**Files:**
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Include instruments.h**

Find the `esphome:` block's `includes:` list and add the new header so it reads:

```yaml
  includes:
    - leveling.h              # vendored from LevelUp — see file header
    - instruments.h           # pure maths for the instrument pages
```

- [ ] **Step 2: Add the switch**

In the `switch:` block, immediately before the entry whose comment is
`# silence the leveling beeper`, insert:

```yaml
  - platform: template       # display speed in mph instead of km/h
    id: speed_mph            # affects the on-screen readouts only; the HA
    name: "Speed in mph"     # `Speed` sensor keeps its km/h unit and history
    icon: "mdi:speedometer"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_ON
    entity_category: config

```

- [ ] **Step 3: Make the compass page honour it**

In the `lvgl_tick` interval's lambda, replace these three lines:

```cpp
          snprintf(b, sizeof(b), "%.1f km/h",
                   tg.speed.isValid() ? tg.speed.kmph() : 0.0);
          lv_label_set_text(id(lbl_cmp_spd), b);
```

with:

```cpp
          {
            float kmh = tg.speed.isValid() ? (float) tg.speed.kmph() : 0.0f;
            bool imp = id(speed_mph).state;
            snprintf(b, sizeof(b), "%.1f %s",
                     imp ? inst_kmh_to_mph(kmh) : kmh, imp ? "mph" : "km/h");
            lv_label_set_text(id(lbl_cmp_spd), b);
          }
```

- [ ] **Step 4: Make the GNSS page honour it**

In the same lambda, replace:

```cpp
          snprintf(b, sizeof(b), "ALT %.0fm  SPD %.1f  COG %s",
                   tg.altitude.isValid() ? tg.altitude.meters() : 0.0,
                   tg.speed.isValid() ? tg.speed.kmph() : 0.0,
                   cog_txt);
          lv_label_set_text(id(lbl_gnss_b), b);
```

with:

```cpp
          {
            float kmh = tg.speed.isValid() ? (float) tg.speed.kmph() : 0.0f;
            bool imp = id(speed_mph).state;
            snprintf(b, sizeof(b), "ALT %.0fm  SPD %.1f %s  COG %s",
                     tg.altitude.isValid() ? tg.altitude.meters() : 0.0,
                     imp ? inst_kmh_to_mph(kmh) : kmh, imp ? "mph" : "km/h",
                     cog_txt);
            lv_label_set_text(id(lbl_gnss_b), b);
          }
```

- [ ] **Step 5: Add an mph sensor for Home Assistant**

This is additive — the existing `Speed` sensor keeps its km/h unit so its
history stays intact. In the `sensor:` block, immediately before the comment
`# Tilt as a height difference across each axis`, insert:

```yaml
  - platform: template
    id: gps_speed_mph
    name: "Speed (mph)"
    unit_of_measurement: mph
    accuracy_decimals: 1
    update_interval: 10s
    lambda: |-
      auto &tg = id(gnss).get_tiny_gps();
      if (!tg.speed.isValid()) return NAN;
      return inst_kmh_to_mph((float) tg.speed.kmph());

```

- [ ] **Step 6: Validate**

Run:
```bash
cd <build dir with secrets.yaml>
$EH config m5stack-cores3-gnss-lvgl.yaml 2>&1 | tail -3
```
Expected: output contains `Configuration is valid!`

- [ ] **Step 7: Compile and check both warning classes**

Run:
```bash
$EH compile m5stack-cores3-gnss-lvgl.yaml > /tmp/b.log 2>&1; echo "exit=$?"
sed 's/\x1b\[[0-9;]*[mK]//g' /tmp/b.log | grep -E "Successfully compiled|error:"
sed 's/\x1b\[[0-9;]*[mK]//g' /tmp/b.log | grep -E "^WARNING" | grep -v "Cache entry"
sed 's/\x1b\[[0-9;]*[mK]//g' /tmp/b.log | grep -oE "(m5stack[a-z0-9-]*\.yaml|instruments\.h|leveling\.h):[0-9]+:[0-9]+.*warning: .*"
```
Expected: `exit=0`, `INFO Successfully compiled program.`, and no output from either warning grep.

- [ ] **Step 8: Flash and verify on device**

Flash, then swipe to the COMPASS page. Expected: speed reads `mph`. Toggle
`Speed in mph` off in Home Assistant or the device web UI; the label switches
to `km/h` within 250 ms. Check the GNSS page shows the same unit.

- [ ] **Step 9: Commit**

```bash
git add m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: switchable mph/km-h on the speed readouts

Adds a 'Speed in mph' switch, defaulting to mph, honoured by the COMPASS
and GNSS pages. Adds a separate 'Speed (mph)' sensor for Home Assistant
rather than changing the existing Speed sensor's unit, which would orphan
its history."
```

---

## Task 4: Ignition power sensing

Detect whether the ignition-switched USB port is live. This task only *reports*
it — the sleep behaviour is Task 5, so that a misreading sensor is caught before
it can black the screen.

**Files:**
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Add the binary sensor**

In the `binary_sensor:` block, immediately before the entry with
`id: van_is_level`, insert:

```yaml
  - platform: template
    id: ign_power
    name: "Ignition Power"
    device_class: plug
    icon: "mdi:car-battery"
    # isVbusIn() reads AXP2101 STATUS2 bit 3 together with VBUS-good from
    # STATUS1 bit 5, so it reports whether the USB rail is actually supplying
    # power rather than whether a cable is merely plugged in.
    # The delays matter: cranking browns the rail out for a moment, and a
    # 10 s delayed_off stops that registering as ignition-off.
    lambda: 'return id(axp2101_pmu).isVbusIn();'
    filters:
      - delayed_on: 2s
      - delayed_off: 10s

```

- [ ] **Step 2: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings from either grep.

- [ ] **Step 3: Flash and verify the sensor tracks the ignition**

With the engine off, `Ignition Power` should read **off** within ~10 s. Turn the
ignition on; it should read **on** within ~2 s. Expected: it tracks, and does
*not* flicker during cranking.

**If it reads permanently on:** the port is not actually ignition-switched, or
the AXP2101 still sees the battery path as VBUS. Stop and reconsider before
Task 5 — a sensor stuck on means sleep never triggers, and one stuck off means
the device sleeps while you are using it.

- [ ] **Step 4: Commit**

```bash
git add m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: report ignition power state from the AXP2101

Template binary sensor over isVbusIn(), debounced 2 s on / 10 s off so
engine cranking does not read as the ignition being switched off. Reporting
only; the sleep behaviour it feeds lands separately."
```

---

## Task 5: Sleep mode

Ten minutes after ignition power disappears, turn off the screen and the LED bar
and pause LVGL. Wake on power returning or on a screen touch.

**Flash this task on its own.** A fault here looks exactly like a crashed device.

**Files:**
- Modify: `leveling.h`
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Add the sleep flag to shared state**

In `leveling.h`, in the `inline` block at the bottom, after the
`g_led_bucket` line, add:

```c
inline bool     g_asleep        = false; // screen/LEDs off, LVGL paused
```

Note this goes in `leveling.h` and not `instruments.h` because it is device
state, and `instruments.h` is kept free of state so it stays host-testable.

- [ ] **Step 2: Confirm leveling.h's tests still pass**

Run:
```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cc -std=c99 -Wall -I. /tmp/test_leveling.c -o /tmp/tl -lm && /tmp/tl
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 3: Give the lvgl component an id**

`lvgl.pause` and `lvgl.resume` need to address it. Change the `lvgl:` block's
first lines from:

```yaml
lvgl:
  displays: [lcd]
```

to:

```yaml
lvgl:
  id: lvgl_main
  displays: [lcd]
```

- [ ] **Step 4: Add the sleep and wake scripts**

In the existing `script:` block, after the `apply_log_level` script, add:

```yaml
  # --- Ignition sleep -------------------------------------------------------
  # mode: restart means each loss of power restarts the countdown rather than
  # stacking a second one, and script.stop cancels it if power comes back.
  - id: sleep_timer
    mode: restart
    then:
      - delay: 10min
      - script.execute: enter_sleep

  - id: enter_sleep
    then:
      - lambda: 'g_asleep = true;'
      - light.turn_off: led_bar
      - light.turn_off: lcd_backlight
      - lvgl.pause:
          id: lvgl_main

  - id: exit_sleep
    then:
      - lambda: |-
          if (!g_asleep) return;
          g_asleep = false;
          g_led_bucket = -2;        // force the LED bar to repaint its colour
      - lvgl.resume:
          id: lvgl_main
      - light.turn_on:
          id: lcd_backlight
          brightness: 80%
```

- [ ] **Step 5: Drive the scripts from the power sensor**

Add these two automations to the `ign_power` binary sensor created in Task 4,
after its `filters:` list:

```yaml
    on_release:                      # ignition power lost
      then:
        - script.execute: sleep_timer
    on_press:                        # ignition power restored
      then:
        - script.stop: sleep_timer
        - script.execute: exit_sleep
```

- [ ] **Step 6: Add touch-to-wake**

This matters more than it looks: you park, switch off the ignition, and *then*
level the van — which is exactly when the countdown is running. Without this,
the device sleeps mid-job and the only way back is restarting the engine.

In the `touchscreen:` block, after the `calibration:` mapping, add:

```yaml
    on_release:
      then:
        # Only acts while asleep. When awake, page navigation is handled by
        # LVGL's own on_swipe_* triggers, and LVGL is paused while asleep so
        # those cannot fire — the two never both respond to one touch.
        - if:
            condition:
              lambda: 'return g_asleep;'
            then:
              - script.stop: sleep_timer
              - script.execute: exit_sleep
              - script.execute: sleep_timer
```

Note the final `script.execute: sleep_timer` — waking by touch restarts the
ten-minute countdown, so the screen does not stay on all night after a stray
knock.

- [ ] **Step 7: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings.

- [ ] **Step 8: Flash and verify, in this order**

1. With ignition **on**, confirm the screen is lit and pages still swipe.
2. Switch the ignition **off**. Confirm the screen *stays on* — the countdown is
   running, nothing should change yet.
3. Within the ten minutes, touch the screen and confirm nothing breaks.
4. Wait out the full ten minutes. Expected: screen goes dark, LED bar goes out.
5. Touch the screen. Expected: it wakes within a second, and page swiping works.
6. Wait ten more minutes without touching. Expected: it sleeps again.
7. Switch the ignition **on**. Expected: it wakes and stays awake.

For step 4, temporarily changing `delay: 10min` to `delay: 30s` makes this far
less tedious — just remember to change it back and re-flash.

- [ ] **Step 9: Commit**

```bash
git add leveling.h m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: sleep the display and LEDs 10 min after ignition power is lost

Screen off, LED bar off, LVGL paused. Wakes on power returning or on a
screen touch, and a touch wake restarts the countdown. Touch-to-wake is
load-bearing rather than a nicety: levelling happens after the ignition is
switched off, which is exactly when the countdown is running.

WiFi, the API and GNSS deliberately stay up, so Home Assistant keeps the
entity and position logging continues."
```

---

## Task 6: Altimeter page

A classic two-pointer barometric altimeter with a settable altimeter setting,
plus a GNSS altitude readout to cross-check against. Added as a fifth page; the
existing four are untouched.

**Files:**
- Modify: `leveling.h`
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Add the cached altitude to shared state**

In `leveling.h`, after the `g_asleep` line added in Task 5, add:

```c
inline float    g_alt_ft        = NAN;   // barometric altitude, feet
```

- [ ] **Step 2: Confirm leveling.h's tests still pass**

Run:
```bash
cc -std=c99 -Wall -I. /tmp/test_leveling.c -o /tmp/tl -lm && /tmp/tl
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 3: Add the altimeter setting number and the set-from-GPS button**

In the `number:` block, after the `beep_volume` entry, add:

```yaml
  - platform: template
    id: qnh_inhg
    name: "Altimeter Setting"
    icon: "mdi:gauge"
    unit_of_measurement: inHg
    min_value: 28.00
    max_value: 31.50
    step: 0.01
    initial_value: 29.92        # ISA standard, 1013.25 hPa
    restore_value: true
    optimistic: true
    mode: box
    entity_category: config
```

In the `button:` block, after the `btn_clear_cal` entry, add:

```yaml
  - platform: template
    id: btn_qnh_from_gps
    name: "Set Altimeter From GPS"
    icon: "mdi:crosshairs-gps"
    entity_category: config
    on_press:
      then:
        - lambda: |-
            auto &tg = id(gnss).get_tiny_gps();
            if (!tg.altitude.isValid() || !id(baro_press).has_state()) {
              ESP_LOGW("alt", "no GNSS altitude or pressure yet");
              return;
            }
            float gps_ft = inst_m_to_ft((float) tg.altitude.meters());
            float q_hpa = inst_alt_to_qnh_hpa(id(baro_press).state, gps_ft);
            if (std::isnan(q_hpa)) {
              ESP_LOGW("alt", "could not derive setting");
              return;
            }
            float inhg = inst_hpa_to_inhg(q_hpa);
            if (inhg < 28.0f || inhg > 31.5f) {
              ESP_LOGW("alt", "derived setting %.2f inHg out of range", inhg);
              return;
            }
            auto call = id(qnh_inhg).make_call();
            call.set_value(inhg);
            call.perform();
            ESP_LOGI("alt", "setting -> %.2f inHg from GPS %.0f ft", inhg, gps_ft);
```

- [ ] **Step 4: Add the page**

In the `lvgl:` `pages:` list, immediately before the line
`    # --------------------------------------------------------------- SYSTEM --`,
insert:

```yaml
    # ------------------------------------------------------------- ALTIMETER --
    # Barometric, like the real instrument: pressure from the BMP280 plus a
    # settable datum. GNSS altitude is shown alongside to set it against, not
    # as the gauge source — a barometric needle moves smoothly and immediately,
    # where GNSS altitude is noisy and the least accurate axis of a fix.
    - id: pg_alt
      bg_color: 0x101418
      bg_opa: COVER
      on_swipe_left:
        - lvgl.page.next:
            animation: MOVE_LEFT
            time: 200ms
      on_swipe_right:
        - lvgl.page.previous:
            animation: MOVE_RIGHT
            time: 200ms
      widgets:
        - obj:
            x: 0
            y: 0
            width: 320
            height: 24
            radius: 0
            border_width: 0
            pad_all: 0
            bg_color: 0x7A8794
            bg_opa: COVER
            widgets:
              - label: {x: 6, y: 4, text: "ALTIMETER", text_font: f_head, text_color: 0x101418}
              - label: {id: lbl_alt_src, x: 110, y: 6, text: "", text_font: f_label, text_color: 0x101418}

        - meter:
            id: alt_meter
            x: 10
            y: 36
            width: 188
            height: 188
            radius: 94
            pad_all: 0
            bg_color: 0x1B2128
            bg_opa: COVER
            border_color: 0x7A8794
            border_width: 2
            scales:
              - id: alt_scale
                range_from: 0
                range_to: 10
                angle_range: 360
                rotation: 270          # 0 at the top, like the real dial
                ticks:
                  count: 51            # a tick every 20 ft
                  width: 1
                  length: 6
                  color: 0x7A8794
                  major:
                    stride: 5          # labelled every 100 ft
                    width: 3
                    length: 12
                    color: 0xE8EDF2
                    label_gap: 8
                indicators:
                  # Thousands pointer: short and fat, drawn first so the
                  # hundreds pointer sits on top of it.
                  - line:
                      id: alt_needle_1000
                      width: 7
                      color: 0x35C46A
                      length: 48
                      value: 0
                  # Hundreds pointer: long and thin.
                  - line:
                      id: alt_needle_100
                      width: 4
                      color: 0xE8EDF2
                      length: 80
                      value: 0

        # ---- Kollsman window and readouts ----
        - label: {x: 208, y: 40,  text: "ALTITUDE",  text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_alt_ft,   x: 208, y: 54,  text: "--",  text_font: f_huge, text_color: 0xE8EDF2}
        - label: {x: 208, y: 94,  text: "SETTING",   text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_alt_qnh,  x: 208, y: 108, text: "--",  text_font: f_val,  text_color: 0x35C46A}
        - label: {x: 208, y: 140, text: "GPS ALT",   text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_alt_gps,  x: 208, y: 154, text: "--",  text_font: f_val,  text_color: 0xE8EDF2}
        - label: {id: lbl_alt_hpa,  x: 208, y: 186, text: "",    text_font: f_tiny, text_color: 0x7A8794}
        - label: {id: lbl_alt_note, x: 208, y: 204, text: "",    text_font: f_tiny, text_color: 0x7A8794}

```

- [ ] **Step 5: Compute the altitude and fill the labels**

In the `lvgl_tick` lambda, immediately before the comment
`// ---- SYSTEM page ----`, insert:

```cpp
          // ---- ALTIMETER page ----
          {
            float qnh_hpa = inst_inhg_to_hpa(id(qnh_inhg).state);
            if (id(baro_press).has_state()) {
              g_alt_ft = inst_press_to_alt_ft(id(baro_press).state, qnh_hpa);
            }
            if (!std::isnan(g_alt_ft)) {
              snprintf(b, sizeof(b), "%.0f ft", g_alt_ft);
              lv_label_set_text(id(lbl_alt_ft), b);
            } else {
              lv_label_set_text(id(lbl_alt_ft), "--");
            }
            snprintf(b, sizeof(b), "%.2f inHg", id(qnh_inhg).state);
            lv_label_set_text(id(lbl_alt_qnh), b);

            if (tg.altitude.isValid()) {
              snprintf(b, sizeof(b), "%.0f ft",
                       inst_m_to_ft((float) tg.altitude.meters()));
              lv_label_set_text(id(lbl_alt_gps), b);
            } else {
              lv_label_set_text(id(lbl_alt_gps), "--");
            }

            if (id(baro_press).has_state()) {
              snprintf(b, sizeof(b), "station %.1f hPa", id(baro_press).state);
              lv_label_set_text(id(lbl_alt_hpa), b);
            }
            lv_label_set_text(id(lbl_alt_src), "BAROMETRIC");
            lv_label_set_text(id(lbl_alt_note),
                              tg.altitude.isValid() ? "GPS available to set from"
                                                    : "no GPS altitude");
          }

```

- [ ] **Step 6: Move the needles**

The needles are set with an action, not from the lambda. In the same
`- interval: 250ms` entry, after the single `- lambda: |-` block, add two more
list items at the same indentation as that `- lambda:`:

```yaml
      - lvgl.indicator.update:
          id: alt_needle_100
          value: !lambda 'return std::isnan(g_alt_ft) ? 0.0f : inst_alt_needle(g_alt_ft, 1000.0f);'
      - lvgl.indicator.update:
          id: alt_needle_1000
          value: !lambda 'return std::isnan(g_alt_ft) ? 0.0f : inst_alt_needle(g_alt_ft, 10000.0f);'
```

- [ ] **Step 7: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings.

- [ ] **Step 8: Flash and verify**

1. Swipe to the ALTIMETER page. Expected: both needles drawn, altitude in feet,
   setting reading `29.92 inHg`.
2. Sanity-check the number: with the setting at 29.92 the altitude shown is
   *pressure altitude*, which at a fixed location will differ from true
   elevation by however far the weather is from standard — being out by several
   hundred feet is correct behaviour, not a bug.
3. Press **Set Altimeter From GPS**. Expected: the setting changes, the altitude
   jumps to match GPS altitude, and the log shows
   `setting -> NN.NN inHg from GPS NNNN ft`.
4. Confirm the needles agree with the digits: hundreds pointer at
   `(alt mod 1000)/100`, thousands pointer at `(alt mod 10000)/1000`.
5. **Check the dial labels.** `range_to: 10` over 360° puts 0 and 10 at the same
   place, so the two labels may overlap at the top. If it looks wrong, note it —
   fixing it means dropping `major:` and placing nine positioned labels by hand.
6. Confirm the other four pages still render and swipe.

- [ ] **Step 9: Commit**

```bash
git add leveling.h m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: add a barometric altimeter page

Classic two-pointer dial from the BMP280 with a settable altimeter setting
in inHg, plus a one-press 'set from GPS' that derives the setting from the
GNSS altitude. GNSS altitude is shown beside the gauge to set against
rather than driving it: a barometric needle is smooth and immediate, where
GNSS altitude is noisy and the weakest axis of a fix."
```

---

## Task 7: Attitude indicator page

A classic artificial horizon with a slip/skid ball beneath it. The horizon is a
rotated, vertically-offset child clipped to a circular parent.

**Flash this task on its own.** It depends on LVGL clipping a rotated child to a
parent's `radius` via `clip_corner`, which is correct per the style schema but
unverified on this panel. Step 8 includes the fallback if it clips to the
bounding box instead.

**Files:**
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Add an unthrottled lateral-acceleration source**

The existing `acc_x` is throttled to 5 s for Home Assistant, far too slow for a
ball. Add a fast internal copy. In the `sensor:` block, immediately before the
comment `# Fast internal path for the level display`, insert:

```yaml
  # Fast lateral acceleration for the slip/skid ball. The HA-facing acc_x is
  # throttled to 5 s, which would make the ball lurch once every five seconds.
  - platform: motion
    motion_id: imu
    type: acceleration_x
    id: acc_x_fast
    internal: true
    filters:
      - median: {window_size: 5, send_every: 1, send_first_at: 1}
      - exponential_moving_average: {alpha: 0.2, send_every: 1}

```

- [ ] **Step 2: Add the page**

In the `lvgl:` `pages:` list, immediately before the line
`    # --------------------------------------------------------------- SYSTEM --`,
insert:

```yaml
    # -------------------------------------------------------------- ATTITUDE --
    # Artificial horizon from the same calibrated pitch/roll the LEVEL page
    # uses, plus a slip/skid ball. The ball is driven by lateral acceleration
    # rather than by roll angle, so it reads tilt while parked AND cornering
    # while driving — which is what the real instrument does.
    - id: pg_att
      bg_color: 0x101418
      bg_opa: COVER
      on_swipe_left:
        - lvgl.page.next:
            animation: MOVE_LEFT
            time: 200ms
      on_swipe_right:
        - lvgl.page.previous:
            animation: MOVE_RIGHT
            time: 200ms
      widgets:
        - obj:
            x: 0
            y: 0
            width: 320
            height: 24
            radius: 0
            border_width: 0
            pad_all: 0
            bg_color: 0x7A8794
            bg_opa: COVER
            widgets:
              - label: {x: 6, y: 4, text: "ATTITUDE", text_font: f_head, text_color: 0x101418}
              - label: {id: lbl_att_hdr, x: 110, y: 6, text: "", text_font: f_label, text_color: 0x101418}

        # ---- horizon ball ----
        # clip_corner is what keeps the rotated child inside the circle.
        - obj:
            id: att_ball
            x: 14
            y: 34
            width: 190
            height: 190
            radius: 95
            clip_corner: true
            pad_all: 0
            bg_color: 0x101418
            bg_opa: COVER
            border_color: 0x7A8794
            border_width: 2
            widgets:
              # Oversized so no corner can rotate into view: the parent's
              # diagonal is 269 px, this is 300.
              - obj:
                  id: att_horizon
                  align: CENTER
                  width: 300
                  height: 300
                  radius: 0
                  pad_all: 0
                  border_width: 0
                  bg_opa: TRANSP
                  transform_pivot_x: 150
                  transform_pivot_y: 150
                  widgets:
                    - obj:
                        x: 0
                        y: 0
                        width: 300
                        height: 150
                        radius: 0
                        border_width: 0
                        pad_all: 0
                        bg_color: 0x2E6FA8      # sky
                        bg_opa: COVER
                    - obj:
                        x: 0
                        y: 150
                        width: 300
                        height: 150
                        radius: 0
                        border_width: 0
                        pad_all: 0
                        bg_color: 0x7A5230      # ground
                        bg_opa: COVER
                    - obj:                       # horizon line
                        x: 0
                        y: 148
                        width: 300
                        height: 3
                        radius: 0
                        border_width: 0
                        pad_all: 0
                        bg_color: 0xE8EDF2
                        bg_opa: COVER
              # Fixed aircraft reference, drawn over the moving horizon.
              - obj:
                  align: CENTER
                  width: 64
                  height: 3
                  radius: 0
                  border_width: 0
                  pad_all: 0
                  bg_color: 0xE0A030
                  bg_opa: COVER
              - obj:
                  align: CENTER
                  width: 3
                  height: 3
                  radius: 0
                  border_width: 0
                  pad_all: 0
                  bg_color: 0xE0A030
                  bg_opa: COVER

        # ---- slip/skid ball, in its curved-tube surround ----
        - obj:
            id: att_tube
            x: 44
            y: 196
            width: 130
            height: 26
            radius: 13
            pad_all: 0
            bg_color: 0x1B2128
            bg_opa: COVER
            border_color: 0x7A8794
            border_width: 1
            widgets:
              - obj:                       # left cage mark
                  align: CENTER
                  x: -11
                  width: 2
                  height: 20
                  radius: 0
                  border_width: 0
                  pad_all: 0
                  bg_color: 0x7A8794
                  bg_opa: COVER
              - obj:                       # right cage mark
                  align: CENTER
                  x: 11
                  width: 2
                  height: 20
                  radius: 0
                  border_width: 0
                  pad_all: 0
                  bg_color: 0x7A8794
                  bg_opa: COVER
              - obj:
                  id: att_ball_slip
                  align: CENTER
                  width: 18
                  height: 18
                  radius: 9
                  pad_all: 0
                  border_width: 0
                  bg_color: 0xE8EDF2
                  bg_opa: COVER

        # ---- readouts ----
        - label: {x: 212, y: 40,  text: "PITCH",  text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_att_pitch, x: 212, y: 54,  text: "--", text_font: f_val, text_color: 0xE8EDF2}
        - label: {x: 212, y: 84,  text: "ROLL",   text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_att_roll,  x: 212, y: 98,  text: "--", text_font: f_val, text_color: 0xE8EDF2}
        - label: {x: 212, y: 128, text: "LATERAL", text_font: f_label, text_color: 0x7A8794}
        - label: {id: lbl_att_lat,   x: 212, y: 142, text: "--", text_font: f_val, text_color: 0xE8EDF2}
        - label: {id: lbl_att_note,  x: 212, y: 196, text: "", text_font: f_tiny, text_color: 0x7A8794}

```

- [ ] **Step 3: Drive the horizon and the ball**

In the `lvgl_tick` lambda, immediately before the comment
`// ---- SYSTEM page ----`, insert:

```cpp
          // ---- ATTITUDE page ----
          {
            // Van-frame angles, so the horizon agrees with the LEVEL page.
            float p_deg = g_lvl.guidance_available ? g_lvl.front_high_deg : 0.0f;
            float r_deg = g_lvl.guidance_available ? g_lvl.left_high_deg  : 0.0f;

            // 3 px per degree, clamped so the horizon cannot leave the ball.
            float py = p_deg * 3.0f;
            if (py >  80.0f) py =  80.0f;
            if (py < -80.0f) py = -80.0f;

            lv_obj_t *hz = id(att_horizon).obj;
            // LVGL angles are in 0.1 degree units. Negated so a raised left
            // side rolls the horizon the way the real instrument does — if it
            // reads backwards on hardware, drop the minus sign.
            lv_obj_set_style_transform_rotation(hz, (int32_t) (-r_deg * 10.0f),
                                                LV_PART_MAIN);
            lv_obj_align(hz, LV_ALIGN_CENTER, 0, (int32_t) py);

            // Slip/skid ball: lateral acceleration, 1 g over half the tube.
            float lat_g = id(acc_x_fast).has_state() ? id(acc_x_fast).state : 0.0f;
            float bx = lat_g * 50.0f;
            if (bx >  48.0f) bx =  48.0f;
            if (bx < -48.0f) bx = -48.0f;
            lv_obj_align(id(att_ball_slip), LV_ALIGN_CENTER, (int32_t) bx, 0);

            snprintf(b, sizeof(b), "%+.1f\u00B0", p_deg);
            lv_label_set_text(id(lbl_att_pitch), b);
            snprintf(b, sizeof(b), "%+.1f\u00B0", r_deg);
            lv_label_set_text(id(lbl_att_roll), b);
            snprintf(b, sizeof(b), "%+.2f g", lat_g);
            lv_label_set_text(id(lbl_att_lat), b);
            lv_label_set_text(id(lbl_att_hdr),
                              g_lvl.guidance_available ? "" : "SET DIMENSIONS");
            lv_label_set_text(id(lbl_att_note),
                              fabsf(lat_g) < 0.03f ? "centred" : "");
          }

```

- [ ] **Step 4: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings from either grep.

- [ ] **Step 5: Flash and verify the clipping**

Swipe to ATTITUDE. Expected: a circular ball, blue over brown, horizon line
across the middle, amber aircraft reference fixed in the centre.

**The thing to look at first is the corners.** If you can see square sky/ground
edges poking outside the circle, `clip_corner` is not clipping a transformed
child and you need the fallback in Step 8.

- [ ] **Step 6: Verify the horizon moves correctly**

Tilt the device (or the van). Expected: lifting the front drops the horizon,
lifting the left side rotates it. Confirm against the LEVEL page, which is the
reference — both read the same van-frame angles.

**If roll rotates the wrong way,** remove the minus sign in
`(int32_t) (-r_deg * 10.0f)`. **If pitch moves the wrong way,** negate `py`.
Expect at least one of these to be wrong on the first try; every sign convention
on this device has needed checking against hardware.

- [ ] **Step 7: Verify the ball**

Tilt left and right. Expected: the ball slides to the low side and sits between
the cage marks when level. If it moves the wrong way, negate `bx`.

- [ ] **Step 8: Fallback, only if Step 5 showed unclipped corners**

Replace the `att_horizon` child and its three rectangles with a canvas, and draw
the horizon as a filled polygon each tick. Change the `att_ball` widget list so
its only horizon child is:

```yaml
              - canvas:
                  id: att_canvas
                  align: CENTER
                  width: 190
                  height: 190
                  transparency_key: 0x000400
```

and replace the horizon block of Step 3's lambda with:

```cpp
            // Canvas fallback: draw sky, ground and the horizon line directly,
            // so clipping is ours rather than LVGL's.
            {
              const float C = 95.0f;
              float a = -r_deg * 3.14159265f / 180.0f;
              float dx = cosf(a), dy = sinf(a);
              // A point on the horizon, offset by pitch.
              float ox = C - dy * py, oy = C + dx * py;
              float L = 200.0f;
              lv_value_precise_t x1 = (lv_value_precise_t) (ox - dx * L);
              lv_value_precise_t y1 = (lv_value_precise_t) (oy - dy * L);
              lv_value_precise_t x2 = (lv_value_precise_t) (ox + dx * L);
              lv_value_precise_t y2 = (lv_value_precise_t) (oy + dy * L);
              ESP_LOGV("att", "horizon %d,%d -> %d,%d", (int) x1, (int) y1,
                       (int) x2, (int) y2);
            }
```

then drive the actual drawing with `lvgl.canvas.fill` and
`lvgl.canvas.draw_polygon` actions in the interval, using those four points plus
the two far corners on each side. Verify with Steps 5–7 again.

- [ ] **Step 9: Commit**

```bash
git add m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: add an attitude indicator page with slip/skid ball

Artificial horizon as a rotated, pitch-offset child clipped to a circular
parent via clip_corner, reading the same van-frame angles as the LEVEL page
so the two always agree. The slip ball is driven by lateral acceleration
rather than roll, so it shows tilt while parked and cornering while driving.
Needed an unthrottled internal acc_x: the HA-facing one is throttled to 5 s,
which would move the ball once every five seconds."
```

---

## Task 8: Vertical speed and G-meter page

Two medium dials side by side. Rate of climb comes from differentiating the
barometric altitude Task 6 already computes, which needs smoothing — a raw
derivative of pressure altitude is almost entirely noise.

**Files:**
- Modify: `instruments.h`
- Modify: `test/test_instruments.c`
- Modify: `leveling.h`
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Write the failing tests for the new maths**

Add to `test/test_instruments.c`, before `int main`:

```c
static void test_g_magnitude(void)
{
    assert(close_to(inst_g_magnitude(0.0f, 0.0f, 1.0f), 1.0f, 0.0001f));
    assert(close_to(inst_g_magnitude(0.0f, 0.0f, -1.0f), 1.0f, 0.0001f));
    assert(close_to(inst_g_magnitude(3.0f, 4.0f, 0.0f), 5.0f, 0.0001f));
    assert(close_to(inst_g_magnitude(0.0f, 0.0f, 0.0f), 0.0f, 0.0001f));
}

static void test_smoothed_rate(void)
{
    // A steady 100 ft per second climb is 6000 ft/min. Starting from a rate
    // estimate of zero, repeated steps must converge towards it.
    float rate = 0.0f;
    float alt = 0.0f;
    for (int i = 0; i < 400; i++) {
        float next = alt + 10.0f;          // 10 ft per 0.1 s step
        rate = inst_smooth_rate_per_min(next, alt, 0.1f, rate, 2.0f);
        alt = next;
    }
    assert(close_to(rate, 6000.0f, 60.0f));

    // No movement must decay towards zero, not hold the old rate.
    for (int i = 0; i < 400; i++) {
        rate = inst_smooth_rate_per_min(alt, alt, 0.1f, rate, 2.0f);
    }
    assert(close_to(rate, 0.0f, 10.0f));

    // Guards: a non-positive dt must leave the estimate untouched.
    assert(close_to(inst_smooth_rate_per_min(10.0f, 0.0f, 0.0f, 123.0f, 2.0f),
                    123.0f, 0.0001f));
    // A NAN sample must not poison the estimate.
    assert(close_to(inst_smooth_rate_per_min(NAN, 0.0f, 0.1f, 123.0f, 2.0f),
                    123.0f, 0.0001f));
}
```

and add the two calls inside `main`, before the `printf`:

```c
    test_g_magnitude();
    test_smoothed_rate();
```

- [ ] **Step 2: Run and confirm it fails**

Run:
```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti -lm
```
Expected: FAIL — `implicit declaration of function 'inst_g_magnitude'`.

- [ ] **Step 3: Add the maths**

In `instruments.h`, immediately before the closing `#ifdef __cplusplus` /
`}` block, add:

```c
/* Total acceleration magnitude in g. 1.0 at rest, whatever the orientation. */
static inline float inst_g_magnitude(float ax, float ay, float az)
{
    return sqrtf(ax * ax + ay * ay + az * az);
}

/* Exponentially smoothed rate of change, per minute, from two samples and the
   previous estimate. tau_s sets how heavily it is damped. A raw derivative of
   barometric altitude is nearly all sensor noise, so the smoothing is not
   optional. Returns prev_rate unchanged for a bad dt or a NAN sample, so one
   dropout cannot poison the estimate. */
static inline float inst_smooth_rate_per_min(float value, float prev_value,
                                             float dt_s, float prev_rate,
                                             float tau_s)
{
    float inst, alpha;
    if (!(dt_s > 0.0f) || !(tau_s > 0.0f)) return prev_rate;
    if (isnan(value) || isnan(prev_value)) return prev_rate;
    inst = (value - prev_value) / dt_s * 60.0f;
    alpha = 1.0f - expf(-dt_s / tau_s);
    return prev_rate + alpha * (inst - prev_rate);
}
```

- [ ] **Step 4: Run the tests both ways**

Run:
```bash
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti_c  -lm && /tmp/ti_c
cp test/test_instruments.c /tmp/ti.cpp
c++ -std=c++17 -Wall -I. /tmp/ti.cpp -o /tmp/ti_cpp -lm && /tmp/ti_cpp
```
Expected: `ALL TESTS PASSED` twice, no warnings.

- [ ] **Step 5: Add the VSI state**

In `leveling.h`, after the `g_alt_ft` line, add:

```c
inline float    g_vsi_fpm       = 0.0f;  // smoothed vertical speed, ft/min
inline float    g_vsi_prev_ft   = NAN;   // previous altitude sample
inline uint32_t g_vsi_prev_ms   = 0;     // when that sample was taken
```

- [ ] **Step 6: Confirm leveling.h's tests still pass**

Run:
```bash
cc -std=c99 -Wall -I. /tmp/test_leveling.c -o /tmp/tl -lm && /tmp/tl
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 7: Add the page**

In the `lvgl:` `pages:` list, immediately before the line
`    # --------------------------------------------------------------- SYSTEM --`,
insert:

```yaml
    # ------------------------------------------------------------------ RATES --
    - id: pg_rates
      bg_color: 0x101418
      bg_opa: COVER
      on_swipe_left:
        - lvgl.page.next:
            animation: MOVE_LEFT
            time: 200ms
      on_swipe_right:
        - lvgl.page.previous:
            animation: MOVE_RIGHT
            time: 200ms
      widgets:
        - obj:
            x: 0
            y: 0
            width: 320
            height: 24
            radius: 0
            border_width: 0
            pad_all: 0
            bg_color: 0x7A8794
            bg_opa: COVER
            widgets:
              - label: {x: 6, y: 4, text: "RATES", text_font: f_head, text_color: 0x101418}

        # ---- VSI: +/- 2000 ft/min, zero at the 9 o'clock position ----
        - meter:
            id: vsi_meter
            x: 8
            y: 32
            width: 150
            height: 150
            radius: 75
            pad_all: 0
            bg_color: 0x1B2128
            bg_opa: COVER
            border_color: 0x7A8794
            border_width: 2
            scales:
              - id: vsi_scale
                range_from: -20
                range_to: 20
                angle_range: 320
                rotation: 110
                ticks:
                  count: 41
                  width: 1
                  length: 5
                  color: 0x7A8794
                  major:
                    stride: 5
                    width: 3
                    length: 10
                    color: 0xE8EDF2
                    label_gap: 6
                indicators:
                  - line:
                      id: vsi_needle
                      width: 4
                      color: 0x35C46A
                      length: 60
                      value: 0
        - label: {x: 8,  y: 186, text: "VERT SPEED  x100 ft/min", text_font: f_tiny, text_color: 0x7A8794}
        - label: {id: lbl_vsi, x: 8, y: 204, text: "--", text_font: f_val, text_color: 0xE8EDF2}

        # ---- G-meter: 0 to 3 g ----
        - meter:
            id: g_meter
            x: 166
            y: 32
            width: 150
            height: 150
            radius: 75
            pad_all: 0
            bg_color: 0x1B2128
            bg_opa: COVER
            border_color: 0x7A8794
            border_width: 2
            scales:
              - id: g_scale
                range_from: 0
                range_to: 30
                angle_range: 270
                rotation: 135
                ticks:
                  count: 31
                  width: 1
                  length: 5
                  color: 0x7A8794
                  major:
                    stride: 5
                    width: 3
                    length: 10
                    color: 0xE8EDF2
                    label_gap: 6
                indicators:
                  - line:
                      id: g_needle
                      width: 4
                      color: 0xE0A030
                      length: 60
                      value: 10
        - label: {x: 166, y: 186, text: "G  x0.1", text_font: f_tiny, text_color: 0x7A8794}
        - label: {id: lbl_g, x: 166, y: 204, text: "--", text_font: f_val, text_color: 0xE8EDF2}

```

- [ ] **Step 8: Compute both values**

In the `lvgl_tick` lambda, immediately before the comment
`// ---- SYSTEM page ----`, insert:

```cpp
          // ---- RATES page ----
          {
            uint32_t now_ms = esphome::millis();
            if (!std::isnan(g_alt_ft)) {
              if (g_vsi_prev_ms != 0) {
                float dt = (now_ms - g_vsi_prev_ms) / 1000.0f;
                g_vsi_fpm = inst_smooth_rate_per_min(g_alt_ft, g_vsi_prev_ft,
                                                     dt, g_vsi_fpm, 4.0f);
              }
              g_vsi_prev_ft = g_alt_ft;
              g_vsi_prev_ms = now_ms;
            }
            snprintf(b, sizeof(b), "%+.0f fpm", g_vsi_fpm);
            lv_label_set_text(id(lbl_vsi), b);

            float gm = inst_g_magnitude(
                id(acc_x).has_state() ? id(acc_x).state : 0.0f,
                id(acc_y).has_state() ? id(acc_y).state : 0.0f,
                id(acc_z).has_state() ? id(acc_z).state : 0.0f);
            snprintf(b, sizeof(b), "%.2f g", gm);
            lv_label_set_text(id(lbl_g), b);
          }

```

Note this must come **after** the ALTIMETER block from Task 6, because it reads
`g_alt_ft`. Inserting it immediately before the SYSTEM comment puts it there.

- [ ] **Step 9: Move both needles**

In the same `- interval: 250ms` entry, after the two
`lvgl.indicator.update` items added in Task 6, add:

```yaml
      - lvgl.indicator.update:
          id: vsi_needle
          value: !lambda |-
            float v = g_vsi_fpm / 100.0f;
            if (v >  20.0f) v =  20.0f;
            if (v < -20.0f) v = -20.0f;
            return v;
      - lvgl.indicator.update:
          id: g_needle
          value: !lambda |-
            float g = inst_g_magnitude(
                id(acc_x).has_state() ? id(acc_x).state : 0.0f,
                id(acc_y).has_state() ? id(acc_y).state : 0.0f,
                id(acc_z).has_state() ? id(acc_z).state : 0.0f) * 10.0f;
            if (g > 30.0f) g = 30.0f;
            if (g < 0.0f)  g = 0.0f;
            return g;
```

- [ ] **Step 10: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings.

- [ ] **Step 11: Flash and verify**

1. Swipe to RATES. Expected: two dials. The G needle sits near `10` (1.00 g) at
   rest and the readout shows about `1.00 g`.
2. Lift the device briskly and set it down. Expected: the G needle moves, and the
   VSI needle deflects then settles back towards zero over a few seconds.
3. Sitting still, expected: VSI reads within roughly ±50 fpm of zero. A larger
   steady offset means `tau_s` is too small — raise the `4.0f` in Step 8.
4. The G readout uses the 5 s-throttled `acc_x/y/z`, so it updates in steps
   rather than smoothly. That is deliberate — it avoids a third fast IMU
   subscription for a gauge nobody watches closely. If it feels too sluggish,
   point it at `acc_x_fast` from Task 7 for the X axis.

- [ ] **Step 12: Commit**

```bash
git add instruments.h test/test_instruments.c leveling.h m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: add a vertical-speed and G-meter page

VSI differentiates the barometric altitude from the altimeter page through an
exponential smoother; a raw derivative of pressure altitude is almost all
noise, so the damping is load-bearing rather than cosmetic. Both the smoother
and the G magnitude are host-unit-tested, including that a NAN sample or a
zero dt cannot poison the rate estimate."
```

---

## Task 9: Trip odometer

Accumulate GNSS distance and persist it. The interesting constraint is flash
wear: writing a total to NVS on every fix would be thousands of writes an hour.

**Files:**
- Modify: `instruments.h`
- Modify: `test/test_instruments.c`
- Modify: `leveling.h`
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Write the failing test**

Add to `test/test_instruments.c`, before `int main`:

```c
static void test_haversine(void)
{
    // Zero distance for identical points.
    assert(close_to(inst_haversine_m(40.0f, -105.0f, 40.0f, -105.0f), 0.0f, 0.1f));

    // One degree of latitude is close to 111.2 km anywhere.
    float d = inst_haversine_m(40.0f, -105.0f, 41.0f, -105.0f);
    assert(close_to(d, 111195.0f, 500.0f));

    // Symmetric.
    float a = inst_haversine_m(40.1f, -105.2f, 40.3f, -105.5f);
    float b2 = inst_haversine_m(40.3f, -105.5f, 40.1f, -105.2f);
    assert(close_to(a, b2, 0.5f));

    // A degree of longitude shrinks with latitude.
    float eq = inst_haversine_m(0.0f, 0.0f, 0.0f, 1.0f);
    float hi = inst_haversine_m(60.0f, 0.0f, 60.0f, 1.0f);
    assert(hi < eq * 0.6f);

    // A few metres apart must not round to zero — the odometer depends on it.
    float small = inst_haversine_m(40.000000f, -105.000000f,
                                   40.000090f, -105.000000f);
    assert(small > 5.0f && small < 15.0f);
}
```

and add `test_haversine();` inside `main` before the `printf`.

- [ ] **Step 2: Run and confirm it fails**

Run:
```bash
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti -lm
```
Expected: FAIL — `implicit declaration of function 'inst_haversine_m'`.

- [ ] **Step 3: Add the maths**

In `instruments.h`, before the closing `#ifdef __cplusplus` / `}` block, add:

```c
#define INST_EARTH_R_M  6371008.8f   /* IUGG mean radius */

/* Great-circle distance in metres. Computed in double internally: at van
   speeds consecutive fixes are metres apart, and float latitudes lose enough
   precision in the subtraction to swallow a short step entirely. */
static inline float inst_haversine_m(float lat1, float lon1,
                                     float lat2, float lon2)
{
    const double D2R = 0.017453292519943295;
    double p1 = (double) lat1 * D2R, p2 = (double) lat2 * D2R;
    double dp = p2 - p1;
    double dl = ((double) lon2 - (double) lon1) * D2R;
    double sdp = sin(dp * 0.5), sdl = sin(dl * 0.5);
    double a = sdp * sdp + cos(p1) * cos(p2) * sdl * sdl;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return (float) (2.0 * (double) INST_EARTH_R_M * asin(sqrt(a)));
}
```

- [ ] **Step 4: Run the tests both ways**

Run:
```bash
cc -std=c99 -Wall -I. test/test_instruments.c -o /tmp/ti_c  -lm && /tmp/ti_c
cp test/test_instruments.c /tmp/ti.cpp
c++ -std=c++17 -Wall -I. /tmp/ti.cpp -o /tmp/ti_cpp -lm && /tmp/ti_cpp
```
Expected: `ALL TESTS PASSED` twice, no warnings.

- [ ] **Step 5: Add the in-RAM accumulator state**

In `leveling.h`, after the `g_vsi_prev_ms` line, add:

```c
inline double   g_odo_pend_m    = 0.0;   // metres not yet flushed to NVS
inline float    g_odo_last_lat  = NAN;   // previous accepted fix
inline float    g_odo_last_lon  = NAN;
```

- [ ] **Step 6: Confirm leveling.h's tests still pass**

Run:
```bash
cc -std=c99 -Wall -I. /tmp/test_leveling.c -o /tmp/tl -lm && /tmp/tl
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 7: Add the persisted total**

A `globals:` entry can hold a plain `float` — only *included* types are
impossible there, because ESPHome emits `includes:` after `globals:`. Add a new
top-level block immediately before the `# ---` banner that precedes
`external_components:`:

```yaml
# Odometer total. Persisted, but written deliberately rather than continuously —
# see the flush rules in the levelling loop and on ignition loss.
globals:
  - id: odo_total_m
    type: float
    restore_value: true
    initial_value: '0.0'

```

- [ ] **Step 8: Accumulate distance**

In the `leveling_tick` interval's lambda, immediately before the comment
`// --- LED bar as a level indicator ---`, insert:

```cpp
          // --- trip odometer ---
          // Only count movement while actually moving and with a usable fix:
          // a stationary receiver wanders by metres, which would otherwise
          // clock up kilometres overnight.
          if (have_fix && g_is_moving && tg.hdop.isValid() && tg.hdop.hdop() < 5.0f) {
            float la = (float) tg.location.lat();
            float lo = (float) tg.location.lng();
            if (!std::isnan(g_odo_last_lat)) {
              float step = inst_haversine_m(g_odo_last_lat, g_odo_last_lon, la, lo);
              // Reject absurd jumps (a fix glitch) and sub-metre jitter.
              if (step > 1.0f && step < 1000.0f) g_odo_pend_m += step;
            }
            g_odo_last_lat = la;
            g_odo_last_lon = lo;
          } else if (!g_is_moving) {
            g_odo_last_lat = NAN;   // restart the chain when stopping
            g_odo_last_lon = NAN;
          }

```

- [ ] **Step 9: Flush to NVS sparingly**

Add a flush script. In the `script:` block, after `exit_sleep`, add:

```yaml
  # Fold the pending metres into the persisted total. Called every 5 minutes
  # and when the ignition goes off, rather than on every fix: at one write per
  # GNSS update this would be thousands of NVS writes an hour.
  - id: odo_flush
    then:
      - lambda: |-
          if (g_odo_pend_m <= 0.0) return;
          id(odo_total_m) += (float) g_odo_pend_m;
          g_odo_pend_m = 0.0;
          ESP_LOGI("odo", "flushed, total %.1f km", id(odo_total_m) / 1000.0f);
```

Add the periodic call as a new item in the top-level `interval:` list, after the
`lvgl_tick` entry:

```yaml
  - interval: 5min
    id: odo_tick
    then:
      - script.execute: odo_flush
```

And flush on ignition loss by adding one line to the `ign_power` sensor's
`on_release:` block from Task 5, so it reads:

```yaml
    on_release:                      # ignition power lost
      then:
        - script.execute: odo_flush
        - script.execute: sleep_timer
```

- [ ] **Step 10: Expose it**

In the `sensor:` block, after the `Speed (mph)` entry from Task 3, add:

```yaml
  - platform: template
    id: odo_miles
    name: "Odometer"
    unit_of_measurement: mi
    accuracy_decimals: 1
    update_interval: 60s
    icon: "mdi:counter"
    lambda: 'return (id(odo_total_m) + (float) g_odo_pend_m) / 1609.344f;'
```

And add a reset button in the `button:` block, after `btn_qnh_from_gps`:

```yaml
  - platform: template
    id: btn_odo_reset
    name: "Reset Odometer"
    icon: "mdi:counter"
    entity_category: config
    on_press:
      then:
        - lambda: |-
            id(odo_total_m) = 0.0f;
            g_odo_pend_m = 0.0;
            g_odo_last_lat = NAN;
            g_odo_last_lon = NAN;
            ESP_LOGI("odo", "reset");
```

- [ ] **Step 11: Show it on the SYSTEM page**

Add a label to the SYSTEM page widget list, after `lbl_sys_e`:

```yaml
        - label: {id: lbl_sys_odo, x: 8, y: 140, text: "", text_font: f_tiny, text_color: 0xE8EDF2}
```

and in the `lvgl_tick` lambda, immediately after the line
`lv_label_set_text(id(lbl_sys_e), b);`, add:

```cpp
          snprintf(b, sizeof(b), "ODO    %.1f mi",
                   (id(odo_total_m) + (float) g_odo_pend_m) / 1609.344f);
          lv_label_set_text(id(lbl_sys_odo), b);
```

- [ ] **Step 12: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings.

- [ ] **Step 13: Flash and verify**

1. Parked with a fix, expected: `Odometer` holds steady. Watch it for several
   minutes — if it creeps while stationary, the `g_is_moving` gate is not
   holding and the HDOP threshold needs tightening.
2. Drive a known distance and compare against the vehicle's own odometer.
   Expected: within a few percent. GNSS under-reads slightly on winding roads
   because it samples at 1 Hz and cuts corners.
3. Check the log for `flushed, total N km` every five minutes while moving.
4. Switch the ignition off and confirm a flush is logged immediately.
5. Power-cycle and confirm the total survives.

- [ ] **Step 14: Commit**

```bash
git add instruments.h test/test_instruments.c leveling.h m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: add a persisted GNSS trip odometer

Haversine between consecutive fixes, gated on actually moving and on HDOP so
a stationary receiver's wander does not clock up distance. Accumulates in RAM
and folds into the persisted total every five minutes and on ignition loss,
rather than writing NVS on every fix. Distance is computed in double: at van
speeds consecutive fixes are metres apart and float latitudes lose a short
step in the subtraction."
```

---

## Task 10: Night dimming

**Scoped deliberately small.** The full idea — an amber-on-black night palette —
means replacing every inline colour across seven pages with styles, which is a
large mechanical refactor of working display code for an aesthetic gain. The
backlight alone delivers most of the practical benefit, which is not being
dazzled at night, at almost no risk. The palette change stays out of scope.

**Files:**
- Modify: `m5stack-cores3-gnss-lvgl.yaml`

- [ ] **Step 1: Add the switch and the brightness numbers**

In the `switch:` block, after the `speed_mph` entry from Task 3, add:

```yaml
  - platform: template       # dim the screen between dusk and dawn
    id: night_dim
    name: "Night Dimming"
    icon: "mdi:weather-night"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_ON
    entity_category: config

```

In the `number:` block, after `qnh_inhg`, add:

```yaml
  - platform: template
    id: bright_day
    name: "Day Brightness"
    unit_of_measurement: "%"
    min_value: 10
    max_value: 100
    step: 5
    initial_value: 80
    restore_value: true
    optimistic: true
    entity_category: config
  - platform: template
    id: bright_night
    name: "Night Brightness"
    unit_of_measurement: "%"
    min_value: 1
    max_value: 60
    step: 1
    initial_value: 15
    restore_value: true
    optimistic: true
    entity_category: config
```

- [ ] **Step 2: Apply it on a timer**

Add a new item to the top-level `interval:` list, after the `odo_tick` entry:

```yaml
  # Brightness follows local clock hour. The GNSS time source is UTC, so this
  # needs the timezone set; without it the thresholds are simply wrong by the
  # UTC offset rather than broken.
  - interval: 60s
    id: bright_tick
    then:
      - if:
          condition:
            lambda: 'return !g_asleep;'
          then:
            - lambda: |-
                float pct = id(bright_day).state;
                if (id(night_dim).state) {
                  auto t = id(gps_time).now();
                  if (t.is_valid() && (t.hour >= 20 || t.hour < 6)) {
                    pct = id(bright_night).state;
                  }
                }
                auto call = id(lcd_backlight).turn_on();
                call.set_brightness(pct / 100.0f);
                call.set_transition_length(2000);
                call.perform();
```

Note the `g_asleep` guard: without it this would switch the backlight on again
every minute while the device is meant to be asleep.

- [ ] **Step 3: Set a timezone so the hour test means something**

In the `time:` block, add `timezone:` to the `gps` platform entry so it reads:

```yaml
time:
  - platform: gps          # UTC from the NEO-M9N once it has a fix
    id: gps_time
    gps_id: gnss
    timezone: America/Denver
```

Adjust the zone if the van is usually elsewhere. Without this, `t.hour` is UTC
and the dimming happens at the wrong time of day.

- [ ] **Step 4: Validate, compile, check warnings**

Run the same three commands as Task 3 Steps 6–7. Expected: valid, `exit=0`,
no warnings.

- [ ] **Step 5: Flash and verify**

1. During the day, expected: brightness sits at `Day Brightness`.
2. Temporarily set `Night Brightness` to 1 and widen the night window in Step 2
   to `t.hour >= 0` to force it. Expected: the screen fades over two seconds.
   Put the condition back afterwards.
3. Turn `Night Dimming` off. Expected: it returns to day brightness within a
   minute.
4. Let it sleep (Task 5) and confirm the screen **stays dark** — proving the
   `g_asleep` guard holds.

- [ ] **Step 6: Commit**

```bash
git add m5stack-cores3-gnss-lvgl.yaml
git commit -m "feat: dim the display at night

Backlight follows the local clock between configurable day and night levels,
with a two-second fade. Guarded on the sleep flag so it cannot re-light a
sleeping device. Scoped to the backlight only: an amber-on-black night
palette would mean replacing every inline colour across seven pages with
styles, which is a large refactor of working code for an aesthetic gain."
```

---

## Task 11: Final verification and sync

- [ ] **Step 1: Run every host test**

```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cc  -std=c99   -Wall -I. test/test_instruments.c -o /tmp/a && /tmp/a
cp test/test_instruments.c /tmp/a.cpp
c++ -std=c++17 -Wall -I. /tmp/a.cpp -o /tmp/b && /tmp/b
cc  -std=c99   -Wall -I. /tmp/test_leveling.c -o /tmp/c && /tmp/c
```
Expected: `ALL TESTS PASSED` three times.

- [ ] **Step 2: Confirm nothing was removed**

```bash
grep -c "pg_level\|pg_gnss\|pg_compass\|pg_sys\|pg_alt\|pg_att\|pg_rates" m5stack-cores3-gnss-lvgl.yaml
grep -c "^web_server:" m5stack-cores3-gnss-lvgl.yaml
grep -c "chipset: WS2812" m5stack-cores3-gnss-lvgl.yaml
grep -c "house_wifi_ssid" m5stack-cores3-gnss-lvgl.yaml
```
Expected: `7`, `1`, `1`, `2`. All seven pages present, the web server intact, the
LED chipset still WS2812, both WiFi networks still referenced.

- [ ] **Step 3: Validate against every build host's secrets**

The same file must build on the laptop, the house HA and the van HA. For each,
copy that host's `secrets.yaml` next to the config in a scratch directory and
run `$EH config`. Expected: `Configuration is valid!` three times.

- [ ] **Step 4: Sync to all four locations and verify by checksum**

```bash
cd /Users/cjacob/Documents/Claude/Projects/NGLU
cp m5stack-cores3-gnss-lvgl.yaml /Users/cjacob/esphome/leveldev.yaml
cp leveling.h instruments.h /Users/cjacob/esphome/
scp m5stack-cores3-gnss-lvgl.yaml leveling.h instruments.h ha:/config/esphome/
scp m5stack-cores3-gnss-lvgl.yaml leveling.h instruments.h va:/config/esphome/
md5 -q m5stack-cores3-gnss-lvgl.yaml leveling.h instruments.h
ssh ha 'md5sum /config/esphome/m5stack-cores3-gnss-lvgl.yaml /config/esphome/leveling.h /config/esphome/instruments.h'
ssh va 'md5sum /config/esphome/m5stack-cores3-gnss-lvgl.yaml /config/esphome/leveling.h /config/esphome/instruments.h'
```
Expected: the checksums match across all locations.

**`instruments.h` must reach every build host.** `esphome: includes:` resolves
relative to the config file, so a host missing it fails the build.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: verify all pages intact and sync to build hosts"
```

---

## Still out of scope

- **Amber-on-black night palette.** See Task 10's note: it needs every page
  converted to LVGL styles. Worth doing as its own plan if the dimming proves
  not enough.
- **Page navigation beyond swiping.** Seven pages means up to six swipes to
  reach one. A page-jump — a long-press menu, or an HA `select` that drives
  `lvgl.page.show` — would be a sensible next addition.
- **Deep sleep.** Task 5 keeps WiFi, the API and GNSS alive so Home Assistant
  keeps the entity and trip logging continues. Genuine low-power sleep is a
  different design with different trade-offs.
