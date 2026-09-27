# Cross-major enum renumbering (HEVC/VP9/AV1/subtitle/font)

Found while debugging "HEVC MKV black-screens while H.264 MKV plays" after the
attachment-extradata fix. Read-only IDA; no database modified.

## Symptom

The demux log showed a healthy open and `find_stream_info` for the HEVC file,
then EOF; the app never displayed. H.264 played. The demux was not at fault -
the values handed to the app were.

## Cause (Confirmed)

`libavcodec`/`libavutil` renumber public enums across major versions. The app
matches codec ids and pixel formats against its own 4.4.x values, but the
demux shim copied the 9.0.2 values straight into the 4.4.x-shaped shadow:

| enum | 4.4.8 | 9.0.2 |
|---|---|---|
| `AV_CODEC_ID_HEVC` | 173 | 172 |
| `AV_CODEC_ID_VP9` | 167 | 166 |
| `AV_CODEC_ID_AV1` | 32797 (`Y41P = 0x8000` anchor) | 222 |
| `AV_CODEC_ID_ASS` | 96269 | 94230 |
| `AV_CODEC_ID_OTF` | 100355 | 98310 |
| `AV_PIX_FMT_YUV420P10LE` | 64 | 62 |
| `AV_PIX_FMT_VIDEOTOOLBOX` | 160 | 157 |

233 `AV_CODEC_ID_*` and 224 `AV_PIX_FMT_*` values differ. H.264 (27) and Opus
(86076) happen to match, which is why H.264 MKV worked and HEVC/VP9 did not:
`sub_1007BA2F4` gates the `hevc_mp4toannexb` BSF on `codec_id == 173`, and the
app's decoder lookup by codec id found the wrong entry.

## Fix

`deps/gen_demux_enum_map.py` compiles a name probe against both header trees
and emits `bridge/ffmpeg-demux-enum-map.h`; the shim translates `codec_id` and
`format` before copying them into the shadow. A value with no 4.4.x equivalent
maps to the neutral codec/format, so the app can never match a different codec
or pixel format by accident.

All other enum-valued fields the shim carries (`codec_type`, `disposition`,
`discard`, `field_order`, `color_range`/`primaries`/`trc`/`space`,
`chroma_location`, `AVFormatContext.flags`, `AVPacket.flags`) were probed and
are identical across the two versions; only `codec_id` and `format` differ.

## Note

This invalidates the plan's assumption that fields can be copied verbatim
between majors. Any future shadow field that carries an FFmpeg enum needs the
same treatment.
