# Internals

How the patch is put together and why the units are cut where they are. The
verification contract and the accepted artifacts live in
[Verification.md](Verification.md) and `dev/acceptance.json`; the build, verify
and device-acceptance workflow is in `dev/README.md`.

## Input executable identity

`npa-patch` identifies a supported decrypted executable with the normalized
SHA-256 stored as `main_pin_sha256` in its manifest. A whole-file hash is not a
stable identity: extraction tools may write different values to the inactive
`LC_ENCRYPTION_INFO_64.cryptsize` field and replace the embedded code signature
without changing the decrypted program.

This was confirmed with two nPlayer 3.13.0 executables whose whole-file hashes
were different. Outside the `LC_CODE_SIGNATURE` blob, their only differing
bytes were the `cryptsize` value (`23445504` versus `4096`), while `cryptid` was
zero in both and every manifest call site, old branch target, dynamic stub and
extra-site guard matched.

The identity pin therefore requires a thin Mach-O with valid encryption and
code-signature commands, requires `cryptid == 0`, normalizes `cryptsize` and the
signature `datasize`, and excludes the signature blob from the hash. Every
other byte remains pinned, including code, data, other load-command fields and
the signature offset. Parsing errors, encrypted inputs and any other byte
change fail explicitly; there is no fallback to a less strict match.

## The payload and the units

`npa-patch` rewrites the app's call sites so they jump into a `__NPATCH_TEXT`
payload carried by the patched main. On first use a unit resolves the dylib it
needs: it checks that the file sits at the expected path and exports the expected
symbols, and if either check fails it publishes `OLD` for that unit and leaves
its slots null, so the app's own implementation keeps running. A call site
belongs to exactly one unit, and no site is patched twice.

A **unit** is the granularity of that decision. Each unit covers one library
face and arbitrates its own state, so a failure in one never turns off another.
A dylib carries one or more units, and a unit is only installed when its dylib is
selected: `npa-patch --dylib libass` produces an artifact byte-identical to the
libass-only patch of the same input.

