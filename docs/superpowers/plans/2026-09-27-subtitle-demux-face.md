# Subtitle Demux Face Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redirect the app's external-subtitle demuxer (`media::FFmpegSubtitle`) to the 9.0.2 core unit, so the `ffmpeg-core902` face covers playback **and** external subtitle demuxing.

**Architecture:** Extend the existing `ffmpeg-demux` domain (option A: no new domain, no new dylib) with the 13 `0x100AB4xxx` call sites of `media::FFmpegSubtitle` — 8 added to existing bindings, 5 as new bindings — and add five `npa_demux_*` entry points to the demux shim: four pure forwards (`av_find_input_format`, `av_probe_input_buffer`, `avio_read`, `avio_seek`) and one shadow-translating wrapper (`avformat_seek_file`). The unit, its closure, its domain references and its `conflicts` list do not change. The same sites stay in the `ffmpeg-core` domain as well, which is already mutually exclusive with `ffmpeg-core902`, so `ffmpeg-full` keeps covering them.

**Tech Stack:** C (demux shim, `bridge/npa_ffmpeg_demux_bridge.c`), FFmpeg 9.0.2 static closure, Python manifest/patcher toolchain (`npabridge/`), arm64 iOS dylib.

**Spec:** Design approved in session on 2026-09-27 (its §1–§7). Evidence: `notes/ida-investigation-subtitle-demux-face.md` (plus Addendum A). Face sketch and roadmap: `docs/superpowers/plans/2026-09-27-expand-ffmpeg902-wiring.md` §5.

## Global Constraints

