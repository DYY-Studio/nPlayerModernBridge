/*
 * The 9.0.2 core unit: one dylib for two independent faces.
 *
 * It carries FFmpeg 9.0.2 libavformat (the pure-playback demux face, class A
 * only) plus libswscale and libswresample, and it is the alternative to
 * splitting those across LibFFmpegDemuxBridge.dylib and LibFFmpegBridge.dylib.
 * The two faces share nothing but libavutil, so each stays in its own file and
 * this translation unit pulls both in - the build compiles one source per
 * dylib.
 *
 * Neither face is a pass-through: the demux face hands the app a 4.4.x shaped
 * shadow context and translates the enum-valued fields at the boundary (see
 * ffmpeg-demux-abi.h and ffmpeg-demux-enum-map.h), and the scaler face
 * translates the pixel format (see npa_modern_pixfmt in the util bridge).
 */

#include "npa_ffmpeg_demux_bridge.c"
#include "npa_ffmpeg_util_bridge.c"
