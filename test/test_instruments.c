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
    test_g_magnitude();
    test_smoothed_rate();
    printf("ALL TESTS PASSED\n");
    return 0;
}
