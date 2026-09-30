/* Real bridge/FFmpeg lifetime probe; included only in the test dylib. */
#include "npa_ffmpeg_core902_bridge.c"
#include <stdio.h>

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

NPA_EXPORT int npa_probe_extradata(int mode)
{
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
