# nPlayer iOS Modern Bridge

**Better ASS/SSA subtitles and faster font loading for nPlayer 3.13.0.**

Patch your own nPlayer IPA on macOS, Linux or Windows, then install it on iOS
with your usual sideloading tool or
[LiveContainer](https://github.com/LiveContainer/LiveContainer). 

No jailbreak or JIT is required.

> A small tribute to nPlayer, a player that has served us reliably for years.

> [!Warning]
> Vibe Coding Project

## What improves

- Faster initial subtitle font loading: about **1.3 seconds → 12 ms** in our
  PlayCover test with 95 bundled fonts.
- Updated ASS/SSA rendering, including newer subtitle features such as `\kt`.
- Attached subtitle fonts work when playing several videos in sequence.
- No more stuck / frames-skipping when play High-Resolution movie.

The default includes the subtitle improvements and FFmpeg 4.4.8. 

Experimental FFmpeg 9.0.2 and high-bit-depth software decoding options are available in the
[advanced options](#advanced-options).

## Supported app

**Standard / Basic nPlayer 3.13.0 only.** nPlayer Lite, nPlayer Plus and other
versions are unsupported.

You need a clean, **decrypted IPA of your own licensed copy**. 

An IPA downloaded
directly from the App Store is encrypted and cannot be patched. This project
provides neither the IPA nor a decryption tool.

## Get started

### 1. Prepare the tool

Download this repository or clone it:

```sh
git clone https://github.com/DYY-Studio/nPlayerModernBridge.git
cd nPlayerModernBridge
```

Install Python 3.11–3.14, [uv](https://docs.astral.sh/uv/) and `ldid`.


On macOS, install ldid with `brew install ldid`. 

On Linux, use a
[Procursus binary](https://github.com/ProcursusTeam/ldid/releases) or build it.

[Windows setup](#windows-setup) is described below.
Xcode and the iOS SDK are not needed to use prebuilt files.

### 2. Add the release files

From the [matching release](https://github.com/DYY-Studio/nPlayerLibassBridge/releases),
put these files in the repository root:

| File | Needed for |
| --- | --- |
| `LibASSBridge.dylib` | Subtitle improvements |
| `LibFFmpegFullBridge.dylib` | Default media processing |
| `libkeystone.dylib` / `libkeystone.so` / `keystone.dll` | macOS / Linux / Windows patching tool |

On Windows, also place `ldid.exe` in the repository root or on `PATH`.
Use files from the same release.

### 3. Patch your IPA

Run from the repository root, replacing the example path with your IPA:

```sh
uv run npa-patch "/path/to/nPlayer_3.13.0.ipa"
```

For Windows paths and optional features, see [Windows setup](#windows-setup)
and [advanced options](#advanced-options).

### 4. Install the output

The patched IPA is written next to the input. With the default features, its
name ends in `-libass0.17.5-ffmpeg-full4.4.8.ipa`.
Install it with your usual sideloading tool or LiveContainer.

## Documentation

- [Verification status](docs/Verification.md): tested combinations and results.
- [Developer guide](dev/README.md): building and validating from source.
- [Internals](docs/Internals.md): library replacements and patch implementation.

This is an experimental, AI-assisted project. See the verification status for
what has been tested.

## Windows setup

Download and extract the official
[Keystone 0.9.2 Windows release](https://github.com/keystone-engine/keystone/releases/tag/0.9.2),
then copy `keystone.dll` directly to the repository root.

```powershell
# In your repository checkout:
uv sync --frozen
# You can also copy these files with Windows Explorer.
Copy-Item D:\Downloads\keystone-0.9.2-win64\keystone.dll .\keystone.dll
# After downloading the Procursus Windows x86_64 release:
Copy-Item D:\Downloads\ldid_w64_x86_64.exe .\ldid.exe
# Put the selected Lib*Bridge.dylib release assets in this checkout, then run:
uv run npa-patch --dylibs-dir . D:\IPAs\nPlayer_3.13.0.ipa
```

The repository-root `ldid.exe` takes precedence on Windows. Alternatively,
place it elsewhere and add that directory to `PATH`.

## Advanced options

The default combines `libass` and `ffmpeg-full`. `--dylib` replaces that default,
so list every feature you want. 

Experimental `renderer-highbit` output applies
to software decoding and is disabled by default.

For higher ASS/SSA animation frame rates, **[nPlayerEnhance](https://github.com/DYY-Studio/nPlayerEnhance)** can be used
alongside this project.

- `-o/--output <path>` chooses the output IPA path.
- `--dylib <id>` (repeatable, default: `libass` and `ffmpeg-full`)
- `--dylibs-dir <dir>` (default: the working
  directory; each dylib is looked up as `<dir>/<basename>`).
- `--manifests <dir>` selects the manifest directory (default: `manifests/`).

The command prints a JSON summary with the input hash, output hashes, one hash
per shipped dylib, and the number of verification checks that passed.

Select exactly what you want:

```sh
uv run npa-patch --dylib libass "/path/to/nPlayer_3.13.0.ipa"   # subtitles only, minimal and most benifited
# subtitles plus the whole FFmpeg 4.4.8 (the default):
uv run npa-patch --dylib libass --dylib ffmpeg-full "/path/to/nPlayer_3.13.0.ipa"
# subtitles plus the 4.4.8 core with the 9.0.2 scaler/resampler instead:
uv run npa-patch --dylib libass --dylib ffmpeg --dylib ffmpeg-core "/path/to/nPlayer_3.13.0.ipa"
# subtitles plus the 9.0.2 input side with the output side at 4.4.8:
uv run npa-patch --dylib libass --dylib ffmpeg-core902 --dylib ffmpeg-out448 "/path/to/nPlayer_3.13.0.ipa"
# add the standalone high-bit-depth renderer to a selection:
uv run npa-patch --dylib libass --dylib ffmpeg-full --dylib renderer-highbit "/path/to/nPlayer_3.13.0.ipa"
```

Be careful that the names are little tricky. Please check [Internals](docs/Internals.md) for more details.

Overlapping FFmpeg selections cannot be combined:
the 4.4.8 core and the 9.0.2 core are exclusive, and `ffmpeg-out448` is refused
next to `ffmpeg-core`/`ffmpeg-full`. 

These combinations are rejected before
writing an IPA. Missing or mismatched release files stop the patching process.

## Troubleshooting

| Message | Cause | Fix |
| --- | --- | --- |
| `still FairPlay-encrypted` | the IPA comes straight from the App Store | provide a decrypted dump of your own purchase |
| `no manifest matches this main executable` | unsupported, already-patched or modified input IPA | use a supported, clean decrypted dump |
| `bridge.exports` / `bridge.install_name` failed | wrong or stale dylib | use the dylib from the matching release |
| `bridge dylib for <id> is missing` | the selected dylib is not in `--dylibs-dir` | copy it there or point `--dylibs-dir` at it |
| `unknown dylib ids: <id>` | typo in `--dylib` | use a feature ID from the examples above |
| `conflicting dylib selection: ...` | `ffmpeg-full` was combined with `ffmpeg`/`ffmpeg-core`, or `ffmpeg-out448` with `ffmpeg-core`/`ffmpeg-full` | pick one core selection; `ffmpeg-out448` pairs with `ffmpeg-core902` |
| `ldid is required to assemble the IPA` | ldid is unavailable | use Homebrew on macOS, a prebuilt Linux binary, or copy the Procursus Windows x86_64 release to the repository root as `ldid.exe` (with `PATH` as a fallback) |
| `host assembler library is missing: ...keystone.dll` | the Windows assembler is not at the repository root | copy `keystone.dll` from the official Keystone 0.9.2 Windows release directly to the checkout root |
| `WinError 193` or a DLL load failure mentioning `keystone.dll` | the DLL has the wrong architecture or a runtime dependency is unavailable | use the official 64-bit Windows Keystone 0.9.2 DLL and install its required Microsoft runtime |
| `host assembler library is missing` | the macOS/Linux assembler is not in the checkout | run `make bootstrap`, or use the matching release asset |

## License

The toolchain is [MIT licensed](LICENSE). Bundled library licenses are listed in
[THIRD-PARTY.md](THIRD-PARTY.md). This project is independent of the nPlayer
authors; patch only your own licensed copy.
