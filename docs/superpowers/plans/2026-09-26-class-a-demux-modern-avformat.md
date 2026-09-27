# Class-A Demux Partial Swap to Modern libavformat — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route the pure-playback demuxer's `AVFormatContext`-producing entry points through a modern (FFmpeg 9.0.2) `libavformat`, while the rest of the app keeps its 4.4.8 core, by translating the ABI at the boundary with legacy-shaped shadow structs.

**Architecture:** A new atomic unit `ffmpeg-demux` (`LibFFmpegDemuxBridge.dylib`) carries a `libavformat+libavcodec+libavutil` 9.0.2 closure. Only the **25 class-A call sites / 10 APIs** listed below are redirected to it. The shims keep a per-context legacy-shaped shadow (`AVFormatContext`/`AVStream`/`AVCodecParameters`/`attached_pic`) and sync fields across the boundary, so the app's direct struct reads and its still-4.4.8 `libavcodec`/`libavutil`/BSF sites need no changes. Packets are handed back as legacy-owned buffers.

**Tech Stack:** C (bridge shim), Python (`npabridge` patch toolchain, `deps/` closure builders), arm64 iOS 13 static closures, pytest, ldid, Keystone.

**Spec:** This file (design frozen 2026-09-26; there is no separate spec doc). Reverse-engineering evidence lives in `notes/ida-investigation.md` and the spike recorded in the branch history; the frozen site list is this plan's `class-A` section.

## Objective and stop/go (anti-over-engineering gate)

This is an **integration-feasibility experiment**, not the cheapest way to obtain the avformat fixes. If the objective were shipping fixes, the smaller path is to backport the wanted `n4.4.5..n4.4.8`-style changes into the already-tracked 4.4.8 core. Keep this plan only while the objective is proving that a modern `libavformat` can be seated behind the legacy ABI.

- The unit is **opt-in during the experiment** (`--dylib ffmpeg-demux`); do not merge it to `main`, release it, or let it change the default artifact until the device matrix passes.
- Stop/go after Task 6: if the matrix cannot pass without also translating side data, protocols or the mux face, that is evidence the seam is wider than the class-A boundary — re-evaluate (backport) rather than widen the shim silently.

## Global Constraints

- Do not overwrite existing IDA names/comments/types; do not save the IDA database.
- Every commit message ends with `Co-authored-by: Codex <codex@openai.com>`.
- No new manifest/payload concepts; no v1 compatibility layer; Python ≥ 3.11 via `uv`.
- The unit falls back as a whole: any failure publishes OLD and the app keeps 4.4.8. No per-site or per-API fallback.
- Kept on 4.4.8 (do not redirect): `av_dict_*`, `av_find_default_stream_index`, `avcodec_*`, `av_bsf_*`, `av_packet_*`, `av_frame_*`, `av_freep`, `av_malloc`, `avformat_network_init`.
- `sub_100A46E80` (`av_find_default_stream_index`) is shared with class C and must stay 4.4.8; its correctness relies on the shadow being legacy-shaped.
- No new tests beyond those named in the tasks; the device matrix is the only end-to-end proof.

## Frozen class-A redirect set (25 sites / 10 APIs)

| API | call sites |
|---|---|
| `avformat_alloc_context` | `100A3E390` `100AED780` `100AEF684` |
| `avformat_open_input` | `100A3E900` `100AED878` `100AEF778` |
| `avformat_find_stream_info` | `100A3E910` `100AED8E8` `100AEF7A8` |
| `av_read_frame` | `100A46964` `100AEE510` `100AEEC60` `100AEFE28` |
| `av_seek_frame` | `100A46944` `100AEFC4C` `100AEFCB4` `100AEFCCC` |
| `avformat_close_input` | `100A415B4` `100AEFA04` |
| `avformat_free_context` | `100A415D4` |
| `avio_alloc_context` | `100A3E388` `100AED778` |
| `avio_size` | `100A46874` `100AEFC18` |
| `av_index_search_timestamp` | `100AECEE0` |

