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
 * Sources of a legacy object are normal, not a fallback: the demux unit's
 * codec parameters and, in the playback unit, the temporary source context
 * built by sub_100A8A4F8 are read by their frozen layout.
 *
 * Not synchronised in this unit, deliberately:
 *   - AVCodecContext.pix_fmt: the 4.4 and 9.0.2 enums are renumbered, and the
 *     subtitle path neither writes nor reads it. The playback unit owns that
 *     translation (spec section 3.3).
 *   - bits_per_coded_sample: 9.0.2 removed the field; the subtitle path does
 *     not use it either.
 *   - AVCodecContext.channel_layout on the way out: the modern side carries an
 *     AVChannelLayout and the reverse mapping is the playback unit's concern.
 *
 * Unimplemented entries trap rather than pass through: a half-written unit must
 * fail loudly, never decode with one foot on each ABI.
 */

#include "ffmpeg-subdec-abi.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/mem.h>

#define NPA_EXPORT __attribute__((visibility("default")))

/* Typed access to the frozen legacy layout. The offsets are all naturally
 * aligned, so the casts are safe. */
#define NPA_LD(ptr, off, type) (*(type *)((char *)(ptr) + (off)))
#define NPA_ST(ptr, off, type, value) (*(type *)((char *)(ptr) + (off)) = (value))

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

static void *npa_legacy_alloc(size_t size)
{
    return calloc(1, size);
}

/* ---- registries -------------------------------------------------------- */

static npa_ctx_shadow *npa_ctx_find(const void *ptr)
{
    npa_ctx_shadow *entry;

    if (!ptr)
        return NULL;
    pthread_mutex_lock(&g_subdec_lock);
    for (entry = g_ctx_shadows; entry; entry = entry->next)
        if (entry->modern == ptr || entry->shadow == ptr)
            break;
    pthread_mutex_unlock(&g_subdec_lock);
    return entry;
}

static void npa_ctx_add(AVCodecContext *modern, AVCodecContext *shadow)
{
    npa_ctx_shadow *entry = calloc(1, (size_t)sizeof(*entry));

    entry->modern = modern;
    entry->shadow = shadow;
    pthread_mutex_lock(&g_subdec_lock);
    entry->next = g_ctx_shadows;
    g_ctx_shadows = entry;
    pthread_mutex_unlock(&g_subdec_lock);
}

static void npa_ctx_remove(const void *shadow)
{
    npa_ctx_shadow **link;

    pthread_mutex_lock(&g_subdec_lock);
    for (link = &g_ctx_shadows; *link; link = &(*link)->next) {
        if ((*link)->shadow == shadow) {
            npa_ctx_shadow *doomed = *link;
            *link = doomed->next;
            free(doomed);
            break;
        }
    }
    pthread_mutex_unlock(&g_subdec_lock);
}

static npa_params_shadow *npa_params_find(const void *ptr)
{
    npa_params_shadow *entry;

    if (!ptr)
        return NULL;
    pthread_mutex_lock(&g_subdec_lock);
    for (entry = g_params_shadows; entry; entry = entry->next)
        if (entry->modern == ptr || entry->shadow == ptr)
            break;
    pthread_mutex_unlock(&g_subdec_lock);
    return entry;
}

static void npa_params_add(AVCodecParameters *modern, AVCodecParameters *shadow)
{
    npa_params_shadow *entry = calloc(1, (size_t)sizeof(*entry));

    entry->modern = modern;
    entry->shadow = shadow;
    pthread_mutex_lock(&g_subdec_lock);
    entry->next = g_params_shadows;
    g_params_shadows = entry;
    pthread_mutex_unlock(&g_subdec_lock);
}

static void npa_params_remove(const void *shadow)
{
    npa_params_shadow **link;

    pthread_mutex_lock(&g_subdec_lock);
    for (link = &g_params_shadows; *link; link = &(*link)->next) {
        if ((*link)->shadow == shadow) {
            npa_params_shadow *doomed = *link;
            *link = doomed->next;
            free(doomed);
            break;
        }
    }
    pthread_mutex_unlock(&g_subdec_lock);
}

static void npa_not_implemented(const char *entry)
{
    (void)entry;
    abort();
}

/* ---- legacy <-> modern ------------------------------------------------ */

/* The app's extradata belongs to the app; the modern object gets its own copy
 * so that 9.0.2 can release it with its own allocator. */
static void npa_ctx_set_extradata(AVCodecContext *modern, const void *data, int size)
{
    av_freep(&modern->extradata);
    modern->extradata_size = 0;
    if (size > 0 && data) {
        modern->extradata = av_memdup(data, (size_t)size);
        modern->extradata_size = modern->extradata ? size : 0;
    }
}

