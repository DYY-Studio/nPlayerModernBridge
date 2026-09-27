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
 * The second unit on this shim is the playback/probe decoder face. It shares
 * the context and parameter entries and adds the frame side: send_packet hands
 * the app's stack-local packet to 9.0.2, receive_frame materialises the modern
 * frame into the app's own 4.4 frame with legacy-owned buffers, and close and
 * flush_buffers follow 4.4's observable behaviour (9.0.2 removed avcodec_close).
 * Because it carries real video parameters, format values are translated in
 * both directions here - AVCodecContext.pix_fmt, AVCodecParameters.format and
 * AVFrame.format are 4.4 values on the app side (spec section 3.3), while
 * AVSampleFormat is the same enum in both versions.
 *
 * Deliberately not carried across (both units):
 *   - bits_per_coded_sample: 9.0.2 dropped the context field; nothing in these
 *     two faces reads or writes it.
 *   - frame metadata, frame side data, pkt_pos/pkt_size and reordered_opaque:
 *     9.0.2 dropped some of these fields and the app reads none of them; the
 *     side data stays on the modern frame and is released with it.
 *   - AVCodecContext fields 9.0.2 added (alpha_mode, coded_side_data and the
 *     like): no 4.4 counterpart, so there is nothing to publish.
 *
 * A pointer arriving that this shim did not allocate is either a legacy object
 * built by 4.4.5 code (read by its frozen layout) or a misuse: every entry
 * aborts rather than decoding with one foot on each ABI.
 */

#include "ffmpeg-subdec-abi.h"
#include "ffmpeg-demux-enum-map.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/mem.h>
#include <libavutil/avstring.h>
#include <libavutil/mathematics.h>

#define NPA_EXPORT __attribute__((visibility("default")))

/* Typed access to the frozen legacy layout. The offsets are all naturally
 * aligned, so the casts are safe. */
#define NPA_LD(ptr, off, type) (*(type *)((char *)(ptr) + (off)))
#define NPA_ST(ptr, off, type, value) (*(type *)((char *)(ptr) + (off)) = (value))

/* The generated map in ffmpeg-demux-enum-map.h goes modern -> legacy, which is
 * the direction every shadow needs. These entries also have to go the other
 * way, because the app hands this shim its own 4.4.x codec id. */
static int npa_subdec_modern_codec_id(int legacy)
{
    unsigned i;

    if (legacy < 0)
        return legacy;
    for (i = 0; i < sizeof(npa_codec_id_map) / sizeof(npa_codec_id_map[0]); i++)
        if (npa_codec_id_map[i].legacy == legacy)
            return npa_codec_id_map[i].modern;
    return legacy;
}

/* A codec context this shim owns: the modern object plus the shadow the app
 * holds. Both are looked up by either pointer. */