Old targets (BL destinations) come from the existing `ffmpeg-core` manifest entries for these symbols; reuse them verbatim.

## Review Focus

- **Non-class-A demux still works**: class B (subtitle), class C (remux/mux) and MediaProbe-less playback must be byte-for-byte unaffected. A change outside the 25 sites is a defect.
- **Packet ownership**: a packet returned by the shim must be freeable by the app's legacy `av_packet_unref`; any hand-built `AVBuffer` with the wrong (modern) layout is silent heap corruption.
- **Seek/chapter/metadata fidelity**: fields the app reads (`duration`, `start_time`, `metadata`, `nb_chapters/chapters`, per-stream `duration/avg_frame_rate/sample_aspect_ratio/disposition/discard`) must be populated after open/find/seek, or playback silently loses them.
- **Custom-IO path**: the app installs its own `AVIOContext` (`avio_alloc_context`) and reads `pb->buffer`; the modern context must be accepted and its front fields must stay readable at the legacy offsets.
- **Fallback**: with the dylib absent or its identity check failing, class A must still demux via the app's 4.4.8 (observable as playback continuing).
- **Side data (measured, not assumed)**: the first packet translation drops `side_data`; if the matrix fails on VP9/AV1-in-Matroska (per-frame `BlockAdditional`), add the legacy side-data copy as a follow-up rather than pre-emptively.

## Deferred (YAGNI — do not build in this plan)

- Full enable-set mirroring, muxer/encoder/BSF/avfilter in the demux closure, and rtmp/rtsp protocols.
- Side-data translation (only if the matrix demands it).
- Redirecting `av_dict_*`, `av_find_default_stream_index`, `av_packet_*`, `avcodec_*`, `av_frame_*`, `av_bsf_*` (kept on 4.4.8).
- Class B (subtitle) and class C (remux/mux) routing; multi-version-per-function optimization.
- Any migration/compat layer for already-patched IPAs.

---

## File Structure

- Create `deps/ffmpeg-demux.lock.json` — pins FFmpeg 9.0.2 + dav1d 0.9.2 and the demux-oriented configure set.
- Modify `deps/build_ffmpeg_core.py` — takes `--lock` so one builder serves both closures.
- Modify `Makefile` — add the demux `--lock` invocation to `deps`.
- Create `bridge/ffmpeg-demux-abi.h` — compile-time guards for the legacy shadow layout and the modern fields used.
- Create `bridge/npa_ffmpeg_demux_bridge.c` — the 10 shims + shadow registry + packet translation.
- Create `bridge/ffmpeg-demux.exports` — the 10 `_npa_*` names.
- Modify `manifests/nplayer-3.13.0.json` — new dylib `ffmpeg-demux` + domain `ffmpeg-demux` with the 25 sites.
- Create `tests/test_demux_manifest.py` — asserts the frozen set and that no non-class-A site moved.
- Modify `dev/acceptance.json`, `README.md` — record the class-A device acceptance.

---

### Task 1: Cross-compile the FFmpeg 9.0.2 demux closure

**Files:**
- Create: `deps/ffmpeg-demux.lock.json`
- Modify: `deps/build_ffmpeg_core.py` (add `--lock`, derive the FFmpeg source key)
- Modify: `Makefile` (`deps` target)

**Interfaces:**
- Produces: `build/deps/ffmpeg-demux/lib/{libavformat.a,libavcodec.a,libavutil.a,libdav1d.a}`, `build/deps/ffmpeg-demux-closure.txt`, `build/deps/ffmpeg-demux-verification.json`.

- [ ] **Step 1: Write the lock** `deps/ffmpeg-demux.lock.json`

