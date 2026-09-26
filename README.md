# nPlayer iOS Bridge

> A small tribute to nPlayer, an exceptionally well-designed player that has served us reliably for years.

Replace the bundled ...
- libass 0.13.7 stack with **libass 0.17.5**
- FFmpeg 4.4.5 libs with one of
  1. Full **FFmpeg 4.4.8** 
  2. Swscale + Swresample **FFmpeg 9.0.2** + Core **FFmpeg 4.4.8**

... in your own **nPlayer 3.13.0** install. 

No jailbreak, no inline hooks, bring modern ASS/SSA rendering and media processing to this great player.

> [!Warning]
>
> **Vibe Coding Project**

## What this does

`npa-patch` takes a decrypted nPlayer IPA you own and writes a patched copy:

- **two** existing guards are turned into NOPs, which fixes that only ASS/SSA of 
  the first video in the playback sequence can use font attachments in Container (e.g. Matroska).
- the **fifteen** libass entry points the app calls are redirected through a small
  payload, which loads `LibASSBridge.dylib` on first use and falls back to the
  app's own libass if that ever fails, 
- **FFmpeg**: three choices, mutually exclusive where they overlap
  1. **full 4.4.8** (the default) - the **110** FFmpeg entry points the app calls
    (demuxing, decoding, encoding, muxing, the bitstream filters, the scaler and
    the resampler) are replaced by **one** dylib, `LibFFmpegFullBridge.dylib`
    (FFmpeg 4.4.8, built from the same sources as the app's own 4.4.5 so the two
    share one ABI). They go through three units: `ffmpeg-core` for the 99 core
    entry points, `libswscale` (13 call sites) and `libswresample` (7),
  2. **split** - `LibFFmpegCoreBridge.dylib` carries the 99 core entry points at
    4.4.8 and `LibFFmpegBridge.dylib` carries the `sws_*`/`swr_*` entry points at
    **9.0.2**. Choices 1 and 2 cover the same call sites, so the tool refuses a mix,
  3. **9.0.2 core** (experimental) - `LibFFmpegCore902Bridge.dylib` carries FFmpeg
    **9.0.2** `libavformat` for the pure-playback demuxer's 25 class-A call sites
    plus 9.0.2 libswscale and libswresample, in one dylib. Those 25 sites are the
    4.4.8 core's demux face, so this choice is mutually exclusive with the
    `ffmpeg-core` unit of choices 1 and 2 and with the standalone
    `LibFFmpegBridge.dylib`; class B subtitles, class C remux, the mux face and
    avcodec stay on the app's own 4.4.5, behind a translated legacy-shaped shadow
    context,
- `Frameworks/LibASSBridge.dylib` is added. It statically links libass 0.17.5,
  FreeType, **HarfBuzz**, FriBidi, fontconfig and expat, with no third-party
  dynamic dependency,
- `Frameworks/LibFFmpegFullBridge.dylib` is added for the default selection. It
  statically links libavformat, libavcodec, libavutil, libswscale and
  libswresample 4.4.8 plus libdav1d 0.9.2, and depends on system libraries and
  frameworks only,
- `Frameworks/LibFFmpegCoreBridge.dylib` and `Frameworks/LibFFmpegBridge.dylib`
  are added for the split selection. The first statically links the four 4.4.8
  libraries plus libdav1d 0.9.2, the second the `--disable-everything` 9.0.2
  build of libavutil, libswscale and libswresample; both depend on system
  libraries and frameworks only,
- `Frameworks/LibFFmpegCore902Bridge.dylib` is added for the 9.0.2 core
  selection. It statically links 9.0.2 libavutil, libavcodec, libavformat,
  libswscale and libswresample, and depends on system libraries and frameworks
  only,
- every binary is pseudo-signed so the bundle loads.

The patch is organised in **units**: libass, `ffmpeg-core`, libswscale,
libswresample and `ffmpeg-demux`. 
- Each unit arbitrates its own state at first call and falls back
on its own, so a failure in one never turns off another. 
  - `ffmpeg-core` is 
deliberately one unit covering libavutil, libavcodec and libavformat together: 
the app reads those structures directly, so half a swap would let one library
interpret the other's memory. 
  - The whole-4.4.8 dylib carries all three FFmpeg
units, so a scaler failure does not take the demuxer with it while the three
still share one libavutil. 
  - the 9.0.2 core dylib carries the `ffmpeg-demux`, `libswscale` and
libswresample units together, so it replaces a 4.4.8 face and the
scaler/resampler at once. `ffmpeg-demux` is a demux-only slice of 9.0.2
`libavformat` that keeps a 4.4.x avcodec behind a translated legacy-shaped
shadow context, and it patches the same 25 call sites as the core unit's demux
face, so the tool refuses to select the two together.
- A unit is only installed when its dylib is selected,
so `npa-patch --dylib libass` produces an artifact that is byte-identical to
the libass-only patch of the same input.

The dylibs are built from this repository; only the patch tooling and those
dylibs are distributed. No nPlayer IPA is included.

Recommend to use with **nPlayerEnhance**, which unlock ASS/SSA animation framerate limits.

## Requirements

- macOS (Dev and Patch) or Linux (Patch only)
- Python ≥ 3.11, [uv](https://docs.astral.sh/uv/), `ldid`, `zip` and `unzip`. 
  - macOS: Install ldid with `brew install ldid`
  - Linux: Use a [prebuilt binary](https://github.com/ProcursusTeam/ldid/releases) or build it yourself.
- Your own **decrypted** nPlayer 3.13.0 IPA. 
  - App Store packages are FairPlay-encrypted and are rejected on purpose.
  - This project ships no IPA and no decryption.
- The host files from the release assets. All are host-side build products; 
  `make bootstrap` builds the assembler and `make bridge` builds the
  dylibs locally if you prefer that.
  - `LibASSBridge.dylib` (libass 0.17.5 for iOS arm64)
  - `LibFFmpegFullBridge.dylib` (FFmpeg 4.4.8 for iOS arm64) for the default
    selection.
  - `LibFFmpegBridge.dylib` (FFmpeg 9.0.2 for iOS arm64) and
    `LibFFmpegCoreBridge.dylib` (FFmpeg 4.4.8 for iOS arm64) when you want the
    split selection instead.
  - `LibFFmpegCore902Bridge.dylib` (FFmpeg 9.0.2 for iOS arm64) when you want
    the 9.0.2 core selection instead of the 4.4.8 core unit.
  - `libkeystone.dylib` (the arm64 assembler used to encode the dispatch payload, macOS arm64 only); On Linux, please build the
  assembler `libkeystone.so` with `make bootstrap` instead.
- No Xcode, no iOS SDK, no jailbreak. `npa-patch` runs from the repository
  checkout, next to `manifests/`.

## Quick start

```sh
git clone <this repository> && cd nplayer-libass-bridge
# put LibASSBridge.dylib, LibFFmpegFullBridge.dylib,
# LibFFmpegBridge.dylib, LibFFmpegCoreBridge.dylib, LibFFmpegCore902Bridge.dylib
# and libkeystone.dylib from the release assets here
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
```
The two FFmpeg selections replace the same call sites, so they cannot be
combined: naming both fails before anything is written. A missing or stale dylib
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
| `conflicting dylib selection: ...` | `ffmpeg-full` was combined with `ffmpeg`/`ffmpeg-core` | pick one FFmpeg selection; they replace the same call sites |
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