typedef struct npa_ctx_shadow {
    AVCodecContext *modern;
    AVCodecContext *shadow;
    int closed; /* avcodec_close has no 9.0.2 counterpart; see npa_codec_close */
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

/* ---- legacy <-> modern ------------------------------------------------ */

/* ---- AV1 extradata: the two shapes ------------------------------------ */

/*
 * 4.4.x's matroska demuxer published only the OBUs as AV1 extradata (it skips
 * the AV1CodecConfigurationRecord on purpose; see the demux unit), and the app
 * feeds those bytes to VideoToolbox, so the app side must keep seeing that
 * shape. 9.0.2's decoders want the record back: libdav1d rejects anything whose
 * first byte is not marker+version 0x81 ("Missing AV1 codec configuration
 * record"), and both it and the old native decoder read the config OBUs that
 * follow. So a record header goes on when the bytes enter the modern world and
 * comes off when they leave it.
 *
 * The header carries seq_profile/seq_level_idx_0/seq_tier_0/high_bitdepth/
 * twelve_bit/monochrome/chroma_subsampling_*, which the caller already knows
 * (profile, level, pixel format), so only chroma_sample_position is left at
 * "unknown" - the record's copy of these fields is informational, the decoders
 * read the real ones from the sequence header OBU that follows.
 */
#define NPA_AV1_RECORD_HEADER 4

static int npa_av1_record_size(const void *data, int size)
{
    const uint8_t *p = data;

    if (!p || size < NPA_AV1_RECORD_HEADER)
        return 0;
    if (!(p[0] & 0x80) || (p[0] & 0x7F) != 1)
        return 0;
    return size;
}

static void npa_av1_record_bytes(uint8_t *out, int profile, int level, int format)
{
    int high_bitdepth = 0, twelve_bit = 0, monochrome = 0, sub_x = 1, sub_y = 1;

    switch (format) {
    case AV_PIX_FMT_GRAY8:
        monochrome = 1;
        break;
    case AV_PIX_FMT_GRAY10:
        monochrome = 1;
        high_bitdepth = 1;
        break;
    case AV_PIX_FMT_GRAY12:
        monochrome = 1;
        high_bitdepth = 1;
        twelve_bit = 1;
        break;
    case AV_PIX_FMT_YUV420P10:
        high_bitdepth = 1;
        break;
    case AV_PIX_FMT_YUV420P12:
        high_bitdepth = 1;
        twelve_bit = 1;
        break;
    case AV_PIX_FMT_YUV422P:
        sub_y = 0;
        break;
    case AV_PIX_FMT_YUV422P10:
        high_bitdepth = 1;
        sub_y = 0;
        break;
    case AV_PIX_FMT_YUV422P12:
        high_bitdepth = 1;
        twelve_bit = 1;
        sub_y = 0;
        break;
    case AV_PIX_FMT_YUV444P:
        sub_x = 0;
        sub_y = 0;
        break;
    case AV_PIX_FMT_YUV444P10:
        high_bitdepth = 1;
        sub_x = 0;
        sub_y = 0;
        break;
    case AV_PIX_FMT_YUV444P12:
        high_bitdepth = 1;
        twelve_bit = 1;
        sub_x = 0;
        sub_y = 0;
        break;
    default:
        break; /* 8-bit 4:2:0, the common case */
    }
    out[0] = 0x81;
    out[1] = (uint8_t)(((profile & 0x07) << 5) | (level & 0x1F));
    out[2] = (uint8_t)((high_bitdepth << 6) | (twelve_bit << 5) | (monochrome << 4) |
                       (sub_x << 3) | (sub_y << 2));
    out[3] = 0;
}

/* AV1 bytes on their way into the modern world: a record, if they are not one
 * already. The result is owned by the caller (av_free). */
static void *npa_av1_extradata_modern(const void *data, int size, int profile, int level,
                                      int format, int *out_size)
{
    void *out;

    if (!data || size <= 0) {
        *out_size = 0;
        return NULL;
    }
    if (npa_av1_record_size(data, size)) {
        *out_size = size;
        return av_memdup(data, (size_t)size);
    }
    out = av_malloc((size_t)size + NPA_AV1_RECORD_HEADER);
    if (!out)
        abort();
    npa_av1_record_bytes(out, profile, level, format);
    memcpy((char *)out + NPA_AV1_RECORD_HEADER, data, (size_t)size);
    *out_size = size + NPA_AV1_RECORD_HEADER;
    return out;
}

/* The other direction, for publishing into the shadow: the record header comes
 * off and the result points into the same buffer. */
static const void *npa_av1_extradata_legacy(const void *data, int size, int *out_size)
{
    if (npa_av1_record_size(data, size)) {
        *out_size = size - NPA_AV1_RECORD_HEADER;
        return (const char *)data + NPA_AV1_RECORD_HEADER;
    }
    *out_size = size;
    return data;
}

/* The app's extradata belongs to the app; the modern object gets its own copy
 * so that 9.0.2 can release it with its own allocator. */
static void npa_ctx_set_extradata_owned(AVCodecContext *modern, void *data, int size)
{
    av_freep(&modern->extradata);
    modern->extradata = data;
    modern->extradata_size = data ? size : 0;
}

static void npa_ctx_set_extradata(AVCodecContext *modern, const void *data, int size)
{
    void *copy = NULL;

    /* npa_ctx_out publishes this buffer into the shadow, so a shadow that was
     * read back may alias it: copy before releasing the old one. */
    if (size > 0 && data)
        copy = av_memdup(data, (size_t)size);
    npa_ctx_set_extradata_owned(modern, copy, copy ? size : 0);
}

static void npa_ctx_in(AVCodecContext *modern, const void *shadow)
{
    int channels = NPA_LD(shadow, NPA_LEGACY_CTX_CHANNELS, int);
    int64_t layout = NPA_LD(shadow, NPA_LEGACY_CTX_CHANNEL_LAYOUT, int64_t);

    modern->codec_type = (enum AVMediaType)NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_TYPE, int);
    modern->codec_id = (enum AVCodecID)npa_subdec_modern_codec_id(
        (int)NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_ID, unsigned));
    modern->codec_tag = NPA_LD(shadow, NPA_LEGACY_CTX_CODEC_TAG, unsigned);
    modern->bit_rate = NPA_LD(shadow, NPA_LEGACY_CTX_BIT_RATE, int64_t);
    modern->time_base = NPA_LD(shadow, NPA_LEGACY_CTX_TIME_BASE, AVRational);
    modern->width = NPA_LD(shadow, NPA_LEGACY_CTX_WIDTH, int);
    modern->height = NPA_LD(shadow, NPA_LEGACY_CTX_HEIGHT, int);
    /* The app's pixel formats are 4.4 values - its own format table
     * (0x1016A58B8) is 4.4-numbered - so the modern decoder needs the
     * translated one. The app never writes this on a shim-owned context, but
     * the temporary 4.4.5 source context carries it, and this is the only way
     * the video open path learns the pixel format. */
    modern->pix_fmt = npa_modern_pixfmt(NPA_LD(shadow, NPA_LEGACY_CTX_PIX_FMT, int));
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
    if (modern->codec_id == AV_CODEC_ID_AV1) {
        int modern_size = 0;
        void *modern_extra = npa_av1_extradata_modern(
            NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA, void *),
            NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA_SIZE, int), 0, 0,
            npa_modern_pixfmt(NPA_LD(shadow, NPA_LEGACY_CTX_PIX_FMT, int)), &modern_size);

        npa_ctx_set_extradata_owned(modern, modern_extra, modern_size);
    } else {
        npa_ctx_set_extradata(
            modern,
            NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA, void *),
            NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA_SIZE, int));
    }
}

