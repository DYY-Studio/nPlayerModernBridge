/*
 * Compile-time ABI guard for the ffmpeg-demux unit.
 *
 * The unit seats a modern FFmpeg 9.0.2 libavformat behind the app's 4.4.5
 * ABI by handing the app legacy-shaped "shadow" structures. There is no
 * legacy header in this translation unit (it would clash with the modern
 * one), so the legacy layout is frozen here as constants and mirrored
 * structs, and only the things that can actually drift are asserted:
 *
 *   - the legacy AVBuffer/AVBufferRef layout, which the shim builds by hand
 *     so the app's 4.4 av_packet_unref can free a packet this unit produced.
 *     AVBuffer embeds `buffer_size_t`, which is `int` while
 *     FF_API_BUFFER_SIZE_T holds (libavutil < 57) and `size_t` after, so the
 *     4.4 and 9.0 layouts really do differ (internal fields shift by 8);
 *   - the modern fields the shim reads and writes, against the closure's
 *     headers;
 *   - that each shadow allocation is large enough for the offsets used.
 *
 * The legacy offsets themselves are not re-derived here; they were frozen
 * from the machine code and the 4.4.8 headers in the spike (see
 * notes/ida-investigation.md §8.2 and the class-A demux plan).
 */

#ifndef NPA_FFMPEG_DEMUX_ABI_H
#define NPA_FFMPEG_DEMUX_ABI_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>

#define NPA_ABI_ASSERT(condition, message) _Static_assert(condition, message)

/* ---- Legacy (FFmpeg 4.4.x) AVFormatContext ---- */
#define NPA_LEGACY_FMT_SIZE 0x5e0
#define NPA_LEGACY_FMT_IFORMAT 0x008
#define NPA_LEGACY_FMT_PB 0x020
#define NPA_LEGACY_FMT_NB_STREAMS 0x02c
#define NPA_LEGACY_FMT_STREAMS 0x030
#define NPA_LEGACY_FMT_START_TIME 0x440
#define NPA_LEGACY_FMT_DURATION 0x448
#define NPA_LEGACY_FMT_MAX_DELAY 0x45c
#define NPA_LEGACY_FMT_FLAGS 0x460
#define NPA_LEGACY_FMT_NB_CHAPTERS 0x4a4
#define NPA_LEGACY_FMT_CHAPTERS 0x4a8
#define NPA_LEGACY_FMT_METADATA 0x4b0
#define NPA_LEGACY_FMT_ERROR_RECOGNITION 0x4c4
#define NPA_LEGACY_FMT_INTERRUPT_CALLBACK 0x4c8
#define NPA_LEGACY_FMT_INTERRUPT_OPAQUE 0x4d0

/* ---- Legacy AVStream ---- */
#define NPA_LEGACY_STREAM_SIZE 0x1f0
#define NPA_LEGACY_STREAM_INDEX 0x000
#define NPA_LEGACY_STREAM_ID 0x004
#define NPA_LEGACY_STREAM_CODEC 0x008
#define NPA_LEGACY_STREAM_TIME_BASE 0x018
#define NPA_LEGACY_STREAM_START_TIME 0x020
#define NPA_LEGACY_STREAM_DURATION 0x028
#define NPA_LEGACY_STREAM_NB_FRAMES 0x030
#define NPA_LEGACY_STREAM_DISPOSITION 0x038
#define NPA_LEGACY_STREAM_DISCARD 0x03c
#define NPA_LEGACY_STREAM_SAMPLE_ASPECT_RATIO 0x040
#define NPA_LEGACY_STREAM_METADATA 0x048
#define NPA_LEGACY_STREAM_AVG_FRAME_RATE 0x050
#define NPA_LEGACY_STREAM_ATTACHED_PIC 0x058
#define NPA_LEGACY_STREAM_CODECPAR 0x0d0

/* ---- Legacy AVCodecParameters ---- */
#define NPA_LEGACY_CODECPAR_SIZE 0x090
#define NPA_LEGACY_CODECPAR_CODEC_TYPE 0x000
#define NPA_LEGACY_CODECPAR_CODEC_ID 0x004
#define NPA_LEGACY_CODECPAR_CODEC_TAG 0x008
#define NPA_LEGACY_CODECPAR_EXTRADATA 0x010
#define NPA_LEGACY_CODECPAR_EXTRADATA_SIZE 0x018
#define NPA_LEGACY_CODECPAR_FORMAT 0x01c
#define NPA_LEGACY_CODECPAR_BIT_RATE 0x020
#define NPA_LEGACY_CODECPAR_BITS_PER_CODED_SAMPLE 0x028
#define NPA_LEGACY_CODECPAR_BITS_PER_RAW_SAMPLE 0x02c
#define NPA_LEGACY_CODECPAR_PROFILE 0x030
#define NPA_LEGACY_CODECPAR_LEVEL 0x034
#define NPA_LEGACY_CODECPAR_WIDTH 0x038
#define NPA_LEGACY_CODECPAR_HEIGHT 0x03c
#define NPA_LEGACY_CODECPAR_SAMPLE_ASPECT_RATIO 0x040
#define NPA_LEGACY_CODECPAR_FIELD_ORDER 0x048
#define NPA_LEGACY_CODECPAR_COLOR_RANGE 0x04c
#define NPA_LEGACY_CODECPAR_COLOR_PRIMARIES 0x050
#define NPA_LEGACY_CODECPAR_COLOR_TRC 0x054
#define NPA_LEGACY_CODECPAR_COLOR_SPACE 0x058
#define NPA_LEGACY_CODECPAR_CHROMA_LOCATION 0x05c
#define NPA_LEGACY_CODECPAR_VIDEO_DELAY 0x060
#define NPA_LEGACY_CODECPAR_CHANNEL_LAYOUT 0x068
#define NPA_LEGACY_CODECPAR_CHANNELS 0x070
#define NPA_LEGACY_CODECPAR_SAMPLE_RATE 0x074
#define NPA_LEGACY_CODECPAR_BLOCK_ALIGN 0x078
#define NPA_LEGACY_CODECPAR_FRAME_SIZE 0x07c

