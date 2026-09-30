# nPlayer iOS Modern Bridge

> A small tribute to nPlayer, an exceptionally well-designed player that has served us reliably for years.

A patching tool on macOS, Linux and Windows bringing modern ASS/SSA rendering and
media processing to your own **nPlayer 3.13.0**.

> [!Warning]
>
> **Vibe Coding Project**

Replace the bundled libs with bridge dylib
- libass 0.13.7 ->  **libass 0.17.5**
- FFmpeg 4.4.5 -> one of
  1. Full **FFmpeg 4.4.8** 
  2. Swscale + Swresample **FFmpeg 9.0.2** + Core **FFmpeg 4.4.8**
  3. Input **FFmpeg 9.0.2** (Nightly) + Output **FFmpeg 4.4.8**

Fix the bugs
- ASS/SSA font attachments only avaliable for the first video in playback sequence
- Playback may stalled for about a second 

Update to modern
- High-Bit output when software decoding (S/W, nightly, opt-in)

No jailbreak, no inline hooks, specially designed for sideloading and
[LiveContainer](https://github.com/LiveContainer/LiveContainer) JIT-less.

Recommend to use with **nPlayerEnhance**, which unlock ASS/SSA animation framerate limits.

> [!Caution]
>
> This patch currently can only be applied to **Standard / Basic nPlayer 3.13.0**.
>
> **nPlayer Lite**, **nPlayer Plus** and **other Basic nPlayer version** is unsupported.

## What this exactly does

`npa-patch` takes a decrypted nPlayer IPA you own and writes a patched copy:

- when `libass` is selected, two font guards become NOPs so ASS/SSA attachments
  can load for each video in a playback sequence;
- every selection carries the UPnP/SSDP retry wait retimed from 1000 ms to 50 ms,
  which removes the stall as playback starts;
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

- macOS (development and patching), or Linux / 64-bit Windows (patching only).
- Python ≥ 3.11, [uv](https://docs.astral.sh/uv/) and `ldid`.
  - macOS: install ldid with `brew install ldid`.
  - Linux: use a [prebuilt binary](https://github.com/ProcursusTeam/ldid/releases)
    or build it yourself.
  - Windows: download `ldid_w64_x86_64.exe` from the
    [Procursus releases](https://github.com/ProcursusTeam/ldid/releases), rename
    it to `ldid.exe`, and put it in the repository root or on `PATH`.
- Your own **decrypted** nPlayer 3.13.0 IPA. 
  - App Store packages are FairPlay-encrypted and are rejected on purpose.
  - This project ships no IPA and no decryption.
- The host files from the release assets. On macOS, `make bootstrap` builds the
  assembler and `make bridge` builds the dylibs locally; Linux can build its
  host assembler with `make bootstrap`. Windows uses the official prebuilt
  Keystone DLL as described below.

  | asset | what it is | selected by |
  |---|---|---|
  | `libkeystone.dylib` / `libkeystone.so` / `keystone.dll` | the Keystone 0.9.2 assembler that encodes the dispatch payload (macOS / Linux / Windows) | every patch |
  | `LibASSBridge.dylib` | libass 0.17.5 for iOS arm64 | `libass` |
  | `LibFFmpegFullBridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the whole surface | `ffmpeg-full` |
  | `LibFFmpegBridge.dylib` | FFmpeg 9.0.2 for iOS arm64, the scaler and resampler | `ffmpeg` |
  | `LibFFmpegCoreBridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the core | `ffmpeg-core` |
  | `LibFFmpegCore902Bridge.dylib` | FFmpeg 9.0.2 for iOS arm64, the input side | `ffmpeg-core902` |
  | `LibFFmpegOut448Bridge.dylib` | FFmpeg 4.4.8 for iOS arm64, the output side | `ffmpeg-out448` |
  | `LibRendererHighBitBridge.dylib` | standalone P010 and planar 10-bit / 12-bit / 16-bit renderer for iOS arm64 | `renderer-highbit` |

- No Xcode, iOS SDK or jailbreak is needed for patching. `npa-patch` runs from
  the repository checkout, next to `manifests/`. Windows does not require WSL,
  MSYS2, `zip` or `unzip`.

## Quick start

### macOS / Linux

```sh
git clone <this repository> && cd nPlayerModernBridge
# put LibASSBridge.dylib, LibFFmpegFullBridge.dylib,
# LibFFmpegBridge.dylib, LibFFmpegCoreBridge.dylib, LibFFmpegCore902Bridge.dylib,
# LibFFmpegOut448Bridge.dylib and libkeystone.dylib from the release assets here;
# LibRendererHighBitBridge.dylib is optional and enables the experimental renderer
# (on Linux, run `make bootstrap` to build libkeystone.so instead)
uv run npa-patch "/path/to/nPlayer_3.13.0.ipa"
```

### Windows

Download and extract the official
[Keystone 0.9.2 Windows release](https://github.com/keystone-engine/keystone/releases/tag/0.9.2),
then copy its DLL directly to the repository root. The patcher deliberately
does not search the extracted directory or `PATH` for this file.

```powershell
# If you prefered CMD, replace 
# `Set-Location` -> `cd`, 
# `Copy-Item` -> `copy`
git clone <this repository>
Set-Location nPlayerModernBridge

uv sync --frozen
# Of cource you can simply use Windows Explorer to do this
Copy-Item .\keystone-0.9.2-win64\keystone.dll .\keystone.dll
# After downloading the Procursus Windows x86_64 release:
Copy-Item D:\Downloads\ldid_w64_x86_64.exe .\ldid.exe
.\ldid.exe

# Put the selected Lib*Bridge.dylib release assets in this checkout, then run:
uv run npa-patch --dylibs-dir . D:\IPAs\nPlayer_3.13.0.ipa
```

The repository-root `ldid.exe` takes precedence on Windows. Alternatively,
place it elsewhere and add that directory to `PATH`. 

No MSYS2, WSL, external
`zip`, or external `unzip` installation is used by this flow.

### Output

Default output filename will be
`nPlayer_3.13.0-libass0.17.5-ffmpeg-full4.4.8.ipa`, one
`<id><version>` segment per installed dylib in manifest order. 

Install it with your usual sideload tool (
[TrollStore](https://github.com/opa334/TrollStore),
[SideStore](https://github.com/SideStore/SideStore),
[iloader](https://github.com/nab138/iloader) and more ) or [LiveContainer](https://github.com/LiveContainer/LiveContainer).

### Advance

Options exist: 
- `-o/--output`
- `--dylib <id>` (repeatable, default: the manifest's `default_dylibs`, which
  is `libass` and `ffmpeg-full`) 
- `--dylibs-dir <dir>` (default: the working
directory; each dylib is looked up as `<dir>/<basename>`).
-  `--manifests`
(default `manifests/`) selects the manifest directory. 

On macOS/Linux, `./npa-patch` at the repository root is equivalent to
`uv run npa-patch`. On every supported host, `uv run python tools/patch.py` is
also an equivalent entry point.

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

Before anything is written, the input executable is matched by a normalized
SHA-256 that excludes the embedded code signature and normalizes decrypted
`cryptsize` metadata. All other executable bytes remain pinned, so an
unsupported version, an already-patched IPA and an encrypted package fail with
a named reason instead of producing a broken bundle. See
[Internals.md](docs/Internals.md#input-executable-identity) for the exact
identity contract.

Adding a version means adding one manifest (addresses, call sites, frozen iOS
ABI). That work needs the binary analyzed; see `dev/README.md`.

## Verification Status

See [Verification.md](docs/Verification.md)

## Troubleshooting

| Message | Cause | Fix |
| --- | --- | --- |
| `still FairPlay-encrypted` | the IPA comes straight from the App Store | provide a decrypted dump of your own purchase |
| `no manifest matches this main executable` | wrong nPlayer version, an already-patched IPA, or bytes changed outside normalized extraction/signature metadata | use a supported, clean decrypted dump |
| `bridge.exports` / `bridge.install_name` failed | wrong or stale dylib | use the dylib from the matching release |
| `bridge dylib for <id> is missing` | the selected dylib is not in `--dylibs-dir` | copy it there or point `--dylibs-dir` at it |
| `unknown dylib ids: <id>` | typo in `--dylib` | the ids are the manifest's `dylibs[].id` values |
| `conflicting dylib selection: ...` | `ffmpeg-full` was combined with `ffmpeg`/`ffmpeg-core`, or `ffmpeg-out448` with `ffmpeg-core`/`ffmpeg-full` | pick one core selection for the call sites; `ffmpeg-out448` pairs with `ffmpeg-core902` |
| `ldid is required to assemble the IPA` | ldid is unavailable | use Homebrew on macOS, a prebuilt Linux binary, or copy the Procursus Windows x86_64 release to the repository root as `ldid.exe` (with `PATH` as a fallback) |
| `host assembler library is missing: ...keystone.dll` | the Windows assembler is not at the repository root | copy `keystone.dll` from the official Keystone 0.9.2 Windows release directly to the checkout root |
| `WinError 193` or a DLL load failure mentioning `keystone.dll` | the DLL has the wrong architecture or a runtime dependency is unavailable | use the official 64-bit Windows Keystone 0.9.2 DLL and install its required Microsoft runtime |
| `host assembler library is missing` | the macOS/Linux assembler is not in the checkout | run `make bootstrap`, or use the matching release asset |

## Rebuilding from source

`deps/sources.lock.json`, `deps/ffmpeg.lock.json` and
`deps/ffmpeg-core.lock.json` pin every dependency (version, archive URL,
SHA-256) and `make bootstrap deps bridge` rebuilds every closure and dylib. 

`make deps` verifies each closure and refuses a
surprise fourth archive.

Libass defaults to `libass-patch`, which rebuilds directory attachments as a
provider-owned snapshot. To use unchanged libass with per-track Bridge
isolation, run `make bridge ASS_FONT_MODE=bridge-isolation` and
`make verify ASS_FONT_MODE=bridge-isolation`. Both produce the same
`build/LibASSBridge.dylib`. `make bridge` switches back to the default.
See [font lifecycle and build modes](docs/Libass-fonts.md) for maintenance,
validation and isolation costs.


Building the iOS bridge dylibs locally is supported on macOS and needs `Xcode`,
`cmake`, `ninja` and `meson`. Linux can build its host Keystone library with
`make bootstrap`; Windows patch users use the official prebuilt DLL instead.
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
