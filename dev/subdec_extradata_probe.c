/* Real bridge/FFmpeg lifetime probe; included only in the test dylib. */
#include "npa_ffmpeg_core902_bridge.c"
#include <stdio.h>
#include <malloc/malloc.h>

static int check_context(AVCodecContext *shadow, const char *step)
{
    npa_ctx_shadow *entry = npa_ctx_find(shadow);
    const void *extra = NPA_LD(shadow, NPA_LEGACY_CTX_EXTRADATA, void *);
    if (extra != entry->modern->extradata) {
        fprintf(stderr, "%s: shadow extradata no longer aliases its owner\n", step);
        return 1;
    }
    if (entry->modern->extradata_size != 279988 ||
        ((const unsigned char *)extra)[0] != 0x5a ||
        ((const unsigned char *)extra)[279987] != 0x5a) {
        fprintf(stderr, "%s: extradata contents were lost\n", step);
        return 1;
    }
    return 0;
}


/* Check allocation capacity before reading padding; the old copies are too small. */
static int check_padding(const unsigned char *data, int size)
{
    if (!data || malloc_size(data) < (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE) {
        fprintf(stderr, "buffer lacks capacity for FFmpeg padding\n");
        return 1;
    }
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++) {
        if (data[size + i]) {
            fprintf(stderr, "FFmpeg padding is not zero\n");
            return 1;
        }
    }
    return 0;
}

static int probe_padding(int mode)
{
    unsigned char source[65];
    const unsigned char *copy = NULL;
    int size = sizeof(source), result;
    AVCodecContext *ctx = avcodec_alloc_context3(NULL);
    AVCodecParameters *par = avcodec_parameters_alloc();
    unsigned char shadow[NPA_LEGACY_PARAMS_SIZE] = {0};
    unsigned char packet[NPA_LEGACY_PACKET_SIZE] = {0};
    npa_shadow *format = NULL;
    AVPacket modern_packet = {0};

    memset(source, 0x5a, sizeof(source));
    if (mode == 5) {
        npa_ctx_set_extradata(ctx, source, size);
        copy = ctx->extradata;
    } else if (mode == 6) {
        NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA, void *, source);
        NPA_ST(shadow, NPA_LEGACY_PARAMS_EXTRADATA_SIZE, int, size);
        npa_params_in(par, shadow);
        copy = par->extradata;
    } else if (mode == 7 || mode == 8) {
        if (mode == 7)
            source[0] = 0x81; /* Existing AV1 record. */
        copy = npa_av1_extradata_modern(source, sizeof(source), 0, 0,
                                       AV_PIX_FMT_YUV420P, &size);
        if (size != (int)sizeof(source) + (mode == 8 ? 4 : 0))
            abort();
    } else if (mode == 9) {
        AVFormatContext *modern = avformat_alloc_context();
        AVStream *stream = avformat_new_stream(modern, NULL);
        stream->codecpar->extradata = av_mallocz(sizeof(source) + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(stream->codecpar->extradata, source, sizeof(source));
        stream->codecpar->extradata_size = sizeof(source);
        format = shadow_create(modern);
        shadow_rebuild_streams(format);
        void *params = npa_ld_ptr(format->streams[0], NPA_LEGACY_STREAM_CODECPAR);
        copy = npa_ld_ptr(params, NPA_LEGACY_CODECPAR_EXTRADATA);
        if (npa_ld_u32(params, NPA_LEGACY_CODECPAR_EXTRADATA_SIZE) != sizeof(source))
            abort();
    } else if (mode == 10) {
        modern_packet.data = source;
        modern_packet.size = sizeof(source);
        if (legacy_packet_from(packet, &modern_packet) < 0)
            abort();
        copy = npa_ld_ptr(packet, NPA_LEGACY_PACKET_DATA);
        if (npa_ld_u32(packet, NPA_LEGACY_PACKET_SIZE_FIELD) != sizeof(source))
            abort();
    } else {
        abort();
    }
    result = check_padding(copy, size);
    if (memcmp(copy + (mode == 8 ? 4 : 0), source, sizeof(source))) {
        fprintf(stderr, "copied payload changed\n");
        result = 1;
    }
    if (mode == 7 || mode == 8) {
        int legacy_size;
        const void *legacy = npa_av1_extradata_legacy(copy, size, &legacy_size);
        if (legacy != copy + 4 || legacy_size != size - 4)
            abort();
        av_free((void *)copy);
    }
    if (format) {
        avformat_free_context(format->modern);
        shadow_destroy(format);
    }
    if (mode == 10)
        legacy_packet_release(packet);
    avcodec_free_context(&ctx);
    avcodec_parameters_free(&par);
    return result;
}

