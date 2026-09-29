# Third-party notices

The release set contains seven bridge dylibs and a host Keystone assembler.
The sections below identify the third-party code used by each distributed
binary. The bridge dependencies are built from pinned sources recorded in
`deps/sources.lock.json` (libass closure), `deps/ffmpeg.lock.json` (9.0.2
scaler/resampler), `deps/ffmpeg-core.lock.json` (the 4.4.8 closure) and
`deps/ffmpeg-core902.lock.json` (the 9.0.2 core closure).

The dependency locks and `dev/README.md` identify the exact inputs and build
configuration. Anyone distributing these binaries is responsible for providing
the license texts, corresponding source and relinking materials required by the
applicable upstream licenses.

No nPlayer code is included or redistributed by this project.

## `LibASSBridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| libass | ISC | 0.17.5 | https://github.com/libass/libass |
| FreeType | FTL (FreeType License) or GPL-2.0 | 2.14.3 | https://github.com/freetype/freetype |
| HarfBuzz | Old MIT | 14.2.1 | https://github.com/harfbuzz/harfbuzz |
| FriBidi | LGPL-2.1-or-later | 1.0.16 | https://github.com/fribidi/fribidi |
| fontconfig | MIT-style (Keith Packard) | 2.17.1 | https://gitlab.freedesktop.org/fontconfig/fontconfig |
| expat | MIT | 2.8.5 | https://github.com/libexpat/libexpat |

## `LibFFmpegBridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg (libavutil, libswscale, libswresample) | LGPL-2.1-or-later | 9.0.2 | https://ffmpeg.org/releases/ |

The FFmpeg closure is configured with `--disable-everything` plus
`--enable-swscale --enable-swresample`, and builds no decoder, demuxer, muxer,
filter, device or program. No GPL component is enabled, so the LGPL-2.1-or-later
license above is the one that applies.

## `LibFFmpegCoreBridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg (libavformat, libavcodec, libavutil, libswresample) | LGPL-2.1-or-later | 4.4.8 | https://ffmpeg.org/releases/ |
| dav1d | BSD-2-Clause | 1.5.4 | https://code.videolan.org/videolan/dav1d |

This closure mirrors the FFmpeg the app already carries, which is what makes
replacing it ABI-safe: same major versions, same components, the same external
libraries and no GPL part - the app links no libx264 and no libxml2, so neither
does this closure. zlib, bzlib and iconv are system libraries; dav1d is the only
bundled one and is what decodes AV1 in this build. The closure also builds
libswscale, for `LibFFmpegFullBridge.dylib`; this dylib references no `sws_*`
symbol, so no scaler object is linked into it, and in the split selection the
app's `sws_*` calls belong to `LibFFmpegBridge.dylib`.

## `LibFFmpegFullBridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample) | LGPL-2.1-or-later | 4.4.8 | https://ffmpeg.org/releases/ |
| dav1d | BSD-2-Clause | 1.5.4 | https://code.videolan.org/videolan/dav1d |

The same 4.4.8 closure as `LibFFmpegCoreBridge.dylib`, linked whole so that one
dylib carries every FFmpeg entry point the app calls. Same licenses, same
external libraries, no GPL part.

## `LibFFmpegCore902Bridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample) | LGPL-2.1-or-later | 9.0.2 | https://ffmpeg.org/releases/ |
| dav1d | BSD-2-Clause | 1.5.4 | https://code.videolan.org/videolan/dav1d |

This closure uses Secure Transport and the system zlib, bzlib and iconv
libraries. FFmpeg's native AV1 decoder is disabled and AV1 decoding is provided
by dav1d. No GPL component is enabled.

## `LibFFmpegOut448Bridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg (libavformat, libavcodec, libavutil) | LGPL-2.1-or-later | 4.4.8 | https://ffmpeg.org/releases/ |
| dav1d | BSD-2-Clause | 1.5.4 | https://code.videolan.org/videolan/dav1d |

This output-side bridge is built from the same pinned 4.4.8 closure as
`LibFFmpegCoreBridge.dylib` and `LibFFmpegFullBridge.dylib`. No GPL component is
enabled.

## `LibRendererHighBitBridge.dylib`

| Library | License | Version | Source |
| --- | --- | --- | --- |
| FFmpeg headers (libavutil) | LGPL-2.1-or-later | 4.4.8 | https://ffmpeg.org/releases/ |

This bridge compiles against the pinned FFmpeg headers for ABI definitions but
links no FFmpeg archive. Its runtime dependencies are Apple system frameworks.

## Host patching tools

| Tool | License | Version | Source |
| --- | --- | --- | --- |
| Keystone Engine (`libkeystone.dylib`) | GPL-2.0-only with the Keystone FOSS License Exception; bundled LLVM code under the University of Illinois/NCSA license | 0.9.2-compatible, commit `dc7932ef2b2c4a793836caec6ecab485005139d6` | https://github.com/keystone-engine/keystone/tree/dc7932ef2b2c4a793836caec6ecab485005139d6 |
| LIEF | Apache-2.0 | 1.0.0 | https://github.com/lief-project/LIEF |

`libkeystone.dylib` is a host-side release asset used to assemble patch
payloads; it is not linked into an iOS bridge. The release artifact includes
Keystone's `COPYING`, `EXCEPTIONS-CLIENT` and `llvm/LICENSE.TXT` files. LIEF is
installed as a Python dependency and is not copied into the released binary
set. `ldid` is an external prerequisite supplied separately by the user and is
not redistributed by this project.

The authoritative license texts are the `COPYING`/`LICENSE` files inside each
upstream source tree. The versions above are the ones pinned by the dependency
locks; when a pin changes, this table and the matching `dylibs[].library_version`
in `manifests/nplayer-3.13.0.json` change in the same commit.
