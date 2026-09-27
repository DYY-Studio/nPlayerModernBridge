/*
 * Subtitle-decode-boundary shim for the ffmpeg-subdecode unit.
 *
 * The unit carries FFmpeg 9.0.2 libavcodec for the app's subtitle decode path.
 * The app was built against 4.4.5 and reads AVCodecContext/AVCodecParameters/
 * AVSubtitle by offset, so every entry point that creates or consumes one of
 * those objects hands the app a legacy-shaped "shadow" and translates at the
 * boundary:
 *
 *   - the codec context and the codec parameters are allocated by this shim:
 *     the modern object is the real one, the app only ever sees the shadow;
 *   - avcodec_decode_subtitle2 fills a modern AVSubtitle and materialises a
 *     legacy-shaped one in the structure the app passed in;
 *   - avsubtitle_free is therefore also this shim's, because the internals it
 *     releases are the ones it allocated.
 *
 * Entries that only pass a value through (avcodec_find_decoder) forward
 * directly; the app never dereferences the AVCodec it returns, only hands it
 * back to the entries above (notes/ida-investigation.md section 7.3).
 *
 * The packet and dictionary calls of the same functions stay on 4.4.5: this
 * unit never hands the app a packet or a dictionary of its own, and the demux
 * unit already showed legacy-owned packets survive the app's own releases.
 *
 * Unimplemented entries trap rather than pass through: a half-written unit must
 * fail loudly, never decode with one foot on each ABI.
 */

#include "ffmpeg-subdec-abi.h"

#include <pthread.h>
#include <stdlib.h>

#define NPA_EXPORT __attribute__((visibility("default")))

/* A codec context this shim owns: the modern object plus the shadow the app
 * holds. Both are looked up by either pointer. */
typedef struct npa_ctx_shadow {
    AVCodecContext *modern;
    AVCodecContext *shadow;
    struct npa_ctx_shadow *next;
} npa_ctx_shadow;

/* A codec-parameters object this shim owns: same pairing. */
typedef struct npa_params_shadow {
    AVCodecParameters *modern;
    AVCodecParameters *shadow;
    struct npa_params_shadow *next;
} npa_params_shadow;

/* A subtitle this shim materialised into the structure the app passed in. The
 * app's structure is its own (a stack local), so only the mapping is kept. */
typedef struct npa_sub_shadow {
    AVSubtitle *app;
    AVSubtitle *modern;
    struct npa_sub_shadow *next;
} npa_sub_shadow;

static npa_ctx_shadow *g_ctx_shadows;
static npa_params_shadow *g_params_shadows;
static npa_sub_shadow *g_sub_shadows;
static pthread_mutex_t g_subdec_lock = PTHREAD_MUTEX_INITIALIZER;

/* The registrar and the lookups arrive with the translation they serve. */
static void npa_not_implemented(const char *entry)
{
    (void)entry;
    abort();
}

NPA_EXPORT const AVCodec *npa_subdec_avcodec_find_decoder(enum AVCodecID id)
{
    return avcodec_find_decoder(id);
}

NPA_EXPORT AVCodecContext *npa_subdec_avcodec_alloc_context3(const AVCodec *codec)
{
    (void)codec;
    npa_not_implemented("npa_subdec_avcodec_alloc_context3");
    return NULL;
}

NPA_EXPORT void npa_subdec_avcodec_free_context(AVCodecContext **pavctx)
{
    (void)pavctx;
    npa_not_implemented("npa_subdec_avcodec_free_context");
}

NPA_EXPORT int npa_subdec_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
                                        AVDictionary **options)
{
    (void)avctx;
    (void)codec;
    (void)options;
    npa_not_implemented("npa_subdec_avcodec_open2");
    return 0;
}

NPA_EXPORT AVCodecParameters *npa_subdec_avcodec_parameters_alloc(void)
{
    npa_not_implemented("npa_subdec_avcodec_parameters_alloc");
    return NULL;
}

NPA_EXPORT void npa_subdec_avcodec_parameters_free(AVCodecParameters **ppar)
{
    (void)ppar;
    npa_not_implemented("npa_subdec_avcodec_parameters_free");
}

NPA_EXPORT int npa_subdec_avcodec_parameters_from_context(AVCodecParameters *par,
                                                          const AVCodecContext *ctx)
{
    (void)par;
    (void)ctx;
    npa_not_implemented("npa_subdec_avcodec_parameters_from_context");
    return 0;
}

NPA_EXPORT int npa_subdec_avcodec_parameters_to_context(AVCodecContext *ctx,
                                                        const AVCodecParameters *par)
{
    (void)ctx;
    (void)par;
    npa_not_implemented("npa_subdec_avcodec_parameters_to_context");
    return 0;
}

NPA_EXPORT int npa_subdec_avcodec_decode_subtitle2(AVCodecContext *avctx, AVSubtitle *sub,
                                                   int *got_sub_ptr, const AVPacket *avpkt)
{
    (void)avctx;
    (void)sub;
    (void)got_sub_ptr;
    (void)avpkt;
    npa_not_implemented("npa_subdec_avcodec_decode_subtitle2");
    return 0;
}

NPA_EXPORT void npa_subdec_avsubtitle_free(AVSubtitle *sub)
{
    (void)sub;
    npa_not_implemented("npa_subdec_avsubtitle_free");
}