static void npa_ctx_out(void *shadow, const AVCodecContext *modern)
{
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_TYPE, int, (int)modern->codec_type);
    /*
     * The app reads codec before it decodes anything - sub_100A04BCC bails out
     * when it is NULL (0x100A04D00) - and avcodec_open2 is the only thing that
     * ever fills it. The app also reads extradata right after opening a
     * subtitle decoder to seed libass (0x100A047F8), so both have to be
     * published here; the shadow aliases the modern buffers, which is safe
     * because the in-bound copy takes its own copy first.
     */
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC, void *, (void *)(uintptr_t)modern->codec);
    {
        const void *extra = modern->extradata;
        int extra_size = modern->extradata_size;

        if (modern->codec_id == AV_CODEC_ID_AV1)
            extra = npa_av1_extradata_legacy(extra, extra_size, &extra_size);
        NPA_ST(shadow, NPA_LEGACY_CTX_EXTRADATA, void *, (void *)(uintptr_t)extra);
        NPA_ST(shadow, NPA_LEGACY_CTX_EXTRADATA_SIZE, int, extra_size);
    }
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_ID, unsigned,
           (unsigned)npa_codec_id_to_legacy((int)modern->codec_id));
    NPA_ST(shadow, NPA_LEGACY_CTX_CODEC_TAG, unsigned, modern->codec_tag);
    NPA_ST(shadow, NPA_LEGACY_CTX_BIT_RATE, int64_t, modern->bit_rate);
    NPA_ST(shadow, NPA_LEGACY_CTX_TIME_BASE, AVRational, modern->time_base);
    NPA_ST(shadow, NPA_LEGACY_CTX_WIDTH, int, modern->width);
    NPA_ST(shadow, NPA_LEGACY_CTX_HEIGHT, int, modern->height);
    /* Format values cross in the legacy direction (spec section 3.3): the app
     * hands whatever it reads here straight to sws_getContext and to its own
     * 4.4-numbered format table. */
    NPA_ST(shadow, NPA_LEGACY_CTX_PIX_FMT, int,
           npa_pix_fmt_to_legacy((int)modern->pix_fmt));
    NPA_ST(shadow, NPA_LEGACY_CTX_SAMPLE_ASPECT_RATIO, AVRational,
           modern->sample_aspect_ratio);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLOR_PRIMARIES, int, (int)modern->color_primaries);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLOR_TRC, int, (int)modern->color_trc);
    NPA_ST(shadow, NPA_LEGACY_CTX_COLORSPACE, int, (int)modern->colorspace);
    NPA_ST(shadow, NPA_LEGACY_CTX_SAMPLE_RATE, int, modern->sample_rate);
    NPA_ST(shadow, NPA_LEGACY_CTX_CHANNELS, int, modern->ch_layout.nb_channels);
    /* The audio frame consumer reads this (sub_100A89A28 @0x100A89CA0) and
     * falls back to av_get_default_channel_layout when it is zero, so the
     * round trip through the shadow has to carry the real mask. */
    NPA_ST(shadow, NPA_LEGACY_CTX_CHANNEL_LAYOUT, int64_t,
           npa_legacy_mask_from_layout(&modern->ch_layout));
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
    int type = NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_TYPE, int);

    modern->codec_type = (enum AVMediaType)type;
    modern->codec_id = (enum AVCodecID)npa_subdec_modern_codec_id(
        (int)NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_ID, unsigned));
    modern->codec_tag = NPA_LD(shadow, NPA_LEGACY_PARAMS_CODEC_TAG, unsigned);
    modern->bit_rate = NPA_LD(shadow, NPA_LEGACY_PARAMS_BIT_RATE, int64_t);
    modern->bits_per_coded_sample =
        NPA_LD(shadow, NPA_LEGACY_PARAMS_BITS_PER_CODED_SAMPLE, int);
    modern->bits_per_raw_sample =
        NPA_LD(shadow, NPA_LEGACY_PARAMS_BITS_PER_RAW_SAMPLE, int);
    modern->profile = NPA_LD(shadow, NPA_LEGACY_PARAMS_PROFILE, int);
    modern->level = NPA_LD(shadow, NPA_LEGACY_PARAMS_LEVEL, int);
    /*
     * The playback unit carries real video and audio parameters through the
     * app: the video open path is source context -> parameters_from_context ->
     * parameters_to_context -> decoder context, and every hop goes through this
     * translation. A field left out here is not "unused", it is a value the
     * decoder never sees.
     */
    switch (type) {
    case AVMEDIA_TYPE_VIDEO:
        modern->format = npa_modern_pixfmt(NPA_LD(shadow, NPA_LEGACY_PARAMS_FORMAT, int));
        modern->width = NPA_LD(shadow, NPA_LEGACY_PARAMS_WIDTH, int);
        modern->height = NPA_LD(shadow, NPA_LEGACY_PARAMS_HEIGHT, int);
        modern->sample_aspect_ratio =
            NPA_LD(shadow, NPA_LEGACY_PARAMS_SAMPLE_ASPECT_RATIO, AVRational);
        modern->field_order =
            (enum AVFieldOrder)NPA_LD(shadow, NPA_LEGACY_PARAMS_FIELD_ORDER, int);
        modern->color_range =
            (enum AVColorRange)NPA_LD(shadow, NPA_LEGACY_PARAMS_COLOR_RANGE, int);
        modern->color_primaries =
            (enum AVColorPrimaries)NPA_LD(shadow, NPA_LEGACY_PARAMS_COLOR_PRIMARIES, int);
        modern->color_trc =
            (enum AVColorTransferCharacteristic)NPA_LD(shadow, NPA_LEGACY_PARAMS_COLOR_TRC, int);
        modern->color_space =
            (enum AVColorSpace)NPA_LD(shadow, NPA_LEGACY_PARAMS_COLORSPACE, int);
        modern->chroma_location =
            (enum AVChromaLocation)NPA_LD(shadow, NPA_LEGACY_PARAMS_CHROMA_LOCATION, int);
        modern->video_delay = NPA_LD(shadow, NPA_LEGACY_PARAMS_VIDEO_DELAY, int);
        break;
    case AVMEDIA_TYPE_AUDIO:
        /* AVSampleFormat is numbered the same in both versions. */
        modern->format = NPA_LD(shadow, NPA_LEGACY_PARAMS_FORMAT, int);
        modern->sample_rate = NPA_LD(shadow, NPA_LEGACY_PARAMS_SAMPLE_RATE, int);
        if (layout || channels) {
            npa_layout_from_legacy_mask(&modern->ch_layout, layout);
            if (modern->ch_layout.nb_channels == 0)
                modern->ch_layout.nb_channels = channels;
        }
        modern->block_align = NPA_LD(shadow, NPA_LEGACY_PARAMS_BLOCK_ALIGN, int);
        modern->frame_size = NPA_LD(shadow, NPA_LEGACY_PARAMS_FRAME_SIZE, int);
        modern->initial_padding = NPA_LD(shadow, NPA_LEGACY_PARAMS_INITIAL_PADDING, int);
        modern->trailing_padding = NPA_LD(shadow, NPA_LEGACY_PARAMS_TRAILING_PADDING, int);
        modern->seek_preroll = NPA_LD(shadow, NPA_LEGACY_PARAMS_SEEK_PREROLL, int);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        /* 4.4's avcodec_parameters_to_context copies the stored dimensions for
         * subtitles too; the format field is unused for them. */
        modern->format = NPA_LD(shadow, NPA_LEGACY_PARAMS_FORMAT, int);
        modern->width = NPA_LD(shadow, NPA_LEGACY_PARAMS_WIDTH, int);
        modern->height = NPA_LD(shadow, NPA_LEGACY_PARAMS_HEIGHT, int);
        break;
    default:
        modern->format = NPA_LD(shadow, NPA_LEGACY_PARAMS_FORMAT, int);
        break;
    }
    {
        int size = NPA_LD(shadow, NPA_LEGACY_PARAMS_EXTRADATA_SIZE, int);
        void *data = NPA_LD(shadow, NPA_LEGACY_PARAMS_EXTRADATA, void *);

        if (modern->codec_id == AV_CODEC_ID_AV1) {
            int modern_size = 0;
            void *modern_extra = npa_av1_extradata_modern(
                data, size, modern->profile, modern->level, modern->format, &modern_size);

            av_freep(&modern->extradata);
            modern->extradata = modern_extra;
            modern->extradata_size = modern_extra ? modern_size : 0;
        } else {
            void *copy = NULL;

            /* npa_params_out aliases this buffer into the shadow, so the copy
             * has to be taken before the previous one is released. */
            if (size > 0 && data)
                copy = av_memdup(data, (size_t)size);
            av_freep(&modern->extradata);
            modern->extradata = copy;
            modern->extradata_size = copy ? size : 0;
        }
    }
}

