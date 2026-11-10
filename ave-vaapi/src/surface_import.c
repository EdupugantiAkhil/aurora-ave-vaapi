// SPDX-License-Identifier: MIT
#include "surface_import.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <string.h>

static unsigned int bytes_per_sample(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_P010 ? 2u
           : fourcc == VA_FOURCC_NV12 ? 1u : 0u;
}

static int finish_layout(uint32_t fourcc, uint32_t width, uint32_t height,
                         uint32_t pitch, uint64_t y_offset,
                         uint64_t uv_offset, uint64_t object_size, int fd,
                         struct ave_import_layout *layout)
{
    unsigned int bps = bytes_per_sample(fourcc);
    uint64_t row_bytes;
    uint64_t y_end;
    uint64_t uv_end;

    if (!bps || !width || !height || (width & 1u) || (height & 1u) ||
        fd < 0)
        return -EINVAL;
    row_bytes = (uint64_t)width * bps;
    if (pitch < row_bytes)
        return -EINVAL;
    y_end = y_offset + (uint64_t)pitch * height;
    uv_end = uv_offset + (uint64_t)pitch * (height / 2u);
    /* Luma first, chroma after it, no overlap. */
    if (uv_offset < y_end)
        return -EINVAL;
    if (object_size && uv_end > object_size)
        return -EINVAL;
    if (uv_end > SIZE_MAX)
        return -EOVERFLOW;

    *layout = (struct ave_import_layout) {
        .fourcc = fourcc,
        .width = width,
        .height = height,
        .stride = pitch,
        .y_offset = (size_t)y_offset,
        .uv_offset = (size_t)uv_offset,
        .required_size = (size_t)uv_end,
        .object_size = (size_t)object_size,
        .fd = fd,
    };
    return 0;
}

int ave_import_layout_from_external(const VASurfaceAttribExternalBuffers *ext,
                                    uint32_t width, uint32_t height,
                                    uint32_t fourcc,
                                    struct ave_import_layout *layout)
{
    if (!ext || !layout || !ext->buffers)
        return -EINVAL;
    /* Tiled or protected buffers cannot be read through a CPU mapping. */
    if (ext->flags & (VA_SURFACE_EXTBUF_DESC_ENABLE_TILING |
                      VA_SURFACE_EXTBUF_DESC_PROTECTED))
        return -ENOTSUP;
    if (ext->pixel_format != fourcc || ext->num_planes != 2 ||
        ext->num_buffers != 1 || ext->width < width ||
        ext->height < height || ext->pitches[0] != ext->pitches[1])
        return -EINVAL;
    if (ext->buffers[0] > (uintptr_t)INT32_MAX)
        return -EINVAL;
    return finish_layout(fourcc, width, height, ext->pitches[0],
                         ext->offsets[0], ext->offsets[1], ext->data_size,
                         (int)ext->buffers[0], layout);
}

int ave_import_layout_from_prime2(const VADRMPRIMESurfaceDescriptor *prime,
                                  uint32_t width, uint32_t height,
                                  uint32_t fourcc,
                                  struct ave_import_layout *layout)
{
    uint32_t y_pitch;
    uint32_t uv_pitch;
    uint64_t y_offset;
    uint64_t uv_offset;
    uint32_t luma_format = fourcc == VA_FOURCC_P010 ? DRM_FORMAT_R16
                                                      : DRM_FORMAT_R8;
    uint32_t chroma_format = fourcc == VA_FOURCC_P010 ? DRM_FORMAT_GR1616
                                                        : DRM_FORMAT_GR88;
    uint32_t combined_format = fourcc == VA_FOURCC_P010 ? DRM_FORMAT_P010
                                                          : DRM_FORMAT_NV12;

    if (!prime || !layout)
        return -EINVAL;
    if (prime->fourcc != fourcc || prime->width < width ||
        prime->height < height || prime->num_objects != 1)
        return -EINVAL;
    /* Only linear buffers can be read row by row through mmap. */
    if (prime->objects[0].drm_format_modifier != DRM_FORMAT_MOD_LINEAR)
        return -ENOTSUP;

    if (prime->num_layers == 1) {
        if (prime->layers[0].drm_format != combined_format ||
            prime->layers[0].num_planes != 2 ||
            prime->layers[0].object_index[0] != 0 ||
            prime->layers[0].object_index[1] != 0)
            return -EINVAL;
        y_pitch = prime->layers[0].pitch[0];
        uv_pitch = prime->layers[0].pitch[1];
        y_offset = prime->layers[0].offset[0];
        uv_offset = prime->layers[0].offset[1];
    } else if (prime->num_layers == 2) {
        if (prime->layers[0].drm_format != luma_format ||
            prime->layers[1].drm_format != chroma_format ||
            prime->layers[0].num_planes != 1 ||
            prime->layers[1].num_planes != 1 ||
            prime->layers[0].object_index[0] != 0 ||
            prime->layers[1].object_index[0] != 0)
            return -EINVAL;
        y_pitch = prime->layers[0].pitch[0];
        uv_pitch = prime->layers[1].pitch[0];
        y_offset = prime->layers[0].offset[0];
        uv_offset = prime->layers[1].offset[0];
    } else {
        return -EINVAL;
    }
    if (y_pitch != uv_pitch)
        return -EINVAL;
    return finish_layout(fourcc, width, height, y_pitch, y_offset, uv_offset,
                         prime->objects[0].size, prime->objects[0].fd,
                         layout);
}