static void npa_ctx_in(AVCodecContext *modern, const void *shadow)
{
    int channels = NPA_LD(shadow, NPA_LEGACY_CTX_CHANNELS, int);
    int64_t layout = NPA_LD(shadow, NPA_LEGACY_CTX_CHANNEL_LAYOUT, int64_t);

    modern->codec_type = (enum AVMediaType)NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_TYPE, int);
    modern->codec_id = (enum AVCodecID)NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_ID, unsigned);
    modern->codec_tag = NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_TAG, unsigned);
    modern->bit_rate = NPA_LD(shadow, NPA_LEGACY_CTX_BIT_RATE, int64_t);
    modern->time_base = NPA_LD(shadow, NPA_LEGACY_CTX_TIME_BASE, AVRational);
    modern->width = NPA_LD(shadow, NPA_LEGACY_CTX_WIDTH, int);
    modern->height = NPA_LD(shadow, NPA_LEGACY_CTX_HEIGHT, int);
    modern->sample_aspect_ratio =
        NPA_LD(shadow, NPA_LEGACY_CTX_SAMPLE_ASPECT_RATIO, AVRational);
    modern->color_primaries =
        (enum AVColorPrimaries)NPA_LD(shadow, NPA_LEGACY_CTX_COLOR_PRIMARIES, int);
    modern->color_trc = (enum AVColorTransferCharacteristic)NPA_LD(
        shadow, NPA_LEGACY_CTX_COLOR_TRC, int);
    modern->colorspace = (enum AVColorSpace)NPA_LD(shadow, NPA_LEGACY_CTX_COLORSPACE, int);
    modern->sample_rate = NPA_LD(shadow, NPA_LEGACY_CTX_SAMPLE_RATE, int);
    modern->sample_fmt = (enum AVSampleFormat)NPA_LD(shadow, NPA_LEGACY_CTX_SAMPLE_FMT, int);
    modern->block_align = NPA_LD(shadow, NPA_LEGACY_CTX_BLOCK_ALIGN, int);
    modern->thread_count = NPA_LD(shadow, NPA_LEGACY_CTX_THREAD_COUNT, int);
    modern->thread_type = NPA_LD(shadow, NPA_LEGACY_CTX_THREAD_TYPE, int);
    modern->skip_loop_filter =
        (enum AVDiscard)NPA_LD(shadow, NPA_LEGACY_CTX_SKIP_LOOP_FILTER, int);
    modern->skip_idct = (enum AVDiscard)NPA_LD(shadow, NPA_LEGACY_CTX_SKIP_IDCT, int);
    modern->skip_frame = (enum AVDiscard)NPA_LD(shadow, NPA_LEGACY_CTX_SKIP_FRAME, int);
    modern->pkt_timebase = NPA_LD(shadow, NPA_LEGACY_CTX_PKT_TIMEBASE, AVRational);
    if (layout || channels) {
        npa_layout_from_legacy_mask(&modern->ch_layout, layout);
        if (modern->ch_layout.nb_channels == 0)
            modern->ch_layout.nb_channels = channels;
    }
    npa_ctx_set_extradata(
        modern,
        NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA, void *),
        NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA_SIZE, int));
}

static void npa_ctx_out(void *shadow, const AVCodecContext *modern)
{
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_TYPE, int, (int)modern->codec_type);
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_ID, unsigned, (unsigned)modern->codec_id);
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_TAG, unsigned, modern->codec_tag);
    NPA_ST(shadow, NPA_LEGACY_CTX_BIT_RATE, int64_t, modern->bit_rate);
    NPA_ST(shadow, NPA_LEGACY_CTX_TIME_BASE, AVRational, modern->time_base);
    NPA_ST(shadow, NPA_LEGACY_CTX_WIDTH, int, modern->width);
    NPA_ST(shadow, NPA_LEGACY_CTX_HEIGHT, int, modern->height);
    NPA_ST(shadow, NPA_LEGACY_CTX_SAMPLE_ASPECT_RATIO, AVRational,
           modern->sample_aspect_ratio);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLOR_PRIMARIES, int, (int)modern->color_primaries);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLOR_TRC, int, (int)modern->color_trc);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLORSPACE, int, (int)modern->colorspace);
    NPA_ST(shadow, NPA_LEGACY_CTX_SAMPLE_RATE, int, modern->sample_rate);
    NPA_ST(shadow, NPA_LEGACY_CTX_CHANNELS, int, modern->ch_layout.nb_channels);
    NPA_ST(shadow, NPA_LEGACY_CTX_SAMPLE_FMT, int, (int)modern->sample_fmt);
    NPA_ST(shadow, NPA_LEGACY_CTX_BLOCK_ALIGN, int, modern->block_align);
    NPA_ST(shadow, NPA_LEGACY_CTX_THREAD_COUNT, int, modern->thread_count);
    NPA_ST(shadow, NPA_LEGACY_CTX_THREAD_TYPE, int, modern->thread_type);
    NPA_ST(shadow, NPA_LEGACY_CTX_SKIP_LOOP_FILTER, int, (int)modern->skip_loop_filter);
    NPA_ST(shadow, NPA_LEGACY_CTX_SKIP_IDCT, int, (int)modern->skip_idct);
    NPA_ST(shadow, NPA_LEGACY_CTX_SKIP_FRAME, int, (int)modern->skip_frame);
    NPA_ST(shadow, NPA_LEGACY_CTX_PKT_TIMEBASE, AVRational, modern->pkt_timebase);
    NPA_ST(shadow, NPA_LEGACY_CTX_SUBTITLE_HEADER, void *, modern->subtitle_header);
    NPA_ST(shadow, NPA_LEGACY_CTX_SUBTITLE_HEADER_SIZE, int, modern->subtitle_header_size);
}