static void npa_params_out(void *shadow, const AVCodecParameters *modern)
{
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_TYPE, int, (int)modern->codec_type);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_ID, unsigned,
           (unsigned)npa_codec_id_to_legacy((int)modern->codec_id));
    NPA_ST(shadow, NPA_LEGACY_PARAMS_CODEC_TAG, unsigned, modern->codec_tag);
    {
        const void *extra = modern->extradata;
        int extra_size = modern->extradata_size;

        if (modern->codec_id == AV_CODEC_ID_AV1)
            extra = npa_av1_extradata_legacy(extra, extra_size, &extra_size);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA, void *, (void *)(uintptr_t)extra);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA_SIZE, int, extra_size);
    }
    NPA_ST(shadow, NPA_LEGACY_PARAMS_BIT_RATE, int64_t, modern->bit_rate);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_BITS_PER_CODED_SAMPLE, int,
           modern->bits_per_coded_sample);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_BITS_PER_RAW_SAMPLE, int,
           modern->bits_per_raw_sample);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_PROFILE, int, modern->profile);
    NPA_ST(shadow, NPA_LEGACY_PARAMS_LEVEL, int, modern->level);
    switch ((int)modern->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FORMAT, int,
               npa_pix_fmt_to_legacy((int)modern->format));
        NPA_ST(shadow, NPA_LEGACY_PARAMS_WIDTH, int, modern->width);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_HEIGHT, int, modern->height);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_SAMPLE_ASPECT_RATIO, AVRational,
               modern->sample_aspect_ratio);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FIELD_ORDER, int, (int)modern->field_order);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_COLOR_RANGE, int, (int)modern->color_range);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_COLOR_PRIMARIES, int,
               (int)modern->color_primaries);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_COLOR_TRC, int, (int)modern->color_trc);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_COLORSPACE, int, (int)modern->color_space);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_CHROMA_LOCATION, int,
               (int)modern->chroma_location);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_VIDEO_DELAY, int, modern->video_delay);
        break;
    case AVMEDIA_TYPE_AUDIO:
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FORMAT, int, (int)modern->format);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_CHANNEL_LAYOUT, int64_t,
               npa_legacy_mask_from_layout(&modern->ch_layout));
        NPA_ST(shadow, NPA_LEGACY_PARAMS_CHANNELS, int, modern->ch_layout.nb_channels);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_SAMPLE_RATE, int, modern->sample_rate);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_BLOCK_ALIGN, int, modern->block_align);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FRAME_SIZE, int, modern->frame_size);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_INITIAL_PADDING, int, modern->initial_padding);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_TRAILING_PADDING, int, modern->trailing_padding);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_SEEK_PREROLL, int, modern->seek_preroll);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FORMAT, int, (int)modern->format);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_WIDTH, int, modern->width);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_HEIGHT, int, modern->height);
        break;
    default:
        NPA_ST(shadow, NPA_LEGACY_PARAMS_FORMAT, int, (int)modern->format);
        break;
    }
}

/* The modern object behind an app-visible codec context. A pointer this shim
 * did not allocate is a legacy object built by 4.4.5 code; reading it by its
 * frozen layout is how the source contexts arrive, so it is turned into a
 * temporary modern context the caller releases. */
