# Internals

How the patch is put together and why the units are cut where they are. The
verification contract and the accepted artifacts live in
[Verification.md](Verification.md) and `dev/acceptance.json`; the build, verify
and device-acceptance workflow is in `dev/README.md`.

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

Three sites belong to no bridge library, so they are declared in the manifest as
`main_sites` and every selection carries them - including `--dylib ffmpeg`, which
installs no libass at all:

- the two guards turned into NOPs. The app records "fonts already set" on a
  subtitle wrapper (`wrapper+0x1C`, read again by `-[Subtitle updateFontCache]`)
  and then skips re-registering fonts, which is why only the first video in a
  playback sequence could use font attachments. The skip happens before the call,
  so the defect is there whichever libass is loaded.
- the UPnP/SSDP retiming: `net::`'s discovery loop retries a `select()` that came
  back empty after a 1000 ms wait; at `0x100AE3C7C` that constant becomes 50 ms
  (`MOVZ W0, #1000` -> `MOVZ W0, #50`). The scan still runs, only the wait
  between retries is shorter. Left as it is, the app stalls about a second at the
  start of playback while that wait elapses.

Each site is rejected unless the instruction there still matches `expected`, like
every call site.

## What each selection carries

| dylib | ffmpeg / libass | units (APIs / call sites) |
|---|---|---|
| `libass` | libass 0.17.5 | `libass` 15 / 16 |
| `ffmpeg-full` (default) | 4.4.8 | `ffmpeg-core` 99 / 449, `libswscale` 5 / 13, `libswresample` 6 / 7 = 110 / 469 |
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

## Fallback

Falling back is per unit and happens at that unit's first call; the app then runs
its own implementation for every call site of that unit, and its state word reads
`3`. Because a unit covers one face, the granularity of a fallback is a face, not
a call site: removing one dylib turns off every unit it carries and nothing else.
`dev/acceptance.json` records the whole-dylib A/B for each accepted selection, and
`docs/Verification.md` states what the device runs proved.
