/*
 * Forwarding stubs for the ffmpeg-full unit.
 *
 * One dylib carries the whole FFmpeg 4.4.8 the app links statically:
 * libavutil, libavcodec, libavformat, libswscale and libswresample. It is the
 * alternative to splitting the swap across LibFFmpegCoreBridge.dylib (the core
 * at 4.4.8) and LibFFmpegBridge.dylib (the scaler and resampler at 9.0.2), and
 * it is selected or left out as a whole.
 *
 * Every entry point is a plain tail branch. 4.4.5 and 4.4.8 share one ABI, so
 * unlike the 9.0.2 dylib this one needs no pixel-format translation and no
 * rebuilt swr_alloc_set_opts(). The abi header makes the build fail if the
 * closure drifts away from the 4.4.5 the app was built against; the swscale
 * version assert is the one check the core's header does not already make.
 *
 * Generated from the closed call-site table; see
 * docs/superpowers/plans/2026-09-25-ffmpeg-core-fullswap.md.
 */

#include "ffmpeg-core-abi.h"
#include "npa_ffmpeg_forwards.h"

#include <libswscale/version.h>

NPA_ABI_ASSERT(LIBSWSCALE_VERSION_INT == 0x050964, "libswscale is not 5.9.100");

#define NPA_FORWARD(symbol) \
    __asm__(".globl _npa_" #symbol "\n_npa_" #symbol ":\n\tb _" #symbol "\n");

NPA_CORE_FORWARDS(NPA_FORWARD)
NPA_UTIL_FORWARDS(NPA_FORWARD)
