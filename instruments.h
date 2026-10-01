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

#ifdef __cplusplus
}  /* extern "C" */
#endif
