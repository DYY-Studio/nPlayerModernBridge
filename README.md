# nPlayer iOS Bridge

> A small tribute to nPlayer, an exceptionally well-designed player that has served us reliably for years.

> [!Warning]
>
> **Vibe Coding Project**

Replace the bundled ...
- libass 0.13.7 stack with **libass 0.17.5**
- FFmpeg 4.4.5 libs with one of
  1. Full **FFmpeg 4.4.8** 
  2. Swscale + Swresample **FFmpeg 9.0.2** + Core **FFmpeg 4.4.8**
  3. Input **FFmpeg 9.0.2** (Nightly) + Output **FFmpeg 4.4.8**

... in your own **nPlayer 3.13.0** install. 

No jailbreak, no inline hooks, bring modern ASS/SSA rendering and media processing to this great player.

Recommend to use with **nPlayerEnhance**, which unlock ASS/SSA animation framerate limits.

> [!Caution]
>
> This patch can only be applied to **Standard / Basic nPlayer 3.13.0**.
>
> **nPlayer Lite**, **nPlayer Plus** and **other Basic nPlayer version** is unsupported.

## What this does

`npa-patch` takes a decrypted nPlayer IPA you own and writes a patched copy:

- three sites are declared at app level, so every selection carries them
  - the two guards that are turned into NOPs, which is what lets ASS/SSA font attachments work for every video in a
  playback sequence rather than only the first
  - a UPnP/SSDP retry wait retimed from 1000 ms to 50 ms, which removes the stall the app used to take as
  playback starts;
- libass 0.17.5 (with FreeType, **HarfBuzz**, FriBidi, fontconfig and expat) takes
  over the 15 libass entry points the app calls;
- FFmpeg is replaced per unit, and each unit is one generation of the library (table below);
  - Units can be selected on their own or in the combinations the table notes.
  - Each arbitrates its own state at first call and falls back to the app's own build on its own.

| selection | dylib | carries | ffmpeg |
|---|---|---|---|
| `libass` | `LibASSBridge.dylib` | subtitles | libass 0.17.5 |
| `ffmpeg-full` *(default)* | `LibFFmpegFullBridge.dylib` | the whole surface: demux, decode, encode, mux, bitstream filters, scaler, resampler | 4.4.8 |
| `ffmpeg-core` | `LibFFmpegCoreBridge.dylib` | the core: demux, decode, encode, mux, bitstream filters | 4.4.8 |
| `ffmpeg` | `LibFFmpegBridge.dylib` | the scaler and resampler - usually pairs with `ffmpeg-core` | 9.0.2 |
| `ffmpeg-core902` *(nightly)* | `LibFFmpegCore902Bridge.dylib` | the input side: demux, subtitle decoding, playback/probe/poster decoding, scaler, resampler | 9.0.2 |
| `ffmpeg-out448` | `LibFFmpegOut448Bridge.dylib` | the output side: HLS session and muxer, SPDIF, poster encoding - usually pairs with `ffmpeg-core902` | 4.4.8 |
| `renderer-highbit` *(nightly, opt-in)* | `LibRendererHighBitBridge.dylib` | (S/W) converts P010 and planar 10-bit frames to `x420` / `x422` / `x444`, plus planar 12-bit and 16-bit frames to `sv22` / `sv44`; independent of the selected FFmpeg bridge | app 3.13.0 frame ABI |

> [!Important]
> 
> Selecting `ffmpeg` and `ffmpeg-core` together is the verified form, 
> each alone is selectable but unverified.

Selections whose call sites overlap are mutually exclusive: the tool refuses the
combination before writing anything. 

The default selection omits `renderer-highbit`. Add it explicitly to any supported
FFmpeg selection to enable the experimental high-bit-depth renderer path.

Every dylib is built from this repository and
links system libraries and frameworks only. What the units are, why they are
grouped this way, what each dylib links and how a fallback behaves are in
[docs/Internals.md](docs/Internals.md).

## Requirements

