/*
 * Demux-boundary shim for the ffmpeg-demux unit.
 *
 * The unit carries FFmpeg 9.0.2 libavformat. The app was built against 4.4.5
 * and reads AVFormatContext/AVStream/AVCodecParameters by offset, so each
 * entry point here hands the app a legacy-shaped "shadow" and translates at
 * the boundary:
 *
 *   - before a modern call, the fields the app wrote (pb, interrupt callback,
 *     error_recognition, flags, per-stream discard) are copied to the modern
 *     context;
 *   - after a call that can change the graph, a shadow AVStream[]/codecpar[]
 *     is rebuilt and the fields the app reads are copied back.
 *
 * The app's own FFmpeg (avcodec/avutil, bitstream filters) stays 4.4.5, so the
 * shadow is the single ABI the app and the legacy libraries agree on.
 *
 * av_read_frame's packets are returned as legacy-owned buffers, and a stream's
 * attached_pic is materialised the same way.
 *
 * The frozen call-site table is in
 * docs/superpowers/plans/2026-09-26-class-a-demux-modern-avformat.md.
 */

#include "ffmpeg-demux-abi.h"
#include "ffmpeg-demux-enum-map.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include <libavutil/buffer.h>
#include <libavutil/mem.h>

#define NPA_EXPORT __attribute__((visibility("default")))

typedef struct npa_shadow {
    AVFormatContext *modern;
    void *shadow;
    unsigned nb_streams;
    void **streams;
    unsigned nb_chapters;
    void **chapters;
    struct npa_shadow *next;
} npa_shadow;

static npa_shadow *g_shadows;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static npa_shadow *shadow_lookup(void *shadow)
{
    npa_shadow *s;
    if (!shadow)
        return NULL;
    pthread_mutex_lock(&g_lock);
    for (s = g_shadows; s; s = s->next)
        if (s->shadow == shadow)
            break;
    pthread_mutex_unlock(&g_lock);
    return s;
}

