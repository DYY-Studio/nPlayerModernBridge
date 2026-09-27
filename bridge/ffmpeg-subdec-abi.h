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
#define NPA_LEGACY_PARAMS_BITS_PER_CODED_SAMPLE 0x028
#define NPA_LEGACY_PARAMS_BITS_PER_RAW_SAMPLE 0x02C
#define NPA_LEGACY_PARAMS_PROFILE 0x030
#define NPA_LEGACY_PARAMS_LEVEL 0x034
#define NPA_LEGACY_PARAMS_WIDTH 0x038
#define NPA_LEGACY_PARAMS_HEIGHT 0x03C
#define NPA_LEGACY_PARAMS_SAMPLE_ASPECT_RATIO 0x040
#define NPA_LEGACY_PARAMS_FIELD_ORDER 0x048
#define NPA_LEGACY_PARAMS_COLOR_RANGE 0x04C
#define NPA_LEGACY_PARAMS_COLOR_PRIMARIES 0x050
#define NPA_LEGACY_PARAMS_COLOR_TRC 0x054
#define NPA_LEGACY_PARAMS_COLORSPACE 0x058
#define NPA_LEGACY_PARAMS_CHROMA_LOCATION 0x05C
#define NPA_LEGACY_PARAMS_VIDEO_DELAY 0x060
#define NPA_LEGACY_PARAMS_CHANNEL_LAYOUT 0x068
#define NPA_LEGACY_PARAMS_CHANNELS 0x070
#define NPA_LEGACY_PARAMS_SAMPLE_RATE 0x074
#define NPA_LEGACY_PARAMS_BLOCK_ALIGN 0x078
#define NPA_LEGACY_PARAMS_FRAME_SIZE 0x07C
#define NPA_LEGACY_PARAMS_INITIAL_PADDING 0x080
#define NPA_LEGACY_PARAMS_TRAILING_PADDING 0x084
#define NPA_LEGACY_PARAMS_SEEK_PREROLL 0x088

/* The playback unit carries real video and audio parameters through the app,
 * so the whole 4.4 layout is pinned here rather than only the fields the
 * subtitle unit needed. */
struct npa_legacy_params {
    int codec_type;                  /* 0x000 */
    int codec_id;                    /* 0x004 */
    uint32_t codec_tag;              /* 0x008 */
    void *extradata;                 /* 0x010 */
    int extradata_size;              /* 0x018 */
    int format;                      /* 0x01C */
    int64_t bit_rate;                /* 0x020 */
    int bits_per_coded_sample;       /* 0x028 */
    int bits_per_raw_sample;         /* 0x02C */
    int profile;                     /* 0x030 */
    int level;                       /* 0x034 */
    int width;                       /* 0x038 */
    int height;                      /* 0x03C */
    AVRational sample_aspect_ratio;  /* 0x040 */
    int field_order;                 /* 0x048 */
    int color_range;                 /* 0x04C */
    int color_primaries;             /* 0x050 */
    int color_trc;                   /* 0x054 */
    int color_space;                 /* 0x058 */
    int chroma_location;             /* 0x05C */
    int video_delay;                 /* 0x060 */
    uint64_t channel_layout;         /* 0x068 */
    int channels;                    /* 0x070 */
    int sample_rate;                 /* 0x074 */
    int block_align;                 /* 0x078 */
    int frame_size;                  /* 0x07C */
    int initial_padding;             /* 0x080 */
    int trailing_padding;            /* 0x084 */
    int seek_preroll;                /* 0x088 */
};

NPA_ABI_ASSERT(
    sizeof(struct npa_legacy_params) == NPA_LEGACY_PARAMS_SIZE,
    "the mirrored AVCodecParameters does not match the frozen legacy size"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, extradata) == NPA_LEGACY_PARAMS_EXTRADATA,
    "the mirrored AVCodecParameters.extradata is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, width) == NPA_LEGACY_PARAMS_WIDTH,
    "the mirrored AVCodecParameters.width is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, color_range) == NPA_LEGACY_PARAMS_COLOR_RANGE,
    "the mirrored AVCodecParameters.color_range is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, channel_layout) == NPA_LEGACY_PARAMS_CHANNEL_LAYOUT,
    "the mirrored AVCodecParameters.channel_layout is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, channels) == NPA_LEGACY_PARAMS_CHANNELS,
    "the mirrored AVCodecParameters.channels is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_params, sample_rate) == NPA_LEGACY_PARAMS_SAMPLE_RATE,
    "the mirrored AVCodecParameters.sample_rate is not at the frozen offset"
);

