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
`e84ef5b5e10cb10940ecffe73c3509f932a4aa6d2cba053052a7d9e7549792fe`, the
FFmpeg-only patch
`a5243f0a36baf5ef5209d51f312bd9d6f0c8d3b05d4053fcbbaa48735339f83b`, and the
libass+ffmpeg selection
`3bee29d20c4cc6e5979f594dc8df34a6c0fd96e48240e1c2d6a0065fe69be810`. The default
selection, libass plus the whole FFmpeg 4.4.8, is
`f22d7af623272032e3c529b0cfc0e1b9340b42e310756f6b817f438e57f6758a`; the split
alternative, libass with the 4.4.8 core and the 9.0.2 scaler/resampler, is
`638c00d9602b2797d3f18030ebc6ada4bf825a3f871f2f549374fbdecddcdd78`, the three-unit
artifact the bridge was device-accepted on. Both selections rewrite the same 485
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