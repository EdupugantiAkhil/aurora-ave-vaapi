// SPDX-License-Identifier: MIT
#include "backend_internal.h"
#include "surface_import.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

/*
 * VA surfaces are either plain CPU allocations (VA_SURFACE_ATTRIB_MEM_TYPE_VA)
 * or imported linear NV12/P010 DMA-BUFs (DRM_PRIME / DRM_PRIME_2), which is
 * how Chrome's VaapiVideoEncodeAccelerator hands over WebCodecs frames on
 * Linux.  Imported buffers are mapped read-only and bracketed with
 * DMA_BUF_IOCTL_SYNC; their pixels are copied into the encoder's own MMAP
 * OUTPUT buffers in AVE's layout at vaEndPicture time.
 *
 * The Qualcomm original allocated surfaces with DRM_IOCTL_MSM_GEM_NEW on
 * /dev/dri/renderD128.  That driver-private ioctl number means something
 * else on other DRM drivers, so it is not issued at all in this port.
 */
static unsigned int surface_bytes_per_sample(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_P010 ? 2u
           : fourcc == VA_FOURCC_NV12 ? 1u : 0u;
}

static int yuv420_size(unsigned int width, unsigned int height,
                       uint32_t fourcc, size_t *size)
{
    unsigned int bytes_per_sample =
        surface_bytes_per_sample(fourcc);
    size_t pixels;

    if (!width || !height || (width & 1u) || (height & 1u) ||
        !bytes_per_sample || width > SIZE_MAX / height)
        return -EINVAL;

    pixels = (size_t)width * height;
    if (pixels > SIZE_MAX / (3u * bytes_per_sample))
        return -EOVERFLOW;

    *size = pixels * 3u / 2u * bytes_per_sample;
    return 0;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void release_surface(struct venus_surface *surface)
{
    if (!surface)
        return;

    if (surface->dma_backed) {
        if (surface->map_base && surface->map_size)
            munmap(surface->map_base, surface->map_size);
        if (surface->dma_fd >= 0)
            close(surface->dma_fd);
    } else {
        free(surface->data);
    }
    memset(surface, 0, sizeof(*surface));
    surface->dma_fd = -1;
}

/* Map an imported DMA-BUF according to an already validated layout. */
static int import_dma_surface(struct venus_surface *surface, int fd,
                              const struct ave_import_layout *layout)
{
    off_t end;
    size_t size;
    void *base;
    int owned_fd;

    end = lseek(fd, 0, SEEK_END);
    if (end > 0) {
        if ((uint64_t)end < layout->required_size)
            return -EINVAL;
        size = (size_t)end;
    } else {
        size = layout->object_size;
    }
    if (size < layout->required_size)
        return -EINVAL;

    owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned_fd < 0)
        return -errno;
    base = mmap(NULL, size, PROT_READ, MAP_SHARED, owned_fd, 0);
    if (base == MAP_FAILED) {
        int saved_errno = errno;

        close(owned_fd);
        return -saved_errno;
    }

    surface->dma_fd = owned_fd;
    surface->dma_backed = true;
    surface->map_base = base;
    surface->map_size = size;
    surface->data = (uint8_t *)base + layout->y_offset;
    surface->stride = layout->stride;
    surface->scanlines = layout->height;
    surface->uv_offset = layout->uv_offset - layout->y_offset;
    surface->capacity = size - layout->y_offset;
    surface->data_size = surface->capacity;
    return 0;
}