Same schema as `deps/ffmpeg-core.lock.json` with: the FFmpeg source keyed `ffmpeg-demux` at `version "9.0.2"` (`archive_name "ffmpeg-9.0.2.tar.xz"`, sha256 from `notes/ida-investigation.md` §7.8); `sources.dav1d` unchanged; `prefix/closure/verification/output_archives` pointed at `ffmpeg-demux`. `configure_args` = the core lock's, minus every muxer/encoder/bsf/filter enable (mux stays on 4.4.8), and for this first attempt **restrict** the container/protocol enables to the matrix in Task 6 (`matroska/webm/mov/mpegts/avi/flv/ogg`, `file/http/https/hls`, plus the decoders `avformat_find_stream_info` needs and `libdav1d`). Expand an entry only when a Task 6 row fails — do not mirror the full app set up front.

- [ ] **Step 2: Parameterize the existing builder instead of copying it**

In `deps/build_ffmpeg_core.py`: add an argparse `--lock` (default the current `LOCK_PATH`); in `load_lock`, accept any two-source lock whose second source is `dav1d` and derive the FFmpeg key as the other one; replace the module constant `FFMPEG_SOURCE` uses with that derived key. Default invocation stays byte-for-byte equivalent. Do **not** create `deps/build_ffmpeg_demux.py` — the 260-line builder stays single-sourced.

- [ ] **Step 3: Wire `Makefile`**

Add `$(UV) run python deps/build_ffmpeg_core.py --lock deps/ffmpeg-demux.lock.json` after the existing core line in the `deps` target.

- [ ] **Step 4: Build and check idempotence**

Run: `make deps`
Expected: the demux closure appears as a fourth `*-closure.txt` with the four archives; a second `make deps` reuses the cache without network; the existing core/dav1d closures are unchanged.

- [ ] **Step 5: Commit**

```bash
git add deps/ffmpeg-demux.lock.json deps/build_ffmpeg_core.py Makefile
git commit -m "build: cross-compile the FFmpeg 9.0.2 demux closure"
```

---

### Task 2: Add the ffmpeg-demux unit skeleton

**Files:**
- Create: `bridge/ffmpeg-demux-abi.h`
- Create: `bridge/ffmpeg-demux.exports`
- Create: `bridge/npa_ffmpeg_demux_bridge.c`
- Modify: `manifests/nplayer-3.13.0.json`

**Interfaces:**
- Produces: dylib id `ffmpeg-demux`, basename `LibFFmpegDemuxBridge.dylib`, domain `ffmpeg-demux`, exports `_npa_avformat_alloc_context` `_npa_avformat_open_input` `_npa_avformat_find_stream_info` `_npa_av_read_frame` `_npa_av_seek_frame` `_npa_avformat_close_input` `_npa_avformat_free_context` `_npa_avio_alloc_context` `_npa_avio_size` `_npa_av_index_search_timestamp`.
- Consumes: the closure from Task 1.

- [ ] **Step 1: Write `bridge/ffmpeg-demux-abi.h`**

Include the modern `<libavformat/avformat.h>`/`<libavcodec/avcodec.h>`. Define the legacy shadow offsets as named `NPA_LEGACY_*` constants (from the spike, not re-derived): the AVFormatContext fields the shim writes (0x20/0x2C/0x30/0x440/0x448/0x45C/0x460/0x4A4/0x4A8/0x4B0/0x4C4/0x4C8/0x4D0) and size (0x5E0); the AVStream fields the shim writes (0x00/0x04/0x18/0x20/0x28/0x30/0x38/0x3C/0x40/0x48/0x50/0x58/0xD0) and size (0x1F0); the AVCodecParameters fields (0x00/0x04/0x08/0x10/0x18/0x1C/0x20/0x38/0x3C/0x40/0x48/0x60/0x68/0x70/0x74/0x78/0x7C) and size (0x90); the AVPacket fields (0x00/0x08/0x10/0x18/0x20/0x24/0x28/0x40/0x48/0x50) and size (0x58). Provide small helpers `npa_st_u32/u64/ptr(void *shadow, size_t off, value)`.

