// SPDX-License-Identifier: MIT
#include "v4l2_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef VENUS_VAAPI_PROBE_LIMIT
#define VENUS_VAAPI_PROBE_LIMIT 64
#endif

/*
 * Apple AVE port: the only supported device is the stateful apple-ave
 * encoder.  Other V4L2 nodes on the same machine (the AVD decoder, the ISP
 * camera) must not be opened at all, because opening a V4L2 node can power
 * up or otherwise disturb unrelated hardware.  Candidate nodes are therefore
 * selected from sysfs (/sys/class/video4linux/videoN/name, which is the
 * video_device name and needs no open()), and only matching nodes are
 * opened and confirmed with VIDIOC_QUERYCAP.
 *
 * AVE_VAAPI_DEVICE=/dev/videoN may name the node explicitly; it is still
 * checked against sysfs and QUERYCAP before use.
 */
static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

bool ave_probe_name_matches(const char *contents)
{
    size_t length;

    if (!contents)
        return false;
    length = strcspn(contents, "\r\n");
    return length == strlen(AVE_V4L2_DRIVER_NAME) &&
           strncmp(contents, AVE_V4L2_DRIVER_NAME, length) == 0;
}

static bool sysfs_node_is_ave(const char *sysfs_root, unsigned int index)
{
    char path[256];
    char contents[64] = { 0 };
    ssize_t length;
    int fd;

    if (snprintf(path, sizeof(path), "%s/video%u/name", sysfs_root, index) >=
        (int)sizeof(path))
        return false;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    length = read(fd, contents, sizeof(contents) - 1);
    close(fd);
    if (length <= 0)
        return false;
    contents[length] = '\0';
    return ave_probe_name_matches(contents);
}

size_t ave_probe_select_nodes(const char *sysfs_root, unsigned int limit,
                              unsigned int *indices, size_t max_indices)
{
    size_t count = 0;
    unsigned int index;

    if (!sysfs_root || !indices)
        return 0;
    for (index = 0; index < limit && count < max_indices; index++) {
        if (sysfs_node_is_ave(sysfs_root, index))
            indices[count++] = index;
    }
    return count;
}

bool ave_capability_is_encoder(const struct v4l2_capability *capability)
{
    uint32_t caps;

    if (!capability)
        return false;
    caps = capability->capabilities & V4L2_CAP_DEVICE_CAPS
               ? capability->device_caps
               : capability->capabilities;
    return strcmp((const char *)capability->driver,
                  AVE_V4L2_DRIVER_NAME) == 0 &&
           strstr((const char *)capability->card, "encoder") &&
           (caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
           (caps & V4L2_CAP_STREAMING);
}

static void enumerate_formats(int fd, struct venus_capabilities *caps)
{
    struct v4l2_fmtdesc format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
         format.index++)
        venus_capabilities_add_fourcc(caps, VENUS_ROLE_ENCODER,
                                      format.pixelformat);

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
         format.index++)
        venus_capabilities_add_raw_fourcc(caps, VENUS_ROLE_ENCODER,
                                           format.pixelformat);
}

static int probe_node(struct venus_capabilities *caps, const char *path)
{
    struct v4l2_capability capability = { 0 };
    int fd;
    int status = -ENODEV;

    fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 &&
        ave_capability_is_encoder(&capability)) {
        snprintf(caps->encoder_path, sizeof(caps->encoder_path), "%s",
                 path);
        enumerate_formats(fd, caps);
        status = 0;
    }
    close(fd);
    return status;
}

static int parse_device_index(const char *path, const char *device_prefix,
                              unsigned int *index)
{
    size_t prefix_length = strlen(device_prefix);
    char *end = NULL;
    unsigned long value;

    if (strncmp(path, device_prefix, prefix_length) != 0 ||
        !path[prefix_length])
        return -EINVAL;
    errno = 0;
    value = strtoul(path + prefix_length, &end, 10);
    if (errno || !end || *end || value > 4096)
        return -EINVAL;
    *index = (unsigned int)value;
    return 0;
}

int venus_v4l2_probe_paths(struct venus_capabilities *caps,
                           const char *sysfs_root,
                           const char *device_prefix, unsigned int limit)
{
    const char *requested = getenv("AVE_VAAPI_DEVICE");
    unsigned int indices[8];
    size_t count;
    size_t i;

    if (!caps || !sysfs_root || !device_prefix || limit == 0)
        return -EINVAL;

    venus_capabilities_reset(caps);

    if (requested && requested[0]) {
        unsigned int index;

        if (parse_device_index(requested, device_prefix, &index) < 0 ||
            !sysfs_node_is_ave(sysfs_root, index))
            return -ENODEV;
        return probe_node(caps, requested);
    }

    count = ave_probe_select_nodes(sysfs_root, limit, indices,
                                   sizeof(indices) / sizeof(indices[0]));
    for (i = 0; i < count; i++) {
        char path[64];

        if (snprintf(path, sizeof(path), "%s%u", device_prefix,
                     indices[i]) >= (int)sizeof(path))
            return -ENAMETOOLONG;
        if (probe_node(caps, path) == 0)
            return 0;
        venus_capabilities_reset(caps);
    }

    return -ENODEV;
}

int venus_v4l2_probe(struct venus_capabilities *caps)
{
    return venus_v4l2_probe_paths(caps, "/sys/class/video4linux",
                                  "/dev/video", VENUS_VAAPI_PROBE_LIMIT);
}