static AVCodecContext *npa_ctx_of(void *ptr, AVCodecContext **temp)
{
    npa_ctx_shadow *entry = npa_ctx_find(ptr);

    *temp = NULL;
    if (entry) {
        /* The shadow is the app's copy of the object: everything it wrote since
         * the last crossing has to reach the modern one before it is used, or
         * the app's codec id, extradata and time base are silently ignored. */
        npa_ctx_in(entry->modern, entry->shadow);
        return entry->modern;
    }
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
    if (entry) {
        /* Same rule as npa_ctx_of: the shadow is the app's copy, so anything
         * written into it since the last crossing - the app's own stores, or
         * the 4.4.5 avcodec_parameters_copy that fills a shim-allocated shadow
         * in the source-context helper - has to reach the modern object. */
        npa_params_in(entry->modern, entry->shadow);
        return entry->modern;
    }
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

/* ---- subtitles -------------------------------------------------------- */

/* 4.4.x's avcodec_decode_subtitle2 rewrote a Matroska-form ASS event ("ReadOrder,
 * Layer,Style,...") into a standalone ASS dialogue line with real timestamps
 * (FF_API_ASS_TIMING), gated on avctx->sub_text_format. 9.0.2 dropped both the
 * rewrite and the field, but the app hands rect->ass straight to libass
 * (sub_100A04BCC @0x100A0529C), so the shim has to keep producing it. */
#define NPA_SUB_TEXT_FMT_ASS_WITH_TIMINGS 1

static void npa_ass_insert_ts(char *out, size_t size, int ts)
{
    if (ts == -1) {
        snprintf(out, size, "9:59:59.99,");
    } else {
        int h = ts / 360000, m, s;

        ts -= 360000 * h;
        m = ts / 6000;
        ts -= 6000 * m;
        s = ts / 100;
        ts -= 100 * s;
        snprintf(out, size, "%d:%02d:%02d.%02d,", h, m, s, ts);
    }
}

static void npa_sub_convert_ass(const void *shadow, AVSubtitle *sub,
                                const AVPacket *pkt)
{
    AVRational pkt_tb = NPA_LD(shadow, NPA_LEGACY_CTX_PKT_TIMEBASE, AVRational);
    AVRational tb = pkt_tb.num ? pkt_tb
                               : NPA_LD(shadow, NPA_LEGACY_CTX_TIME_BASE, AVRational);
    unsigned i;

    for (i = 0; i < sub->num_rects; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        const char *dialog;
        int ts_start, ts_duration = -1;
        char start[32], end[32];
        long layer;
        char *final;

        if (!rect || rect->type != SUBTITLE_ASS || !rect->ass)
            continue;
        if (!strncmp(rect->ass, "Dialogue: ", 10))
            continue;
        dialog = strchr(rect->ass, ',');
        if (!dialog)
            continue;
        dialog++;
        layer = strtol(dialog, (char **)&dialog, 10);
        if (*dialog != ',')
            continue;
        dialog++;

        ts_start = av_rescale_q(pkt->pts, tb, av_make_q(1, 100));
        if (pkt->duration != -1)
            ts_duration = av_rescale_q(pkt->duration, tb, av_make_q(1, 100));
        if (sub->end_display_time < (unsigned)(10 * ts_duration))
            sub->end_display_time = (unsigned)(10 * ts_duration);

        npa_ass_insert_ts(start, sizeof(start), ts_start);
        npa_ass_insert_ts(end, sizeof(end),
                          ts_duration == -1 ? -1 : ts_start + ts_duration);
        final = av_asprintf("Dialogue: %ld,%s%s%s\r\n", layer, start, end, dialog);
        if (!final)
            continue;
        av_freep(&rect->ass);
        rect->ass = final;
    }
}

static npa_sub_shadow *npa_sub_find(const AVSubtitle *ptr)
{
    npa_sub_shadow *entry;

    if (!ptr)
        return NULL;
    pthread_mutex_lock(&g_subdec_lock);
    for (entry = g_sub_shadows; entry; entry = entry->next)
        if (entry->app == ptr)
            break;
    pthread_mutex_unlock(&g_subdec_lock);
    return entry;
}

static void npa_sub_add(AVSubtitle *app, AVSubtitle *modern)
{
    npa_sub_shadow *entry = calloc(1, (size_t)sizeof(*entry));

    entry->app = app;
    entry->modern = modern;
    pthread_mutex_lock(&g_subdec_lock);
    entry->next = g_sub_shadows;
    g_sub_shadows = entry;
    pthread_mutex_unlock(&g_subdec_lock);
}

static void npa_sub_remove(const AVSubtitle *app)
{
    npa_sub_shadow **link;

    pthread_mutex_lock(&g_subdec_lock);
    for (link = &g_sub_shadows; *link; link = &(*link)->next) {
        if ((*link)->app == app) {
            npa_sub_shadow *doomed = *link;
            *link = doomed->next;
            free(doomed);
            break;
        }
    }
    pthread_mutex_unlock(&g_subdec_lock);
}

/* Fill the app's legacy-shaped structure from a decoded modern one. The plane
 * pointers are mirrored, not copied: the app consumes them inside the same
 * function that frees the subtitle (sub_100A04BCC copies bitmap planes in
 * sub_100A05EEC and calls avsubtitle_free at the end), and the shim keeps the
 * modern object alive until exactly then. */
static void npa_sub_materialise(AVSubtitle *app, const AVSubtitle *modern)
{
    struct npa_legacy_subtitle *out = (struct npa_legacy_subtitle *)app;
    unsigned i;

    memset(out, 0, (size_t)NPA_LEGACY_SUB_SIZE);
    out->format = (uint16_t)modern->format;
    out->start_display_time = modern->start_display_time;
    out->end_display_time = modern->end_display_time;
    out->num_rects = modern->num_rects;
    out->pts = modern->pts;
    if (modern->num_rects == 0)
        return;
    {
        struct npa_legacy_subtitle_rect **rects =
            calloc(modern->num_rects, (size_t)sizeof(*rects));
        if (!rects)
            abort();
        out->rects = rects;
        for (i = 0; i < modern->num_rects; i++) {
            const AVSubtitleRect *src = modern->rects[i];
            struct npa_legacy_subtitle_rect *dst;
            int j;

            if (!src)
                continue;
            dst = calloc(1, (size_t)sizeof(*dst));
            if (!dst)
                abort();
            rects[i] = dst;
            dst->x = src->x;
            dst->y = src->y;
            dst->w = src->w;
            dst->h = src->h;
            dst->nb_colors = src->nb_colors;
            for (j = 0; j < 4; j++) {
                dst->data[j] = src->data[j];
                dst->linesize[j] = src->linesize[j];
            }
            dst->type = (int)src->type;
            if (src->text) {
                dst->text = av_strdup(src->text);
                if (!dst->text)
                    abort();
            }
            if (src->ass) {
                dst->ass = av_strdup(src->ass);
                if (!dst->ass)
                    abort();
            }
            dst->flags = src->flags;
        }
    }
}

static void npa_sub_release_legacy(AVSubtitle *app)
{
    struct npa_legacy_subtitle *out = (struct npa_legacy_subtitle *)app;
    struct npa_legacy_subtitle_rect **rects = out->rects;
    uint32_t i;

    if (rects) {
        for (i = 0; i < out->num_rects; i++) {
            if (!rects[i])
                continue;
            av_freep(&rects[i]->text);
            av_freep(&rects[i]->ass);
            free(rects[i]);
        }
        free(rects);
    }
    memset(out, 0, (size_t)NPA_LEGACY_SUB_SIZE);
}

/* ---- entry points ----------------------------------------------------- */

static const AVCodec *npa_shadow_avcodec_find_decoder(enum AVCodecID id)
{
    /* The app asks with its own 4.4.x id; the codec ids in the subtitle range
     * were renumbered in 9.0.2 (see npa_subdec_modern_codec_id). */
    return avcodec_find_decoder(npa_subdec_modern_codec_id((int)id));
}

static AVCodecContext *npa_shadow_avcodec_alloc_context3(const AVCodec *codec)
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
    /*
     * 9.0.2 no longer has sub_text_format, so npa_ctx_out cannot carry it. Its
     * 4.4.x default is FF_SUB_TEXT_FMT_ASS_WITH_TIMINGS (options_table.h), and
     * the app reads the shadow to decide whether a decoded ASS event still
     * needs the standalone dialogue form - see npa_sub_convert_ass.
     */
    NPA_ST(shadow, NPA_LEGACY_CTX_SUB_TEXT_FORMAT, int,
           NPA_SUB_TEXT_FMT_ASS_WITH_TIMINGS);
    return shadow;
}