Assert with `_Static_assert` only what can actually drift:
  - the hand-built legacy buffer layouts: `struct npa_legacy_avbuffer { uint8_t *data; int size; _Atomic uint32_t refcount; void (*free)(void*, uint8_t*); void *opaque; int flags; int flags_internal; }` → `offsetof(size)==8`, `offsetof(refcount)==0xC`, `offsetof(free)==0x10`, `offsetof(opaque)==0x18`, `sizeof==0x28`; `struct npa_legacy_avbuffer_ref { struct npa_legacy_avbuffer *buffer; uint8_t *data; int size; }` → `sizeof==0x18`;
  - the modern fields the shim reads/writes: `offsetof(AVFormatContext,nb_streams)==0x2C`, `pb==0x20`, `streams==0x30`, `interrupt_callback.callback==0xD8`, `interrupt_callback.opaque==0xE0`; `offsetof(AVStream,index)==0x08`, `codecpar==0x10`; `offsetof(AVCodecParameters,ch_layout)` compiles; `sizeof(AVPacket)==0x68`, `offsetof(AVPacket,duration)==0x40`, `offsetof(AVPacket,time_base)==0x60`;
  - each shadow allocation size is at least the largest offset it uses (`NPA_LEGACY_AVSTREAM_SIZE > 0x1D0`, etc.).

Keep the legacy field values as constants only; do not transcribe the legacy headers. A comment cites `notes/ida-investigation.md` §8.2, this plan's frozen table, and the spike.

- [ ] **Step 2: Write `bridge/ffmpeg-demux.exports`**

The 10 `_npa_*` names, one per line, in the frozen table's order.

- [ ] **Step 3: Write the bridge skeleton `bridge/npa_ffmpeg_demux_bridge.c`**

Include `ffmpeg-demux-abi.h`; define the 10 `npa_*` symbols as tail branches `b <symbol>` exactly like `bridge/npa_ffmpeg_core_bridge.c`'s `NPA_FORWARD` macro. This proves build/verify/exports before any shim logic; Task 3+ replaces the bodies.

- [ ] **Step 4: Add the manifest dylib**

Append to `dylibs[]`:
```json
{"id":"ffmpeg-demux","library_version":"9.0.2","basename":"LibFFmpegDemuxBridge.dylib",
 "build":{"source":"bridge/npa_ffmpeg_demux_bridge.c","exports":"bridge/ffmpeg-demux.exports",
          "closure":"build/deps/ffmpeg-demux-closure.txt",
          "include_root":"build/deps/ffmpeg-demux/include","lib_root":"build/deps/ffmpeg-demux/lib"},
 "domains":[{"id":"ffmpeg-demux","apis":[ ...10 api objects, same shape as ffmpeg-core... ]}]}
```
Each API object: `symbol`, `call_sites` (the frozen addresses), `old_target` (copied from the matching `ffmpeg-core` API).

- [ ] **Step 5: Build and verify**

Run: `make bridge verify`
Expected: four dylibs build; the new one exports exactly the 10 `_npa_*`, has no initializer/third-party dynamic dependency, and installs at `@rpath/LibFFmpegDemuxBridge.dylib`.

- [ ] **Step 6: Commit**

```bash
git add bridge/ffmpeg-demux-abi.h bridge/ffmpeg-demux.exports bridge/npa_ffmpeg_demux_bridge.c manifests/nplayer-3.13.0.json
git commit -m "feat: add the ffmpeg-demux unit skeleton"
```

---

### Task 3: Shadow registry and boundary field sync

**Files:**
- Modify: `bridge/npa_ffmpeg_demux_bridge.c`

**Interfaces:**
- Produces (static, internal): `struct npa_shadow { AVFormatContext *modern; AVFormatContext *shadow; AVStream **streams; unsigned nb_streams; };` plus a registry keyed by shadow pointer; `npa_avformat_alloc_context`, `npa_avformat_open_input`, `npa_avformat_find_stream_info`, `npa_avformat_close_input`, `npa_avformat_free_context`, `npa_avio_alloc_context`, `npa_avio_size`, `npa_av_index_search_timestamp` implemented over it.