static npa_shadow *shadow_of_stream(void *stream, unsigned *index)
{
    npa_shadow *s, *found = NULL;
    unsigned i;

    if (!stream)
        return NULL;
    pthread_mutex_lock(&g_lock);
    for (s = g_shadows; s && !found; s = s->next)
        for (i = 0; i < s->nb_streams; i++)
            if (s->streams[i] == stream) {
                found = s;
                *index = i;
                break;
            }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void shadow_register(npa_shadow *s)
{
    pthread_mutex_lock(&g_lock);
    s->next = g_shadows;
    g_shadows = s;
    pthread_mutex_unlock(&g_lock);
}

static void shadow_unregister(npa_shadow *s)
{
    npa_shadow **p;

    pthread_mutex_lock(&g_lock);
    for (p = &g_shadows; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            break;
        }
    pthread_mutex_unlock(&g_lock);
}

static void packet_buffer_free(void *opaque, uint8_t *data)
{
    if (opaque) {
        AVBufferRef *retained = (AVBufferRef *)opaque;
        av_buffer_unref(&retained);
    } else {
        av_free(data);
    }
}

/*
 * Release one legacy AVBufferRef the way the app's 4.4 av_buffer_unref does:
 * drop the ref, and when it was the last one run the buffer's free callback
 * and release the buffer. The buffer shell is freed by whoever performs the
 * last unref, so this must not run ahead of an outstanding legacy reference.
 */
static void legacy_ref_release(struct npa_legacy_avbuffer_ref **pref)
{
    struct npa_legacy_avbuffer_ref *ref = *pref;
    struct npa_legacy_avbuffer *buf;

    if (!ref)
        return;
    *pref = NULL;
    buf = ref->buffer;
    free(ref);
    if (buf && atomic_fetch_sub_explicit(&buf->refcount, 1, memory_order_acq_rel) == 1) {
        if (buf->free)
            buf->free(buf->opaque, buf->data);
        free(buf);
    }
}

static void legacy_packet_release(void *pkt)
{
    legacy_ref_release((struct npa_legacy_avbuffer_ref **)((char *)pkt + NPA_LEGACY_PACKET_BUF));
}

/*
 * Fill the app's 0x58 AVPacket from a modern packet, wrapping the payload in a
 * hand-built legacy AVBufferRef so the app's 4.4 av_packet_unref frees it.
 * The modern buffer is retained (zero copy); a buf-less source is copied.
 */
static int legacy_packet_from(void *dst, const AVPacket *src)
{
    struct npa_legacy_avbuffer *buf = calloc(1, sizeof(*buf));
    struct npa_legacy_avbuffer_ref *ref = calloc(1, sizeof(*ref));
    uint8_t *data = src->data;
    void *opaque = NULL;

    if (!buf || !ref) {
        free(buf);
        free(ref);
        return -1;
    }
    if (src->buf) {
        opaque = av_buffer_ref(src->buf);
        if (!opaque) {
            free(buf);
            free(ref);
            return -1;
        }
    } else if (src->data && src->size > 0) {
        data = av_malloc((size_t)src->size);
        if (!data) {
            free(buf);
            free(ref);
            return -1;
        }
        memcpy(data, src->data, (size_t)src->size);
    }

    buf->data = data;
    buf->size = src->size;
    atomic_init(&buf->refcount, 1);
    buf->free = packet_buffer_free;
    buf->opaque = opaque;
    ref->buffer = buf;
    ref->data = data;
    ref->size = src->size;

    npa_st_ptr(dst, NPA_LEGACY_PACKET_BUF, ref);
    npa_st_u64(dst, NPA_LEGACY_PACKET_PTS, (uint64_t)src->pts);
    npa_st_u64(dst, NPA_LEGACY_PACKET_DTS, (uint64_t)src->dts);
    npa_st_ptr(dst, NPA_LEGACY_PACKET_DATA, data);
    npa_st_u32(dst, NPA_LEGACY_PACKET_SIZE_FIELD, (uint32_t)src->size);
    npa_st_u32(dst, NPA_LEGACY_PACKET_STREAM_INDEX, (uint32_t)src->stream_index);
    npa_st_u32(dst, NPA_LEGACY_PACKET_FLAGS, (uint32_t)src->flags);
    npa_st_ptr(dst, NPA_LEGACY_PACKET_SIDE_DATA, NULL);
    npa_st_u32(dst, NPA_LEGACY_PACKET_SIDE_DATA_ELEMS, 0);
    npa_st_u64(dst, NPA_LEGACY_PACKET_DURATION, (uint64_t)src->duration);
    npa_st_u64(dst, NPA_LEGACY_PACKET_POS, (uint64_t)src->pos);
    npa_st_u64(dst, NPA_LEGACY_PACKET_CONVERGENCE_DURATION, 0);
    return 0;
}

static void shadow_free_streams(npa_shadow *s)
{
    unsigned i;

    for (i = 0; i < s->nb_streams; i++) {
        void *codecpar;

        if (!s->streams[i])
            continue;
        legacy_packet_release((char *)s->streams[i] + NPA_LEGACY_STREAM_ATTACHED_PIC);
        codecpar = npa_ld_ptr(s->streams[i], NPA_LEGACY_STREAM_CODECPAR);
        if (codecpar) {
            av_free(npa_ld_ptr(codecpar, NPA_LEGACY_CODECPAR_EXTRADATA));
            free(codecpar);
        }
        free(s->streams[i]);
    }
    free(s->streams);
    s->streams = NULL;
    s->nb_streams = 0;
}

static void shadow_free_chapters(npa_shadow *s)
{
    unsigned i;

    for (i = 0; i < s->nb_chapters; i++)
        free(s->chapters[i]);
    free(s->chapters);
    s->chapters = NULL;
    s->nb_chapters = 0;
}

static void shadow_destroy(npa_shadow *s)
{
    shadow_unregister(s);
    shadow_free_streams(s);
    shadow_free_chapters(s);
    free(s->shadow);
    free(s);
}

static void store_rational(void *dst, size_t offset, AVRational r)
{
    npa_st_u32(dst, offset, (uint32_t)r.num);
    npa_st_u32(dst, offset + 4, (uint32_t)r.den);
}

static void shadow_rebuild_streams(npa_shadow *s)
{
    unsigned i, n;

    shadow_free_streams(s);
    n = s->modern ? s->modern->nb_streams : 0;
    if (!n)
        return;
    s->streams = calloc(n, sizeof(void *));
    if (!s->streams)
        return;
    s->nb_streams = n;

    for (i = 0; i < n; i++) {
        AVStream *ms = s->modern->streams[i];
        AVCodecParameters *mp;
        void *ls;
        void *lp;

        if (!ms)
            continue;
        mp = ms->codecpar;
        ls = calloc(1, NPA_LEGACY_STREAM_SIZE);
        lp = calloc(1, NPA_LEGACY_CODECPAR_SIZE);

        if (!ls || !lp) {
            free(ls);
            free(lp);
            continue;
        }
        s->streams[i] = ls;

        npa_st_u32(ls, NPA_LEGACY_STREAM_INDEX, (uint32_t)ms->index);
        npa_st_u32(ls, NPA_LEGACY_STREAM_ID, (uint32_t)ms->id);
        npa_st_ptr(ls, NPA_LEGACY_STREAM_CODEC, NULL);
        store_rational(ls, NPA_LEGACY_STREAM_TIME_BASE, ms->time_base);
        npa_st_u64(ls, NPA_LEGACY_STREAM_START_TIME, (uint64_t)ms->start_time);
        npa_st_u64(ls, NPA_LEGACY_STREAM_DURATION, (uint64_t)ms->duration);
        npa_st_u64(ls, NPA_LEGACY_STREAM_NB_FRAMES, (uint64_t)ms->nb_frames);
        npa_st_u32(ls, NPA_LEGACY_STREAM_DISPOSITION, (uint32_t)ms->disposition);
        npa_st_u32(ls, NPA_LEGACY_STREAM_DISCARD, (uint32_t)ms->discard);
        store_rational(ls, NPA_LEGACY_STREAM_SAMPLE_ASPECT_RATIO, ms->sample_aspect_ratio);
        npa_st_ptr(ls, NPA_LEGACY_STREAM_METADATA, ms->metadata);
        store_rational(ls, NPA_LEGACY_STREAM_AVG_FRAME_RATE, ms->avg_frame_rate);
        npa_st_ptr(ls, NPA_LEGACY_STREAM_CODECPAR, lp);
        if (ms->attached_pic.size > 0)
            legacy_packet_from((char *)ls + NPA_LEGACY_STREAM_ATTACHED_PIC, &ms->attached_pic);

        if (!mp)
            continue;
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_TYPE, (uint32_t)mp->codec_type);
        /*
         * libavcodec/avutil renumber AVCodecID and AVPixelFormat across major
         * versions; the app matches codec ids and pixel formats against 4.4.x
         * values, so translate before copying.
         */
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_ID, (uint32_t)npa_codec_id_to_legacy(mp->codec_id));
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_TAG, (uint32_t)mp->codec_tag);
        /*
         * The shadow owns its extradata copy. The app frees extradata through
         * the shadow for attachment streams (sub_100AEDD90, 0x100AEE36C), so
         * aliasing the modern buffer would make the modern close free it a
         * second time.
         */
        if (mp->extradata && mp->extradata_size > 0) {
            /*
             * 4.4.x published only the OBUs for AV1, on purpose: its
             * matroskadec.c sets `extradata_offset = 4` for AV_CODEC_ID_AV1
             * with the comment "For now, propagate only the OBUs, if any.
             * Once libavcodec is updated to handle isobmff style extradata this
             * can be removed." 9.0.2's libavcodec *was* updated, so its
             * demuxer hands out the AV1CodecConfigurationRecord instead - and
             * the app feeds these bytes straight to VideoToolbox when it builds
             * the hardware decoder's format description. Measured on device:
             * with the record the description came out 0x0 and
             * VTDecompressionSessionCreateWithOptions failed (-12910), so the
             * player fell back to software; with the OBUs the hardware decoder
             * is installed. The codec unit turns the OBUs back into a record
             * for 9.0.2's decoders (npa_av1_extradata_modern).
             */
            const uint8_t *src = mp->extradata;
            int size = mp->extradata_size;

            if (mp->codec_id == AV_CODEC_ID_AV1 && size > 4 && (src[0] & 0x80) &&
                (src[0] & 0x7F) == 1) {
                src += 4;
                size -= 4;
            }
            if (size > 0) {
                void *copy = av_malloc((size_t)size);
                if (copy) {
                    memcpy(copy, src, (size_t)size);
                    npa_st_ptr(lp, NPA_LEGACY_CODECPAR_EXTRADATA, copy);
                    npa_st_u32(lp, NPA_LEGACY_CODECPAR_EXTRADATA_SIZE, (uint32_t)size);
                }
            }
        }
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_FORMAT, (uint32_t)npa_pix_fmt_to_legacy(mp->format));
        npa_st_u64(lp, NPA_LEGACY_CODECPAR_BIT_RATE, (uint64_t)mp->bit_rate);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_BITS_PER_CODED_SAMPLE, (uint32_t)mp->bits_per_coded_sample);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_BITS_PER_RAW_SAMPLE, (uint32_t)mp->bits_per_raw_sample);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_PROFILE, (uint32_t)mp->profile);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_LEVEL, (uint32_t)mp->level);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_WIDTH, (uint32_t)mp->width);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_HEIGHT, (uint32_t)mp->height);
        store_rational(lp, NPA_LEGACY_CODECPAR_SAMPLE_ASPECT_RATIO, mp->sample_aspect_ratio);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_FIELD_ORDER, (uint32_t)mp->field_order);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_RANGE, (uint32_t)mp->color_range);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_PRIMARIES, (uint32_t)mp->color_primaries);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_TRC, (uint32_t)mp->color_trc);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_SPACE, (uint32_t)mp->color_space);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CHROMA_LOCATION, (uint32_t)mp->chroma_location);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_VIDEO_DELAY, (uint32_t)mp->video_delay);
        npa_st_u64(
            lp,
            NPA_LEGACY_CODECPAR_CHANNEL_LAYOUT,
            mp->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? (uint64_t)mp->ch_layout.u.mask : 0
        );
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CHANNELS, (uint32_t)mp->ch_layout.nb_channels);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_SAMPLE_RATE, (uint32_t)mp->sample_rate);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_BLOCK_ALIGN, (uint32_t)mp->block_align);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_FRAME_SIZE, (uint32_t)mp->frame_size);
    }
}

