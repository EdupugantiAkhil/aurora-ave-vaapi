// SPDX-License-Identifier: MIT
#ifndef AVE_SURFACE_IMPORT_H
#define AVE_SURFACE_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

/*
 * Validated description of an imported two-plane 4:2:0 DMA-BUF.  Offsets
 * are from the start of the single DMA-BUF object; both planes share one
 * pitch, the only layout the copy into AVE's OUTPUT buffer handles.
 */
struct ave_import_layout {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t y_offset;
    size_t uv_offset;
    size_t required_size;
    size_t object_size;
    int fd;
};

/* Legacy VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME + VASurfaceAttribExternalBuffers. */
int ave_import_layout_from_external(const VASurfaceAttribExternalBuffers *ext,
                                    uint32_t width, uint32_t height,
                                    uint32_t fourcc,
                                    struct ave_import_layout *layout);
/* VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 + VADRMPRIMESurfaceDescriptor. */
int ave_import_layout_from_prime2(const VADRMPRIMESurfaceDescriptor *prime,
                                  uint32_t width, uint32_t height,
                                  uint32_t fourcc,
                                  struct ave_import_layout *layout);

#endif