- [ ] **Step 1: Implement the registry and legacy shadow allocation**

`npa_avformat_alloc_context`: call modern `avformat_alloc_context`; `calloc(1, 0x5E0)` the shadow; register; return the shadow. `npa_avformat_open_input`: look up the shadow from `*ps` (if `*ps` is NULL, create both); copy app-written fields shadow→modern before the call (`pb`, `interrupt_callback.callback/opaque`, `error_recognition`, `flags`); call modern `avformat_open_input(&modern, url, fmt, options)`; on ≥0 rebuild streams and sync modern→shadow; set `*ps = shadow`. Keep `shadow->pb = modern->pb` so `avio_*` maps.

- [ ] **Step 2: Implement stream/codecpar shadow rebuild + sync**

After open/find_stream_info and before every read/seek: allocate `shadow->streams = calloc(nb_streams, 8)` and per stream a `calloc(1,0x1F0)` stream + `calloc(1,0x90)` codecpar, writing fields with the `npa_st_*` helpers. Copy the app-visible fields only: stream `index/id/codecpar/time_base/start_time/duration/nb_frames/disposition/discard/sample_aspect_ratio/avg_frame_rate`, `metadata` (shared pointer — `AVDictionary` layout is identical), `codec` = NULL (the app never reads it); codecpar `codec_type/codec_id/codec_tag/extradata/extradata_size/format/bit_rate/width/height/sample_aspect_ratio/field_order/color_primaries/color_trc/color_space/video_delay/sample_rate/block_align/frame_size`, plus `channels = ch_layout.nb_channels` and `channel_layout = (ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? ch_layout.mask : 0)` so the still-4.4.8 `avcodec_parameters_to_context` sees a sane audio config. `shadow->metadata = modern->metadata`. Sync fmt scalars too (`nb_streams/streams/iformat/start_time/duration/flags/nb_chapters/chapters/max_delay`).

- [ ] **Step 3: Implement seek/close/free/avio/index**

`npa_av_seek_frame`: map; sync shadow→modern (per-stream `discard`, `flags`); call modern; sync back the fields read after a seek. `npa_avformat_close_input(&shadow)`: map, call modern `avformat_close_input(&modern)`, free the shadow graph, unregister, `*ps=NULL`. `npa_avformat_free_context(shadow)`: same without the pointer argument. `npa_avio_alloc_context`: forward to modern. `npa_avio_size`: forward to modern. `npa_av_index_search_timestamp(shadow_stream, ts, flags)`: find the shadow stream's index in its context's shadow `streams` array and call modern on `modern->streams[index]`.

- [ ] **Step 4: Build and export-check**

Run: `make bridge verify`
Expected: PASS; no compiler warning on struct offset usage.

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_demux_bridge.c
git commit -m "feat: shadow the AVFormatContext at the demux boundary"
```

---

### Task 4: Translate AVPacket and attached_pic through legacy buffers

**Files:**
- Modify: `bridge/npa_ffmpeg_demux_bridge.c`

**Interfaces:**
- Produces (static): `npa_legacy_packet_from(AVPacket *dst, const AVPacket *src)` — fills the app's 0x58 packet with a legacy-owned buffer; used by `npa_av_read_frame` and by the `attached_pic` build in Task 3's stream sync.

- [ ] **Step 1: Implement the legacy buffer builder**

`npa_legacy_packet_from`: create one `struct npa_legacy_avbuffer` (0x28) + `struct npa_legacy_avbuffer_ref` (0x18) with `refcount=1`, `data=src->data`, `size=src->size`, and a `free` callback that takes the retained modern `AVBufferRef` (a second `av_buffer_ref(src->buf)`) and calls modern `av_buffer_unref` on it, then frees the two shells. Set `dst->buf`, `dst->data/size/pts/dts/stream_index/flags/duration/pos` from `src`; `dst->side_data=NULL`, `dst->side_data_elems=0`, `dst->convergence_duration=0`. If `src->buf==NULL` (buf-less source packet), copy the payload into an `av_malloc`'d block and free it in the callback instead.

- [ ] **Step 2: Implement `npa_av_read_frame`**

Map the shadow; `AVPacket *tmp = av_packet_alloc()`; modern `av_read_frame(modern, tmp)`; on ≥0 `npa_legacy_packet_from(pkt, tmp)`; modern `av_packet_unref(tmp)`; `av_packet_free(&tmp)`; return the code.

- [ ] **Step 3: Build the `attached_pic` in the stream sync**

In Task 3's stream sync, if `modern_stream->attached_pic.size > 0`, `npa_legacy_packet_from(&shadow_stream->attached_pic, &modern_stream->attached_pic)`.

- [ ] **Step 4: Build**

Run: `make bridge verify`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_demux_bridge.c
git commit -m "feat: hand legacy packet buffers back across the demux boundary"
```

