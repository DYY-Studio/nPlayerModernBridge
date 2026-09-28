#include "ffmpeg-core-abi.h"
#include "npa_highbit_copy.h"

#include <CoreVideo/CoreVideo.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>

#include <libavutil/pixfmt.h>

#define NPA_EXPORT __attribute__((visibility("default")))

NPA_ABI_ASSERT(AV_PIX_FMT_P010LE == 161, "legacy P010 pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV420P10LE == 64, "legacy YUV420P10LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV422P10LE == 66, "legacy YUV422P10LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV444P10LE == 70, "legacy YUV444P10LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV420P16LE == 47, "legacy YUV420P16LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV422P16LE == 49, "legacy YUV422P16LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV444P16LE == 51, "legacy YUV444P16LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV420P12LE == 125, "legacy YUV420P12LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV422P12LE == 129, "legacy YUV422P12LE pixel format changed");
NPA_ABI_ASSERT(AV_PIX_FMT_YUV444P12LE == 133, "legacy YUV444P12LE pixel format changed");

typedef struct {
    uint8_t opaque_00[0x18];
    int32_t width;
    int32_t height;
    uint8_t opaque_20[0x0c];
    int32_t pixel_format;
    uint8_t opaque_30[0x10];
    AVFrame *av_frame;
} NPAFFmpegVideoFrame;

NPA_ABI_ASSERT(offsetof(NPAFFmpegVideoFrame, width) == 0x18, "video-frame width moved");
NPA_ABI_ASSERT(offsetof(NPAFFmpegVideoFrame, height) == 0x1c, "video-frame height moved");
NPA_ABI_ASSERT(offsetof(NPAFFmpegVideoFrame, pixel_format) == 0x2c, "video-frame format moved");
NPA_ABI_ASSERT(offsetof(NPAFFmpegVideoFrame, av_frame) == 0x40, "video-frame AVFrame moved");

typedef CVPixelBufferRef (*NPAOriginalPixelBufferFn)(NPAFFmpegVideoFrame *, CVPixelBufferPoolRef);

#define NPA_RENDERER_POOL_COUNT 5

typedef struct {
    CVPixelBufferPoolRef pool;
    int width;
    int height;
    OSType pixel_format;
} NPAPixelBufferPoolCacheEntry;

typedef struct {
    OSType pixel_format;
    int horizontal_subsample;
    int vertical_subsample;
    int packed_p010;
} NPA10BitLayout;

typedef struct {
    OSType pixel_format;
    int horizontal_subsample;
    int vertical_subsample;
    int bit_depth;
} NPA16BitLayout;

static pthread_mutex_t npa_highbit_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static NPAPixelBufferPoolCacheEntry npa_highbit_pools[NPA_RENDERER_POOL_COUNT];

static void npa_highbit_log(const char *message, int status)
{
    fprintf(stderr, "[npa-highbit] %s (status=%d)\n", message, status);
}

static CVPixelBufferPoolRef npa_create_p010_pool(
    CVPixelBufferPoolRef source_pool,
    int width,
    int height,
    OSType pixel_format
)
{
    CFDictionaryRef source_attributes = source_pool
        ? CVPixelBufferPoolGetAttributes(source_pool)
        : NULL;
    CFMutableDictionaryRef attributes = source_attributes
        ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, source_attributes)
        : CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );
    if (!attributes) {
        return NULL;
    }

    int pixel_format_value = (int)pixel_format;
    CFNumberRef format_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format_value
    );
    CFNumberRef width_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &width
    );
    CFNumberRef height_number = CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt32Type, &height
    );
    CFMutableDictionaryRef io_surface = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    if (!format_number || !width_number || !height_number || !io_surface) {
        if (format_number) CFRelease(format_number);
        if (width_number) CFRelease(width_number);
        if (height_number) CFRelease(height_number);
        if (io_surface) CFRelease(io_surface);
        CFRelease(attributes);
        return NULL;
    }

    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, format_number);
    CFDictionarySetValue(attributes, kCVPixelBufferWidthKey, width_number);
    CFDictionarySetValue(attributes, kCVPixelBufferHeightKey, height_number);
    if (!CFDictionaryContainsKey(attributes, kCVPixelBufferIOSurfacePropertiesKey)) {
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface);
    }

    CVPixelBufferPoolRef result = NULL;
    CVReturn status = CVPixelBufferPoolCreate(
        kCFAllocatorDefault, NULL, attributes, &result
    );
    if (status != kCVReturnSuccess) {
        npa_highbit_log("high-bit-depth pixel-buffer pool creation failed", status);
    }

    CFRelease(io_surface);
    CFRelease(height_number);
    CFRelease(width_number);
    CFRelease(format_number);
    CFRelease(attributes);
    return result;
}

