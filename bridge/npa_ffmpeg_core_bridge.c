/*
 * Forwarding stubs for the ffmpeg-core unit.
 *
 * The unit carries libavformat, libavcodec, libavutil and libswresample 4.4.8
 * and replaces the 4.4.5 the app links statically. Each entry point the app
 * calls is forwarded by a tail branch to the same symbol inside this dylib, so
 * the call reaches 4.4.8 with no argument translation and no hand-written
 * prototype that could disagree with the headers. Phase B replaces these
 * branches with real shims, one symbol at a time.
 *
 * The abi header below is the reason this translation unit exists even though
 * it defines no C function: it fails the build if the closure's ABI drifts away
 * from the 4.4.5 the app was built against. The forward list itself is shared
 * with the all-4.4.8 dylib.
 *
 * Generated from the closed call-site table; see
 * docs/superpowers/plans/2026-09-25-ffmpeg-core-fullswap.md.
 */

#include "ffmpeg-core-abi.h"
#include "npa_ffmpeg_forwards.h"

#define NPA_FORWARD(symbol) \
    __asm__(".globl _npa_" #symbol "\n_npa_" #symbol ":\n\tb _" #symbol "\n");

NPA_CORE_FORWARDS(NPA_FORWARD)
