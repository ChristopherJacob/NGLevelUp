// =============================================================================
//  Pure leveling-guidance math — ported from LevelUp `main/leveling.c`
//  https://github.com/ChristopherJacob/LevelUp
// =============================================================================
//  Header-only so ESPHome can pull it in with `esphome: includes:`.
//  Compiles as both C99 and C++ so LevelUp's test/leveling/test_leveling.c
//  still builds and passes against it unchanged.
//
//  Deviations from the original leveling.c, all deliberate:
//    * function bodies moved into this header as `static inline`
//    * C99 compound literals ((leveling_orient_t){...}) rewritten as field
//      assignment — compound literals are not valid C++
//    * leveling_result_t gains front_high_deg / left_high_deg. The original
//      computed these internally and discarded them; the bubble display and
//      the beep cadence both need them, and recomputing in the caller would
//      duplicate the orientation mapping.
//    * leveling_beep_interval_ms() ported from audio_mgr.c
//    * leveling_deadband_hyst() ported from imu_task.c
//  The math itself is unchanged.
// =============================================================================
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LEVEL_MODE_BLOCKS = 0, LEVEL_MODE_RAMPS = 1 } leveling_mode_t;

// Corner index order used throughout the codebase.
typedef enum { CORNER_FL = 0, CORNER_FR = 1, CORNER_RL = 2, CORNER_RR = 3 } corner_t;

// Which screen edge points to the front of the van (wizard answer).
typedef enum {
    ORIENT_FRONT_TOP = 0,
    ORIENT_FRONT_BOTTOM = 1,
    ORIENT_FRONT_LEFT = 2,
    ORIENT_FRONT_RIGHT = 3,
} leveling_front_t;

// Mapping from device roll/pitch into van-frame "front-high" / "left-high" tilts.
typedef struct {
    bool  front_is_pitch_axis; // true: van front/back lies along the pitch axis
    float front_sign;          // +1/-1 so (axis*front_sign) > 0 means FRONT is high
    float left_sign;           // +1/-1 so (axis*left_sign)  > 0 means LEFT side is high
} leveling_orient_t;

typedef struct {
    bool     guidance_available; // false when vehicle dimensions are unset/zero
    float    corner_lift_in[4];  // indexed by corner_t; highest corner == 0.0
    corner_t worst_corner;       // largest lift — act on first
    float    max_lift_in;        // magnitude of the worst-corner lift

    // Ramp mode (dominant single axis):
    bool     ramp_axis_is_roll;  // dominant correction is roll (side) vs pitch (end)
    bool     ramp_lift_left;     // ramp goes under LEFT wheels (else right)  [roll axis]
    bool     ramp_lift_front;    // ramp goes under FRONT wheels (else rear)  [pitch axis]
    float    ramp_target_in;     // ramp height needed
    float    ramp_remaining_in;  // live distance-to-level (== ramp_target_in here)

    bool     is_level;           // both van-frame tilts within LEVELING_LEVEL_DEG

    // Added for this port — van-frame tilts, +ve means that side is HIGH.
    float    front_high_deg;
    float    left_high_deg;
} leveling_result_t;

#define LEVELING_LEVEL_DEG 0.5f
#define LEVELING_DEG2RAD   0.017453292519943295f

// Resolve a wizard front-direction choice into an axis/sign mapping.
static inline leveling_orient_t leveling_orient_from_front(leveling_front_t front)
{
    // NOTE: signs assume the device mounted screen-up with the default IMU axis
    // convention. Flip a branch here if the FRONT marker points wrong on-device.
    leveling_orient_t o;
    o.front_is_pitch_axis = true;
    o.front_sign = 1.0f;
    o.left_sign  = 1.0f;

    switch (front) {
        case ORIENT_FRONT_BOTTOM:
            o.front_sign = -1.0f; o.left_sign = -1.0f;
            break;
        case ORIENT_FRONT_LEFT:
            o.front_is_pitch_axis = false; o.front_sign =  1.0f; o.left_sign = -1.0f;
            break;
        case ORIENT_FRONT_RIGHT:
            o.front_is_pitch_axis = false; o.front_sign = -1.0f; o.left_sign =  1.0f;
            break;
        case ORIENT_FRONT_TOP:
        default:
            break;
    }
    return o;
}

