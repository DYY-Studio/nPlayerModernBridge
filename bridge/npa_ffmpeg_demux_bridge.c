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
 * av_read_frame is still a pass-through here; the packet translation lands in
 * the next commit.
 *
 * The frozen call-site table is in
 * docs/superpowers/plans/2026-09-26-class-a-demux-modern-avformat.md.
 */

#include "ffmpeg-demux-abi.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define NPA_EXPORT __attribute__((visibility("default")))

typedef struct npa_shadow {
    AVFormatContext *modern;
    void *shadow;
    unsigned nb_streams;
    void **streams;
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

static void shadow_free_streams(npa_shadow *s)
{
    unsigned i;

    for (i = 0; i < s->nb_streams; i++) {
        if (!s->streams[i])
            continue;
        free(npa_ld_ptr(s->streams[i], NPA_LEGACY_STREAM_CODECPAR));
        free(s->streams[i]);
    }
    free(s->streams);
    s->streams = NULL;
    s->nb_streams = 0;
}

static void shadow_destroy(npa_shadow *s)
{
    shadow_unregister(s);
    shadow_free_streams(s);
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
        AVCodecParameters *mp = ms ? ms->codecpar : NULL;
        void *ls = calloc(1, NPA_LEGACY_STREAM_SIZE);
        void *lp = calloc(1, NPA_LEGACY_CODECPAR_SIZE);

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

        if (!mp)
            continue;
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_TYPE, (uint32_t)mp->codec_type);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_ID, (uint32_t)mp->codec_id);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_CODEC_TAG, (uint32_t)mp->codec_tag);
        npa_st_ptr(lp, NPA_LEGACY_CODECPAR_EXTRADATA, mp->extradata);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_EXTRADATA_SIZE, (uint32_t)mp->extradata_size);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_FORMAT, (uint32_t)mp->format);
        npa_st_u64(lp, NPA_LEGACY_CODECPAR_BIT_RATE, (uint64_t)mp->bit_rate);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_WIDTH, (uint32_t)mp->width);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_HEIGHT, (uint32_t)mp->height);
        store_rational(lp, NPA_LEGACY_CODECPAR_SAMPLE_ASPECT_RATIO, mp->sample_aspect_ratio);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_FIELD_ORDER, (uint32_t)mp->field_order);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_PRIMARIES, (uint32_t)mp->color_primaries);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_TRC, (uint32_t)mp->color_trc);
        npa_st_u32(lp, NPA_LEGACY_CODECPAR_COLOR_SPACE, (uint32_t)mp->color_space);
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
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_NB_STREAMS, (uint32_t)m->nb_streams);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_STREAMS, s->streams);
    npa_st_u64(s->shadow, NPA_LEGACY_FMT_START_TIME, (uint64_t)m->start_time);
    npa_st_u64(s->shadow, NPA_LEGACY_FMT_DURATION, (uint64_t)m->duration);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_MAX_DELAY, (uint32_t)m->max_delay);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_FLAGS, (uint32_t)m->flags);
    npa_st_u32(s->shadow, NPA_LEGACY_FMT_NB_CHAPTERS, (uint32_t)m->nb_chapters);
    npa_st_ptr(s->shadow, NPA_LEGACY_FMT_CHAPTERS, m->chapters);
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

NPA_EXPORT AVFormatContext *npa_avformat_alloc_context(void)
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

NPA_EXPORT int npa_avformat_open_input(
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
    modern_to_shadow(s);
    if (ps)
        *ps = (AVFormatContext *)s->shadow;
    return ret;
}

NPA_EXPORT int npa_avformat_find_stream_info(AVFormatContext *ctx, AVDictionary **options)
{
    npa_shadow *s = shadow_lookup((void *)ctx);
    int ret;

    if (!s || !s->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(s);
    ret = avformat_find_stream_info(s->modern, options);
    if (ret >= 0) {
        shadow_rebuild_streams(s);
        modern_to_shadow(s);
    }
    return ret;
}

NPA_EXPORT int npa_av_seek_frame(
    AVFormatContext *ctx, int stream_index, int64_t timestamp, int flags
)
{
    npa_shadow *s = shadow_lookup((void *)ctx);

    if (!s || !s->modern)
        return AVERROR(EINVAL);
    shadow_to_modern(s);
    return av_seek_frame(s->modern, stream_index, timestamp, flags);
}

NPA_EXPORT void npa_avformat_close_input(AVFormatContext **ps)
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

NPA_EXPORT void npa_avformat_free_context(AVFormatContext *ctx)
{
    npa_shadow *s = shadow_lookup((void *)ctx);

    if (!s)
        return;
    if (s->modern)
        avformat_free_context(s->modern);
    shadow_destroy(s);
}

NPA_EXPORT AVIOContext *npa_avio_alloc_context(
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

NPA_EXPORT int64_t npa_avio_size(AVIOContext *s)
{
    return avio_size(s);
}

NPA_EXPORT int npa_av_index_search_timestamp(AVStream *st, int64_t timestamp, int flags)
{
    unsigned index = 0;
    npa_shadow *s = shadow_of_stream((void *)st, &index);

    if (!s || !s->modern || index >= s->modern->nb_streams)
        return -1;
    return av_index_search_timestamp(s->modern->streams[index], timestamp, flags);
}

/* Packet translation is the next commit; keep forwarding until then. */
__asm__(".globl _npa_av_read_frame\n_npa_av_read_frame:\n\tb _av_read_frame\n");
