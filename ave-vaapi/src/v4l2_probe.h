// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_PROBE_H
#define VENUS_V4L2_PROBE_H

#include "capabilities.h"

#include <stdbool.h>
#include <stddef.h>

struct v4l2_capability;

/* video_device name (sysfs) and QUERYCAP driver of the stateful encoder. */
#define AVE_V4L2_DRIVER_NAME "apple-ave"

int venus_v4l2_probe(struct venus_capabilities *caps);
int venus_v4l2_probe_paths(struct venus_capabilities *caps,
                           const char *sysfs_root,
                           const char *device_prefix, unsigned int limit);

/* Exposed for host tests: no device is opened by these helpers. */
bool ave_probe_name_matches(const char *contents);
size_t ave_probe_select_nodes(const char *sysfs_root, unsigned int limit,
                              unsigned int *indices, size_t max_indices);
bool ave_capability_is_encoder(const struct v4l2_capability *capability);

#endif
