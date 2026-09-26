# Class-A demux: attachment extradata ownership (crash)

Investigated after the first class-A device run aborted on a background demux
thread. Read-only IDA session on `nPlayer.i64`; no database was modified.

## Symptom

```
6  LibFFmpegDemuxBridge.dylib  codec_parameters_reset + 8 (codec_par.c:34) [inlined]
7  LibFFmpegDemuxBridge.dylib  avcodec_parameters_free + 28 (codec_par.c:73)
8  nPlayer                    0x104b67a08   -> static 0x100AEFA08
malloc: pointer being freed was not allocated
```

Frame 8 is the return address of the call at `0x100AEFA04`, i.e. the crash
happened inside the app's `avformat_close_input` (redirected to
`npa_demux_avformat_close_input`, which calls the modern close). The modern
`codec_parameters_reset` (9.0.2) runs `av_freep(&par->extradata)` first
(`codec_par.c:35`).

## Evidence

`sub_100AEF9B0` (0x100AEF9B0, class-A teardown):

- `0x100AEFA04` `avformat_close_input(&ctx)` (old target `0x100827B08`).

`sub_100AEDD90` (0x100AEDD90, class-A stream setup) per stream
(`st = fmt->streams[i]`, `codecpar = st->codecpar` at `st+0xD0`):

- `0x100AEE35C` `STR WZR, [X19,#0x18]` — clears `codecpar->extradata_size`.
- `0x100AEE368` `ADD X0, codecpar, #0x10` then `0x100AEE36C` `av_freep` —
  frees **`codecpar->extradata`** for one stream kind (the branch tested at
  `0x100AEE350`, reached for the attachment/media kind; Matroska attachment
  streams carry embedded fonts).

So the app deliberately drops a codecpar's extradata through the shadow.

## Cause (Confirmed)

The demux shim's shadow `AVCodecParameters.extradata` was set to the modern
`codecpar->extradata` pointer. When the app freed it through the shadow, the
modern buffer was freed underneath the modern context; `avformat_close_input`
then freed the same pointer again.

## Fix

The shadow owns a copy of `extradata` (`av_malloc` + `memcpy`) and frees it in
`shadow_free_streams`. `extradata` was the only field the app frees through the
shadow: the class-A `av_dict_free` sites (`0x100AEF790`, `0x100AEF7D4`) free the
open options dictionary, and the MediaProbe `av_freep` (`0x100A415CC`) frees the
app's custom-IO buffer, not a shadow field.

## Unresolved

- `st->metadata` and `fmt->chapters` are still aliased to the modern objects.
  No app site frees them in class A, but if one is found the same copy
  treatment applies.
