/*
 * Thin legacy-name shims over the modern FFmpeg utility libraries.
 *
 * nPlayer 3.13.0 links the FFmpeg 4.4 scaler and resampler. Every function
 * below keeps the 4.4 prototype and forwards to the modern implementation so
 * the app's call sites stay untouched.
 *
 * One function has no modern counterpart: swr_alloc_set_opts() (the int64
 * channel-layout form) was removed in FFmpeg 6.0. It is rebuilt here from
 * av_channel_layout_from_mask() and swr_alloc_set_opts2(). Note that the 4.4
 * prototype returns the SwrContext pointer, not an error code, and that the
 * caller keeps the swr_init() responsibility.
 *
 * AVPixelFormat is the one argument that is not version-neutral: 4.4 still
 * numbers three members that modern FFmpeg removed, so the enum the app passes
 * and the enum the modern scaler reads disagree from AV_PIX_FMT_VAAPI on. The
 * swscale shims translate before they forward; see npa_modern_pixfmt(). The
 * shims that only move opaque contexts or raw planes (sws_alloc_context,
 * sws_scale, sws_freeContext, all swr_*) need no translation, and the other
 * enums that cross the boundary (AVSampleFormat, SWS_*) are identical in both
 * versions.
 */

#include <stdint.h>

#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define NPA_EXPORT __attribute__((visibility("default")))

/*
 * Legacy AVPixelFormat -> modern AVPixelFormat.
 *
 * FFmpeg 4.4 numbers three members that modern FFmpeg no longer has, and all
 * three sit inside the enum instead of at its end, so every later value moved:
 *
 *   legacy 44  AV_PIX_FMT_VAAPI_MOCO   \
 *   legacy 45  AV_PIX_FMT_VAAPI_IDCT    > dropped; modern 44 is AV_PIX_FMT_VAAPI
 *   legacy 46  AV_PIX_FMT_VAAPI        /
 *   legacy 153 AV_PIX_FMT_XVMC           dropped, no replacement
 *   legacy 198 AV_PIX_FMT_NB             one past the last 4.4 format
 *
 * Hence: 0..45 unchanged, 46..153 remapped to value-2, 154..197 to value-3.
 * Reading the member names out of the app's FFmpeg 4.4 headers is what pins
 * the numbers; there is no 4.4 header in this build to compute them from.
 *
 * The three dropped members themselves have no modern value, so they, an
 * unknown negative, and anything past the 4.4 enum all become
 * AV_PIX_FMT_NONE -- sws_getContext() then fails instead of scaling with a
 * format that means something else.
 *
 * Consequence if this is wrong: AV_PIX_FMT_P010LE (legacy 161) was read as
 * AV_PIX_FMT_GBRAP12LE, so P010/HEVC thumbnails rendered garbage or crashed
 * while 8-bit sources (NV12 23, YUV420P 0, both unchanged) stayed correct.
 */
enum {
    NPA_LEGACY_PIXFMT_VAAPI_MOCO = 44, /* dropped in modern FFmpeg */
    NPA_LEGACY_PIXFMT_VAAPI_IDCT = 45, /* dropped in modern FFmpeg */
    NPA_LEGACY_PIXFMT_VAAPI = 46,      /* first value after the dropped pair */
    NPA_LEGACY_PIXFMT_XVMC = 153,      /* dropped in modern FFmpeg */
    NPA_LEGACY_PIXFMT_NB = 198,        /* one past the last 4.4 format */
    NPA_LEGACY_DROPPED_BEFORE_VAAPI = 2,
    NPA_LEGACY_DROPPED_BEFORE_XVMC = 3
};

static enum AVPixelFormat npa_modern_pixfmt(int legacy)
{
    if (legacy == NPA_LEGACY_PIXFMT_VAAPI_MOCO
        || legacy == NPA_LEGACY_PIXFMT_VAAPI_IDCT
        || legacy == NPA_LEGACY_PIXFMT_XVMC) {
        return AV_PIX_FMT_NONE;
    }
    if (legacy < 0) {
        return AV_PIX_FMT_NONE;
    }
    if (legacy < NPA_LEGACY_PIXFMT_VAAPI) {
        return (enum AVPixelFormat)legacy;
    }
    if (legacy <= NPA_LEGACY_PIXFMT_XVMC) {
        return (enum AVPixelFormat)(legacy - NPA_LEGACY_DROPPED_BEFORE_VAAPI);
    }
    if (legacy < NPA_LEGACY_PIXFMT_NB) {
        return (enum AVPixelFormat)(legacy - NPA_LEGACY_DROPPED_BEFORE_XVMC);
    }
    return AV_PIX_FMT_NONE;
}