- No new dependency and no closure change: every symbol used is already in `deps/ffmpeg-core902.lock.json`'s closure. `Makefile` and `deps/` are untouched.
- Do not change the default selection (`libass + ffmpeg-full`), the split selection (`libass + ffmpeg + ffmpeg-core`), any dylib's `conflicts` list, or the `ffmpeg-core` domain.
- New symbols keep the `npa_demux_` prefix. The domains stay declared once; the 13 sites stay in the `ffmpeg-core` domain as well.
- The face moves as **one** commit. A partial move is unsound: once `avformat_alloc_context` returns a shimmed shadow context, the app's own 4.4.5 `avformat_seek_file` call at `0x100AB4F7C` would receive that shadow, which is not a real `AVFormatContext` (no `iformat`, no `priv_data`). This is why Task 1 is not split.
- Call sites are written in the manifest as **uppercase** hex strings (`"0x100AB4DF4"`), one list per API binding, and `old_target` is the address of the app's own 4.4.5 function, which the dispatch payload jumps to when the unit's state word is not `NEW`.
- Reverse-engineering rules from `AGENTS.md`: evidence grades (Confirmed / Probable / Hypothesis), findings recorded under `notes/ida-investigation-*.md`, never save the IDA database.
- Out of scope: the cluster's avcodec/avutil sites (they create objects, and the codec context escapes the class — P2 territory); the `ffmpeg-core` domain's missing `avformat_seek_file` binding (separate follow-up, it would move the default artifact's anchor); class B container-subtitle demux (the bare `media::Subtitle` cue feed); class C mux/record.

## Review Focus

This face's behaviour cannot be unit-tested here. The automated gates are `make verify` (the built dylib's exports must equal the union of its domains' symbols), `make bridge`, and `pytest` (the manifest guard). The conditions those gates and the device rows must cover explicitly, most likely to bite first:

1. **A shimmed context escaping to the app's own 4.4.5.** The known instance is `avformat_seek_file` from `0x100AB4F58` (virtual idx10, site `0x100AB4F7C`), which the manifest never bound. Task 1's guard asserts the binding exists with the right `old_target`; Task 2's "seek with an external `.ass` loaded" row exercises it.
2. **Teardown of a shimmed context.** The class frees `avio->buffer` itself and then calls `avformat_close_input` on the shadow; neither may free twice. The class-A face already had one double-free of exactly this shape (codecpar extradata). Task 2's "switch/close subtitle tracks repeatedly" row exercises it.
3. **An external `.ass`/`.ssa` must still load and render** through the mount path (`av_find_input_format` → `av_probe_input_buffer` → `avio_alloc_context` → `avformat_alloc_context` → `avformat_open_input` → `avformat_find_stream_info`). Task 2 row.
4. **An external `.srt`/`.vtt` must be unaffected** — the app routes those to `SRTParser`, not `FFmpegSubtitle`; a regression would mean sites were moved by mistake. Task 2 row.
5. **A container-embedded subtitle track must be unaffected** (class B is served by the bare `media::Subtitle` cue feed, never by this class). Task 2 row, together with the HEVC Main10 / thumbnail rows that prove the scaler face is untouched.

---

### Task 1: Move the external-subtitle demux sites onto the 9.0.2 unit

**Files:**
- Modify: `manifests/nplayer-3.13.0.json` (the top-level `ffmpeg-demux` domain only)
- Modify: `bridge/npa_ffmpeg_demux_bridge.c` (five new exported entry points)
- Modify: `bridge/ffmpeg-core902.exports` (21 → 26 symbols)
- Test: `tests/test_demux_manifest.py`

**Interfaces:**
- Consumes: `npa_shadow`, `shadow_lookup(void *)`, `shadow_to_modern(npa_shadow *)` — existing helpers in `bridge/npa_ffmpeg_demux_bridge.c`; the manifest's `Domain.apis` (`APIBinding.symbol`, `.call_sites`, `.old_target`).
- Produces: five exported C entry points named `npa_demux_av_find_input_format`, `npa_demux_av_probe_input_buffer`, `npa_demux_avio_read`, `npa_demux_avio_seek`, `npa_demux_avformat_seek_file`; the extended `ffmpeg-demux` domain (15 APIs / 38 sites).

- [ ] **Step 1: Write the failing manifest guard**

In `tests/test_demux_manifest.py`: rename the module docstring's subject from the pure-playback demuxer to the demux face, rename `CLASS_A` to `DEMUX_FACE`, and extend it with the subtitle sites. The dict becomes exactly:

```python
DEMUX_FACE = {
    "npa_demux_avformat_alloc_context": {
        0x100A3E390, 0x100AED780, 0x100AEF684, 0x100AB4DF4,
    },
    "npa_demux_avformat_open_input": {
        0x100A3E900, 0x100AED878, 0x100AEF778, 0x100AB451C, 0x100AB4E1C,
    },
    "npa_demux_avformat_find_stream_info": {
        0x100A3E910, 0x100AED8E8, 0x100AEF7A8, 0x100AB4E2C,
    },
    "npa_demux_av_read_frame": {
        0x100A46964, 0x100AEE510, 0x100AEEC60, 0x100AEFE28, 0x100AB4934, 0x100AB4FE0,
    },
    "npa_demux_av_seek_frame": {0x100A46944, 0x100AEFC4C, 0x100AEFCB4, 0x100AEFCCC},
    "npa_demux_avformat_seek_file": {0x100AB4F7C},
    "npa_demux_avformat_close_input": {0x100A415B4, 0x100AEFA04, 0x100AB4470},
    "npa_demux_avformat_free_context": {0x100A415D4},
    "npa_demux_avio_alloc_context": {0x100A3E388, 0x100AED778, 0x100AB4DCC},
    "npa_demux_av_find_input_format": {0x100AB4D74},
    "npa_demux_av_probe_input_buffer": {0x100AB4DEC},
    "npa_demux_avio_read": {0x100AB4B00},
    "npa_demux_avio_seek": {0x100AB4B14},
    "npa_demux_avio_size": {0x100A46874, 0x100AEFC18},
    "npa_demux_av_index_search_timestamp": {0x100AECEE0},
}
```

Also in that file, rename the three places that still say `CLASS_A` (`EXPECTED_SITES = frozenset().union(*DEMUX_FACE.values())`, and the two test methods `test_has_exactly_the_class_a_symbols` → `test_has_exactly_the_demux_face_symbols`, `test_each_symbol_redirects_exactly_its_class_a_sites` → `test_each_symbol_redirects_exactly_its_sites`), rename `test_redirects_exactly_25_sites` to `test_redirects_exactly_38_sites` and assert `len(sites) == 38`, and change the `test_overlapping_units_are_mutually_exclusive` docstring's "the 25 class-A sites" to "the 38 demux-face sites".

`test_old_targets_match_the_ffmpeg_core_unit` needs one exception: the `ffmpeg-core` domain has **no** `npa_avformat_seek_file` binding. Skip that symbol in the loop and assert it separately:

```python
        for symbol, api in self.apis.items():
            if symbol == "npa_demux_avformat_seek_file":
                continue
            core_symbol = "npa_" + symbol[len("npa_demux_") :]
            self.assertEqual(api.old_target, core_targets[core_symbol], symbol)
        # the 4.4.8 core unit never bound this call; the address is the app's own
        # avformat_seek_file, confirmed from the tail branch at 0x100AB4F7C in
        # notes/ida-investigation-subtitle-demux-face.md (Addendum A)
        self.assertEqual(self.apis["npa_demux_avformat_seek_file"].old_target, "0x100823AA8")
```

- [ ] **Step 2: Run the guard to verify it fails**

Run: `uv run pytest tests/test_demux_manifest.py -q`
Expected: FAIL — the domain still holds 25 sites and no `npa_demux_avformat_seek_file`.

- [ ] **Step 3: Extend the `ffmpeg-demux` domain**

In `manifests/nplayer-3.13.0.json`, edit the single top-level `domains` entry whose `id` is `ffmpeg-demux`: set each existing binding's `call_sites` to the final list below, and add the five new bindings. `old_target` values are copied from the same-named `ffmpeg-core` binding (four of them), except `npa_demux_avformat_seek_file`, whose `old_target` is `0x100823AA8` (the core domain has no such binding). Keep the file's existing formatting: 2-space indent, one `call_sites` array per line.

| symbol | old_target | call_sites |
|---|---|---|
| `npa_demux_avformat_alloc_context` | `0x1007DCCF8` | `0x100A3E390, 0x100AED780, 0x100AEF684, 0x100AB4DF4` |
| `npa_demux_avformat_open_input` | `0x10081FADC` | `0x100A3E900, 0x100AED878, 0x100AEF778, 0x100AB451C, 0x100AB4E1C` |
| `npa_demux_avformat_find_stream_info` | `0x100824C8C` | `0x100A3E910, 0x100AED8E8, 0x100AEF7A8, 0x100AB4E2C` |
| `npa_demux_av_read_frame` | `0x100821298` | `0x100A46964, 0x100AEE510, 0x100AEEC60, 0x100AEFE28, 0x100AB4934, 0x100AB4FE0` |
| `npa_demux_av_seek_frame` | `0x10082357C` | unchanged |
| `npa_demux_avformat_seek_file` | `0x100823AA8` | `0x100AB4F7C` |
| `npa_demux_avformat_close_input` | `0x100827B08` | `0x100A415B4, 0x100AEFA04, 0x100AB4470` |
| `npa_demux_avformat_free_context` | unchanged | unchanged |
| `npa_demux_avio_alloc_context` | `0x10073AD30` | `0x100A3E388, 0x100AED778, 0x100AB4DCC` |
| `npa_demux_av_find_input_format` | `0x100755304` | `0x100AB4D74` |
| `npa_demux_av_probe_input_buffer` | `0x1007558B0` | `0x100AB4DEC` |
| `npa_demux_avio_read` | `0x10073C2F0` | `0x100AB4B00` |
| `npa_demux_avio_seek` | `0x10073B228` | `0x100AB4B14` |
| `npa_demux_avio_size` | unchanged | unchanged |
| `npa_demux_av_index_search_timestamp` | unchanged | unchanged |

- [ ] **Step 4: Re-run the guard to verify it passes**

Run: `uv run pytest tests/test_demux_manifest.py -q`
Expected: PASS (five tests).

- [ ] **Step 5: Add the five entry points to the shim**

In `bridge/npa_ffmpeg_demux_bridge.c`, next to the existing exports, add five functions with these exact signatures. Mark each with a short comment saying why it needs no translation (or needs one):

```c
NPA_EXPORT const AVInputFormat *npa_demux_av_find_input_format(const char *short_name);
NPA_EXPORT int npa_demux_av_probe_input_buffer(AVIOContext *pb, const AVInputFormat **fmt,
                                               const char *url, void *logctx,
                                               unsigned int offset, unsigned int max_probe_size);
NPA_EXPORT int npa_demux_avio_read(AVIOContext *s, unsigned char *buf, int size);
NPA_EXPORT int64_t npa_demux_avio_seek(AVIOContext *s, int64_t offset, int whence);
NPA_EXPORT int npa_demux_avformat_seek_file(AVFormatContext *s, int stream_index,
                                            int64_t min_ts, int64_t ts, int64_t max_ts, int flags);
```

The first four are one-line forwards to the 9.0.2 `av_find_input_format`, `av_probe_input_buffer`, `avio_read` and `avio_seek`: the `AVInputFormat` and `AVIOContext` they hand out are created by this same unit and never escape the class, so a pointer the app stores and hands back is already a 9.0.2 object. The fifth mirrors `npa_demux_av_seek_frame` — `shadow_lookup((void *)s)`, return `AVERROR(EINVAL)` when it or `s->modern` is missing, `shadow_to_modern(s)`, then `return avformat_seek_file(s->modern, stream_index, min_ts, ts, max_ts, flags);`.

- [ ] **Step 6: Extend the export list**

Append the five matching underscored names to `bridge/ffmpeg-core902.exports`, keeping the file's existing order convention (the demux block first):

```
_npa_demux_avformat_seek_file
_npa_demux_av_find_input_format
_npa_demux_av_probe_input_buffer
_npa_demux_avio_read
_npa_demux_avio_seek
```

Expected file length: 26 lines.

- [ ] **Step 7: Build and verify**

Run: `make bridge && make verify`
Expected: five dylibs build with no warnings and each reports 7 checks OK; `ffmpeg-core902`'s export check compares the binary against the 15 domain symbols and passes. A mismatch here means the exports file, the shim and the manifest disagree.

- [ ] **Step 8: Run the whole suite**

Run: `uv run pytest -q && uv run pytest dev/tests -q`
Expected: 86 passed (plus subtests) and 7 passed. Nothing outside `tests/test_demux_manifest.py` may need a change; if `tests/test_verify.py` or `tests/test_macho.py` fails, the `ffmpeg-core902` dylib and the manifest disagree.

- [ ] **Step 9: Commit**

```bash
git add manifests/nplayer-3.13.0.json bridge/npa_ffmpeg_demux_bridge.c bridge/ffmpeg-core902.exports tests/test_demux_manifest.py
git commit -m "feat: move the external-subtitle demuxer onto the 9.0.2 unit"
```

---

### Task 2: Record the device acceptance

**Files:**
- Modify: `dev/acceptance.json`
- Produce (not committed): `build/accept/subtitle-face.ipa`

**Interfaces:**
- Consumes: the manifest from Task 1; `npabridge.patch.patch_ipa(source, output, dylibs_dir, manifests, work, dylibs)`; the built dylibs in `build/`.
- Produces: one new entry in `dev/acceptance.json`'s `results` list (`variant: "subtitle-demux-face"`) and one new `reproducibility` note carrying this selection's packaged-main anchor.

- [ ] **Step 1: Build the acceptance artifact**

Patch the source IPA with the selection `libass + ffmpeg-core902` into `build/accept/subtitle-face.ipa` and print `packaged_main_sha256` plus the signed dylib hashes. Re-patch once into a scratch output and confirm the packaged-main hash is identical before trusting it as an anchor.

- [ ] **Step 2: Hand the artifact over for the device rows**

Ask the user to install `build/accept/subtitle-face.ipa` and report on these rows, which is where Review Focus 1–5 is measured:

| row | what it proves |
|---|---|
| an external `.ass`/`.ssa` subtitle file loads and renders | the mount path (Review Focus 3) |
| seeking during playback with that `.ass` loaded still shows the right subtitle, and does not crash | `avformat_seek_file` is redirected (Review Focus 1) |
| switching subtitle files / turning subtitles off and on again repeatedly does not crash | teardown of the shimmed context (Review Focus 2) |
| an external `.srt`/`.vtt` still renders | the SRTParser path is untouched (Review Focus 4) |
| a container-embedded subtitle track still renders, HEVC Main10 plays and thumbnails generate | class B and the scaler face are untouched (Review Focus 5) |

- [ ] **Step 3: Write the acceptance entry**

Append the entry with `variant: "subtitle-demux-face"` and `result: "partial"` unless the user reports a failure, in which case record what failed and stop. The notes must state: the selection and its packaged-main anchor (labelled as the anchor, with the dylib hashes labelled as built-this-pass because the dylibs are not byte-reproducible); which rows were measured in this pass; that the playback-demux rows of the `ffmpeg-core902` entry are carried over rather than re-measured; and the open items — the whole-unit fallback proof for this face, the `[Reference]` custom-IO branch, and the recording/mux row that is not testable here. Add the selection's packaged-main anchor to the `reproducibility` list.

- [ ] **Step 4: Commit**

```bash
git add dev/acceptance.json
git commit -m "test: record the subtitle demux face acceptance"
```
