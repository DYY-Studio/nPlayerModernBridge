/*
 * Compile-time ABI guard for the ffmpeg-subdecode unit.
 *
 * The unit seats a modern FFmpeg 9.0.2 libavcodec behind the app's 4.4.5 ABI by
 * handing the app legacy-shaped "shadow" structures. There is no legacy header
 * in this translation unit (it would clash with the modern one), so the legacy
 * layout is frozen here as constants and mirrored structs, and the things that
 * can actually drift are asserted:
 *
 *   - the mirrored AVSubtitle/AVSubtitleRect layout against the frozen offsets,
 *     because the shim materialises those field by field;
 *   - that each shadow allocation is large enough for the offsets used.
 *
 * The legacy offsets themselves are not re-derived here; they were frozen from
 * the machine code and the 4.4.5 headers in notes/ida-investigation-2.md and
 * notes/ida-investigation.md section 8.2, and re-confirmed per field in
 * notes/ida-investigation-p2-field-surface.md.
 */

#ifndef NPA_FFMPEG_SUBDEC_ABI_H
#define NPA_FFMPEG_SUBDEC_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

#define NPA_ABI_ASSERT(condition, message) _Static_assert(condition, message)

/* ---- Legacy (FFmpeg 4.4.x) AVCodecContext ---- */
#define NPA_LEGACY_CTX_SIZE 0x438
#define NPA_LEGACY_CTX_CODEC_TYPE 0x00C
#define NPA_LEGACY_CTX_CODEC 0x010 /* const AVCodec *; the app gates decoding on it */
#define NPA_LEGACY_CTX_CODEC_ID 0x018
#define NPA_LEGACY_CTX_CODEC_TAG 0x01C
#define NPA_LEGACY_CTX_BIT_RATE 0x038
#define NPA_LEGACY_CTX_EXTRADATA 0x058
#define NPA_LEGACY_CTX_EXTRADATA_SIZE 0x060
#define NPA_LEGACY_CTX_TIME_BASE 0x064
#define NPA_LEGACY_CTX_WIDTH 0x074
#define NPA_LEGACY_CTX_HEIGHT 0x078
#define NPA_LEGACY_CTX_PIX_FMT 0x088
#define NPA_LEGACY_CTX_GET_FORMAT 0x098
#define NPA_LEGACY_CTX_SAMPLE_ASPECT_RATIO 0x0E8
#define NPA_LEGACY_CTX_COLOR_PRIMARIES 0x174
#define NPA_LEGACY_CTX_COLOR_TRC 0x178
#define NPA_LEGACY_CTX_COLORSPACE 0x17C
#define NPA_LEGACY_CTX_SAMPLE_RATE 0x190
#define NPA_LEGACY_CTX_CHANNELS 0x194
#define NPA_LEGACY_CTX_SAMPLE_FMT 0x198
#define NPA_LEGACY_CTX_BLOCK_ALIGN 0x1A4
#define NPA_LEGACY_CTX_CHANNEL_LAYOUT 0x1B0
#define NPA_LEGACY_CTX_BITS_PER_CODED_SAMPLE 0x2F8
#define NPA_LEGACY_CTX_THREAD_COUNT 0x310
#define NPA_LEGACY_CTX_THREAD_TYPE 0x314
#define NPA_LEGACY_CTX_SKIP_LOOP_FILTER 0x33C
#define NPA_LEGACY_CTX_SKIP_IDCT 0x340
#define NPA_LEGACY_CTX_SKIP_FRAME 0x344
#define NPA_LEGACY_CTX_SUBTITLE_HEADER 0x348
#define NPA_LEGACY_CTX_SUBTITLE_HEADER_SIZE 0x350
#define NPA_LEGACY_CTX_PKT_TIMEBASE 0x374
#define NPA_LEGACY_CTX_SUB_TEXT_FORMAT 0x3F8
#define NPA_LEGACY_CTX_HW_FRAMES_CTX 0x3F0
#define NPA_LEGACY_CTX_HW_DEVICE_CTX 0x408

NPA_ABI_ASSERT(
    NPA_LEGACY_CTX_SIZE > NPA_LEGACY_CTX_HW_DEVICE_CTX,
    "the legacy AVCodecContext shadow is too small for the offsets it is read at"
);

/* ---- Legacy (FFmpeg 4.4.x) AVCodecParameters ---- */
#define NPA_LEGACY_PARAMS_SIZE 0x090
#define NPA_LEGACY_PARAMS_CODEC_TYPE 0x000
#define NPA_LEGACY_PARAMS_CODEC_ID 0x004
#define NPA_LEGACY_PARAMS_CODEC_TAG 0x008
#define NPA_LEGACY_PARAMS_EXTRADATA 0x010
#define NPA_LEGACY_PARAMS_EXTRADATA_SIZE 0x018
#define NPA_LEGACY_PARAMS_FORMAT 0x01C
#define NPA_LEGACY_PARAMS_BIT_RATE 0x020
#define NPA_LEGACY_PARAMS_CHANNEL_LAYOUT 0x068
#define NPA_LEGACY_PARAMS_CHANNELS 0x070
#define NPA_LEGACY_PARAMS_SAMPLE_RATE 0x074

