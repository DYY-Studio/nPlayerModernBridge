# Developer material

Everything here exists to build or re-verify the bridge, not to patch an IPA.
The public flow (`npa-patch`) never touches this directory.

- `smoke/` + `tools/smoke.py` — the BridgeSmoke app that proves the 15-export
  ABI contract on a device. Requires Xcode and the iOS SDK.
- `smoke_package.py` — pseudo-signs and packages that app bundle.
- `tools/phase_a.py`, `tools/phase_b.py` — run the two layout stages separately
  to isolate a failure. `npa-patch` runs the same functions in one pass.
- `abi_probe.py` + `target_abi_probe.c` — compile a probe against the iOS SDK
  and print the constants that `manifests/*.json` freezes in `target_abi`.
  Run it after an SDK change and compare with the manifest.
- `acceptance.json` — the recorded device acceptance (iPhone SE 3rd gen,
  iOS 17.7.2, LiveContainer 3.7.2).
- `ffmpeg-core-enable-set.md` + `tools/enable_set_diff.py` — the comparison of
  the core unit's FFmpeg component set against the app's own, and the TLS gap
  it found.
- `plans/` — the research plan that produced this toolchain.
- `tests/` — developer tests (dependency closure, Keystone/probe, relinking a
  bridge with an extra export). Run them explicitly: `uv run pytest dev/tests`.
  The default `uv run pytest` only covers the public suite.

## Rebuild everything from source

```sh
uv sync --frozen --all-groups
make bootstrap deps bridge      # host Keystone, iOS dependency closure, bridge dylib
make verify test
```

`deps/sources.lock.json` (libass closure), `deps/ffmpeg.lock.json` (9.0.2
scaler/resampler) and `deps/ffmpeg-core.lock.json` (the 4.4.8 closure: core,
scaler and resampler) pin every dependency (version, archive URL, SHA-256);
the closure is built with `deps/ios-arm64.cross` and `deps/macos-arm64.native`.
`deps/` is optional for users: the release ships the built `LibASSBridge.dylib`.

## Re-run the device acceptance

1. `make bridge smoke`, then install `dist/smoke.ipa` on a device and read the
   on-screen log; the last line must be `SMOKE: PASS`.
2. Patch your own decrypted IPA with the release dylib and walk the subtitle
   matrix in `plans/2026-09-24-libass-bridge-prototype.md` (Task 11, Step 2-4):
   SRT, embedded ASS, Matroska embedded fonts, the font cache, seek/flush and
   continuous playback.
3. Prove the whole-unit fallback: the payload resolves a unit's symbols through
   `dlsym` and publishes `OLD` when a lookup or the identity check fails, so an
   artifact that keeps the patch but loses the dylib must behave exactly like
   the baseline. Build one by removing the selected dylib from a patched IPA -
   the main is untouched, so this isolates the missing dylib:

   ```sh
   uv run npa-patch --dylibs-dir build -o build/accept/full.ipa "<clean IPA>"
   rm -rf build/accept/fallback-tree
   unzip -q build/accept/full.ipa -d build/accept/fallback-tree
   rm build/accept/fallback-tree/Payload/nPlayer.app/Frameworks/LibFFmpegFullBridge.dylib
   (cd build/accept/fallback-tree && zip -q -r -y ../full-fallback.ipa Payload)
   ```

   `unzip -p build/accept/full-fallback.ipa Payload/nPlayer.app/nPlayer |
   shasum -a 256` must still print the packaged main of the unmodified patch.
   On the device, play the same material as the pass above (a 10-bit HEVC /
   Matroska file, thumbnails, and an AirPlay cast): it must work, and the app
   must be using its own FFmpeg 4.4.5 for all 110 entry points. Reading the
   three unit state words under PlayCover is the stronger form - each must read
   3 (`OLD`) and playback must be unchanged. A Frida attach reads them without
   elevation: the state words sit in `__NPATCH_DATA` at the installed binary's
   segment RVA plus each unit's offset from the manifest, and the 2026-09-27
   pass recorded them that way (unit offsets `0x0`, `0x80`, `0x3a0`, `0x3d0`
   for the default selection). The identity arm of the check (a
   dylib at the right path exporting the right symbols) is pinned by
   `tests/test_payload.py`, not constructible as an artifact, because the load
   path and the expected basename both come from the same manifest field.