static CVPixelBufferPoolRef npa_get_p010_pool(
    CVPixelBufferPoolRef source_pool,
    int width,
    int height,
    OSType pixel_format
)
{
    NPAPixelBufferPoolCacheEntry *entry = NULL;
    pthread_mutex_lock(&npa_highbit_pool_mutex);
    for (size_t index = 0; index < NPA_RENDERER_POOL_COUNT; index++) {
        if (npa_highbit_pools[index].pixel_format == pixel_format) {
            entry = &npa_highbit_pools[index];
            break;
        }
        if (!entry && !npa_highbit_pools[index].pool) {
            entry = &npa_highbit_pools[index];
        }
    }
    if (!entry) {
        pthread_mutex_unlock(&npa_highbit_pool_mutex);
        npa_highbit_log("high-bit-depth pixel-buffer pool cache is full", -1);
        return NULL;
    }
    if (!entry->pool || entry->width != width || entry->height != height) {
        CVPixelBufferPoolRef replacement = npa_create_p010_pool(
            source_pool, width, height, pixel_format
        );
        if (!replacement) {
            pthread_mutex_unlock(&npa_highbit_pool_mutex);
            return NULL;
        }
        if (entry->pool) {
            CFRelease(entry->pool);
        }
        entry->pool = replacement;
        entry->width = width;
        entry->height = height;
        entry->pixel_format = pixel_format;
    }
    CVPixelBufferPoolRef result = (CVPixelBufferPoolRef)CFRetain(entry->pool);
    pthread_mutex_unlock(&npa_highbit_pool_mutex);
    return result;
}

static CFStringRef npa_frame_primaries(enum AVColorPrimaries value)
{
    switch (value) {
    case AVCOL_PRI_BT709: return kCVImageBufferColorPrimaries_ITU_R_709_2;
    case AVCOL_PRI_BT2020: return kCVImageBufferColorPrimaries_ITU_R_2020;
    default: return NULL;
    }
}

static CFStringRef npa_frame_transfer(enum AVColorTransferCharacteristic value)
{
    switch (value) {
    case AVCOL_TRC_BT709: return kCVImageBufferTransferFunction_ITU_R_709_2;
    case AVCOL_TRC_SMPTE2084: return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case AVCOL_TRC_ARIB_STD_B67: return kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    default: return NULL;
    }
}

static CFStringRef npa_frame_matrix(enum AVColorSpace value)
{
    switch (value) {
    case AVCOL_SPC_BT709: return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    default: return NULL;
    }
}

static void npa_attach_frame_colors(AVFrame *frame, CVPixelBufferRef pixel_buffer)
{
    CFStringRef keys[] = {
        kCVImageBufferColorPrimariesKey,
        kCVImageBufferTransferFunctionKey,
        kCVImageBufferYCbCrMatrixKey,
    };
    CFStringRef values[] = {
        npa_frame_primaries(frame->color_primaries),
        npa_frame_transfer(frame->color_trc),
        npa_frame_matrix(frame->colorspace),
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (values[index]) {
            CVBufferSetAttachment(
                pixel_buffer, keys[index], values[index],
                kCVAttachmentMode_ShouldPropagate
            );
        } else {
            CVBufferRemoveAttachment(pixel_buffer, keys[index]);
        }
    }
}

static int npa_10bit_layout(int pixel_format, NPA10BitLayout *layout)
{
    switch (pixel_format) {
    case AV_PIX_FMT_P010LE:
        *layout = (NPA10BitLayout){
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, 1, 1, 1
        };
        return 1;
    case AV_PIX_FMT_YUV420P10LE:
        *layout = (NPA10BitLayout){
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, 1, 1, 0
        };
        return 1;
    case AV_PIX_FMT_YUV422P10LE:
        *layout = (NPA10BitLayout){
            kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, 1, 0, 0
        };
        return 1;
    case AV_PIX_FMT_YUV444P10LE:
        *layout = (NPA10BitLayout){
            kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, 0, 0, 0
        };
        return 1;
    default:
        return 0;
    }
}

