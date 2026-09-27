/*
 * Forwarding stubs for the ffmpeg-out448 unit.
 *
 * The unit carries libavformat, libavcodec and libavutil 4.4.8 and forwards
 * the app's output-side calls -- the HLS session, the SPDIF mux and the MJPEG
 * cover encoder -- into that closure by tail branch, with no argument
 * translation and no hand-written prototype that could disagree with the
 * headers. The abi header below fails the build if the closure's ABI drifts
 * away from the 4.4.5 the app was built against.
 *
 * The site set is frozen in
 * docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md and guarded by
 * tests/test_out448_manifest.py.
 */

#include "ffmpeg-core-abi.h"
#include "npa_ffmpeg_out448_forwards.h"

#define NPA_FORWARD(prefix, symbol) \
    __asm__(".globl _npa_" #prefix "_" #symbol "\n_npa_" #prefix "_" #symbol ":\n\tb _" #symbol "\n");

#define NPA_HLS_FORWARD(symbol) NPA_FORWARD(hls, symbol)
#define NPA_SPDIF_FORWARD(symbol) NPA_FORWARD(spdif, symbol)
#define NPA_MJPEG_FORWARD(symbol) NPA_FORWARD(mjpeg, symbol)

NPA_HLS_FORWARDS(NPA_HLS_FORWARD)
NPA_SPDIF_FORWARDS(NPA_SPDIF_FORWARD)
NPA_MJPEG_FORWARDS(NPA_MJPEG_FORWARD)
