// SPDX-License-Identifier: MIT
#ifndef AVE_ENCODE_PARAMS_H
#define AVE_ENCODE_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <va/va.h>

/* Rate-related values collected from VAEncMiscParameterBuffer entries. */
struct ave_rate_request {
    bool rate_seen;
    uint32_t bits_per_second;
    uint32_t target_percentage;
    uint32_t min_qp;
    uint32_t max_qp;
    bool frame_rate_seen;
    uint32_t frames_per_second;
};

/* What apple-ave is told (V4L2 bit-rate mode, bit rates, QP range). */
struct ave_rate_settings {
    uint32_t v4l2_mode;
    uint32_t bitrate;
    uint32_t peak;
    bool qp_range;
    uint32_t min_qp;
    uint32_t max_qp;
};

int ave_parse_misc(VAEncMiscParameterType type, const void *data,
                   size_t size, struct ave_rate_request *rate);
int ave_rate_settings_from_va(uint32_t va_rate_control,
                              const struct ave_rate_request *rate,
                              uint32_t sequence_bits_per_second,
                              struct ave_rate_settings *settings);
uint32_t ave_gop_from_va(uint32_t intra_idr_period, uint32_t intra_period);
int ave_parse_frame_rate(uint32_t value, uint32_t *result);

#endif