// Compute guidance. roll/pitch are the device (already zero-offset) angles in degrees.
static inline leveling_result_t leveling_compute(float roll_deg, float pitch_deg,
                                                 float trackwidth_in, float wheelbase_in,
                                                 leveling_mode_t mode,
                                                 leveling_orient_t orient)
{
    int i;
    leveling_result_t r;

    r.guidance_available = false;
    r.corner_lift_in[0] = r.corner_lift_in[1] = 0.0f;
    r.corner_lift_in[2] = r.corner_lift_in[3] = 0.0f;
    r.worst_corner       = CORNER_FL;
    r.max_lift_in        = 0.0f;
    r.ramp_axis_is_roll  = false;
    r.ramp_lift_left     = false;
    r.ramp_lift_front    = false;
    r.ramp_target_in     = 0.0f;
    r.ramp_remaining_in  = 0.0f;
    r.is_level           = false;
    r.front_high_deg     = 0.0f;
    r.left_high_deg      = 0.0f;

    // Both block and ramp fields are always populated; callers select which to
    // render via `mode`, so the parameter itself is intentionally unused here.
    (void) mode;

    if (trackwidth_in <= 0.0f || wheelbase_in <= 0.0f) {
        return r;
    }
    r.guidance_available = true;

    // Map device angles into van-frame tilts (degrees).
    if (orient.front_is_pitch_axis) {
        r.front_high_deg = orient.front_sign * pitch_deg;
        r.left_high_deg  = orient.left_sign  * roll_deg;
    } else {
        r.front_high_deg = orient.front_sign * roll_deg;
        r.left_high_deg  = orient.left_sign  * pitch_deg;
    }

    r.is_level = (fabsf(r.front_high_deg) <= LEVELING_LEVEL_DEG) &&
                 (fabsf(r.left_high_deg)  <= LEVELING_LEVEL_DEG);

    {
        const float half_base = wheelbase_in  * 0.5f; // along front/rear
        const float half_wt   = trackwidth_in * 0.5f; // along left/right
        const float tan_front = tanf(r.front_high_deg * LEVELING_DEG2RAD);
        const float tan_left  = tanf(r.left_high_deg  * LEVELING_DEG2RAD);

        // Relative corner heights (inches). +front and +left are "high".
        float h[4];
        float hmax;
        h[CORNER_FL] =  half_base * tan_front + half_wt * tan_left;
        h[CORNER_FR] =  half_base * tan_front - half_wt * tan_left;
        h[CORNER_RL] = -half_base * tan_front + half_wt * tan_left;
        h[CORNER_RR] = -half_base * tan_front - half_wt * tan_left;

        hmax = h[0];
        for (i = 1; i < 4; i++) if (h[i] > hmax) hmax = h[i];

        for (i = 0; i < 4; i++) {
            float lift = hmax - h[i];
            if (lift < 0.0f) lift = 0.0f;
            r.corner_lift_in[i] = lift;
            if (lift > r.max_lift_in) {
                r.max_lift_in  = lift;
                r.worst_corner = (corner_t) i;
            }
        }

        // Ramp mode collapses to the single dominant axis.
        {
            float roll_lift  = fabsf(trackwidth_in * tan_left);   // cancel side tilt
            float pitch_lift = fabsf(wheelbase_in  * tan_front);  // cancel end tilt
            // roll wins on tie (side tilt is harder to ignore)
            r.ramp_axis_is_roll = (roll_lift >= pitch_lift);
            if (r.ramp_axis_is_roll) {
                r.ramp_target_in = roll_lift;
                // Lift the LOW side. left_high>0 means left is high -> lift right.
                r.ramp_lift_left = (r.left_high_deg < 0.0f);
            } else {
                r.ramp_target_in  = pitch_lift;
                r.ramp_lift_front = (r.front_high_deg < 0.0f);
            }
            r.ramp_remaining_in = r.ramp_target_in;
        }
    }

    return r;
}

static inline const char *leveling_corner_name(corner_t c)
{
    switch (c) {
        case CORNER_FL: return "FL";
        case CORNER_FR: return "FR";
        case CORNER_RL: return "RL";
        case CORNER_RR: return "RR";
        default:        return "??";
    }
}

// --- Beep cadence (ported from LevelUp main/audio_mgr.c) ---------------------
#define LEVELING_BEEP_FAST_MS            120
#define LEVELING_BEEP_SLOW_MS            1200
#define LEVELING_BEEP_MAX_ERR_DEG        8.0f
#define LEVELING_BEEP_MOVING_MIN_ERR_DEG 2.0f

static inline uint32_t leveling_beep_interval_ms(float a_deg, float b_deg, bool stationary)
{
    float norm;
    float e = fmaxf(fabsf(a_deg), fabsf(b_deg));
    // While moving, avoid the ultra-fast "centered" cadence.
    if (!stationary && e < LEVELING_BEEP_MOVING_MIN_ERR_DEG) {
        e = LEVELING_BEEP_MOVING_MIN_ERR_DEG;
    }
    norm = e / LEVELING_BEEP_MAX_ERR_DEG;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return (uint32_t) (LEVELING_BEEP_FAST_MS +
                       norm * (float) (LEVELING_BEEP_SLOW_MS - LEVELING_BEEP_FAST_MS));
}

// --- Deadband with hysteresis (ported from LevelUp main/imu_task.c) ----------
static inline float leveling_deadband_hyst(float v, float enter_deg, float exit_deg,
                                           bool stationary, bool *latched_zero)
{
    float av;
    if (!latched_zero) return v;

    av = fabsf(v);
    if (*latched_zero) {
        if (av >= exit_deg) {
            *latched_zero = false;
        } else {
            return 0.0f;
        }
    }

    // Only snap into "exact zero" when motion is settled.
    if (stationary && av <= enter_deg) {
        *latched_zero = true;
        return 0.0f;
    }

    return v;
}

// --- Shared state for ESPHome lambdas ---------------------------------------
// ESPHome emits `includes:` files AFTER the `globals:` block in main.cpp, so a
// `globals:` entry of type leveling_result_t cannot compile — the type is not
// yet declared at that point. C++17 inline variables give the same shared state
// without the ordering problem, and everything ESPHome generates lives in one
// translation unit. None of this needed NVS persistence.
#ifdef __cplusplus
}  // extern "C"

inline leveling_result_t g_lvl{};        // latest leveling_compute() result
inline bool     g_roll_latched  = false; // deadband hysteresis state, per axis
inline bool     g_pitch_latched = false;
inline bool     g_is_moving     = false; // from GNSS ground speed
inline uint32_t g_last_beep_ms  = 0;
inline int      g_move_count    = 0;     // GNSS motion hysteresis counter
inline int      g_led_bucket    = -2;    // last colour bucket pushed to the LED bar
#endif