static void npa_shadow_avcodec_free_context(AVCodecContext **pavctx)
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

static int npa_shadow_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
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
    if (ret >= 0)
        entry->closed = 0;
    return ret;
}

static AVCodecParameters *npa_shadow_avcodec_parameters_alloc(void)
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

static void npa_shadow_avcodec_parameters_free(AVCodecParameters **ppar)
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

static int npa_shadow_avcodec_parameters_from_context(AVCodecParameters *par,
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

static int npa_shadow_avcodec_parameters_to_context(AVCodecContext *ctx,
                                                        const AVCodecParameters *par)
{
    npa_ctx_shadow *entry = npa_ctx_find(ctx);
    AVCodecParameters *temp = NULL;
    AVCodecParameters *modern_par;
    int ret;

    if (!entry)
        abort();
    npa_ctx_in(entry->modern, entry->shadow);
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
    npa_ctx_shadow *entry = npa_ctx_find(avctx);
    AVSubtitle *modern;
    AVPacket *packet;
    int ret;
    int got = 0;

    if (!entry || !sub || !got_sub_ptr)
        abort();
    /* The app writes more context fields after opening it (pkt_timebase and the
     * text format, sub_100A03DF4), so the modern twin is refreshed here too. */
    npa_ctx_in(entry->modern, avctx);
    /* The app frees each decoded subtitle before reusing the structure
     * (sub_100A04BCC ends with avsubtitle_free on every path). */
    if (npa_sub_find(sub))
        abort();
    modern = av_mallocz(sizeof(*modern));
    packet = av_packet_alloc();
    if (!modern || !packet)
        abort();
    if (avpkt) {
        /* The app's packet is stack-local and non-refcounted, so the modern
         * view owns a copy of the bytes; the subtitle decoders are small
         * enough that the copy is not worth avoiding. */
        const void *data = NPA_LD(avpkt, NPA_LEGACY_PKT_DATA, void *);
        int size = NPA_LD(avpkt, NPA_LEGACY_PKT_SIZE, int);

        if (size > 0) {
            if (!data)
                abort();
            if (av_new_packet(packet, size) < 0)
                abort();
            memcpy(packet->data, data, (size_t)size);
        }
        /* av_new_packet() resets the timing fields, so they are set after it;
         * the ASS rewrite needs the packet's real pts and duration. */
        packet->pts = NPA_LD(avpkt, NPA_LEGACY_PKT_PTS, int64_t);
        packet->dts = NPA_LD(avpkt, NPA_LEGACY_PKT_DTS, int64_t);
        packet->duration = NPA_LD(avpkt, NPA_LEGACY_PKT_DURATION, int64_t);
        packet->flags = NPA_LD(avpkt, NPA_LEGACY_PKT_FLAGS, int);
    }
    ret = avcodec_decode_subtitle2(entry->modern, modern, &got, packet);
    if (ret >= 0 && got && modern->num_rects &&
        NPA_LD(avctx, NPA_LEGACY_CTX_SUB_TEXT_FORMAT, int) ==
            NPA_SUB_TEXT_FMT_ASS_WITH_TIMINGS)
        npa_sub_convert_ass(avctx, modern, packet);
    av_packet_free(&packet);
    if (ret < 0 || !got) {
        avsubtitle_free(modern);
        av_free(modern);
        *got_sub_ptr = got;
        return ret;
    }
    npa_sub_add(sub, modern);
    npa_sub_materialise(sub, modern);
    *got_sub_ptr = 1;
    return ret;
}

NPA_EXPORT void npa_subdec_avsubtitle_free(AVSubtitle *sub)
{
    npa_sub_shadow *entry;
    AVSubtitle *modern;

    if (!sub)
        return;
    entry = npa_sub_find(sub);
    if (!entry)
        abort();
    /* npa_sub_remove releases the entry, so the modern object has to be taken
     * out of it first. */
    modern = entry->modern;
    npa_sub_remove(sub);
    avsubtitle_free(modern);
    av_free(modern);
    npa_sub_release_legacy(sub);
}

/* ---- shared entries and the two units' forwarders --------------------- */

/*
 * Both units sit on one registry and one shadow translation. The context and
 * parameter entries behave identically for the subtitle decoder and for the
 * playback/probe decoders, so the implementation lives once (npa_shadow_*) and
 * each unit's export is a forwarder. The unit split is what falls back
 * independently, not the code.
 */

NPA_EXPORT const AVCodec *npa_subdec_avcodec_find_decoder(enum AVCodecID id)
{
    return npa_shadow_avcodec_find_decoder(id);
}

NPA_EXPORT const AVCodec *npa_codec_avcodec_find_decoder(enum AVCodecID id)
{
    return npa_shadow_avcodec_find_decoder(id);
}

NPA_EXPORT AVCodecContext *npa_subdec_avcodec_alloc_context3(const AVCodec *codec)
{
    return npa_shadow_avcodec_alloc_context3(codec);
}

NPA_EXPORT AVCodecContext *npa_codec_avcodec_alloc_context3(const AVCodec *codec)
{
    return npa_shadow_avcodec_alloc_context3(codec);
}

NPA_EXPORT void npa_subdec_avcodec_free_context(AVCodecContext **pavctx)
{
    npa_shadow_avcodec_free_context(pavctx);
}

NPA_EXPORT void npa_codec_avcodec_free_context(AVCodecContext **pavctx)
{
    npa_shadow_avcodec_free_context(pavctx);
}

NPA_EXPORT int npa_subdec_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
                                        AVDictionary **options)
{
    return npa_shadow_avcodec_open2(avctx, codec, options);
}