---

### Task 5: Prove the changed instruction set is exactly the 25 class-A sites

**Files:**
- Create: `tests/test_demux_manifest.py`

**Interfaces:**
- Consumes: the `ffmpeg-demux` domain from the manifest.

- [ ] **Step 1: Write the test**

`tests/test_demux_manifest.py`: load the manifest; assert the `ffmpeg-demux` domain has exactly 10 APIs and 25 call sites; assert the exact address set equals the frozen table; assert every one of those addresses is **absent** from every other unit's `call_sites` (no double-redirect); assert each `old_target` equals the matching `ffmpeg-core` symbol's `old_target`.

- [ ] **Step 2: Run it**

Run: `uv run pytest tests/test_demux_manifest.py -v`
Expected: PASS.

- [ ] **Step 3: Patch a clean IPA and check the change set**

Run: `uv run npa-patch --dylib libass --dylib ffmpeg --dylib ffmpeg-core --dylib ffmpeg-demux <clean ipa>`
Expected: the patch summary's changed-instruction count equals the 25 demux sites plus the existing libass NOPs and the already-present units; record the packaged-main sha256 in the test as the branch anchor (same pattern as `tests/test_verify.py`).

- [ ] **Step 4: Commit**

```bash
git add tests/test_demux_manifest.py
git commit -m "test: pin the ffmpeg-demux unit to the 25 class-A sites"
```

---

### Task 6: Record the class-A device acceptance

**Files:**
- Modify: `dev/acceptance.json`
- Modify: `README.md`

- [ ] **Step 1: Apply the artifact to the device**

Install the patched IPA (iPhone SE 3 / iOS 17.7.2 / LiveContainer) with `--dylib ffmpeg-demux` (plus libass/ffmpeg/ffmpeg-core as desired).

- [ ] **Step 2: Walk the matrix and record pass/fail per row**

Rows: local-file containers (mkv/webm, mp4, mpeg, ts, wmv) playback; seek; MediaProbe thumbnails/metadata; https HLS; the custom-IO path (`[Reference]`/mmsh); audio; subtitle files (proving class B is untouched); **fallback proof** (replace the selected `LibFFmpegDemuxBridge.dylib` with a wrong-identity copy so the unit's resolve fails, and confirm the same files still play through the app's 4.4.8). Also confirm class B/C behavior is unchanged by running a remux/record action and an embedded-subtitle file.

- [ ] **Step 3: Add the `class-a-demux` acceptance entry**

Append to `dev/acceptance.json` with the packaged-main sha, the signed dylib sha, the device/iOS/install method, and one line per matrix row; mark `result` `pass` only if every row passed, else `partial` with the open rows.

- [ ] **Step 4: Update `README.md`**

Add the `ffmpeg-demux` unit to the unit list and the release-asset list, and note it is the first partial (demux-only) modern-core integration.

- [ ] **Step 5: Commit**

```bash
git add dev/acceptance.json README.md
git commit -m "test: record the class-A demux device acceptance"
```