/* ---- Legacy AVPacket ---- */
#define NPA_LEGACY_PACKET_SIZE 0x058
#define NPA_LEGACY_PACKET_BUF 0x000
#define NPA_LEGACY_PACKET_PTS 0x008
#define NPA_LEGACY_PACKET_DTS 0x010
#define NPA_LEGACY_PACKET_DATA 0x018
#define NPA_LEGACY_PACKET_SIZE_FIELD 0x020
#define NPA_LEGACY_PACKET_STREAM_INDEX 0x024
#define NPA_LEGACY_PACKET_FLAGS 0x028
#define NPA_LEGACY_PACKET_SIDE_DATA 0x030
#define NPA_LEGACY_PACKET_SIDE_DATA_ELEMS 0x038
#define NPA_LEGACY_PACKET_DURATION 0x040
#define NPA_LEGACY_PACKET_POS 0x048
#define NPA_LEGACY_PACKET_CONVERGENCE_DURATION 0x050

/* ---- Legacy AVChapter (mirrored; the modern id is int64_t so time_base moved) ---- */
#define NPA_LEGACY_CHAPTER_SIZE 0x028
#define NPA_LEGACY_CHAPTER_ID 0x000
#define NPA_LEGACY_CHAPTER_TIME_BASE 0x004
#define NPA_LEGACY_CHAPTER_START 0x010
#define NPA_LEGACY_CHAPTER_END 0x018
#define NPA_LEGACY_CHAPTER_METADATA 0x020

/*
 * The 4.4 AVBuffer, mirrored. The app's av_packet_unref unrefs the packet's
 * `buf` through this layout, so the shim must hand it a buffer built to
 * these offsets even though it links the modern avutil.
 */
struct npa_legacy_avbuffer {
    uint8_t *data;
    int size;
    _Atomic uint32_t refcount;
    void (*free)(void *opaque, uint8_t *data);
    void *opaque;
    int flags;
    int flags_internal;
};

struct npa_legacy_avbuffer_ref {
    struct npa_legacy_avbuffer *buffer;
    uint8_t *data;
    int size;
};

NPA_ABI_ASSERT(offsetof(struct npa_legacy_avbuffer, size) == 0x08, "legacy AVBuffer.size moved");
NPA_ABI_ASSERT(offsetof(struct npa_legacy_avbuffer, refcount) == 0x0c, "legacy AVBuffer.refcount moved");
NPA_ABI_ASSERT(offsetof(struct npa_legacy_avbuffer, free) == 0x10, "legacy AVBuffer.free moved");
NPA_ABI_ASSERT(offsetof(struct npa_legacy_avbuffer, opaque) == 0x18, "legacy AVBuffer.opaque moved");
NPA_ABI_ASSERT(sizeof(struct npa_legacy_avbuffer) == 0x28, "legacy AVBuffer size moved");
NPA_ABI_ASSERT(offsetof(struct npa_legacy_avbuffer_ref, data) == 0x08, "legacy AVBufferRef.data moved");
NPA_ABI_ASSERT(sizeof(struct npa_legacy_avbuffer_ref) == 0x18, "legacy AVBufferRef size moved");

/* Shadow allocations must cover every offset the app reads or the shim writes. */
NPA_ABI_ASSERT(NPA_LEGACY_FMT_SIZE > NPA_LEGACY_FMT_INTERRUPT_OPAQUE, "shadow AVFormatContext too small");
NPA_ABI_ASSERT(NPA_LEGACY_STREAM_SIZE > NPA_LEGACY_STREAM_CODECPAR, "shadow AVStream too small");
NPA_ABI_ASSERT(NPA_LEGACY_CODECPAR_SIZE > NPA_LEGACY_CODECPAR_FRAME_SIZE, "shadow AVCodecParameters too small");
NPA_ABI_ASSERT(NPA_LEGACY_PACKET_SIZE > NPA_LEGACY_PACKET_CONVERGENCE_DURATION, "shadow AVPacket too small");
NPA_ABI_ASSERT(NPA_LEGACY_CHAPTER_SIZE > NPA_LEGACY_CHAPTER_METADATA, "shadow AVChapter too small");