NPA_EXPORT int npa_codec_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
                                       AVDictionary **options)
{
    return npa_shadow_avcodec_open2(avctx, codec, options);
}

NPA_EXPORT AVCodecParameters *npa_subdec_avcodec_parameters_alloc(void)
{
    return npa_shadow_avcodec_parameters_alloc();
}

NPA_EXPORT AVCodecParameters *npa_codec_avcodec_parameters_alloc(void)
{
    return npa_shadow_avcodec_parameters_alloc();
}

NPA_EXPORT void npa_subdec_avcodec_parameters_free(AVCodecParameters **ppar)
{
    npa_shadow_avcodec_parameters_free(ppar);
}

NPA_EXPORT void npa_codec_avcodec_parameters_free(AVCodecParameters **ppar)
{
    npa_shadow_avcodec_parameters_free(ppar);
}

NPA_EXPORT int npa_subdec_avcodec_parameters_from_context(AVCodecParameters *par,
                                                          const AVCodecContext *ctx)
{
    return npa_shadow_avcodec_parameters_from_context(par, ctx);
}

NPA_EXPORT int npa_codec_avcodec_parameters_from_context(AVCodecParameters *par,
                                                         const AVCodecContext *ctx)
{
    return npa_shadow_avcodec_parameters_from_context(par, ctx);
}

NPA_EXPORT int npa_subdec_avcodec_parameters_to_context(AVCodecContext *ctx,
                                                        AVCodecParameters *par)
{
    return npa_shadow_avcodec_parameters_to_context(ctx, par);
}

NPA_EXPORT int npa_codec_avcodec_parameters_to_context(AVCodecContext *ctx,
                                                       AVCodecParameters *par)
{
    return npa_shadow_avcodec_parameters_to_context(ctx, par);
}

/* ---- decoded frames ---------------------------------------------------- */

/*
 * One legacy AVBuffer owns one modern frame. The app's 4.4 av_frame_unref and
 * av_buffer_unref drive this frozen layout, so the refcount lives there and the
 * last reference releases the modern frame. The app refs a received frame into
 * its own wrapper (sub_100A817AC: av_frame_ref(wrapper+0x40, src)) before using
 * it, which is exactly what that refcount is for.
 */
static void npa_frame_buffer_free(void *opaque, uint8_t *data)
{
    AVFrame *modern = opaque;

    (void)data;
    av_frame_free(&modern);
}

/* The free callback doubles as the ownership tag: a buffer slot whose callback
 * is not this function was not put there by this shim. */
static int npa_frame_ref_is_ours(const void *ref_ptr)
{
    const struct npa_legacy_avbuffer_ref *ref = ref_ptr;

    return ref && ref->buffer && ref->buffer->free == npa_frame_buffer_free;
}

/*
 * Drop the buffers a previous call installed in the app's frame. The app's own
 * av_frame_unref normally did that already (its get_frame_defaults put
 * extended_data back on the inline array), so this usually finds empty slots;
 * anything still filled has to be ours.
 */
static void npa_frame_wipe(struct npa_legacy_frame *frame)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        if (!frame->buf[i])
            continue;
        if (!npa_frame_ref_is_ours(frame->buf[i]))
            abort();
        legacy_ref_release((struct npa_legacy_avbuffer_ref **)&frame->buf[i]);
    }
    if (frame->extended_buf || frame->nb_extended_buf)
        abort();
    /* This unit never builds the >8-channel pointer array, so anything but the
     * inline one is a state it does not model. */
    if (frame->extended_data && frame->extended_data != frame->data)
        abort();
    frame->extended_data = frame->data;
}

/*
 * Materialise one modern frame into the app's legacy frame. The modern frame's
 * ownership moves into the legacy buffers installed here, so the caller must
 * not release it afterwards; the app's own releases do that when the last
 * reference goes.
 *
 * Deliberately not carried across, because 9.0.2 no longer has the field or the
 * app never reads it: pkt_pos/pkt_size and reordered_opaque (9.0.2 dropped
 * them; 4.4.5 filled them from the packet and the context), the frame's
 * metadata dictionary and its side data (the app reads neither; the side data
 * stays with the modern frame and is released with it), and pkt_pts, which is
 * the deprecated alias of pts.
 */
