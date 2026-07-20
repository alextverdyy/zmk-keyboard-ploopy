/*
 * Copyright (c) 2025 George Norton
 * SPDX-License-Identifier: MIT
 *
 * Scroll smoothing input processor.
 *
 * Three-stage pipeline fed from zip_xy_to_scroll_mapper:
 *
 *   1. DEADZONE . . . . . . . zero sub-threshold deltas
 *   2. EMA LOW-PASS  . . . . . exponential moving average per-axis
 *   3. ADAPTIVE GAIN . . . . . velocity-dependent scaling
 *
 * Output is cleaned scroll deltas (REL_WHEEL / REL_HWHEEL) that feed
 * downstream into scroll_inertia for kinetic coasting.
 *
 * Each stage is independently configurable via Devicetree properties;
 * default values are tuned for a PMW3360 at 900 CPI, 125 Hz event rate.
 */

#define DT_DRV_COMPAT zmk_input_processor_scroll_smooth

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* =========================================================================
 * Devicetree-derived configuration (per-instance)
 * ========================================================================= */

struct scroll_smooth_cfg {
    int32_t deadzone;
    int32_t alpha;       /* permille: 0..1000 */
    int32_t gain_min;    /* permille */
    int32_t gain_max;    /* permille */
    int32_t gain_thresh; /* delta scale */
};

/* =========================================================================
 * Per-instance runtime state
 * ========================================================================= */

struct scroll_smooth_data {
    int32_t vel_y; /* EMA velocity, REL_WHEEL (×256 fixed-point) */
    int32_t vel_x; /* EMA velocity, REL_HWHEEL (×256 fixed-point) */
};

/* =========================================================================
 * EMA + deadzone + adaptive gain (per-axis)
 *
 * `delta_raw` is the raw event value before processing.
 * `*vel_p` points at the persistent EMA velocity for this axis.
 * `event` is the event struct; event->value is modified in place.
 * ========================================================================= */

#define FP_SCALE 256
#define FP_SHIFT 8

static void process_axis(int32_t delta_raw, int32_t *vel_p,
                         struct input_event *event,
                         int16_t *remainder,
                         const struct scroll_smooth_cfg *cfg)
{
    /* ── skip keep-alive zero events ── */
    if (delta_raw == 0) {
        return;
    }

    /* ── 1. DEADZONE ──
     * Sub-threshold events are zeroed (removed from pipeline).
     * Internal velocity still EMA-decays toward zero so a micro-pause
     * in genuine motion doesn't leave an artificially inflated vel
     * that would cause a velocity overshoot at resume.
     */
    if (abs32(delta_raw) < cfg->deadzone) {
        *vel_p = (int64_t)(*vel_p) * (1000 - cfg->alpha) / 1000;
        event->value = 0;
        return;
    }

    /* ── 2. EMA LOW-PASS ──
     * vel = (delta × alpha + vel × (1000 − alpha)) / 1000
     * vel stored in ×256 fixed-point for sub-unit precision.
     * α = 1000 would give passthrough.
     */
    int32_t vel_fp = *vel_p;
    int32_t delta_fp = delta_raw * FP_SCALE;
    int32_t smoothed_fp = ((int64_t)delta_fp * cfg->alpha +
                           (int64_t)vel_fp * (1000 - cfg->alpha)) / 1000;
    *vel_p = smoothed_fp;

    /* ── 3. ADAPTIVE GAIN ──
     * Gain interpolates linearly from gain_min at |vel| ≤ threshold
     * to gain_max at |vel| ≥ 2× threshold.
     *
     * We compute gain against |smoothed_fp| (fixed-point) so the
     * velocity threshold is directly in the event's own delta units
     * (×256 fixed-point preserves the EMA's sub-unit benefit).
     * Finally we convert back to integer scroll ticks for the output.
     */
    int32_t abs_vel = abs32(smoothed_fp);
    int32_t gain;
    int32_t thresh = cfg->gain_thresh * FP_SCALE;

    if (abs_vel <= thresh) {
        gain = cfg->gain_min;
    } else if (abs_vel >= thresh * 2) {
        gain = cfg->gain_max;
    } else {
        int32_t t = abs_vel - thresh;
        gain = cfg->gain_min +
               (int64_t)(cfg->gain_max - cfg->gain_min) * t / thresh;
    }

    /* Apply gain and convert back to integer. Result is truncated toward
     * zero via integer division; the original delta's sign is preserved.
     *
     * We re-fetch the smoothed value (not |abs_vel|) to keep the sign.
     */
    int32_t output_fp = (int64_t)smoothed_fp * gain / 1000;

    /* Accumulate the fixed-point remainder to avoid losing sub-unit ticks
     * (same pattern as the standard scaler processor).  `remainder` is the
     * per-code state slot provided by the input-listener framework.
     */
    if (remainder) {
        output_fp += *remainder;
    }
    int16_t output = output_fp / FP_SCALE;

    if (remainder) {
        *remainder = output_fp - (output * FP_SCALE);
    }

    event->value = output;

    LOG_DBG("scroll_smooth  raw=%d  vel=%d  gain=%d‰  out=%d  rem=%d",
            delta_raw, smoothed_fp / FP_SCALE, gain, output,
            remainder ? *remainder : 0);
}

/* =========================================================================
 * Input processor entry point
 * ========================================================================= */

static int handle_event(const struct device *dev,
                        struct input_event *event,
                        uint32_t param1, uint32_t param2,
                        struct zmk_input_processor_state *state)
{
    struct scroll_smooth_data *data = dev->data;
    const struct scroll_smooth_cfg *cfg = dev->config;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Pick axis, state pointer, and remainder slot */
    int32_t *vel_p;
    int16_t *rem;

    switch (event->code) {
    case INPUT_REL_WHEEL:
        vel_p = &data->vel_y;
        rem   = state ? &state->remainder[0] : NULL;
        break;
    case INPUT_REL_HWHEEL:
        vel_p = &data->vel_x;
        rem   = state ? &state->remainder[1] : NULL;
        break;
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }

    process_axis(event->value, vel_p, event, rem, cfg);

    return ZMK_INPUT_PROC_CONTINUE;
}

/* =========================================================================
 * Device boilerplate
 * ========================================================================= */

static struct zmk_input_processor_driver_api driver_api = {
    .handle_event = handle_event,
};

#define SCROLL_SMOOTH_INST(n)                                                       \
    static struct scroll_smooth_data data_##n = {0};                                \
    static const struct scroll_smooth_cfg cfg_##n = {                               \
        .deadzone    = DT_INST_PROP_OR(n, deadzone, 0),                             \
        .alpha       = DT_INST_PROP_OR(n, alpha, 500),                              \
        .gain_min    = DT_INST_PROP_OR(n, gain_min, 800),                           \
        .gain_max    = DT_INST_PROP_OR(n, gain_max, 1500),                          \
        .gain_thresh = DT_INST_PROP_OR(n, gain_threshold, 500),                     \
    };                                                                              \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL,                                            \
                          &data_##n, &cfg_##n,                                      \
                          POST_KERNEL,                                              \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                          &driver_api);

DT_INST_FOREACH_STATUS_OKAY(SCROLL_SMOOTH_INST)
