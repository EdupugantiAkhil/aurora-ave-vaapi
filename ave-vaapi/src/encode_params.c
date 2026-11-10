// SPDX-License-Identifier: MIT
#include "encode_params.h"

#include <errno.h>
#include <linux/v4l2-controls.h>
#include <string.h>

/*
 * VA -> apple-ave rate-control mapping.  Each VA field is either applied
 * through an apple-ave control, or explicitly documented here as having no
 * effect.  Anything else is refused.
 */
int ave_parse_frame_rate(uint32_t value, uint32_t *result)
{
    uint32_t numerator = value & 0xffffu;
    uint32_t denominator = value >> 16;
    uint32_t rounded;

    if (denominator == 0)
        denominator = 1;
    if (numerator == 0)
        return -EINVAL;
    rounded = (numerator + denominator / 2) / denominator;
    /* V4L2 S_PARM on apple-ave accepts 1..240 frames per second. */
    if (rounded == 0 || rounded > 240)
        return -ERANGE;
    *result = rounded;
    return 0;
}

int ave_parse_misc(VAEncMiscParameterType type, const void *data,
                   size_t size, struct ave_rate_request *rate)
{
    if (!data || !rate)
        return -EINVAL;

    switch (type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl control;

        if (size < sizeof(control))
            return -EINVAL;
        memcpy(&control, data, sizeof(control));
        /*
         * Not possible on apple-ave: resetting the rate controller
         * mid-stream, per-temporal-layer rates, parallel/dynamic BRC, and
         * turning off CBR bit stuffing (the kernel driver pads CBR output
         * with filler data).  disable_frame_skip is satisfied (AVE never
         * skips).  mb_rate_control and frame_tolerance_mode only apply
         * with VA_RC_MB / FrameSizeToleranceSupport, neither advertised.
         */
        if (control.rc_flags.bits.reset ||
            control.rc_flags.bits.temporal_id ||
            control.rc_flags.bits.disable_bit_stuffing ||
            control.rc_flags.bits.enable_parallel_brc ||
            control.rc_flags.bits.enable_dynamic_scaling)
            return -ENOTSUP;
        rate->rate_seen = true;
        rate->bits_per_second = control.bits_per_second;
        rate->target_percentage = control.target_percentage;
        rate->min_qp = control.min_qp;
        rate->max_qp = control.max_qp;
        /*
         * window_size and initial_qp have no apple-ave control: the
         * firmware uses its own one-second window and initial QP.
         */
        return 0;
    }
    case VAEncMiscParameterTypeFrameRate: {
        VAEncMiscParameterFrameRate frame_rate;
        int status;

        if (size < sizeof(frame_rate))
            return -EINVAL;
        memcpy(&frame_rate, data, sizeof(frame_rate));
        if (frame_rate.framerate_flags.bits.temporal_id)
            return -ENOTSUP;
        status = ave_parse_frame_rate(frame_rate.framerate,
                                      &rate->frames_per_second);
        if (status < 0)
            return status;
        rate->frame_rate_seen = true;
        return 0;
    }
    case VAEncMiscParameterTypeHRD:
        /*
         * apple-ave uses a fixed 0.5 s VBV (ave-sess.c CBR filler model);
         * buffer size/fullness are advisory and cannot be applied.
         */
        if (size < sizeof(VAEncMiscParameterHRD))
            return -EINVAL;
        return 0;
    case VAEncMiscParameterTypeQualityLevel:
        /*
         * VAConfigAttribEncQualityRange is not advertised, which per va.h
         * means the driver has a single quality level.
         */
        return 0;
    default:
        return -ENOTSUP;
    }
}

int ave_rate_settings_from_va(uint32_t va_rate_control,
                              const struct ave_rate_request *rate,
                              uint32_t sequence_bits_per_second,
                              struct ave_rate_settings *settings)
{
    uint32_t bits_per_second;

    if (!rate || !settings)
        return -EINVAL;
    memset(settings, 0, sizeof(*settings));

    bits_per_second = rate->rate_seen && rate->bits_per_second
                          ? rate->bits_per_second
                          : sequence_bits_per_second;
    if (!bits_per_second)
        return -EINVAL;

    if (va_rate_control == VA_RC_CBR) {
        settings->v4l2_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
        settings->bitrate = bits_per_second;
    } else if (va_rate_control == VA_RC_VBR) {
        uint32_t percentage = rate->rate_seen ? rate->target_percentage : 0;
        uint64_t target;

        /* VA VBR: bits_per_second is the peak, the target a percentage. */
        if (percentage == 0 || percentage > 100)
            percentage = 100;
        target = (uint64_t)bits_per_second * percentage / 100u;
        settings->v4l2_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
        settings->bitrate = (uint32_t)target;
        /*
         * A peak equal to the average starves apple-ave's VBR; 0 selects
         * the driver default (twice the average, as macOS configures it).
         */
        settings->peak = percentage < 100 ? bits_per_second : 0;
    } else {
        return -ENOTSUP;
    }

    if (rate->rate_seen && (rate->min_qp || rate->max_qp)) {
        settings->qp_range = true;
        settings->min_qp = rate->min_qp;
        settings->max_qp = rate->max_qp ? rate->max_qp : 51u;
        if (settings->max_qp > 51u || settings->min_qp > settings->max_qp)
            return -ERANGE;
    }
    return 0;
}

uint32_t ave_gop_from_va(uint32_t intra_idr_period, uint32_t intra_period)
{
    uint32_t gop = intra_idr_period ? intra_idr_period : intra_period;

    /*
     * apple-ave's GOP control is the IDR interval, 1..65535 frames (0 would
     * mean the firmware's own 120).  "No periodic IDR" and longer periods
     * become the longest interval it supports.
     */
    if (gop == 0 || gop > 65535u)
        gop = 65535u;
    return gop;
}
