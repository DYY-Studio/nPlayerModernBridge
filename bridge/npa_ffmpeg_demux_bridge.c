/*
 * Shim skeleton for the ffmpeg-demux unit.
 *
 * The unit carries libavformat/libavcodec/libavutil 9.0.2 and takes over the
 * pure-playback demuxer's AVFormatContext entry points. Each entry point is a
 * tail branch to the same symbol inside this dylib, so the first landing of
 * the unit is a build/export check before the shadow translation is written;
 * the following commits replace these branches with real shims.
 *
 * The include below is the reason this translation unit exists even though it
 * defines no C function: it fails the build if the hand-built legacy layouts
 * or the modern field offsets drift.
 *
 * The frozen call-site table is in
 * docs/superpowers/plans/2026-09-26-class-a-demux-modern-avformat.md.
 */

#include "ffmpeg-demux-abi.h"

#define NPA_FORWARD(symbol) \
    __asm__(".globl _npa_" #symbol "\n_npa_" #symbol ":\n\tb _" #symbol "\n")

NPA_FORWARD(avformat_alloc_context);
NPA_FORWARD(avformat_open_input);
NPA_FORWARD(avformat_find_stream_info);
NPA_FORWARD(av_read_frame);
NPA_FORWARD(av_seek_frame);
NPA_FORWARD(avformat_close_input);
NPA_FORWARD(avformat_free_context);
NPA_FORWARD(avio_alloc_context);
NPA_FORWARD(avio_size);
NPA_FORWARD(av_index_search_timestamp);
