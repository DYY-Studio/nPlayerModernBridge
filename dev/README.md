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

The expected packaged main hashes after the 2026-09-26 fixes are
`e84ef5b5e10cb10940ecffe73c3509f932a4aa6d2cba053052a7d9e7549792fe` for
`--dylib libass`, `a5243f0a36baf5ef5209d51f312bd9d6f0c8d3b05d4053fcbbaa48735339f83b`
for `--dylib ffmpeg` and
`3bee29d20c4cc6e5979f594dc8df34a6c0fd96e48240e1c2d6a0065fe69be810` for
libass + ffmpeg. The default selection, libass plus the whole FFmpeg 4.4.8, is
`f22d7af623272032e3c529b0cfc0e1b9340b42e310756f6b817f438e57f6758a`; the split
alternative, libass + ffmpeg-core + ffmpeg, is
`638c00d9602b2797d3f18030ebc6ada4bf825a3f871f2f549374fbdecddcdd78`. The pre-fix values
(`19d3447193bcd66e03b850876a1281c4bceac087dd50cf6db534e0527fb3a887` and
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
   `build/LibFFmpegBridge.dylib` and `build/LibFFmpegCoreBridge.dylib`, plus
   `libkeystone.dylib`, as release assets together with their SHA-256, plus
   `LICENSE` and `THIRD-PARTY.md`. These are host-side products; `make bootstrap`
   reproduces the assembler.
3. When any pinned dependency version changes, update `THIRD-PARTY.md` and the
   matching `dylibs[].library_version` in the manifest in the same commit.
4. Run `uv run python dev/abi_probe.py` after an iOS SDK change and re-check
   the manifest's `target_abi` block.