/*
 * The app reads AVChapter at 4.4.x offsets, where id is an int and time_base
 * sits at 0x4; the modern id is int64_t and time_base sits at 0x8. Build a
 * legacy-shaped array rather than alias the modern one.
 */
static void shadow_rebuild_chapters(npa_shadow *s)
{
    unsigned i, n;

    shadow_free_chapters(s);
    n = s->modern ? s->modern->nb_chapters : 0;
    if (!n)
        return;
    s->chapters = calloc(n, sizeof(void *));
    if (!s->chapters)
        return;
    s->nb_chapters = n;

    for (i = 0; i < n; i++) {
        AVChapter *src = s->modern->chapters ? s->modern->chapters[i] : NULL;
        void *dst = calloc(1, NPA_LEGACY_CHAPTER_SIZE);

        if (!src || !dst) {
            free(dst);
            continue;
        }
        s->chapters[i] = dst;
        npa_st_u32(dst, NPA_LEGACY_CHAPTER_ID, (uint32_t)src->id);
        npa_st_u32(dst, NPA_LEGACY_CHAPTER_TIME_BASE, (uint32_t)src->time_base.num);
        npa_st_u32(dst, NPA_LEGACY_CHAPTER_TIME_BASE + 4, (uint32_t)src->time_base.den);
        npa_st_u64(dst, NPA_LEGACY_CHAPTER_START, (uint64_t)src->start);
        npa_st_u64(dst, NPA_LEGACY_CHAPTER_END, (uint64_t)src->end);
        npa_st_ptr(dst, NPA_LEGACY_CHAPTER_METADATA, src->metadata);
    }
}