static void npa_params_in(AVCodecParameters *modern, const void *shadow)
{
    int channels = NPA_LD(shadow, NPA_LEGACY_PARAMS_CHANNELS, int);
    int64_t layout = NPA_LD(shadow, NPA_LEGACY_PARAMS_CHANNEL_LAYOUT, int64_t);

    modern->codec_type = (enum AVMediaType)NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_TYPE, int);
    modern->codec_id = (enum AVCodecID)NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_ID, unsigned);
    modern->codec_tag = NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_TAG, unsigned);
    modern->format = NPA_LD(shadow, NPA_LEGACY_PARAMS_FORMAT, int);
    modern->bit_rate = NPA_LD(shadow, NPA_LEGACY_PARAMS_BIT_RATE, int64_t);
    modern->sample_rate = NPA_LD(shadow, NPA_LEGACY_PARAMS_SAMPLE_RATE, int);
    if (layout || channels) {
        npa_layout_from_legacy_mask(&modern->ch_layout, layout);
        if (modern->ch_layout.nb_channels == 0)
            modern->ch_layout.nb_channels = channels;
    }
    av_freep(&modern->extradata);
    modern->extradata_size = 0;
    {
        int size = NPA_LD(shadow, NPA_LEGACY_PARAMS_EXTRADATA_SIZE, int);
        void *data = NPA_LD(shadow, NPA_LEGACY_PARAMS_EXTRADATA, void *);
        if (size > 0 && data) {
            modern->extradata = av_memdup(data, (size_t)size);
            modern->extradata_size = modern->extradata ? size : 0;
        }
    }
}

static void npa_params_out(void *shadow, const AVCodecParameters *modern)
{
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_TYPE, int, (int)modern->codec_type);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_ID, unsigned, (unsigned)modern->codec_id);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_TAG, unsigned, modern->codec_tag);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA, void *, modern->extradata);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA_SIZE, int, modern->extradata_size);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_FORMAT, int, modern->format);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_BIT_RATE, int64_t, modern->bit_rate);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CHANNELS, int, modern->ch_layout.nb_channels);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_SAMPLE_RATE, int, modern->sample_rate);
}

/* The modern object behind an app-visible codec context. A pointer this shim
 * did not allocate is a legacy object built by 4.4.5 code; reading it by its
 * frozen layout is how the source contexts arrive, so it is turned into a
 * temporary modern context the caller releases. */
static AVCodecContext *npa_ctx_of(void *ptr, AVCodecContext **temp)
{
    npa_ctx_shadow *entry = npa_ctx_find(ptr);

    *temp = NULL;
    if (entry)
        return entry->modern;
    if (!ptr)
        abort();
    *temp = avcodec_alloc_context3(NULL);
    if (!*temp)
        abort();
    npa_ctx_in(*temp, ptr);
    return *temp;
}

static AVCodecParameters *npa_params_of(void *ptr, AVCodecParameters **temp)
{
    npa_params_shadow *entry = npa_params_find(ptr);

    *temp = NULL;
    if (entry)
        return entry->modern;
    if (!ptr)
        abort();
    *temp = avcodec_parameters_alloc();
    if (!*temp)
        abort();
    npa_params_in(*temp, ptr);
    return *temp;
}

static void npa_ctx_require_own(AVCodecContext *avctx)
{
    if (!npa_ctx_find(avctx))
        abort();
}

/* ---- entry points ----------------------------------------------------- */

