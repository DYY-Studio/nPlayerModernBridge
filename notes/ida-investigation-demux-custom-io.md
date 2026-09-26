# Demux custom IO: the app always feeds libavformat through its own AVIOContext

Investigated to answer "what is the custom-IO path and is it reachable".
DB `nPlayer.i64` read-only. All addresses are this build's.

## Shape (Confirmed)

Both class-A opens build the input by hand:

```
avio_alloc_context(NULL, 0, 0, opaque, read_packet, NULL, seek)
ctx->pb                   = avio
ctx->flags               |= 0x80001
ctx->error_recognition   |= 0x8000            // AV_EF_IGNORE_ERR
ctx->interrupt_callback   = { callback, opaque }
avformat_open_input(ctx, url, NULL, &opts)    // url is only a hint
```

- `opaque` is a wrapper `{ stream* @+0, int state @+8, byte flag @+12 }`; the stream
  itself is an app object: `core::FileStream` (local file), `net::CURLStream`
  (app-managed http/https), `core::MemoryCachedStream`, or a DRM stream.
- read/seek delegate to the stream's vtable: **+56 getSize, +64 getPosition,
  +72 read, +88 seek**. No crypto/zlib in the callbacks; decryption lives inside
  the stream object.
- AVFormatContext offsets seen in the app match the shim's legacy constants:
  pb `0x20`, flags `0x460`, error_recognition `0x4c4`, interrupt_callback
  `0x4c8`/`0x4d0`.
- buffer=NULL / buffer_size=0 is deliberate and supported: `avio_read` bypasses
  the internal buffer when `size > s->buffer_size` and reads straight into the
  caller's buffer (`libavformat/aviobuf.c: avio_read`). So no internal buffer is
  ever allocated and **`pb->buffer` stays NULL**; `ffio_init_context` does not
  allocate one either.

## Why CUSTOM_IO is set although the app only ORs 0x80001

`AVFMT_FLAG_CUSTOM_IO` is `0x80` and the app never sets it. libavformat sets it
itself: `avformat_open_input` does `if (s->pb) s->flags |= AVFMT_FLAG_CUSTOM_IO;`
(`libavformat/demux.c:256`, also `init_input` `:166`). `avformat_close_input`
then nulls its pb copy and never closes it (`demux.c:389`). The app owns the
AVIOContext and frees `pb->buffer` itself - in practice `free(NULL)`.

## The five avio_alloc_context sites (xrefs to 0x10073AD30)

| site | enclosing fn | what it is | class |
| --- | --- | --- | --- |
| `0x100A3E388` | `sub_100A3E1F8` | **media::MediaInfoFetcher** metadata/artwork/thumbnail probe | A |
| `0x100AED778` | `sub_100AED374` | **media::FFmpegDemuxer** open-from-stream (custom IO) | A |
| `0x100AB4DCC` | `sub_100AB4C60` | **media::FFmpegSubtitle** (ASS) | B |
| `0x100B30234` | `sub_100B3010C` | **media::SPDIF** | C |
| `0x100B95008` | `sub_100B94DA0` | **media::MediaServer** HLS session | C |

## The two class-A open variants

- `sub_100AED374` - FFmpegDemuxer vtable idx 33 (only xref: vtable data
  `0x1016C2b28`). The custom-IO open above; stream stored at `0x100AED6E0`. If
  the stream's first bytes are `"[Reference]"` (`0x1011D1099`, single xref
  `0x100AED470`) it parses `KEY=VALUE` lines, keeps keys containing `"REF"`, and
  delegates to idx 34.
- `sub_100AEF53C` - idx 34. The **native-URL** open: no `avio_alloc_context`, it
  rewrites the URL to `mmsh`/`mmst`/`librtmp` (`sub_1009ADF4C`) and lets
  libavformat open it.

## What the app reads from the AVIOContext

Only `*(void **)(avio + 8)` = `buffer`, at close, to free it:
`sub_100A3E1F8` @`0x100A415C0` and `sub_100AEF9B0` @`0x100AEFA18`. Nothing reads
`error`/`write_flag`/`eof_reached`/`pos`/`direct`/`opaque`. `avio_size(pb)` is a
call, not a field read (`0x100A46874`, `0x100AEFC18`).

=> The cross-major `AVIOContext` offset drift is **unreachable** here: `buffer`
is `0x8` in both majors, and it is NULL anyway.

## Reachability (Confirmed)

- idx 33 / `0x100AED778` is the **ordinary playback open**. `media::MediaPlayerImpl`
  open builds a stream (`core::FileStream` for local files, `net::CURLStream` for
  app-managed http/https, optionally wrapped in `core::MemoryCachedStream` with an
  8 MB cache) and calls demuxer idx 7 -> `sub_1009F0D18` -> idx 33. So the device
  matrix's local-file and app-managed-network rows already exercised this site.
- `0x100A3E388` is `media::MediaInfoFetcher` (`fetchInfoWithURL:` /
  `fetchInfoWithThumbnailProvider:` @`0x100AAAC38` / `0x100AAB6D4`) - the
  info/artwork/thumbnail route.
- the `[Reference]`/mmsh branch and idx 34 are the narrower capability.

## https HLS - resolved (it is the FFmpeg chain)

The `tls-fix` acceptance entry settles it: a build without a TLS backend failed
an https HLS with "https protocol not found", while the same HLS over http
played and an https *direct* URL played. The same entry records that an https
direct URL goes through the app's curl stack, not FFmpeg. So:

- an https direct URL -> the app's own stream -> the **custom-IO open (idx 33)**;
- https HLS -> FFmpeg opens the URL itself -> the **native-URL open (idx 34)**.

Consistent with that, the 9.0.2 demux closure carries a TLS backend and the HLS
demuxer: `deps/ffmpeg-demux.lock.json` has `--enable-securetransport` and
`-framework Security`, and `build/deps/ffmpeg-demux/lib/libavformat.a` defines
`ff_tls_protocol` / `ff_https_protocol` (`tls.o`, `tls_securetransport.o`) and
`ff_hls_demuxer` (`hls.o`). So the https HLS row already exercised the class-A
native-URL open: sites `0x100AEF684` / `0x100AEF778` / `0x100AEF7A8` and read
`0x100AEFE28`.

## Still open

- the `[Reference]` branch only: the `KEY=VALUE` parsing and the delegation from
  idx 33 to idx 34. No device row has driven it. It is undriven, not a capability
  gap - the demux closure carries the same protocol set as the core
  (`ff_mmsh_protocol`, `ff_rtmp_protocol`, `ff_http_protocol` are defined in both
  archives).
- the field identities of AVFormatContext `+0x4c8`/`+0x4d0` are recorded by
  offset only (they are not the `io_open`/`io_close` defaults at
  `0x5b8`/`0x5c0`).