static int npa_16bit_layout(int pixel_format, NPA16BitLayout *layout)
{
    int bit_depth;
    int horizontal_subsample;
    int vertical_subsample;
    switch (pixel_format) {
    case AV_PIX_FMT_YUV420P12LE:
        bit_depth = 12; horizontal_subsample = 1; vertical_subsample = 1;
        break;
    case AV_PIX_FMT_YUV422P12LE:
        bit_depth = 12; horizontal_subsample = 1; vertical_subsample = 0;
        break;
    case AV_PIX_FMT_YUV444P12LE:
        bit_depth = 12; horizontal_subsample = 0; vertical_subsample = 0;
        break;
    case AV_PIX_FMT_YUV420P16LE:
        bit_depth = 16; horizontal_subsample = 1; vertical_subsample = 1;
        break;
    case AV_PIX_FMT_YUV422P16LE:
        bit_depth = 16; horizontal_subsample = 1; vertical_subsample = 0;
        break;
    case AV_PIX_FMT_YUV444P16LE:
        bit_depth = 16; horizontal_subsample = 0; vertical_subsample = 0;
        break;
    default:
        return 0;
    }
    *layout = (NPA16BitLayout){
        horizontal_subsample ? kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange
                             : kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange,
        horizontal_subsample,
        vertical_subsample,
        bit_depth,
    };
    return 1;
}

static CVPixelBufferRef npa_call_original_pixel_buffer(
    NPAFFmpegVideoFrame *frame,
    CVPixelBufferPoolRef pool
)
{
    const struct mach_header_64 *header =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE) {
        npa_highbit_log("main executable image is unavailable", -1);
        return NULL;
    }

    /* The manifest SHA pins this image to nPlayer 3.13.0. */
    uintptr_t address = (uintptr_t)header + (uintptr_t)(0x100A22F2C - 0x100000000);
    NPAOriginalPixelBufferFn original = (NPAOriginalPixelBufferFn)address;
    return original(frame, pool);
}

static CVPixelBufferRef npa_create_16bit_pixel_buffer(
    NPAFFmpegVideoFrame *frame,
    CVPixelBufferPoolRef source_pool,
    const NPA16BitLayout *layout
)
{
    AVFrame *av_frame = frame->av_frame;
    if (!av_frame || frame->width <= 0 || frame->height <= 0
        || !av_frame->data[0] || !av_frame->data[1] || !av_frame->data[2]) {
        npa_highbit_log("16-bit frame has invalid dimensions or planes", -1);
        return NULL;
    }

    CVPixelBufferPoolRef pool = npa_get_p010_pool(
        source_pool, frame->width, frame->height, layout->pixel_format
    );
    if (!pool) {
        npa_highbit_log("16-bit pixel-buffer pool is unavailable", -1);
        return NULL;
    }

    CVPixelBufferRef pixel_buffer = NULL;
    CVReturn status = CVPixelBufferPoolCreatePixelBuffer(
        kCFAllocatorDefault, pool, &pixel_buffer
    );
    CFRelease(pool);
    if (status != kCVReturnSuccess || !pixel_buffer) {
        npa_highbit_log("16-bit pixel-buffer allocation failed", status);
        return NULL;
    }

    status = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    if (status != kCVReturnSuccess) {
        npa_highbit_log("16-bit pixel-buffer lock failed", status);
        CFRelease(pixel_buffer);
        return NULL;
    }

    size_t chroma_width = ((size_t)frame->width
        + ((1U << layout->horizontal_subsample) - 1)) >> layout->horizontal_subsample;
    size_t chroma_height = (size_t)frame->height;
    int copied = -1;
    if (CVPixelBufferGetPixelFormatType(pixel_buffer) == layout->pixel_format
        && CVPixelBufferGetPlaneCount(pixel_buffer) == 2
        && CVPixelBufferGetWidth(pixel_buffer) == (size_t)frame->width
        && CVPixelBufferGetHeight(pixel_buffer) == (size_t)frame->height
        && CVPixelBufferGetWidthOfPlane(pixel_buffer, 0) == (size_t)frame->width
        && CVPixelBufferGetHeightOfPlane(pixel_buffer, 0) == (size_t)frame->height
        && CVPixelBufferGetWidthOfPlane(pixel_buffer, 1) == chroma_width
        && CVPixelBufferGetHeightOfPlane(pixel_buffer, 1) == chroma_height) {
        copied = npa_pack_yuvp16_to_biplanar16(
            av_frame->data[0], av_frame->data[1], av_frame->data[2],
            av_frame->linesize[0], av_frame->linesize[1], av_frame->linesize[2],
            CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0),
            CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1),
            (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0),
            (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1),
            frame->width, frame->height,
            layout->horizontal_subsample, layout->vertical_subsample,
            layout->bit_depth, av_frame->color_range == AVCOL_RANGE_JPEG
        );
    }
    if (copied != 0) {
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        npa_highbit_log("pixel-buffer layout or 16-bit plane conversion failed", copied);
        CFRelease(pixel_buffer);
        return NULL;
    }

    npa_attach_frame_colors(av_frame, pixel_buffer);
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    return pixel_buffer;
}