NPA_EXPORT int npa_probe_extradata(int mode)
{
    if (mode >= 5)
        return probe_padding(mode);
    AVCodecContext *ctx = npa_shadow_avcodec_alloc_context3(NULL);
    AVCodecParameters *par = npa_shadow_avcodec_parameters_alloc();
    const AVCodec *codec = npa_shadow_avcodec_find_decoder(0x17006);
    unsigned char *extra = av_mallocz(279988 + AV_INPUT_BUFFER_PADDING_SIZE);
    int result = 0;

    if (!ctx || !par || !codec || !extra)
        abort();
    memset(extra, 0x5a, 279988);
    NPA_ST(ctx, NPA_LEGACY_CTX_CODEC_TYPE, int, AVMEDIA_TYPE_SUBTITLE);
    NPA_ST(ctx, NPA_LEGACY_CTX_CODEC_ID, int, 0x17006);
    NPA_ST(ctx, NPA_LEGACY_CTX_EXTRADATA, void *, extra);
    NPA_ST(ctx, NPA_LEGACY_CTX_EXTRADATA_SIZE, int, 279988);
    NPA_ST(ctx, NPA_LEGACY_CTX_PKT_TIMEBASE, AVRational, ((AVRational){1, 1000}));
    if (npa_shadow_avcodec_open2(ctx, codec, NULL) < 0)
        abort();
    av_free(extra);

    if (mode == 0) {
        /* Match the app's write after open2. */
        NPA_ST(ctx, NPA_LEGACY_CTX_PKT_TIMEBASE, AVRational, ((AVRational){1, 10000}));
        /* Presentation with a 1920x1080 canvas and no objects, then display.
         * The second packet clears using the cached presentation state. */
        unsigned char presentation[] = {
            0x16, 0, 11, 0x07, 0x80, 0x04, 0x38, 0, 0, 0, 0, 0, 0, 0,
            0x80, 0, 0
        };
        unsigned char display[] = {0x80, 0, 0};
        unsigned char packet[NPA_LEGACY_PACKET_SIZE] = {0};
        struct npa_legacy_subtitle sub = {0};
        for (int i = 0; i < 3; i++) {
            int got = 0;
            void *data = i ? (void *)display : (void *)presentation;
            NPA_ST(packet, NPA_LEGACY_PKT_DATA, void *, data);
            NPA_ST(packet, NPA_LEGACY_PKT_SIZE, int,
                   i ? (int)sizeof(display) : (int)sizeof(presentation));
            NPA_ST(packet, NPA_LEGACY_PKT_PTS, int64_t, i * 1000);
            if (npa_subdec_avcodec_decode_subtitle2(ctx, (AVSubtitle *)&sub,
                                                   &got, (AVPacket *)packet) < 0 || !got)
                abort();
            if (sub.pts != i * 100000) {
                fprintf(stderr, "subtitle decode: post-open timebase was ignored\n");
                result = 1;
            }
            npa_subdec_avsubtitle_free((AVSubtitle *)&sub);
            if (result)
                break;
            result = check_context(ctx, "subtitle decode");
            if (result)
                break;
            npa_ctx_shadow *entry = npa_ctx_find(ctx);
            if (entry->modern->width != 1920 || entry->modern->height != 1080) {
                fprintf(stderr, "subtitle decode: decoder state was overwritten\n");
                result = 1;
                break;
            }
        }
    } else if (mode == 1) {
        for (int i = 0; i < 3 && !result; i++) {
            if (npa_shadow_avcodec_parameters_from_context(par, ctx) < 0)
                abort();
            result = check_context(ctx, "parameters_from_context");
        }
    } else if (mode == 2) {
        /* Reuse source parameters across independent destination conversions. */
        AVCodecContext *destination = npa_shadow_avcodec_alloc_context3(NULL);
        if (npa_shadow_avcodec_parameters_from_context(par, ctx) < 0)
            abort();
        for (int i = 0; i < 3 && !result; i++) {
            if (npa_shadow_avcodec_parameters_to_context(destination, par) < 0)
                abort();
            npa_params_shadow *entry = npa_params_find(par);
            if (NPA_LD(par, NPA_LEGACY_PARAMS_EXTRADATA, void *) != entry->modern->extradata) {
                fprintf(stderr, "parameters_to_context: source shadow lost its owner\n");
                result = 1;
            } else {
                result = check_context(destination, "parameters_to_context destination");
            }
        }
        npa_shadow_avcodec_free_context(&destination);
    } else if (mode == 3) {
        NPA_ST(ctx, NPA_LEGACY_CTX_SKIP_FRAME, int, AVDISCARD_NONKEY);
        for (int i = 0; i < 3 && !result; i++) {
            npa_codec_avcodec_flush_buffers(ctx);
            result = check_context(ctx, "flush");
            if (npa_ctx_find(ctx)->modern->skip_frame != AVDISCARD_NONKEY) {
                fprintf(stderr, "flush: app discard setting was ignored\n");
                result = 1;
            }
        }
    } else if (mode == 4) {
        if (npa_codec_avcodec_close(ctx) < 0)
            abort();
        result = check_context(ctx, "close");
    } else {
        abort();
    }
    npa_shadow_avcodec_parameters_free(&par);
    npa_shadow_avcodec_free_context(&ctx);
    return result;
}