NPA_EXPORT const AVCodec *npa_subdec_avcodec_find_decoder(enum AVCodecID id)
{
    return avcodec_find_decoder(id);
}

NPA_EXPORT AVCodecContext *npa_subdec_avcodec_alloc_context3(const AVCodec *codec)
{
    AVCodecContext *modern = avcodec_alloc_context3(codec);
    AVCodecContext *shadow;

    if (!modern)
        return NULL;
    shadow = npa_legacy_alloc(NPA_LEGACY_CTX_SIZE);
    if (!shadow) {
        avcodec_free_context(&modern);
        return NULL;
    }
    npa_ctx_add(modern, shadow);
    npa_ctx_out(shadow, modern);
    return shadow;
}

NPA_EXPORT void npa_subdec_avcodec_free_context(AVCodecContext **pavctx)
{
    AVCodecContext *shadow;

    if (!pavctx)
        return;
    shadow = *pavctx;
    if (!shadow) {
        avcodec_free_context(pavctx);
        return;
    }
    npa_ctx_require_own(shadow);
    {
        npa_ctx_shadow *entry = npa_ctx_find(shadow);
        AVCodecContext *modern = entry->modern;
        npa_ctx_remove(shadow);
        avcodec_free_context(&modern);
    }
    free(shadow);
    *pavctx = NULL;
}

NPA_EXPORT int npa_subdec_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
                                        AVDictionary **options)
{
    npa_ctx_shadow *entry = npa_ctx_find(avctx);
    int ret;

    if (!entry)
        abort();
    /* The app does not use FFmpeg's hardware acceleration; a context carrying
     * hardware fields would have to be translated, not ignored. */
    if (NPA_LD(avctx, NPA_LEGACY_CTX_HW_DEVICE_CTX, void *) ||
        NPA_LD(avctx, NPA_LEGACY_CTX_HW_FRAMES_CTX, void *) ||
        NPA_LD(avctx, NPA_LEGACY_CTX_GET_FORMAT, void *))
        abort();
    npa_ctx_in(entry->modern, avctx);
    ret = avcodec_open2(entry->modern, codec, options);
    npa_ctx_out(avctx, entry->modern);
    return ret;
}

NPA_EXPORT AVCodecParameters *npa_subdec_avcodec_parameters_alloc(void)
{
    AVCodecParameters *modern = avcodec_parameters_alloc();
    AVCodecParameters *shadow;

    if (!modern)
        return NULL;
    shadow = npa_legacy_alloc(NPA_LEGACY_PARAMS_SIZE);
    if (!shadow) {
        avcodec_parameters_free(&modern);
        return NULL;
    }
    npa_params_add(modern, shadow);
    npa_params_out(shadow, modern);
    return shadow;
}

NPA_EXPORT void npa_subdec_avcodec_parameters_free(AVCodecParameters **ppar)
{
    AVCodecParameters *shadow;

    if (!ppar)
        return;
    shadow = *ppar;
    if (!shadow) {
        avcodec_parameters_free(ppar);
        return;
    }
    {
        npa_params_shadow *entry = npa_params_find(shadow);
        AVCodecParameters *modern;
        if (!entry)
            abort();
        modern = entry->modern;
        npa_params_remove(shadow);
        avcodec_parameters_free(&modern);
    }
    free(shadow);
    *ppar = NULL;
}

NPA_EXPORT int npa_subdec_avcodec_parameters_from_context(AVCodecParameters *par,
                                                          const AVCodecContext *ctx)
{
    npa_params_shadow *entry = npa_params_find(par);
    AVCodecContext *temp = NULL;
    AVCodecContext *modern_ctx;
    int ret;

    if (!entry)
        abort();
    modern_ctx = npa_ctx_of((void *)ctx, &temp);
    ret = avcodec_parameters_from_context(entry->modern, modern_ctx);
    if (ret >= 0)
        npa_params_out(entry->shadow, entry->modern);
    if (temp)
        avcodec_free_context(&temp);
    return ret;
}

NPA_EXPORT int npa_subdec_avcodec_parameters_to_context(AVCodecContext *ctx,
                                                        const AVCodecParameters *par)
{
    npa_ctx_shadow *entry = npa_ctx_find(ctx);
    AVCodecParameters *temp = NULL;
    AVCodecParameters *modern_par;
    int ret;

    if (!entry)
        abort();
    modern_par = npa_params_of((void *)par, &temp);
    ret = avcodec_parameters_to_context(entry->modern, modern_par);
    if (ret >= 0)
        npa_ctx_out(entry->shadow, entry->modern);
    if (temp)
        avcodec_parameters_free(&temp);
    return ret;
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