NPA_ABI_ASSERT(
    NPA_LEGACY_PARAMS_SIZE > NPA_LEGACY_PARAMS_SEEK_PREROLL,
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
#define NPA_LEGACY_PKT_STREAM_INDEX 0x024
#define NPA_LEGACY_PKT_FLAGS 0x028
#define NPA_LEGACY_PKT_DURATION 0x040
#define NPA_LEGACY_PKT_POS 0x048

/* ---- Legacy (FFmpeg 4.4.x) AVFrame ---- */
/* The app allocates, refs and releases its frames with 4.4.5 av_frame_* (none
 * of those calls move), so a decoded frame has to be materialised into the
 * legacy layout and its buffers handed over as legacy AVBufferRefs. Offsets
 * derived from the 4.4.5 libavutil/frame.h field order and confirmed against
 * every frame read in notes/ida-investigation-p2-field-surface.md section 2
 * (format 0x74, interlaced 0xFC, best_effort 0x198, pkt_duration 0x1A8, pts
 * 0x88, extended_data 0x60, nb_samples 0x70, channels 0x1BC). */
#define NPA_LEGACY_FRAME_SIZE 0x218
#define NPA_LEGACY_FRAME_DATA 0x000 /* uint8_t *data[8] */
#define NPA_LEGACY_FRAME_LINESIZE 0x040 /* int linesize[8] */
#define NPA_LEGACY_FRAME_EXTENDED_DATA 0x060
#define NPA_LEGACY_FRAME_WIDTH 0x068
#define NPA_LEGACY_FRAME_HEIGHT 0x06C
#define NPA_LEGACY_FRAME_NB_SAMPLES 0x070
#define NPA_LEGACY_FRAME_FORMAT 0x074
#define NPA_LEGACY_FRAME_KEY_FRAME 0x078
#define NPA_LEGACY_FRAME_PICT_TYPE 0x07C
#define NPA_LEGACY_FRAME_SAMPLE_ASPECT_RATIO 0x080
#define NPA_LEGACY_FRAME_PTS 0x088
#define NPA_LEGACY_FRAME_PKT_PTS 0x090
#define NPA_LEGACY_FRAME_PKT_DTS 0x098
#define NPA_LEGACY_FRAME_REPEAT_PICT 0x0F8
#define NPA_LEGACY_FRAME_INTERLACED 0x0FC
#define NPA_LEGACY_FRAME_TOP_FIELD_FIRST 0x100
#define NPA_LEGACY_FRAME_REORDERED_OPAQUE 0x108
#define NPA_LEGACY_FRAME_SAMPLE_RATE 0x110
#define NPA_LEGACY_FRAME_CHANNEL_LAYOUT 0x118
#define NPA_LEGACY_FRAME_BUF 0x120 /* AVBufferRef *buf[8] */
#define NPA_LEGACY_FRAME_EXTENDED_BUF 0x160
#define NPA_LEGACY_FRAME_NB_EXTENDED_BUF 0x168
#define NPA_LEGACY_FRAME_SIDE_DATA 0x170
#define NPA_LEGACY_FRAME_NB_SIDE_DATA 0x178
#define NPA_LEGACY_FRAME_FLAGS 0x17C
#define NPA_LEGACY_FRAME_COLOR_RANGE 0x180
#define NPA_LEGACY_FRAME_COLOR_PRIMARIES 0x184
#define NPA_LEGACY_FRAME_COLOR_TRC 0x188
#define NPA_LEGACY_FRAME_COLORSPACE 0x18C
#define NPA_LEGACY_FRAME_CHROMA_LOCATION 0x190
#define NPA_LEGACY_FRAME_BEST_EFFORT_TIMESTAMP 0x198
#define NPA_LEGACY_FRAME_PKT_POS 0x1A0
#define NPA_LEGACY_FRAME_PKT_DURATION 0x1A8
#define NPA_LEGACY_FRAME_METADATA 0x1B0
#define NPA_LEGACY_FRAME_DECODE_ERROR_FLAGS 0x1B8
#define NPA_LEGACY_FRAME_CHANNELS 0x1BC
#define NPA_LEGACY_FRAME_PKT_SIZE 0x1C0
#define NPA_LEGACY_FRAME_HW_FRAMES_CTX 0x1E0

/* The two frame flags that mean the same in 4.4 and 9.0.2 (verified against
 * both libavutil/frame.h: CORRUPT is bit 0 and DISCARD is bit 2 in each). The
 * flags 9.0.2 added (key, interlaced, top-field-first) live in the same field
 * there but as *different bits*, so the frame materialisation translates them
 * into the 4.4 fields instead of copying the word. */
#define NPA_LEGACY_FRAME_FLAG_CORRUPT (1 << 0)
#define NPA_LEGACY_FRAME_FLAG_DISCARD (1 << 2)

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

/* The frame mirror exists to pin the offsets above against the 4.4.5 field
 * order; the shim materialises a subset of these field by field, but a drift
 * anywhere in the order would move every later offset. */
struct npa_legacy_frame {
    uint8_t *data[8];               /* 0x000 */
    int linesize[8];                /* 0x040 */
    uint8_t **extended_data;        /* 0x060 */
    int width, height;              /* 0x068, 0x06C */
    int nb_samples;                 /* 0x070 */
    int format;                     /* 0x074 */
    int key_frame;                  /* 0x078 */
    int pict_type;                  /* 0x07C */
    AVRational sample_aspect_ratio; /* 0x080 */
    int64_t pts;                    /* 0x088 */
    int64_t pkt_pts;                /* 0x090 */
    int64_t pkt_dts;                /* 0x098 */
    int coded_picture_number;       /* 0x0A0 */
    int display_picture_number;     /* 0x0A4 */
    int quality;                    /* 0x0A8 */
    void *opaque;                   /* 0x0B0 */
    uint64_t error[8];              /* 0x0B8 */
    int repeat_pict;                /* 0x0F8 */
    int interlaced_frame;           /* 0x0FC */
    int top_field_first;            /* 0x100 */
    int palette_has_changed;        /* 0x104 */
    int64_t reordered_opaque;       /* 0x108 */
    int sample_rate;                /* 0x110 */
    uint64_t channel_layout;        /* 0x118 */
    void *buf[8];                   /* 0x120 */
    void *extended_buf;             /* 0x160 */
    int nb_extended_buf;            /* 0x168 */
    void *side_data;                /* 0x170 */
    int nb_side_data;               /* 0x178 */
    int flags;                      /* 0x17C */
    int color_range;                /* 0x180 */
    int color_primaries;            /* 0x184 */
    int color_trc;                  /* 0x188 */
    int colorspace;                 /* 0x18C */
    int chroma_location;            /* 0x190 */
    int64_t best_effort_timestamp;  /* 0x198 */
    int64_t pkt_pos;                /* 0x1A0 */
    int64_t pkt_duration;           /* 0x1A8 */
    void *metadata;                 /* 0x1B0 */
    int decode_error_flags;         /* 0x1B8 */
    int channels;                   /* 0x1BC */
    int pkt_size;                   /* 0x1C0 */
    int8_t *qscale_table;           /* 0x1C8 */
    int qstride;                    /* 0x1D0 */
    int qscale_type;                /* 0x1D4 */
    void *qp_table_buf;             /* 0x1D8 */
    void *hw_frames_ctx;            /* 0x1E0 */
    void *opaque_ref;               /* 0x1E8 */
    size_t crop_top;                /* 0x1F0 */
    size_t crop_bottom;             /* 0x1F8 */
    size_t crop_left;               /* 0x200 */
    size_t crop_right;              /* 0x208 */
    void *private_ref;              /* 0x210 */
};

NPA_ABI_ASSERT(
    sizeof(struct npa_legacy_frame) == NPA_LEGACY_FRAME_SIZE,
    "the mirrored AVFrame does not match the frozen legacy size"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, linesize) == NPA_LEGACY_FRAME_LINESIZE,
    "the mirrored AVFrame.linesize is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, extended_data) == NPA_LEGACY_FRAME_EXTENDED_DATA,
    "the mirrored AVFrame.extended_data is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, width) == NPA_LEGACY_FRAME_WIDTH,
    "the mirrored AVFrame.width is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, format) == NPA_LEGACY_FRAME_FORMAT,
    "the mirrored AVFrame.format is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, pts) == NPA_LEGACY_FRAME_PTS,
    "the mirrored AVFrame.pts is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, interlaced_frame) == NPA_LEGACY_FRAME_INTERLACED,
    "the mirrored AVFrame.interlaced_frame is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, sample_rate) == NPA_LEGACY_FRAME_SAMPLE_RATE,
    "the mirrored AVFrame.sample_rate is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, channel_layout) == NPA_LEGACY_FRAME_CHANNEL_LAYOUT,
    "the mirrored AVFrame.channel_layout is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, buf) == NPA_LEGACY_FRAME_BUF,
    "the mirrored AVFrame.buf is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, best_effort_timestamp) ==
        NPA_LEGACY_FRAME_BEST_EFFORT_TIMESTAMP,
    "the mirrored AVFrame.best_effort_timestamp is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, pkt_duration) == NPA_LEGACY_FRAME_PKT_DURATION,
    "the mirrored AVFrame.pkt_duration is not at the frozen offset"
);
NPA_ABI_ASSERT(
    offsetof(struct npa_legacy_frame, channels) == NPA_LEGACY_FRAME_CHANNELS,
    "the mirrored AVFrame.channels is not at the frozen offset"
);

#endif /* NPA_FFMPEG_SUBDEC_ABI_H */
