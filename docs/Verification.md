## Verification status

> [!Warning]
> Every result recorded before 2026-09-26 was produced with a dispatch payload
> that never activated: 
> 
> a successful `dladdr` was read as a failure and the
> basename scan stopped at the first slash, so each unit stayed on the app's own
> library. Those entries are void; see the `invalidated` section of
> `dev/acceptance.json`.

The dispatch fix (2026-09-26) makes the resolve block treat a `dladdr` success
as a success and compare the final path component of `dli_fname`. Both units are
now verified on a device (iPhone SE 3rd generation, iOS 17.7.2, LiveContainer
3.7.2): with the fixed payload every unit state word reads `NEW` and the `\kt`
probe renders libass 0.17 behaviour (`\kt` only exists from 0.17.0), where the
bundled 0.13.7 ignores it. The standalone bridge smoke app ends with
`SMOKE: PASS`. `dev/acceptance.json` records the details and `dev/plans/` holds
the reverse-engineering plan behind the addresses. Rendering differs pixel-wise
from libass 0.13, which is expected.

The FFmpeg unit needed one more fix before it worked. The app's FFmpeg 4.4
numbers three `AVPixelFormat` members that 9.0.2 removed, and because they sit
inside the enum every later value moved, so the scaler read `AV_PIX_FMT_P010LE`
as `AV_PIX_FMT_GBRAP12LE` and P010/HEVC thumbnails either crashed (iOS) or
rendered garbage (macOS, lower half green). The swscale shims now translate the
legacy format before forwarding (commit `05423b2`). On the device P010/HEVC
thumbnails, playback, H.264/AVC and audio are all normal. HDR tone mapping
differs slightly, which is expected: the app never calls
`sws_setColorspaceDetails` and routes its Color Space setting only to the
display layer, so HDR conversion uses swscale 9.0.2's own defaults instead of
4.4.5's.

The FFmpeg core needed TLS as well, which the app's FFmpeg gets from OpenSSL.
Without it an https M3U8 did not play on the default selection at all - no
duration, no stream data, and the hardware decoder failing over to software -
while the same stream over http played and an https direct URL played through
the app's curl stack. The closure is now configured with
`--enable-securetransport` and the dylib links `Security.framework`
(`deps/ffmpeg-core.lock.json`), so the unit carries `ff_tls_protocol` and
`ff_https_protocol`. Apple's TLS is a different implementation than the app's
OpenSSL, so its behaviour is measured rather than assumed: https HLS now passes
on the device, and rtmps stays on the same open list as the rest of the matrix.

For the same input the libass-only patch produces a main whose SHA-256 is
`09dcc851d8a26fb27a6d7dbc789e3147f146ff4a7e6f1eb6b9df82bf105fd469`, the
FFmpeg-only patch
`6ecb085b98f04abe6af27d909a51a7a90a14f748efcc35d97acd195d452ccb52`, and the
libass+ffmpeg selection
`9f7acf21d112c5711cd505ce7d54c17742c577bcebb2c04d5f2aea7662cd6ded`. The default
selection, libass plus the whole FFmpeg 4.4.8, is
`0ab724dc04125b44b69806fb60b30aad240289a59787d2bbe8be4aeaa88d98f4`; the split
alternative, libass with the 4.4.8 core and the 9.0.2 scaler/resampler, is
`3dbcf7de246581c5b876e533a463960a094643f88e790cda118240462de9a9de`. Every
selection also carries the manifest's app-level site (see the README), so these
anchors moved on 2026-09-28, when that site started being applied to all of them;
the artifacts the earlier matrix was accepted on therefore carry the previous
hashes. Both selections rewrite the same 485
call sites plus the two libass NOP guards, so they differ only in which dylibs
carry the units.

The whole-4.4.8 dylib carries the `ffmpeg-core` code and the scaler/resampler of
the same closure, so it inherits every core result below; the new part is that
`sws_*`/`swr_*` now run on 4.4.8 instead of 9.0.2, which is what removes the
legacy `AVPixelFormat` translation the 9.0.2 shim needed for P010/HEVC. On the
device that row passes on this dylib - it shows the path needs no translation at
all, rather than that a translation is correct. The rest of the matrix has not
been re-run on it.

The `ffmpeg-core` unit is live on the same artifacts. Its code paths were
demonstrably running when they failed - the P010/HEVC crash and the https failure
above are only reachable inside that unit - and after the two fixes the default
selection passes https HLS, P010/HEVC on the hardware and the software decode
path, AV1 software decode through libdav1d, audio, the containers mkv/webm, mp4,
mpeg, ts and wmv, software decode of mpeg1/vp8/wmv3, continuous playback, seek,
thumbnails and speed change; the subtitle matrix, with the `\kt` probe, shows the
libass unit itself live in the same run. SPDIF passthrough, rtmps and rtsp
cannot be corroborated here - no passthrough-capable output chain and no such
servers - so they are recorded as unavailable rather than failed or unmeasured;
the whole-unit fallback is proven (with the selected dylib removed, every unit it
carries reads `OLD` while the other dylib keeps its own `NEW`), and the mux/encode
surface does have user entries -
AirPlay and Chromecast start an HLS transcode/mux session for a local non-mp4
source, the digital-audio passthrough setting drives the SPDIF muxer, and MJPEG
cover encoding runs for the browser and info panels - and AirPlay playback passes
on the device. `dev/acceptance.json` records the artifact and the full list.

The app-level site is verified as well. The main binary waits 1000 ms in
`net::`'s UPnP/SSDP discovery loop whenever its `select()` comes back empty, and
on device that wait lands on the start of playback: a run without the retiming
stalls about a second (`sleep ms=1000`, `enqueue-gap 1004`, PTS continuous, so
the queue empties rather than frames being dropped). Retiming `0x100AE3C7C` to
50 ms removes the stall on the default selection and on the libass-only one
alike, and a build left at 1000 ms stalls in the same place with the whole
FFmpeg 4.4.8 payload installed - the stall follows that constant, not the FFmpeg
version. Measured with VP9 4K60 material.