static void shadow_to_modern(npa_shadow *s)
{
    AVFormatContext *m = s->modern;
    unsigned i;

    if (!m)
        return;
    m->pb = (AVIOContext *)npa_ld_ptr(s->shadow, NPA_LEGACY_FMT_PB);
    m->interrupt_callback.callback =
        (int (*)(void *))npa_ld_ptr(s->shadow, NPA_LEGACY_FMT_INTERRUPT_CALLBACK);
    m->interrupt_callback.opaque = npa_ld_ptr(s->shadow, NPA_LEGACY_FMT_INTERRUPT_OPAQUE);
    m->error_recognition = (int)npa_ld_u32(s->shadow, NPA_LEGACY_FMT_ERROR_RECOGNITION);
    m->flags = (int)npa_ld_u32(s->shadow, NPA_LEGACY_FMT_FLAGS);

    for (i = 0; i < s->nb_streams && i < m->nb_streams; i++)
        if (s->streams[i] && m->streams[i])
            m->streams[i]->discard =
                (enum AVDiscard)npa_ld_u32(s->streams[i], NPA_LEGACY_STREAM_DISCARD);
}

static void modern_to_shadow(npa_shadow *s)
{
    AVFormatContext *m = s->modern;

    if (!m)
        return;
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_IFORMAT, m->iformat);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_PB, m->pb);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_NB_STREAMS, s->nb_streams);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_STREAMS, s->streams);
    npa_st_u64(s->shadow, NPA_LEGACY_FMT_START_TIME, (uint64_t)m->start_time);
    npa_st_u64(s->shadow, NPA_LEGACY_FMT_DURATION, (uint64_t)m->duration);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_MAX_DELAY, (uint32_t)m->max_delay);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_FLAGS, (uint32_t)m->flags);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_NB_CHAPTERS, s->nb_chapters);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_CHAPTERS, s->chapters);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_METADATA, m->metadata);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_ERROR_RECOGNITION, (uint32_t)m->error_recognition);
}