NPA_EXPORT CVPixelBufferRef npa_renderer_create_pixel_buffer(
    NPAFFmpegVideoFrame *frame,
    CVPixelBufferPoolRef source_pool
)
{
    if (!frame) {
        npa_highbit_log("renderer frame is null", -1);
        return NULL;
    }
    NPA16BitLayout layout16;
    if (npa_16bit_layout(frame->pixel_format, &layout16)) {
        return npa_create_16bit_pixel_buffer(frame, source_pool, &layout16);
    }
    NPA10BitLayout layout;
    if (!npa_10bit_layout(frame->pixel_format, &layout)) {
        npa_highbit_log("unsupported pixel format; calling the original renderer", frame->pixel_format);
        return npa_call_original_pixel_buffer(frame, source_pool);
    }
    AVFrame *av_frame = frame->av_frame;
    if (av_frame && av_frame->color_range == AVCOL_RANGE_JPEG && layout.packed_p010) {
        npa_highbit_log("full-range P010 cannot be represented by the x420 video-range pool", -1);
        return NULL;
    }
    if (!av_frame || frame->width <= 0 || frame->height <= 0
        || !av_frame->data[0] || !av_frame->data[1]
        || (!layout.packed_p010 && !av_frame->data[2])) {
        npa_highbit_log("10-bit frame has invalid dimensions or planes", -1);
        return NULL;
    }

    CVPixelBufferPoolRef pool = npa_get_p010_pool(
        source_pool, frame->width, frame->height, layout.pixel_format
    );
    if (!pool) {
        npa_highbit_log("10-bit pixel-buffer pool is unavailable", -1);
        return NULL;
    }

    CVPixelBufferRef pixel_buffer = NULL;
    CVReturn status = CVPixelBufferPoolCreatePixelBuffer(
        kCFAllocatorDefault, pool, &pixel_buffer
    );
    CFRelease(pool);
    if (status != kCVReturnSuccess || !pixel_buffer) {
        npa_highbit_log("10-bit pixel-buffer allocation failed", status);
        return NULL;
    }

    status = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    if (status != kCVReturnSuccess) {
        npa_highbit_log("10-bit pixel-buffer lock failed", status);
        CFRelease(pixel_buffer);
        return NULL;
    }

    int copied = -1;
    size_t expected_chroma_width = ((size_t)frame->width
        + ((1U << layout.horizontal_subsample) - 1)) >> layout.horizontal_subsample;
    size_t expected_chroma_height = ((size_t)frame->height
        + ((1U << layout.vertical_subsample) - 1)) >> layout.vertical_subsample;
    if (CVPixelBufferGetPixelFormatType(pixel_buffer) == layout.pixel_format
        && CVPixelBufferGetPlaneCount(pixel_buffer) == 2
        && CVPixelBufferGetWidth(pixel_buffer) == (size_t)frame->width
        && CVPixelBufferGetHeight(pixel_buffer) == (size_t)frame->height
        && CVPixelBufferGetWidthOfPlane(pixel_buffer, 0) == (size_t)frame->width
        && CVPixelBufferGetHeightOfPlane(pixel_buffer, 0) == (size_t)frame->height
        && CVPixelBufferGetWidthOfPlane(pixel_buffer, 1) == expected_chroma_width
        && CVPixelBufferGetHeightOfPlane(pixel_buffer, 1) == expected_chroma_height) {
        if (!layout.packed_p010) {
            copied = npa_pack_yuvp10_to_biplanar(
                av_frame->data[0], av_frame->data[1], av_frame->data[2],
                av_frame->linesize[0], av_frame->linesize[1], av_frame->linesize[2],
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0),
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1),
                (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0),
                (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1),
                frame->width, frame->height,
                layout.horizontal_subsample, layout.vertical_subsample,
                av_frame->color_range == AVCOL_RANGE_JPEG
            );
        } else {
            copied = npa_copy_p010_planes(
                av_frame->data[0], av_frame->data[1],
                av_frame->linesize[0], av_frame->linesize[1],
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0),
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1),
                (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0),
                (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1),
                frame->width, frame->height
            );
        }
    }
    if (copied != 0) {
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        npa_highbit_log("pixel-buffer layout or 10-bit plane conversion failed", copied);
        CFRelease(pixel_buffer);
        return NULL;
    }

    npa_attach_frame_colors(av_frame, pixel_buffer);
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    return pixel_buffer;
}