static int wait_surface_access(const struct venus_surface *surface,
                               short events)
{
    struct pollfd fence;
    unsigned int attempts;

    if (!surface || !surface->dma_backed)
        return 0;
    if (surface->dma_fd < 0)
        return -EBADF;

    fence = (struct pollfd) {
        .fd = surface->dma_fd,
        .events = events,
    };
    for (attempts = 0; attempts < 50; attempts++) {
        int result;

        do {
            result = poll(&fence, 1, 100);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return -errno;
        if (result == 0)
            continue;
        if (fence.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -EIO;
        if (fence.revents & events)
            return 0;
    }

    return -ETIMEDOUT;
}

int venus_surface_wait_for_device_read(
    const struct venus_surface *surface)
{
    /* POLLIN on a DMA-BUF waits for pending writers (e.g. GPU fills). */
    return wait_surface_access(surface, POLLIN);
}

static int surface_cpu_sync(struct venus_surface *surface,
                            uint64_t flags)
{
    struct dma_buf_sync sync = {
        .flags = flags,
    };
    int status;

    if (!surface || !surface->dma_backed)
        return 0;
    if (surface->dma_fd < 0)
        return -EBADF;

    /*
     * DMA_BUF_IOCTL_SYNC provides cache coherency, but does not replace
     * waiting for a GPU writer or reader attached to the DMA-BUF reservation.
     * Vulkan writes exported VA surfaces asynchronously, so wait before
     * exposing the mapping to the CPU.
     */
    if (!(flags & DMA_BUF_SYNC_END)) {
        status = wait_surface_access(
            surface,
            (flags & DMA_BUF_SYNC_WRITE) ? POLLOUT : POLLIN);
        if (status < 0)
            return status;
    }

    if (xioctl(surface->dma_fd, DMA_BUF_IOCTL_SYNC,
               &sync) < 0)
        return -errno;
    return 0;
}

int venus_surface_begin_cpu_read(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

int venus_surface_end_cpu_read(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

int venus_surface_begin_cpu_write(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

int venus_surface_end_cpu_write(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

int venus_surface_begin_cpu_rw(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
}

int venus_surface_end_cpu_rw(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}

int venus_surface_copy_from_yuv420(
    struct venus_surface *surface, const uint8_t *source,
    size_t source_size, uint32_t source_stride,
    size_t source_uv_offset, unsigned int width,
    unsigned int height)
{
    uint32_t destination_stride;
    size_t destination_uv_offset;
    size_t source_required;
    size_t destination_required;
    uint32_t row_bytes;
    unsigned int row;
    int status;
    int end_status;

    if (!surface || !source || !surface->data ||
        width == 0 || height == 0 || (width & 1u) ||
        (height & 1u) || width > surface->width ||
        height > surface->height ||
        !surface_bytes_per_sample(surface->fourcc))
        return -EINVAL;
    row_bytes = width * surface_bytes_per_sample(surface->fourcc);
    if (source_stride < row_bytes)
        return -EINVAL;
    if (source_stride > SIZE_MAX / height ||
        source_uv_offset < (size_t)source_stride * height ||
        source_stride > SIZE_MAX / (height / 2u))
        return -EOVERFLOW;

    source_required =
        source_uv_offset + (size_t)source_stride * (height / 2u);
    destination_stride =
        surface->stride ? surface->stride : row_bytes;
    destination_uv_offset =
        surface->uv_offset
            ? surface->uv_offset
            : (size_t)destination_stride * surface->height;
    if (destination_stride < row_bytes ||
        destination_stride > SIZE_MAX / (height / 2u) ||
        destination_uv_offset >
            SIZE_MAX - (size_t)destination_stride *
                           (height / 2u))
        return -EOVERFLOW;

    destination_required =
        destination_uv_offset +
        (size_t)destination_stride * (height / 2u);
    if (source_required > source_size ||
        destination_required > surface->capacity)
        return -ENOSPC;

    status = venus_surface_begin_cpu_write(surface);
    if (status < 0)
        return status;

    memset(surface->data, 0, surface->capacity);
    for (row = 0; row < height; row++)
        memcpy(surface->data +
                   (size_t)row * destination_stride,
               source + (size_t)row * source_stride, row_bytes);
    for (row = 0; row < height / 2u; row++)
        memcpy(surface->data + destination_uv_offset +
                   (size_t)row * destination_stride,
               source + source_uv_offset +
                   (size_t)row * source_stride,
               row_bytes);

    surface->data_size = destination_required;
    surface->ready = true;
    end_status = venus_surface_end_cpu_write(surface);
    return end_status < 0 ? end_status : 0;
}

int venus_surface_copy_to_yuv420(
    struct venus_surface *surface, uint8_t *destination,
    size_t destination_size, uint32_t destination_stride,
    size_t destination_uv_offset, unsigned int width,
    unsigned int height)
{
    uint32_t source_stride;
    size_t source_uv_offset;
    size_t source_required;
    size_t destination_required;
    uint32_t row_bytes;
    unsigned int row;
    int status;
    int end_status;

    if (!surface || !destination || !surface->data ||
        width == 0 || height == 0 || (width & 1u) ||
        (height & 1u) || width > surface->width ||
        height > surface->height ||
        !surface_bytes_per_sample(surface->fourcc))
        return -EINVAL;
    row_bytes = width * surface_bytes_per_sample(surface->fourcc);
    if (destination_stride < row_bytes)
        return -EINVAL;

    source_stride =
        surface->stride ? surface->stride : row_bytes;
    source_uv_offset =
        surface->uv_offset
            ? surface->uv_offset
            : (size_t)source_stride * surface->height;
    if (source_stride < row_bytes ||
        source_stride > SIZE_MAX / (height / 2u) ||
        source_uv_offset >
            SIZE_MAX - (size_t)source_stride *
                           (height / 2u) ||
        destination_stride > SIZE_MAX / height ||
        destination_uv_offset <
            (size_t)destination_stride * height ||
        destination_stride > SIZE_MAX / (height / 2u) ||
        destination_uv_offset >
            SIZE_MAX - (size_t)destination_stride *
                           (height / 2u))
        return -EOVERFLOW;

    source_required =
        source_uv_offset + (size_t)source_stride * (height / 2u);
    destination_required =
        destination_uv_offset +
        (size_t)destination_stride * (height / 2u);
    if (source_required > surface->data_size ||
        destination_required > destination_size)
        return -ENOSPC;

    status = venus_surface_begin_cpu_read(surface);
    if (status < 0)
        return status;

    memset(destination, 0, destination_size);
    for (row = 0; row < height; row++)
        memcpy(destination +
                   (size_t)row * destination_stride,
               surface->data + (size_t)row * source_stride,
               row_bytes);
    for (row = 0; row < height / 2u; row++)
        memcpy(destination + destination_uv_offset +
                   (size_t)row * destination_stride,
               surface->data + source_uv_offset +
                   (size_t)row * source_stride,
               row_bytes);

    end_status = venus_surface_end_cpu_read(surface);
    return end_status < 0 ? end_status : 0;
}

static struct venus_buffer *allocate_buffer(
    struct venus_backend *backend, VAContextID context_id,
    VABufferType type, size_t element_size, size_t num_elements,
    const void *data, bool owns_data)
{
    size_t total_size;
    unsigned int index;

    if (element_size == 0 || num_elements == 0 ||
        element_size > SIZE_MAX / num_elements)
        return NULL;
    total_size = element_size * num_elements;

    for (index = 0; index < VENUS_MAX_BUFFERS; index++) {
        struct venus_buffer *buffer = &backend->buffers[index];

        if (buffer->used)
            continue;

        *buffer = (struct venus_buffer) {
            .used = true,
            .id = VENUS_BUFFER_BASE | (index + 1),
            .context_id = context_id,
            .type = type,
            .element_size = element_size,
            .capacity_elements = num_elements,
            .num_elements = num_elements,
            .owns_data = owns_data,
            .source_surface_id = VA_INVALID_ID,
        };

        if (owns_data) {
            buffer->data = malloc(total_size);
            if (!buffer->data) {
                memset(buffer, 0, sizeof(*buffer));
                return NULL;
            }
            if (data)
                memcpy(buffer->data, data, total_size);
            else
                memset(buffer->data, 0, total_size);
        } else {
            buffer->data = (uint8_t *)data;
        }

        return buffer;
    }

    return NULL;
}

struct surface_request {
    uint32_t fourcc;
    int memory_type;
    void *external;
};

static bool valid_surface_attributes(
    VASurfaceAttrib *attributes, unsigned int num_attributes,
    struct surface_request *request)
{
    unsigned int index;

    request->memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    request->external = NULL;
    for (index = 0; index < num_attributes; index++) {
        const VASurfaceAttrib *attribute = &attributes[index];

        if (attribute->type == VASurfaceAttribPixelFormat) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                !surface_bytes_per_sample(
                    (uint32_t)attribute->value.value.i))
                return false;
            request->fourcc = (uint32_t)attribute->value.value.i;
        } else if (attribute->type == VASurfaceAttribMemoryType) {
            int memory_type;

            if (attribute->value.type != VAGenericValueTypeInteger)
                return false;
            memory_type = attribute->value.value.i;
            if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA &&
                memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
                memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
                return false;
            request->memory_type = memory_type;
        } else if (attribute->type ==
                   VASurfaceAttribExternalBufferDescriptor) {
            if (attribute->value.type != VAGenericValueTypePointer ||
                !attribute->value.value.p)
                return false;
            request->external = attribute->value.value.p;
        } else if (attribute->type ==
                   VASurfaceAttribUsageHint) {
            if (attribute->value.type !=
                VAGenericValueTypeInteger)
                return false;
            /* Surfaces cannot be exported as DMA-BUF in this port. */
            if (attribute->value.value.i &
                VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT)
                return false;
        } else if (attribute->flags &
                   VA_SURFACE_ATTRIB_SETTABLE) {
            return false;
        }
    }

    /* A descriptor without an import memory type (or vice versa) is bogus. */
    if ((request->memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA) !=
        (request->external != NULL))
        return false;
    return true;
}

static VAStatus create_surfaces_locked(
    struct venus_backend *backend, unsigned int format,
    unsigned int width, unsigned int height,
    VASurfaceID *surface_ids, unsigned int num_surfaces,
    VASurfaceAttrib *attributes, unsigned int num_attributes)
{
    struct venus_surface *created[VENUS_MAX_SURFACES];
    struct surface_request request = { 0 };
    struct ave_import_layout layout;
    size_t capacity;
    unsigned int free_slots = 0;
    unsigned int created_count = 0;
    unsigned int index;

    if (!(format & (VA_RT_FORMAT_YUV420 |
                    VA_RT_FORMAT_YUV420_10)))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    request.fourcc = (format & VA_RT_FORMAT_YUV420)
                         ? VA_FOURCC_NV12 : VA_FOURCC_P010;
    /*
     * Surfaces may be smaller than the encode limits (reference copies are
     * driver-internal), but never larger than what AVE can encode.
     */
    if (!surface_ids || num_surfaces == 0 ||
        num_surfaces > VENUS_MAX_SURFACES ||
        width == 0 || height == 0 ||
        width > VENUS_MAX_WIDTH || height > VENUS_MAX_HEIGHT)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attributes > 0 && !attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!valid_surface_attributes(
            attributes, num_attributes, &request))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    if ((request.fourcc == VA_FOURCC_NV12 &&
         !(format & VA_RT_FORMAT_YUV420)) ||
        (request.fourcc == VA_FOURCC_P010 &&
         !(format & VA_RT_FORMAT_YUV420_10)))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (yuv420_size(width, height, request.fourcc, &capacity) < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (request.memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA) {
        int status;

        /* One descriptor describes one buffer. */
        if (num_surfaces != 1)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        status = request.memory_type ==
                         VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2
                     ? ave_import_layout_from_prime2(
                           request.external, width, height,
                           request.fourcc, &layout)
                     : ave_import_layout_from_external(
                           request.external, width, height,
                           request.fourcc, &layout);
        if (status == -ENOTSUP)
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        if (status < 0)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (index = 0; index < VENUS_MAX_SURFACES; index++) {
        if (!backend->surfaces[index].used)
            free_slots++;
    }
    if (free_slots < num_surfaces)
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    for (index = 0; index < VENUS_MAX_SURFACES &&
                    created_count < num_surfaces;
         index++) {
        struct venus_surface *surface = &backend->surfaces[index];

        if (surface->used)
            continue;

        surface->dma_fd = -1;
        if (request.memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA) {
            if (import_dma_surface(surface, layout.fd, &layout) < 0) {
                release_surface(surface);
                goto fail;
            }
        } else {
            surface->data = calloc(1, capacity);
            if (!surface->data) {
                release_surface(surface);
                goto fail;
            }
            surface->capacity = capacity;
            surface->data_size = capacity;
            surface->stride = width *
                surface_bytes_per_sample(request.fourcc);
            surface->scanlines = height;
            surface->uv_offset =
                (size_t)surface->stride * height;
        }

        surface->used = true;
        surface->id = VENUS_SURFACE_BASE | (index + 1);
        surface->width = width;
        surface->height = height;
        surface->fourcc = request.fourcc;
        surface->ready = true;
        surface->coded_buffer_id = VA_INVALID_ID;
        surface->context_id = VA_INVALID_ID;
        created[created_count] = surface;
        surface_ids[created_count] = surface->id;
        created_count++;
    }

    return VA_STATUS_SUCCESS;

fail:
    while (created_count > 0) {
        struct venus_surface *surface = created[--created_count];

        release_surface(surface);
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus backend_create_surfaces(
    VADriverContextP context, int width, int height, int format,
    int num_surfaces, VASurfaceID *surfaces)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend || width < 0 || height < 0 || num_surfaces < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    status = create_surfaces_locked(
        backend, (unsigned int)format, (unsigned int)width,
        (unsigned int)height, surfaces, (unsigned int)num_surfaces,
        NULL, 0);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_create_surfaces2(
    VADriverContextP context, unsigned int format,
    unsigned int width, unsigned int height,
    VASurfaceID *surfaces, unsigned int num_surfaces,
    VASurfaceAttrib *attributes, unsigned int num_attributes)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    status = create_surfaces_locked(
        backend, format, width, height, surfaces, num_surfaces,
        attributes, num_attributes);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static bool surface_has_image(struct venus_backend *backend,
                              VASurfaceID surface_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (backend->images[index].used &&
            backend->images[index].surface_id == surface_id)
            return true;
    }

    return false;
}

/*
 * Decoder contexts own their render targets until destroyed.  Encode input
 * surfaces are copied at vaEndPicture and may be destroyed once their
 * encode completed (Chrome imports and destroys one surface per frame).
 */
static bool surface_bound_to_decoder(struct venus_backend *backend,
                                     const struct venus_surface *surface)
{
    struct venus_context *context;
    struct venus_config *config;

    if (surface->context_id == VA_INVALID_ID)
        return false;
    context = venus_backend_find_context(backend, surface->context_id);
    if (!context)
        return false;
    config = venus_backend_find_config(backend, context->config_id);
    return !config || config->entrypoint != VAEntrypointEncSlice;
}

static VAStatus backend_destroy_surfaces(
    VADriverContextP context, VASurfaceID *surface_ids,
    int num_surfaces)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    int index;

    if (!backend || num_surfaces < 0 ||
        (num_surfaces > 0 && !surface_ids))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    for (index = 0; index < num_surfaces; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, surface_ids[index]);

        if (!surface) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface->encode_pending ||
            surface_bound_to_decoder(backend, surface) ||
            surface_has_image(backend, surface->id)) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }

    for (index = 0; index < num_surfaces; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, surface_ids[index]);

        release_surface(surface);
    }

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_create_buffer(
    VADriverContextP context, VAContextID context_id,
    VABufferType type, unsigned int size,
    unsigned int num_elements, void *data,
    VABufferID *buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend || !buffer_id || size == 0 || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (!venus_backend_find_context(backend, context_id)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    buffer = allocate_buffer(
        backend, context_id, type, size, num_elements, data, true);
    if (!buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (type == VAEncCodedBufferType) {
        buffer->coded_segment.buf = buffer->data;
        buffer->coded_segment.next = NULL;
    }

    *buffer_id = buffer->id;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_set_num_elements(
    VADriverContextP context, VABufferID buffer_id,
    unsigned int num_elements)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;
    uint8_t *resized;
    size_t old_size;
    size_t new_size;

    if (!backend || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || !buffer->owns_data ||
        buffer->type == VAEncCodedBufferType) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (num_elements > SIZE_MAX / buffer->element_size) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (num_elements > buffer->capacity_elements) {
        old_size = buffer->capacity_elements * buffer->element_size;
        new_size = (size_t)num_elements * buffer->element_size;
        resized = realloc(buffer->data, new_size);
        if (!resized) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        memset(resized + old_size, 0, new_size - old_size);
        buffer->data = resized;
        buffer->capacity_elements = num_elements;
    }

    buffer->num_elements = num_elements;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static struct venus_surface *surface_for_image_buffer(
    struct venus_backend *backend, VABufferID buffer_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        struct venus_image *image = &backend->images[index];

        if (!image->used || image->buffer_id != buffer_id ||
            image->surface_id == VA_INVALID_ID)
            continue;
        return venus_backend_find_surface(
            backend, image->surface_id);
    }

    return NULL;
}

static VAStatus wait_coded_delivery_locked(
    struct venus_backend *backend, struct venus_buffer **buffer_ptr)
{
    struct venus_buffer *buffer = *buffer_ptr;
    struct venus_context *context;
    unsigned int attempts = 0;

    context = venus_backend_find_context(
        backend, buffer->context_id);
    if (!context || buffer->coded_tag == 0)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (buffer->coded_delivered)
        return VA_STATUS_SUCCESS;

    while (!buffer->coded_delivered &&
           buffer->coded_sequence !=
               context->encode_delivery_sequence) {
        VABufferID buffer_id = buffer->id;

        if (buffer->coded_sequence <
            context->encode_delivery_sequence)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        if (attempts++ >= 5000)
            return VA_STATUS_ERROR_HW_BUSY;

        pthread_mutex_unlock(&backend->mutex);
        usleep(1000);
        pthread_mutex_lock(&backend->mutex);

        buffer = venus_backend_find_buffer(backend, buffer_id);
        context = buffer
                      ? venus_backend_find_context(
                            backend, buffer->context_id)
                      : NULL;
        if (!buffer || !context)
            return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (!buffer->coded_delivered) {
        context->encode_delivery_sequence++;
        buffer->coded_delivered = true;
    }

    *buffer_ptr = buffer;
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_map_buffer(VADriverContextP context,
                                   VABufferID buffer_id,
                                   void **mapped)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_buffer *buffer;
    int status;

    if (!backend || !mapped)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->map_count == UINT_MAX) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buffer->type == VAEncCodedBufferType) {
        VAStatus sync_status;
        VAStatus delivery_status;

        if (!buffer->coded_ready) {
            sync_status = venus_encode_sync_buffer_locked(
                backend, buffer, 5000);
            if (sync_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&backend->mutex);
                return sync_status;
            }
        }

        delivery_status = wait_coded_delivery_locked(
            backend, &buffer);
        if (delivery_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&backend->mutex);
            return delivery_status;
        }
    }

    surface = surface_for_image_buffer(backend, buffer_id);
    if (surface && buffer->map_count == 0) {
        status = venus_surface_begin_cpu_rw(surface);
        if (status < 0) {
            pthread_mutex_unlock(&backend->mutex);
            return venus_backend_status_from_errno(status);
        }
    }

    buffer->map_count++;
    *mapped = buffer->type == VAEncCodedBufferType
                  ? (void *)&buffer->coded_segment
                  : (void *)buffer->data;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_unmap_buffer(VADriverContextP context,
                                     VABufferID buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_buffer *buffer;
    VAStatus result = VA_STATUS_SUCCESS;
    int status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->map_count == 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    surface = surface_for_image_buffer(backend, buffer_id);
    buffer->map_count--;
    if (surface && buffer->map_count == 0) {
        status = venus_surface_end_cpu_rw(surface);
        if (status < 0)
            result = venus_backend_status_from_errno(status);
    }

    pthread_mutex_unlock(&backend->mutex);
    return result;
}

static bool buffer_used_by_image(struct venus_backend *backend,
                                 VABufferID buffer_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (backend->images[index].used &&
            backend->images[index].buffer_id == buffer_id)
            return true;
    }

    return false;
}

static VAStatus backend_destroy_buffer(VADriverContextP context,
                                       VABufferID buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->map_count > 0 ||
        buffer_used_by_image(backend, buffer_id)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buffer->type == VAEncCodedBufferType &&
        buffer->source_surface_id != VA_INVALID_ID) {
        struct venus_surface *surface = venus_backend_find_surface(
            backend, buffer->source_surface_id);

        if (surface && surface->coded_buffer_id == buffer->id) {
            if (surface->encode_pending) {
                VAStatus status = venus_encode_sync_surface_locked(
                    backend, surface, 5000);

                if (status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&backend->mutex);
                    return status;
                }
            }
            surface->coded_buffer_id = VA_INVALID_ID;
        }
    }

    venus_backend_free_buffer(buffer);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_buffer_info(
    VADriverContextP context, VABufferID buffer_id,
    VABufferType *type, unsigned int *size,
    unsigned int *num_elements)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend || !type || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->element_size > UINT_MAX ||
        buffer->num_elements > UINT_MAX) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *type = buffer->type;
    *size = (unsigned int)buffer->element_size;
    *num_elements = (unsigned int)buffer->num_elements;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAImageFormat yuv420_image_format(uint32_t fourcc)
{
    unsigned int bits = fourcc == VA_FOURCC_P010 ? 24u : 12u;

    return (VAImageFormat) {
        .fourcc = fourcc,
        .byte_order = VA_LSB_FIRST,
        .bits_per_pixel = bits,
        .depth = bits,
    };
}