static npa_shadow *shadow_create(AVFormatContext *modern)
{
    npa_shadow *s = calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    s->modern = modern;
    s->shadow = calloc(1, NPA_LEGACY_FMT_SIZE);
    if (!s->shadow) {
        free(s);
        return NULL;
    }
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_FLAGS, (uint32_t)modern->flags);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_ERROR_RECOGNITION, (uint32_t)modern->error_recognition);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_PB, modern->pb);
    shadow_register(s);
    return s;
}

NPA_EXPORT AVFormatContext *npa_demux_avformat_alloc_context(void)
{
    AVFormatContext *modern = avformat_alloc_context();
    npa_shadow *s;

    if (!modern)
        return NULL;
    s = shadow_create(modern);
    if (!s) {
        avformat_free_context(modern);
        return NULL;
    }
    return (AVFormatContext *)s->shadow;
}

NPA_EXPORT int npa_demux_avformat_open_input(
    AVFormatContext **ps, const char *url, const AVInputFormat *fmt, AVDictionary **options
)
{
    npa_shadow *s = ps ? shadow_lookup(*ps) : NULL;
    int ret;

    if (!s) {
        AVFormatContext *modern = avformat_alloc_context();

        if (!modern)
            return AVERROR(ENOMEM);
        s = shadow_create(modern);
        if (!s) {
            avformat_free_context(modern);
            return AVERROR(ENOMEM);
        }
    }

    shadow_to_modern(s);
    ret = avformat_open_input(&s->modern, url, fmt, options);
    if (ret < 0) {
        if (ps)
            *ps = NULL;
        shadow_destroy(s);
        return ret;
    }
    shadow_rebuild_streams(s);
    shadow_rebuild_chapters(s);
    modern_to_shadow(s);
    if (ps)
        *ps = (AVFormatContext *)s->shadow;
    return ret;
}