NPA_ABI_ASSERT(
    NPA_LEGACY_PARAMS_SIZE > NPA_LEGACY_PARAMS_SAMPLE_RATE,
    "the legacy AVCodecParameters shadow is too small for the offsets it is read at"
);

/* ---- Legacy (FFmpeg 4.4.x) AVPacket ---- */
/* The unit never allocates a packet; it reads the app's stack-local one, so the
 * struct size is not needed. `av_init_packet` (4.4.5, never redirected) fills
 * these fields. */
#define NPA_LEGACY_PKT_PTS 0x008
#define NPA_LEGACY_PKT_DTS 0x010
#define NPA_LEGACY_PKT_DATA 0x018
#define NPA_LEGACY_PKT_SIZE 0x020
#define NPA_LEGACY_PKT_FLAGS 0x028
#define NPA_LEGACY_PKT_DURATION 0x040

/* ---- Legacy (FFmpeg 4.4.x) AVSubtitle and AVSubtitleRect ---- */
#define NPA_LEGACY_SUB_SIZE 0x020
#define NPA_LEGACY_SUB_FORMAT 0x000
#define NPA_LEGACY_SUB_START_DISPLAY_TIME 0x004
#define NPA_LEGACY_SUB_END_DISPLAY_TIME 0x008
#define NPA_LEGACY_SUB_NUM_RECTS 0x00C
#define NPA_LEGACY_SUB_RECTS 0x010
#define NPA_LEGACY_SUB_PTS 0x018

#define NPA_LEGACY_RECT_SIZE 0x0C8
#define NPA_LEGACY_RECT_X 0x000
#define NPA_LEGACY_RECT_Y 0x004
#define NPA_LEGACY_RECT_W 0x008
#define NPA_LEGACY_RECT_H 0x00C
#define NPA_LEGACY_RECT_NB_COLORS 0x010
#define NPA_LEGACY_RECT_PICT 0x018
#define NPA_LEGACY_RECT_DATA 0x078
#define NPA_LEGACY_RECT_LINESIZE 0x098
#define NPA_LEGACY_RECT_TYPE 0x0A8
#define NPA_LEGACY_RECT_TEXT 0x0B0
#define NPA_LEGACY_RECT_ASS 0x0B8
#define NPA_LEGACY_RECT_FLAGS 0x0C0

/* Mirrors of the two structures the shim materialises field by field. The
 * deprecated AVPicture is present in this build (LIBAVCODEC_VERSION_MAJOR 58,
 * FF_API_AVPICTURE), so the app's data[]/linesize[] sit behind it. */

struct npa_legacy_subtitle {
    uint16_t format;
    uint32_t start_display_time;
    uint32_t end_display_time;
    uint32_t num_rects;
    void *rects;
    int64_t pts;
};

struct npa_legacy_subtitle_rect {
    int x, y, w, h;
    int nb_colors;
    uint64_t pict[12]; /* the deprecated AVPicture: data[8] + linesize[8], 0x60 bytes */
    uint8_t *data[4];
    int linesize[4];
    int type;
    char *text;
    char *ass;
    int flags;
};

NPA_ABI_ASSERT(
    sizeof(struct npa_legacy_subtitle) == NPA_LEGACY_SUB_SIZE,
    "the mirrored AVSubtitle does not match the frozen legacy size"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle, num_rects) == NPA_LEGACY_SUB_NUM_RECTS,
    "the mirrored AVSubtitle.num_rects is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle, rects) == NPA_LEGACY_SUB_RECTS,
    "the mirrored AVSubtitle.rects is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle, pts) == NPA_LEGACY_SUB_PTS,
    "the mirrored AVSubtitle.pts is not at the frozen offset"
);
NPA_ABI_ASSERT(
    sizeof(struct npa_legacy_subtitle_rect) == NPA_LEGACY_RECT_SIZE,
    "the mirrored AVSubtitleRect does not match the frozen legacy size"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, nb_colors) == NPA_LEGACY_RECT_NB_COLORS,
    "the mirrored AVSubtitleRect.nb_colors is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, data) == NPA_LEGACY_RECT_DATA,
    "the mirrored AVSubtitleRect.data is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, linesize) == NPA_LEGACY_RECT_LINESIZE,
    "the mirrored AVSubtitleRect.linesize is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, type) == NPA_LEGACY_RECT_TYPE,
    "the mirrored AVSubtitleRect.type is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, text) == NPA_LEGACY_RECT_TEXT,
    "the mirrored AVSubtitleRect.text is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, ass) == NPA_LEGACY_RECT_ASS,
    "the mirrored AVSubtitleRect.ass is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_subtitle_rect, flags) == NPA_LEGACY_RECT_FLAGS,
    "the mirrored AVSubtitleRect.flags is not at the frozen offset"
);

#endif /* NPA_FFMPEG_SUBDEC_ABI_H */