Each unit has a state word in `__NPATCH_DATA`: `0` never called, `2` live (the
unit's code ran), `3` fell back. `notes/playcover-debug-path.md` sections 4 and
14 record how to read them, how to check which payload is installed, and how to
drive the faces that the app does not reach on its own.

## App-level sites

The UPnP/SSDP retiming is declared in `main_sites`, so every selection carries
it: at `0x100AE3C7C`, the discovery loop's empty `select()` retry wait changes
from 1000 ms to 50 ms (`MOVZ W0, #1000` -> `MOVZ W0, #50`).

The font guards at `0x100A0392C` and `0x100ACBC14` belong to
`libass.extra_sites`. They become NOPs only when LibASSBridge is installed.
The app's subtitle wrapper records "fonts already set" at `wrapper+0x1C`;
removing the guards permits later media to re-register attachments. Both
Bridge build modes handle that repeated font loading. Selections without
libass retain the original guards.

Each site is rejected unless the instruction there still matches `expected`, like
every call site.

## What each selection carries

| dylib | ffmpeg / libass | units (APIs / call sites) |
|---|---|---|
| `libass` | libass 0.17.5 | `libass` 15 / 16 |
| `ffmpeg-full` (default) | 4.4.8 | `ffmpeg-core` 99 / 449, `libswscale` 5 / 13, `libswresample` 6 / 7 = 110 / 469 |
| `renderer-highbit` (experimental, opt-in) | app 3.13.0 frame ABI | renderer conversion 1 / 1 |
| `ffmpeg-core` | 4.4.8 | `ffmpeg-core` 99 / 449 |
| `ffmpeg` | 9.0.2 | `libswscale` 5 / 13, `libswresample` 6 / 7 = 11 / 20 |
| `ffmpeg-core902` | 9.0.2 | `ffmpeg-demux` 15 / 38, `libswscale` 5 / 13, `libswresample` 6 / 7, `ffmpeg-subdecode` 10 / 16, `ffmpeg-codec` 12 / 46 = 48 / 120 |
| `ffmpeg-out448` | 4.4.8 | `ffmpeg-hls448` 66 / 130, `ffmpeg-spdif448` 11 / 11, `ffmpeg-mjpeg448` 13 / 13 = 90 / 154 |

Selections whose call sites overlap are refused before anything is written:
`ffmpeg-full` excludes `ffmpeg`, `ffmpeg-core`, `ffmpeg-core902` and
`ffmpeg-out448`; `ffmpeg-core902` excludes `ffmpeg`, `ffmpeg-core` and
`ffmpeg-full`; `ffmpeg-out448` excludes `ffmpeg-core` and `ffmpeg-full`, which
already own its call sites at 4.4.8. `ffmpeg-out448` is meant to be paired with
`ffmpeg-core902`: the output side on 4.4.8 next to the 9.0.2 input side.

## Why the units are cut this way

- `ffmpeg-core` is deliberately one unit covering libavutil, libavcodec and
  libavformat together: the app reads those structures directly, so half a swap
  would let one library interpret the other's memory.
- The whole-4.4.8 dylib carries `ffmpeg-core` plus the scaler and resampler units,
  so a scaler failure does not take the demuxer with it while the three still
  share one libavutil.
- The 9.0.2 core dylib carries five units. `ffmpeg-demux` is a demux-only slice of
  9.0.2 libavformat serving the pure-playback demuxer and the loader for external
  subtitle files; `ffmpeg-codec` is the matching slice of 9.0.2 libavcodec for the
  playback, probe and poster software decoders; both leave the rest of the app's
  4.4.5 avcodec behind a translated legacy-shaped shadow context.
  `ffmpeg-subdecode` decodes subtitles through 9.0.2. `libswscale` and
  `libswresample` replace the scaler and resampler. All but one of the demux call
  sites are also the 4.4.8 core's demux face, so this selection excludes
  `ffmpeg-core` and `ffmpeg-full`; the demux face adds `avformat_seek_file`, which
  the 4.4.8 core never bound.
- The `ffmpeg` dylib exists so the 4.4.8 core can be paired with a 9.0.2
  scaler/resampler without touching avcodec; it carries only those two units.
- The output-side dylib carries `ffmpeg-hls448`, `ffmpeg-spdif448` and
  `ffmpeg-mjpeg448` together: they share one 4.4.8 closure, and the app reaches
  them from three different subsystems (the MediaServer HLS session and its
  muxer, digital audio passthrough, poster/cover encoding), so one dylib keeps
  that closure single while each unit still falls back on its own.

`renderer-highbit` is a standalone opt-in dylib. Its direct call site passes the
app's `media::FFmpegVideoFrame` wrapper to the bridge; the bridge reads guarded
width/height/format/AVFrame fields at
`+0x18/+0x1C/+0x2C/+0x40`. It copies P010 frames (161) and packs planar
`YUV420P10LE` (64), `YUV422P10LE` (66), and `YUV444P10LE` (70) frames into
`x420`, `x422`, and `x444` Core Video pixel buffers. It also maps planar
`YUV420P12LE` (125), `YUV422P12LE` (129), `YUV444P12LE` (133),
`YUV420P16LE` (47), `YUV422P16LE` (49), and `YUV444P16LE` (51) frames to
16-bit bi-planar Core Video buffers: 4:2:0 and 4:2:2 use `sv22`, while 4:4:4
uses `sv44`. The 4:2:0 chroma rows are duplicated vertically when packing to
4:2:2. Limited-range 12-bit samples are shifted left four bits; limited-range
16-bit samples are preserved. Full-range input is explicitly scaled to video
range before writing either target format. Other frame formats log and call the
app's original converter. The pixel-buffer pool cache is keyed by width,
height, and target Core Video format. It links only CoreVideo and CoreFoundation
and does not import FFmpeg bridge symbols. Its compile-time headers pin the
fixed AVFrame layout and supported pixel-format values to the app's FFmpeg 4.4
ABI. The default selection omits it; it can be combined with any supported
FFmpeg selection.

The bridge preserves BT.709/BT.2020 primaries, transfer and matrix attachments.
For the 16-bit targets, full-range luma maps to 4096..60160 and chroma maps
around 32768 into 4096..61440, matching the ranges documented for `sv22` and
`sv44` in the iPhoneOS CoreVideo header.
Runtime acceptance with the PlayCover Main10 software-decode fixture found the installed bridge UUID
`747CE7F4-08B2-31BA-B919-9DCF5912E102` matches the v3 build. All 24 enqueued
samples used `x420` (`0x78343230`); the display layer reported
`readyForDisplay=true`, `status=1` (`Rendering`) and `error=null`, and the user
reported normal playback. This verified v3 renderer was embedded in
`LibFFmpegFullBridge.dylib`; HDR metadata, final display precision and EDR
output were not validated. The separated dylib was then installed in PlayCover:
its UUID `D43CDF44-C1E5-3C91-8922-6E0C90B3470C` matched the modular build,
all 24 observed software Main10 enqueues were `x420`, and the display layer
was ready and rendering without error. Later runtime probes accepted planar
10-bit 4:2:2/4:4:4 as `x422`/`x444`, and all six planar 12-bit/16-bit
4:2:0/4:2:2/4:4:4 paths as `sv22`/`sv44`; each reached a ready, rendering
display layer without error. The user reported normal playback for every sample.
Other FFmpeg bridge combinations remain untested at runtime.

## What each dylib links

Every dylib is built from this repository and depends on system libraries and
frameworks only; none pulls a third-party dynamic dependency. The third-party
notices are in `THIRD-PARTY.md`.

- `LibASSBridge.dylib` statically links libass 0.17.5, FreeType, HarfBuzz,
  FriBidi, fontconfig and expat.
- `LibFFmpegFullBridge.dylib` and `LibFFmpegCoreBridge.dylib` statically link
  libavformat, libavcodec, libavutil, libswscale and libswresample 4.4.8 (the core
  dylib without the last two) plus libdav1d 1.5.4, built from the same sources as
  the app's own 4.4.5 so the two share one ABI; both closures pin dav1d 1.5.4
  because the app's own dav1d is a 1.x (API 6) build and FFmpeg 4.4 picks its
  decoder configuration from `FF_DAV1D_VERSION_AT_LEAST(6,0)`.
- `LibFFmpegBridge.dylib` links the `--disable-everything` 9.0.2 build of
  libavutil, libswscale and libswresample.
- `LibFFmpegCore902Bridge.dylib` statically links 9.0.2 libavutil, libavcodec,
  libavformat, libswscale and libswresample, with libdav1d 1.5.4 for AV1.
- `LibFFmpegOut448Bridge.dylib` statically links the same 4.4.8 closure the
  default selection uses. Nothing is translated there: the shim only branches
  into that closure, so no shadow context and no per-call conversion is involved.
- `LibRendererHighBitBridge.dylib` links CoreVideo and CoreFoundation only; its
  frame layout and pixel-format constants are compile-time pinned to the app's
  legacy FFmpeg headers.

## Fallback

Falling back is per unit and happens at that unit's first call; the app then runs
its own implementation for every call site of that unit, and its state word reads
`3`. Because a unit covers one face, the granularity of a fallback is a face, not
a call site: removing one dylib turns off every unit it carries and nothing else.
`dev/acceptance.json` records the whole-dylib A/B for each accepted selection, and
`docs/Verification.md` states what the device runs proved.