- macOS (Dev and Patch) or Linux (Patch only)
- Python ≥ 3.11, [uv](https://docs.astral.sh/uv/), `ldid`, `zip` and `unzip`. 
  - macOS: Install ldid with `brew install ldid`
  - Linux: Use a [prebuilt binary](https://github.com/ProcursusTeam/ldid/releases) or build it yourself.
- Your own **decrypted** nPlayer 3.13.0 IPA. 
  - App Store packages are FairPlay-encrypted and are rejected on purpose.
  - This project ships no IPA and no decryption.
- The host files from the release assets. All are host-side build products;
  `make bootstrap` builds the assembler and `make bridge` builds the dylibs
  locally if you prefer that.

  | asset | what it is | selected by |
  |---|---|---|
  | `libkeystone.dylib` | the assembler that encodes the dispatch payload (macOS arm64; on Linux, build `libkeystone.so` with `make bootstrap`) | every patch |
  | `LibASSBridge.dylib` | libass 0.17.5 for iOS arm64 | `libass` |
  | `LibFFmpegFullBridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the whole surface | `ffmpeg-full` |
  | `LibFFmpegBridge.dylib` | FFmpeg 9.0.2 for iOS arm64, the scaler and resampler | `ffmpeg` |
  | `LibFFmpegCoreBridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the core | `ffmpeg-core` |
  | `LibFFmpegCore902Bridge.dylib` | FFmpeg 9.0.2 for iOS arm64, the input side | `ffmpeg-core902` |
  | `LibFFmpegOut448Bridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the output side | `ffmpeg-out448` |
  | `LibRendererHighBitBridge.dylib` | standalone P010 and planar 10-bit / 12-bit / 16-bit renderer for iOS arm64 | `renderer-highbit` |

- No Xcode, no iOS SDK, no jailbreak. `npa-patch` runs from the repository
  checkout, next to `manifests/`.

## Quick start

```sh
git clone <this repository> && cd nplayer-libass-bridge
# put LibASSBridge.dylib, LibFFmpegFullBridge.dylib,
# LibFFmpegBridge.dylib, LibFFmpegCoreBridge.dylib, LibFFmpegCore902Bridge.dylib,
# LibFFmpegOut448Bridge.dylib and libkeystone.dylib from the release assets here;
# LibRendererHighBitBridge.dylib is optional and enables the experimental renderer
# (on Linux, run `make bootstrap` to build libkeystone.so instead)
uv run npa-patch "/path/to/nPlayer_3.13.0.ipa"
```
The output is written next to the input as
`nPlayer_3.13.0-libass0.17.5-ffmpeg-full4.4.8.ipa`, one
`<id><version>` segment per installed dylib in manifest order. 

Install it with your usual sideload tool (
[TrollStore](https://github.com/opa334/TrollStore),
[SideStore](https://github.com/SideStore/SideStore),
[iloader](https://github.com/nab138/iloader) and more ) or [LiveContainer](https://github.com/LiveContainer/LiveContainer).

Options exist: 
- `-o/--output`
- `--dylib <id>` (repeatable, default: the manifest's `default_dylibs`, which
  is `libass` and `ffmpeg-full`) 
- `--dylibs-dir <dir>` (default: the working
directory; each dylib is looked up as `<dir>/<basename>`).
-  `--manifests`
(default `manifests/`) selects the manifest directory. 

`./npa-patch` at the
repository root and `uv run python tools/patch.py` are equivalent entry points.

The command prints a JSON summary with the input hash, output hashes, one hash
per shipped dylib, and the number of verification checks that passed.

Select exactly what you want:

```sh
uv run npa-patch --dylib libass "/path/to/nPlayer_3.13.0.ipa"   # subtitles only
# subtitles plus the whole FFmpeg 4.4.8 (the default):
uv run npa-patch --dylib libass --dylib ffmpeg-full "/path/to/nPlayer_3.13.0.ipa"
# subtitles plus the 4.4.8 core with the 9.0.2 scaler/resampler instead:
uv run npa-patch --dylib libass --dylib ffmpeg --dylib ffmpeg-core "/path/to/nPlayer_3.13.0.ipa"
# subtitles plus the 9.0.2 input side with the output side at 4.4.8:
uv run npa-patch --dylib libass --dylib ffmpeg-core902 --dylib ffmpeg-out448 "/path/to/nPlayer_3.13.0.ipa"
# add the standalone high-bit-depth renderer to a selection:
uv run npa-patch --dylib libass --dylib ffmpeg-full --dylib renderer-highbit "/path/to/nPlayer_3.13.0.ipa"
```
The FFmpeg selections replace the same call sites, so they cannot be combined:
the 4.4.8 core and the 9.0.2 core are exclusive, and `ffmpeg-out448` is refused
next to `ffmpeg-core`/`ffmpeg-full`, which own its call sites already. Naming
both fails before anything is written. A missing or stale dylib
is a hard error; a unit only ever falls back at runtime when the dylib it needs
fails to load or fails its identity check.

## Supported versions

Exactly the nPlayer versions listed in `manifests/`. 

The input executable is
matched by SHA-256 before anything is written, so an unsupported version, an
already-patched IPA and an encrypted package all fail with a named reason
instead of producing a broken bundle.

Adding a version means adding one manifest (addresses, call sites, frozen iOS
ABI). That work needs the binary analyzed; see `dev/README.md`.

## Verification Status

See [Verification.md](docs/Verification.md)

## Troubleshooting

| Message | Cause | Fix |
| --- | --- | --- |
| `still FairPlay-encrypted` | the IPA comes straight from the App Store | provide a decrypted dump of your own purchase |
| `no manifest matches this main executable` | wrong nPlayer version, or the IPA already has the patch | use a supported, clean dump |
| `bridge.exports` / `bridge.install_name` failed | wrong or stale dylib | use the dylib from the matching release |
| `bridge dylib for <id> is missing` | the selected dylib is not in `--dylibs-dir` | copy it there or point `--dylibs-dir` at it |
| `unknown dylib ids: <id>` | typo in `--dylib` | the ids are the manifest's `dylibs[].id` values |
| `conflicting dylib selection: ...` | `ffmpeg-full` was combined with `ffmpeg`/`ffmpeg-core`, or `ffmpeg-out448` with `ffmpeg-core`/`ffmpeg-full` | pick one core selection for the call sites; `ffmpeg-out448` pairs with `ffmpeg-core902` |
| `ldid is required to assemble the IPA` | ldid is not installed | `brew install ldid`, or a prebuilt Linux ldid |
| `host assembler library is missing` | the host assembler is not in the checkout | run `make bootstrap`, or drop the `libkeystone.dylib` release asset on macOS |

## Rebuilding from source

`deps/sources.lock.json`, `deps/ffmpeg.lock.json` and
`deps/ffmpeg-core.lock.json` pin every dependency (version, archive URL,
SHA-256) and `make bootstrap deps bridge` rebuilds every closure and dylib. 

`make deps` verifies each closure and refuses a
surprise fourth archive. 

That path needs `Xcode`, `cmake`, `ninja` and `meson`;
`dev/README.md` describes it, plus how to re-check the frozen ABI and re-run the
device acceptance.

## Legal

This toolchain is MIT licensed (see `LICENSE`).

The third-party notices for the
statically linked libraries are in `THIRD-PARTY.md`. 

The project is not
affiliated with, or endorsed by, the nPlayer authors. 

You must own a licence
for nPlayer, and you should patch only your own copy.