static VAStatus backend_query_image_formats(
    VADriverContextP context, VAImageFormat *formats,
    int *num_formats)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);

    if (!backend || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (formats) {
        formats[0] = yuv420_image_format(VA_FOURCC_NV12);
        formats[1] = yuv420_image_format(VA_FOURCC_P010);
    }
    *num_formats = 2;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_image_locked(
    struct venus_backend *backend, unsigned int width,
    unsigned int height, uint32_t fourcc,
    uint8_t *external_data, size_t external_size,
    uint32_t stride,
    size_t uv_offset, VASurfaceID surface_id,
    VAImage *result)
{
    struct venus_buffer *buffer = NULL;
    struct venus_image *image = NULL;
    size_t data_size;
    unsigned int index;

    if (!result || width > VENUS_MAX_WIDTH ||
        height > VENUS_MAX_HEIGHT ||
        yuv420_size(width, height, fourcc, &data_size) < 0 ||
        stride < width * surface_bytes_per_sample(fourcc) ||
        uv_offset < (size_t)stride * height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (external_data && external_size <
            uv_offset + (size_t)stride * (height / 2))
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;

    buffer = allocate_buffer(
        backend, VA_INVALID_ID, VAImageBufferType,
        external_data ? external_size : data_size, 1,
        external_data, !external_data);
    if (!buffer)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (!backend->images[index].used) {
            image = &backend->images[index];
            break;
        }
    }
    if (!image) {
        venus_backend_free_buffer(buffer);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    *image = (struct venus_image) {
        .used = true,
        .id = VENUS_IMAGE_BASE | (index + 1),
        .buffer_id = buffer->id,
        .surface_id = surface_id,
        .width = width,
        .height = height,
        .fourcc = fourcc,
        .stride = stride,
        .uv_offset = uv_offset,
    };

    memset(result, 0, sizeof(*result));
    result->image_id = image->id;
    result->format = yuv420_image_format(fourcc);
    result->buf = buffer->id;
    result->width = (uint16_t)width;
    result->height = (uint16_t)height;
    result->data_size = (uint32_t)(
        external_data ? external_size : data_size);
    result->num_planes = 2;
    result->pitches[0] = stride;
    result->pitches[1] = stride;
    result->offsets[0] = 0;
    result->offsets[1] = (uint32_t)uv_offset;
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_create_image(
    VADriverContextP context, VAImageFormat *format,
    int width, int height, VAImage *image)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend || !format || width < 0 || height < 0 ||
        !surface_bytes_per_sample(format->fourcc))
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;

    pthread_mutex_lock(&backend->mutex);
    status = create_image_locked(
        backend, (unsigned int)width, (unsigned int)height,
        format->fourcc, NULL, 0,
        (uint32_t)width *
            surface_bytes_per_sample(format->fourcc),
        (size_t)width * (unsigned int)height *
            surface_bytes_per_sample(format->fourcc),
        VA_INVALID_ID, image);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_derive_image(VADriverContextP context,
                                     VASurfaceID surface_id,
                                     VAImage *image)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    VAStatus status;

    if (!backend || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!surface->ready) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surface->dma_backed) {
        /* Imported buffers are mapped read-only; use vaGetImage. */
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    status = create_image_locked(
        backend, surface->width, surface->height,
        surface->fourcc, surface->data, surface->capacity,
        surface->stride, surface->uv_offset,
        surface->id, image);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_destroy_image(VADriverContextP context,
                                      VAImageID image_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_image *image;
    struct venus_buffer *buffer;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    image = venus_backend_find_image(backend, image_id);
    if (!image) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    buffer = venus_backend_find_buffer(backend, image->buffer_id);
    if (!buffer || buffer->map_count > 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    venus_backend_free_buffer(buffer);
    memset(image, 0, sizeof(*image));
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_get_image(
    VADriverContextP context, VASurfaceID surface_id,
    int x, int y, unsigned int width, unsigned int height,
    VAImageID image_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_image *image;
    struct venus_buffer *buffer;
    size_t buffer_size;
    int status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    image = venus_backend_find_image(backend, image_id);
    buffer = image
                 ? venus_backend_find_buffer(backend, image->buffer_id)
                 : NULL;
    if (!surface || !image || !buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return !surface ? VA_STATUS_ERROR_INVALID_SURFACE
                        : VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (surface->fourcc != image->fourcc) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    if (!surface->ready) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (x != 0 || y != 0 || width != surface->width ||
        height != surface->height ||
        image->width != width || image->height != height ||
        buffer->map_count > 0 ||
        buffer->element_size >
            SIZE_MAX / buffer->num_elements) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    buffer_size =
        buffer->element_size * buffer->num_elements;
    status = venus_surface_copy_to_yuv420(
        surface, buffer->data, buffer_size,
        image->stride, image->uv_offset, width, height);
    pthread_mutex_unlock(&backend->mutex);
    return status < 0
               ? venus_backend_status_from_errno(status)
               : VA_STATUS_SUCCESS;
}

static VAStatus backend_put_image(
    VADriverContextP context, VASurfaceID surface_id,
    VAImageID image_id, int src_x, int src_y,
    unsigned int src_width, unsigned int src_height,
    int dst_x, int dst_y, unsigned int dst_width,
    unsigned int dst_height)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_image *image;
    struct venus_buffer *buffer;
    size_t buffer_size;
    int status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    image = venus_backend_find_image(backend, image_id);
    buffer = image
                 ? venus_backend_find_buffer(backend, image->buffer_id)
                 : NULL;
    if (!surface || !image || !buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return !surface ? VA_STATUS_ERROR_INVALID_SURFACE
                        : VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (surface->encode_pending) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surface->dma_backed) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (surface->fourcc != image->fourcc) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    if (src_x != 0 || src_y != 0 || dst_x != 0 || dst_y != 0 ||
        src_width != surface->width ||
        src_height != surface->height ||
        dst_width != surface->width ||
        dst_height != surface->height ||
        image->width != src_width ||
        image->height != src_height ||
        buffer->map_count > 0 ||
        buffer->element_size >
            SIZE_MAX / buffer->num_elements) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    buffer_size =
        buffer->element_size * buffer->num_elements;
    status = venus_surface_copy_from_yuv420(
        surface, buffer->data, buffer_size,
        image->stride, image->uv_offset,
        src_width, src_height);
    pthread_mutex_unlock(&backend->mutex);
    return status < 0
               ? venus_backend_status_from_errno(status)
               : VA_STATUS_SUCCESS;
}

static VAStatus backend_export_surface_handle(
    VADriverContextP context, VASurfaceID surface_id,
    uint32_t memory_type, uint32_t flags, void *descriptor)
{
    (void)context;
    (void)surface_id;
    (void)memory_type;
    (void)flags;
    (void)descriptor;
    /* Surfaces are CPU memory or read-only imports; nothing to export. */
    return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
}

static VAStatus backend_query_surface_attributes(
    VADriverContextP context, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_config *config;
    VASurfaceAttrib values[8];
    unsigned int required =
        (unsigned int)(sizeof(values) / sizeof(values[0]));

    if (!backend || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    config = venus_backend_find_config(backend, config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    values[0] = (VASurfaceAttrib) {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = config->profile == VAProfileHEVCMain10
                           ? VA_FOURCC_P010
                           : VA_FOURCC_NV12,
        },
    };
    values[1] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMemoryType,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        },
    };
    values[2] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMinWidth,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MIN_WIDTH,
        },
    };
    values[3] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMinHeight,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MIN_HEIGHT,
        },
    };
    values[4] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMaxWidth,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MAX_WIDTH,
        },
    };
    values[5] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMaxHeight,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MAX_HEIGHT,
        },
    };
    values[6] = (VASurfaceAttrib) {
        .type = VASurfaceAttribUsageHint,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC |
                       VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER,
        },
    };
    values[7] = (VASurfaceAttrib) {
        .type = VASurfaceAttribExternalBufferDescriptor,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypePointer,
            .value.p = NULL,
        },
    };

    if (!attributes) {
        *num_attributes = required;
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_SUCCESS;
    }
    if (*num_attributes < required) {
        *num_attributes = required;
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    memcpy(attributes, values, sizeof(values));
    *num_attributes = required;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

void venus_objects_destroy_all(struct venus_backend *backend)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++)
        memset(&backend->images[index], 0,
               sizeof(backend->images[index]));
    for (index = 0; index < VENUS_MAX_BUFFERS; index++)
        venus_backend_free_buffer(&backend->buffers[index]);
    for (index = 0; index < VENUS_MAX_SURFACES; index++)
        release_surface(&backend->surfaces[index]);
    memset(backend->configs, 0, sizeof(backend->configs));
}

void venus_objects_fill_vtable(struct VADriverVTable *vtable)
{
    vtable->vaCreateSurfaces = backend_create_surfaces;
    vtable->vaCreateSurfaces2 = backend_create_surfaces2;
    vtable->vaDestroySurfaces = backend_destroy_surfaces;
    vtable->vaCreateBuffer = backend_create_buffer;
    vtable->vaBufferSetNumElements = backend_set_num_elements;
    vtable->vaMapBuffer = backend_map_buffer;
    vtable->vaUnmapBuffer = backend_unmap_buffer;
    vtable->vaDestroyBuffer = backend_destroy_buffer;
    vtable->vaBufferInfo = backend_buffer_info;
    vtable->vaQueryImageFormats = backend_query_image_formats;
    vtable->vaCreateImage = backend_create_image;
    vtable->vaDeriveImage = backend_derive_image;
    vtable->vaDestroyImage = backend_destroy_image;
    vtable->vaGetImage = backend_get_image;
    vtable->vaPutImage = backend_put_image;
    vtable->vaQuerySurfaceAttributes =
        backend_query_surface_attributes;
    vtable->vaExportSurfaceHandle =
        backend_export_surface_handle;
}