/* ---- Modern fields the shim reads/writes ---- */
NPA_ABI_ASSERT(offsetof(AVFormatContext, pb) == 0x20, "modern AVFormatContext.pb moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, nb_streams) == 0x2c, "modern AVFormatContext.nb_streams moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, streams) == 0x30, "modern AVFormatContext.streams moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, interrupt_callback.callback) == 0xd8, "modern interrupt_callback.callback moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, interrupt_callback.opaque) == 0xe0, "modern interrupt_callback.opaque moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, error_recognition) == 0xd4, "modern error_recognition moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, flags) == 0x80, "modern AVFormatContext.flags moved");
NPA_ABI_ASSERT(offsetof(AVFormatContext, max_delay) == 0x7c, "modern max_delay moved");

NPA_ABI_ASSERT(offsetof(AVStream, index) == 0x08, "modern AVStream.index moved");
NPA_ABI_ASSERT(offsetof(AVStream, codecpar) == 0x10, "modern AVStream.codecpar moved");
NPA_ABI_ASSERT(offsetof(AVStream, discard) == 0x44, "modern AVStream.discard moved");
NPA_ABI_ASSERT(offsetof(AVStream, attached_pic) == 0x60, "modern AVStream.attached_pic moved");

NPA_ABI_ASSERT(offsetof(AVCodecParameters, format) == 0x2c, "modern AVCodecParameters.format moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, bit_rate) == 0x30, "modern AVCodecParameters.bit_rate moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, bits_per_coded_sample) == 0x38, "modern bits_per_coded_sample moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, bits_per_raw_sample) == 0x3c, "modern bits_per_raw_sample moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, profile) == 0x40, "modern AVCodecParameters.profile moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, level) == 0x44, "modern AVCodecParameters.level moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, width) == 0x48, "modern AVCodecParameters.width moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, height) == 0x4c, "modern AVCodecParameters.height moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, color_range) == 0x64, "modern AVCodecParameters.color_range moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, chroma_location) == 0x74, "modern chroma_location moved");
NPA_ABI_ASSERT(offsetof(AVCodecParameters, ch_layout) == 0x80, "modern AVCodecParameters.ch_layout moved");

NPA_ABI_ASSERT(sizeof(AVPacket) == 0x68, "modern AVPacket size moved");
NPA_ABI_ASSERT(offsetof(AVPacket, duration) == 0x40, "modern AVPacket.duration moved");
NPA_ABI_ASSERT(offsetof(AVPacket, time_base) == 0x60, "modern AVPacket.time_base moved");

NPA_ABI_ASSERT(offsetof(AVIOContext, buffer) == 0x08, "modern AVIOContext.buffer moved");
/*
 * The shim hands the app the modern AVIOContext, so only the front fields are
 * valid at legacy offsets. Fields past these (write_flag, error, direct) have a
 * different offset and must not be read by the app.
 */
NPA_ABI_ASSERT(offsetof(AVIOContext, buf_ptr) == 0x18, "modern AVIOContext.buf_ptr moved");
NPA_ABI_ASSERT(offsetof(AVIOContext, opaque) == 0x28, "modern AVIOContext.opaque moved");
NPA_ABI_ASSERT(offsetof(AVIOContext, pos) == 0x48, "modern AVIOContext.pos moved");
NPA_ABI_ASSERT(offsetof(AVIOContext, eof_reached) == 0x50, "modern AVIOContext.eof_reached moved");

NPA_ABI_ASSERT(offsetof(AVChapter, id) == 0x0, "modern AVChapter.id moved");
NPA_ABI_ASSERT(offsetof(AVChapter, time_base) == 0x8, "modern AVChapter.time_base moved");
NPA_ABI_ASSERT(offsetof(AVChapter, start) == 0x10, "modern AVChapter.start moved");
NPA_ABI_ASSERT(offsetof(AVChapter, end) == 0x18, "modern AVChapter.end moved");
NPA_ABI_ASSERT(offsetof(AVChapter, metadata) == 0x20, "modern AVChapter.metadata moved");

/* ---- Shadow field writers (the shadow is untyped legacy memory) ---- */
static inline void npa_st_u32(void *shadow, size_t offset, uint32_t value)
{
    memcpy((char *)shadow + offset, &value, sizeof(value));
}

static inline void npa_st_u64(void *shadow, size_t offset, uint64_t value)
{
    memcpy((char *)shadow + offset, &value, sizeof(value));
}

static inline void npa_st_ptr(void *shadow, size_t offset, const void *value)
{
    memcpy((char *)shadow + offset, &value, sizeof(value));
}

static inline uint32_t npa_ld_u32(const void *shadow, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const char *)shadow + offset, sizeof(value));
    return value;
}

static inline uint64_t npa_ld_u64(const void *shadow, size_t offset)
{
    uint64_t value;
    memcpy(&value, (const char *)shadow + offset, sizeof(value));
    return value;
}

static inline void *npa_ld_ptr(const void *shadow, size_t offset)
{
    void *value;
    memcpy(&value, (const char *)shadow + offset, sizeof(value));
    return value;
}

#endif /* NPA_FFMPEG_DEMUX_ABI_H */