NPA_EXPORT SwsContext *npa_sws_alloc_context(void)
{
    return sws_alloc_context();
}

NPA_EXPORT SwsContext *npa_sws_getContext(
    int srcW,
    int srcH,
    int srcFormat,
    int dstW,
    int dstH,
    int dstFormat,
    int flags,
    SwsFilter *srcFilter,
    SwsFilter *dstFilter,
    const double *param
)
{
    return sws_getContext(
        srcW,
        srcH,
        npa_modern_pixfmt(srcFormat),
        dstW,
        dstH,
        npa_modern_pixfmt(dstFormat),
        flags,
        srcFilter,
        dstFilter,
        param
    );
}

NPA_EXPORT SwsContext *npa_sws_getCachedContext(
    SwsContext *context,
    int srcW,
    int srcH,
    int srcFormat,
    int dstW,
    int dstH,
    int dstFormat,
    int flags,
    SwsFilter *srcFilter,
    SwsFilter *dstFilter,
    const double *param
)
{
    return sws_getCachedContext(
        context,
        srcW,
        srcH,
        npa_modern_pixfmt(srcFormat),
        dstW,
        dstH,
        npa_modern_pixfmt(dstFormat),
        flags,
        srcFilter,
        dstFilter,
        param
    );
}

NPA_EXPORT int npa_sws_scale(
    SwsContext *context,
    const uint8_t *const srcSlice[],
    const int srcStride[],
    int srcSliceY,
    int srcSliceH,
    uint8_t *const dst[],
    const int dstStride[]
)
{
    return sws_scale(context, srcSlice, srcStride, srcSliceY, srcSliceH, dst, dstStride);
}

NPA_EXPORT void npa_sws_freeContext(SwsContext *context)
{
    sws_freeContext(context);
}

NPA_EXPORT struct SwrContext *npa_swr_alloc(void)
{
    return swr_alloc();
}

NPA_EXPORT int npa_swr_init(struct SwrContext *context)
{
    return swr_init(context);
}

NPA_EXPORT void npa_swr_free(struct SwrContext **context)
{
    /* FFmpeg 4.4 swr_free(): both redirected sites pass &ctx, not ctx. */
    swr_free(context);
}

NPA_EXPORT int npa_swr_convert(
    struct SwrContext *context,
    uint8_t *const *out,
    int outCount,
    const uint8_t *const *in,
    int inCount
)
{
    return swr_convert(context, out, outCount, in, inCount);
}

NPA_EXPORT int npa_swr_set_matrix(
    struct SwrContext *context,
    const double *matrix,
    int stride
)
{
    return swr_set_matrix(context, matrix, stride);
}

/*
 * Rebuild one channel layout from a legacy int64 mask. FFmpeg 4.4 accepted any
 * mask here and only failed later, inside swr_init(); the modern helper rejects
 * a zero mask outright, so a zero mask must leave the layout unset and let
 * swr_init() return EINVAL exactly as it used to.
 */
static void npa_layout_from_legacy_mask(AVChannelLayout *layout, int64_t mask)
{
    *layout = (AVChannelLayout){0};
    if (mask != 0 && av_channel_layout_from_mask(layout, (uint64_t)mask) < 0) {
        *layout = (AVChannelLayout){0};
    }
}

/*
 * The other direction: a modern layout as the 4.4 int64 mask. 4.4 could only
 * express a native mask, so a custom or ambisonic layout has no legacy value
 * and reports 0 - the state a 4.4 context was in when it knew only a channel
 * count. npa_ctx_out publishes the count next to it, so nothing is lost.
 */
static int64_t npa_legacy_mask_from_layout(const AVChannelLayout *layout)
{
    if (layout->order == AV_CHANNEL_ORDER_NATIVE)
        return (int64_t)layout->u.mask;
    return 0;
}

NPA_EXPORT struct SwrContext *npa_swr_alloc_set_opts(
    struct SwrContext *context,
    int64_t outChannelLayout,
    int outSampleFormat,
    int outSampleRate,
    int64_t inChannelLayout,
    int inSampleFormat,
    int inSampleRate,
    int logOffset,
    void *logContext
)
{
    AVChannelLayout out = {0};
    AVChannelLayout in = {0};

    npa_layout_from_legacy_mask(&out, outChannelLayout);
    npa_layout_from_legacy_mask(&in, inChannelLayout);
    if (swr_alloc_set_opts2(
            &context,
            &out,
            outSampleFormat,
            outSampleRate,
            &in,
            inSampleFormat,
            inSampleRate,
            logOffset,
            logContext
        ) < 0) {
        context = NULL;
    }
    av_channel_layout_uninit(&out);
    av_channel_layout_uninit(&in);
    return context;
}