static void npa_frame_materialise(struct npa_legacy_frame *frame, AVFrame *modern,
                                  int is_video)
{
    struct npa_legacy_avbuffer *buffer;
    unsigned i;
    unsigned planes = 0;

    if (modern->hw_frames_ctx)
        abort(); /* hardware frames are outside this unit */
    if (modern->extended_buf || modern->nb_extended_buf)
        abort(); /* more planes than the legacy array can point at */
    if (modern->ch_layout.nb_channels > 8)
        abort(); /* would need the extended pointer array */
    for (i = 0; i < 8; i++)
        if (modern->data[i])
            planes++;
    if (!planes)
        abort();

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        abort();
    buffer->data = modern->data[0];
    buffer->size = modern->buf[0] ? (int)modern->buf[0]->size : 0;
    buffer->free = npa_frame_buffer_free;
    buffer->opaque = modern;
    atomic_init(&buffer->refcount, planes);

    for (i = 0; i < 8; i++) {
        struct npa_legacy_avbuffer_ref *ref;

        if (!modern->data[i])
            continue;
        ref = calloc(1, sizeof(*ref));
        if (!ref)
            abort();
        ref->buffer = buffer;
        ref->data = modern->data[i];
        ref->size = modern->buf[i] ? (int)modern->buf[i]->size
                                   : (modern->buf[0] ? (int)modern->buf[0]->size : 0);
        frame->buf[i] = ref;
        frame->data[i] = modern->data[i];
        frame->linesize[i] = modern->linesize[i];
    }

    frame->extended_data = frame->data;
    frame->width = modern->width;
    frame->height = modern->height;
    frame->nb_samples = modern->nb_samples;
    /* Format values cross in the legacy direction (spec section 3.3). Video is
     * the renumbered enum; AVSampleFormat is the same in both versions. */
    frame->format = is_video ? npa_pix_fmt_to_legacy((int)modern->format)
                             : (int)modern->format;
    frame->key_frame = (modern->flags & AV_FRAME_FLAG_KEY) ? 1 : 0;
    frame->pict_type = (int)modern->pict_type;
    frame->sample_aspect_ratio = modern->sample_aspect_ratio;
    frame->pts = modern->pts;
    frame->pkt_dts = modern->pkt_dts;
    frame->repeat_pict = modern->repeat_pict;
    frame->interlaced_frame = (modern->flags & AV_FRAME_FLAG_INTERLACED) ? 1 : 0;
    frame->top_field_first = (modern->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 1 : 0;
    frame->reordered_opaque = 0; /* 4.4.5 took this from the context, which is 0 */
    frame->sample_rate = modern->sample_rate;
    frame->channel_layout = (uint64_t)npa_legacy_mask_from_layout(&modern->ch_layout);
    frame->flags = modern->flags &
                   (NPA_LEGACY_FRAME_FLAG_CORRUPT | NPA_LEGACY_FRAME_FLAG_DISCARD);
    frame->color_range = (int)modern->color_range;
    frame->color_primaries = (int)modern->color_primaries;
    frame->color_trc = (int)modern->color_trc;
    frame->colorspace = (int)modern->colorspace;
    frame->chroma_location = (int)modern->chroma_location;
    frame->best_effort_timestamp = modern->best_effort_timestamp;
    /* 9.0.2 renamed the field: 4.4's pkt_duration is modern's duration. */
    frame->pkt_duration = modern->duration;
    frame->decode_error_flags = modern->decode_error_flags;
    frame->channels = modern->ch_layout.nb_channels;
}

NPA_EXPORT int npa_codec_avcodec_send_packet(AVCodecContext *avctx,
                                             const void *legacy_packet)
{
    npa_ctx_shadow *entry = npa_ctx_find(avctx);
    AVPacket *packet = NULL;
    int ret;

    if (!entry)
        abort();
    if (entry->closed)
        abort(); /* decode after close */
    /* No context refresh here: the app writes its decoder configuration before
     * open2 (thread_count/thread_type), not per packet, and several of the
     * fields a full refresh would write back are owned by the decoder while it
     * runs. Writing those back every packet is not harmless - 9.0.2's H.264
     * decoder then discards every non-key frame (measured: 17 decoded frames
     * instead of 424 on a stream this shim decodes in full when the refresh is
     * gone). See npa_ctx_in's callers for where the refresh does belong. */
    if (legacy_packet) {
        const void *data = NPA_LD(legacy_packet, NPA_LEGACY_PKT_DATA, void *);
        int size = NPA_LD(legacy_packet, NPA_LEGACY_PKT_SIZE, int);

        if (size < 0 || (size > 0 && !data))
            abort();
        packet = av_packet_alloc();
        if (!packet)
            abort();
        /* The modern packet borrows the app's stack-local bytes; 9.0.2's
         * avcodec_send_packet refs the packet, and a packet without a buffer is
         * copied, so the app's memory never outlives the call. */
        packet->data = (uint8_t *)(uintptr_t)data;
        packet->size = size;
        packet->pts = NPA_LD(legacy_packet, NPA_LEGACY_PKT_PTS, int64_t);
        packet->dts = NPA_LD(legacy_packet, NPA_LEGACY_PKT_DTS, int64_t);
        packet->duration = NPA_LD(legacy_packet, NPA_LEGACY_PKT_DURATION, int64_t);
        packet->flags = NPA_LD(legacy_packet, NPA_LEGACY_PKT_FLAGS, int);
        packet->stream_index = NPA_LD(legacy_packet, NPA_LEGACY_PKT_STREAM_INDEX, int);
        packet->pos = NPA_LD(legacy_packet, NPA_LEGACY_PKT_POS, int64_t);
    }
    ret = avcodec_send_packet(entry->modern, packet);
    av_packet_free(&packet);
    return ret;
}

NPA_EXPORT int npa_codec_avcodec_receive_frame(AVCodecContext *avctx, void *legacy)
{
    npa_ctx_shadow *entry = npa_ctx_find(avctx);
    struct npa_legacy_frame *frame = legacy;
    AVFrame *modern;
    int ret;

    if (!entry || !frame)
        abort();
    if (entry->closed)
        abort(); /* decode after close */
    /* No context refresh: see npa_codec_avcodec_send_packet. */
    /* 4.4's avcodec_receive_frame unrefs the frame it is given first; on
     * EAGAIN the app must find it empty, exactly as before. */
    npa_frame_wipe(frame);
    modern = av_frame_alloc();
    if (!modern)
        abort();
    ret = avcodec_receive_frame(entry->modern, modern);
    if (ret < 0) {
        av_frame_free(&modern);
        return ret;
    }
    npa_frame_materialise(frame, modern, entry->modern->codec_type == AVMEDIA_TYPE_VIDEO);
    return 0;
}

NPA_EXPORT void npa_codec_avcodec_flush_buffers(AVCodecContext *avctx)
{
    npa_ctx_shadow *entry = npa_ctx_find(avctx);

    if (!entry)
        abort();
    if (entry->closed)
        abort();
    /* The app clears skip_loop_filter/skip_idct/skip_frame just before it
     * flushes (sub_100A81590), and those have to reach the modern decoder for
     * the discard behaviour to survive a seek. */
    npa_ctx_in(entry->modern, avctx);
    avcodec_flush_buffers(entry->modern);
}

/*
 * 9.0.2 removed avcodec_close: releasing the decoder state is what
 * avcodec_free_context does now. The app's close and free_context calls are
 * adjacent (the thumbnail path ends with both), so the shim flushes and marks
 * the context closed instead of releasing it early, and mirrors the one field
 * the app could look at afterwards - 4.4's close cleared avctx->codec.
 */
NPA_EXPORT int npa_codec_avcodec_close(AVCodecContext *avctx)
{
    npa_ctx_shadow *entry = npa_ctx_find(avctx);

    if (!entry)
        abort();
    if (entry->closed)
        abort();
    npa_ctx_in(entry->modern, avctx);
    avcodec_flush_buffers(entry->modern);
    entry->closed = 1;
    NPA_ST(avctx, NPA_LEGACY_CTX_CODEC, void *, NULL);
    return 0;
}