NPA_EXPORT int npa_demux_avformat_find_stream_info(AVFormatContext *ctx, AVDictionary **options)
{
    npa_shadow *s = shadow_lookup((void *)ctx);
    int ret;

    if (!s || !s->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(s);
    ret = avformat_find_stream_info(s->modern, options);
    if (ret >= 0) {
        shadow_rebuild_streams(s);
        shadow_rebuild_chapters(s);
        modern_to_shadow(s);
    }
    return ret;
}

NPA_EXPORT int npa_demux_av_seek_frame(
    AVFormatContext *ctx, int stream_index, int64_t timestamp, int flags
)
{
    npa_shadow *s = shadow_lookup((void *)ctx);

    if (!s || !s->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(s);
    return av_seek_frame(s->modern, stream_index, timestamp, flags);
}

NPA_EXPORT void npa_demux_avformat_close_input(AVFormatContext **ps)
{
    npa_shadow *s;

    if (!ps || !*ps)
        return;
    s = shadow_lookup(*ps);
    if (!s)
        return;
    if (s->modern)
        avformat_close_input(&s->modern);
    *ps = NULL;
    shadow_destroy(s);
}

NPA_EXPORT void npa_demux_avformat_free_context(AVFormatContext *ctx)
{
    npa_shadow *s = shadow_lookup((void *)ctx);

    if (!s)
        return;
    if (s->modern)
        avformat_free_context(s->modern);
    shadow_destroy(s);
}

NPA_EXPORT AVIOContext *npa_demux_avio_alloc_context(
    unsigned char *buffer,
    int buffer_size,
    int write_flag,
    void *opaque,
    int (*read_packet)(void *, uint8_t *, int),
    int (*write_packet)(void *, const uint8_t *, int),
    int64_t (*seek)(void *, int64_t, int)
)
{
    return avio_alloc_context(buffer, buffer_size, write_flag, opaque, read_packet, write_packet, seek);
}

NPA_EXPORT int64_t npa_demux_avio_size(AVIOContext *s)
{
    return avio_size(s);
}

NPA_EXPORT int npa_demux_av_index_search_timestamp(AVStream *st, int64_t timestamp, int flags)
{
    unsigned index = 0;
    npa_shadow *s = shadow_of_stream((void *)st, &index);

    if (!s || !s->modern || index >= s->modern->nb_streams)
        return -1;
    return av_index_search_timestamp(s->modern->streams[index], timestamp, flags);
}

NPA_EXPORT int npa_demux_av_read_frame(AVFormatContext *ctx, AVPacket *pkt)
{
    npa_shadow *s = shadow_lookup((void *)ctx);
    AVPacket *tmp;
    int ret;

    if (!s || !s->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(s);
    tmp = av_packet_alloc();
    if (!tmp)
        return AVERROR(ENOMEM);
    ret = av_read_frame(s->modern, tmp);
    if (ret >= 0 && legacy_packet_from((void *)pkt, tmp) < 0)
        ret = AVERROR(ENOMEM);
    av_packet_unref(tmp);
    av_packet_free(&tmp);
    return ret;
}

/*
 * The external-subtitle loader (media::FFmpegSubtitle) reaches five entry
 * points the playback demuxer does not. It mounts its own buffered
 * AVIOContext (4096 bytes plus its own read/seek callbacks) and probes or
 * forces the input format, but the AVIOContext and AVInputFormat it stores and
 * hands back are objects this unit created and they never leave the class, so
 * those four are plain forwards. The seek is the exception: it takes the
 * shadow context the app holds, so it needs the same translation as
 * npa_demux_av_seek_frame.
 */

NPA_EXPORT const AVInputFormat *npa_demux_av_find_input_format(const char *short_name)
{
    return av_find_input_format(short_name);
}

NPA_EXPORT int npa_demux_av_probe_input_buffer(
    AVIOContext *pb,
    const AVInputFormat **fmt,
    const char *url,
    void *logctx,
    unsigned int offset,
    unsigned int max_probe_size
)
{
    return av_probe_input_buffer(pb, fmt, url, logctx, offset, max_probe_size);
}

NPA_EXPORT int npa_demux_avio_read(AVIOContext *s, unsigned char *buf, int size)
{
    return avio_read(s, buf, size);
}

NPA_EXPORT int64_t npa_demux_avio_seek(AVIOContext *s, int64_t offset, int whence)
{
    return avio_seek(s, offset, whence);
}

NPA_EXPORT int npa_demux_avformat_seek_file(
    AVFormatContext *s, int stream_index, int64_t min_ts, int64_t ts, int64_t max_ts, int flags
)
{
    npa_shadow *shadow = shadow_lookup((void *)s);

    if (!shadow || !shadow->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(shadow);
    return avformat_seek_file(shadow->modern, stream_index, min_ts, ts, max_ts, flags);
}