4. Append the device, iOS version, install method and both results to
   `acceptance.json`.

The expected packaged main hashes, after the 2026-09-26 fixes and with the
manifest's app-level sites (the two font guards and the UPnP retiming) applied to
every selection (2026-09-28), are
`09dcc851d8a26fb27a6d7dbc789e3147f146ff4a7e6f1eb6b9df82bf105fd469` for
`--dylib libass`, `56b96f63a8f751f8cca791a53b6ebcd7fe92b594c7a633028bd0561603bb63b5`
for `--dylib ffmpeg` and
`9f7acf21d112c5711cd505ce7d54c17742c577bcebb2c04d5f2aea7662cd6ded` for
libass + ffmpeg. The default selection, libass plus the whole FFmpeg 4.4.8, is
`0ab724dc04125b44b69806fb60b30aad240289a59787d2bbe8be4aeaa88d98f4`; the split
alternative, libass + ffmpeg-core + ffmpeg, is
`3dbcf7de246581c5b876e533a463960a094643f88e790cda118240462de9a9de`. The 9.0.2
selections: `libass + ffmpeg-core902` is
`e823aa1a3806edf331e46eaba9c1d297112de9ddbe7c101df7e68a4273672a50` and
`libass + ffmpeg-core902 + ffmpeg-out448`, which adds the 4.4.8 output side, is
`c0990ab70cf7c79cada52923fe889b7e4ffea47cc0481d37b00bfd22285d3fa5`. Every value
here was measured on the merged tree on 2026-09-28; the two 9.0.2 ones moved with
the app-level sites, and `libass + ffmpeg-core902` had already moved once before
that with the AC3 fix (b7a8f017..., 4abfb2ae... are the pre-merge values). The
pre-fix values(`19d3447193bcd66e03b850876a1281c4bceac087dd50cf6db534e0527fb3a887` and
`4d7e79ba3d2a6a1afaa68948002ee3da36ed9c33cf1e7801df122539572b2272` for the
default selection) are void: those payloads never activated the bridge (see
`acceptance.json`).

For the `ffmpeg-core` unit the matrix is wider than for libass: every container
the app supports, the network paths (http, https, HLS and rtmp; rtmps and rtsp
have no source available here and are recorded as uncorroborated), the mux/encode
path (muxers, encoders and bitstream filters), which AirPlay and Chromecast reach
through the HLS transcode/mux session for a local non-mp4 source while the
digital-audio passthrough setting and MJPEG covers reach the muxer and encoder -
see `notes/ida-investigation-7-mux-encode-entry.md` - the audio formats FFmpeg
decodes, AV1
- which this build decodes through libdav1d alone - a software video fallback,
and 10-bit sources (P010/HEVC), which the first device run proved the swscale
shims had to translate and which the all-4.4.8 dylib now handles with no
translation at all. When a pin changes, rebuild, re-run the component
comparison in `dev/ffmpeg-core-enable-set.md` against the app's own FFmpeg and
re-check the offsets asserted in `bridge/ffmpeg-core-abi.h`.

## Release checklist

1. `make bridge` and `make verify`.
2. Publish `build/LibASSBridge.dylib`, `build/LibFFmpegFullBridge.dylib`,
   `build/LibFFmpegBridge.dylib`, `build/LibFFmpegCoreBridge.dylib`,
   `build/LibFFmpegCore902Bridge.dylib`, `build/LibFFmpegOut448Bridge.dylib` and
   `build/LibRendererHighBitBridge.dylib`, plus `libkeystone.dylib`, as release
   assets together with their SHA-256. Include `LICENSE`, `THIRD-PARTY.md` and
   Keystone's `COPYING`, `EXCEPTIONS-CLIENT` and `llvm/LICENSE.TXT`. These are
   host-side products; `make bootstrap` reproduces the assembler.
3. When any pinned dependency version changes, update `THIRD-PARTY.md` and the
   matching `dylibs[].library_version` in the manifest in the same commit.
4. Run `uv run python dev/abi_probe.py` after an iOS SDK change and re-check
   the manifest's `target_abi` block.
