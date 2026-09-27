# 播放/probe 解码单元（P2-B）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Prerequisite:** `docs/superpowers/plans/2026-09-27-subtitle-decode-unit.md`（P2-A）必须已经落地并通过设备验收。
本计划复用它的影子核心与登记表，只新增一个单元与它的站点。

**Goal:** 把 app 的播放软解（`FFmpegVideoDecoder` / `FFmpegAudioDecoder`）与 probe/poster 抽帧解码接到
9.0.2 核心，成为 `ffmpeg-core902` 的第 5 个可独立回落的单元 `ffmpeg-codec`。

**Architecture:** 与 P2-A 同一套影子机制。ctx/params 的翻译代码由 P2-A 的 shim 提供（共享登记表），本单元只
新增导出入口与**帧物化**（`receive_frame` 把现代帧物化进 app 自有的 legacy 帧，缓冲仍为 legacy 归属）。

**Tech Stack:** 同 P2-A。

**Spec:** `docs/superpowers/specs/2026-09-27-p2-codec-object-layer-design.md`

## Global Constraints

- 与 P2-A 完全相同的约束：站点只从 `ffmpeg-core` 域挑、不移走任何站点、dylib 内一个站点只认领一次、
  app 侧只允许 legacy 布局、格式值按 4.4 编号、不引入新依赖、未实现入口显式 `__builtin_trap()`。
- **不改动 P2-A 的 `ffmpeg-subdecode` 域与它的 16 个站点**；两个单元共享 shim 代码与登记表。
- 本单元只覆盖**软解**路径：app 的视频解码优先 VideoToolbox（`AVSampleBufferDecoder` → `VTVideoDecoder`
  → `FFmpegVideoDecoder` 回退），所以验收必须**把 app 解码器切到软解**才真正打到本单元。

## Review Focus

1. `sub_100A8A4F8` 建的**临时源 ctx 是 4.4.5 对象**（该 helper 整体不移动），被本单元的
   `parameters_from_context`/`to_context` 以 legacy 布局读取——这条路径必须只读、不得试图释放或改写它。
2. 视频解码循环有**两条互斥策略**（`AV_DISPOSITION_ATTACHED_PIC` 直解 / 否则 seek 到 `duration*0.5`），
   两条都要过（缩略图行覆盖）。
3. 音频路径 `request_channel_layout=0x60000000` 不经 `parameters_from_context` 传递：不得因此改变声道布局
   行为（spec §6 未决项，实现时保持现状）。
4. 现代帧缓冲只能交给 app 的 4.4.5 `av_frame_unref/free` 释放——物化的 legacy `AVBuffer`/`AVBufferRef`
   必须带 shim 自己的释放回调（沿用 demux 单元 `legacy_ref_release` 的做法），且 app 的 `av_frame_ref`
   必须能对它正确加引用。
5. 9.0.2 **没有 `avcodec_close`**：probe 路径的关闭站点必须由 shim 自己实现（flush + 标记关闭，真正的
   释放在随后的 `free_context`），不得让 4.4.5 的 `avcodec_close` 作用在 shim 的 ctx 上。

---

## Task 1: `ffmpeg-codec` 域、站点表与 §7.2 前置复核

**Files:**
- Modify: `manifests/nplayer-3.13.0.json`
- Create: `tests/test_codec_manifest.py`

**Interfaces:**
- Consumes: P2-A 的 manifest 结构。
- Produces: domain `ffmpeg-codec`（12 API / **44** 站点，2026-09-27 修订见文末与 spec 同名小节），挂在 `ffmpeg-core902` 的 `domains` 末尾；
  符号命名 `npa_codec_<name>`，`old_target` 取 `ffmpeg-core` 域 `npa_<name>` 的 `old_target`。

**站点表（2026-09-27 修订：44 站点）**

| 符号 | video | audio | probe | helper / SPDIF |
|---|---|---|---|---|
| `npa_codec_avcodec_alloc_context3` | `0x100A80F9C` | `0x100A898D4` | `0x100A467B8` | `0x100A8A514` |
| `npa_codec_avcodec_free_context` | `0x100A80ED8` `0x100A80F4C` `0x100A81078` `0x100A81104` | `0x100A89770` `0x100A89880` `0x100A899D0` | `0x100A468EC` | `0x100A8A684` `0x100B300F0` `0x100B63F88` |
| `npa_codec_avcodec_find_decoder` | `0x100A80F8C` | `0x100A898C4` | `0x100A467B0` | — |
| `npa_codec_avcodec_open2` | `0x100A81020` | `0x100A89914` | `0x100A467DC` | — |
| `npa_codec_avcodec_close` | — | — | `0x100A468E0` | — |
| `npa_codec_avcodec_flush_buffers` | `0x100A815AC` | `0x100A89FF4` | — | — |
| `npa_codec_avcodec_parameters_alloc` | `0x100A80FA8` | `0x100A898E0` | — | — |
| `npa_codec_avcodec_parameters_free` | `0x100A80FCC` | `0x100A89904` | — | — |
| `npa_codec_avcodec_parameters_from_context` | `0x100A80FB8` | `0x100A898F0` | — | `0x100B6400C` |
| `npa_codec_avcodec_parameters_to_context` | `0x100A80FC4` | `0x100A898FC` | `0x100A467C8` | — |
| `npa_codec_avcodec_send_packet` | `0x100A81280` `0x100A812AC` | `0x100A89BD8` `0x100A89C04` | `0x100A468A8` `0x100A46998` | — |
| `npa_codec_avcodec_receive_frame` | `0x100A812F8` | `0x100A89C8C` | `0x100A468B4` `0x100A469AC` | — |

> **为什么多了 helper / SPDIF 这 5 个站点**：源 ctx 的 `avcodec_alloc_context3` 在 `sub_100A8A4F8`，
> 而它的 `avcodec_free_context` 落在 5 个 caller 上（3 处本就在表内、2 处在 SPDIF）。
> 若只移动 caller 的 free，就会出现"alloc 在 4.4.5、free 在 shim"，违反 spec §2.4。
> 经用户裁定把该对象的 alloc/consume/free 一并纳入。证据：`notes/ida-investigation-p2-site-attribution.md`
> 的追加 1/追加 2；spec 文末同名修订小节。

**在同一批函数内但按策略保留 4.4.5**（不进本单元）：`av_frame_alloc/free/unref/ref`（21 站点，帧由 app 自己
分配、shim 就地物化，缓冲 legacy 归属）、`av_init_packet`(5)、`av_packet_unref`(4)、`av_dict_*`(4)、
`av_image_*`(4)、`av_reduce`(2)、`av_get_bytes_per_sample`/`av_get_default_channel_layout`/
`av_sample_fmt_is_planar`/`av_samples_get_buffer_size`/`avcodec_descriptor_get`/`avcodec_get_name`/
`av_malloc`/`av_freep`，以及 **`avcodec_parameters_copy`**（两个操作数都是 legacy 形状的影子，4.4.5 的实现
正好按该布局工作；移动无收益——spec §2.5 同日裁定）。理由与 P2-A 的 16 站点同源：本单元只产出/消费 ctx 与 params。

**helper `sub_100A8A4F8` 的认领是部分的（2/6 个 API 站点）**：它的 params 链
（`parameters_alloc`@`0x100A8A528`、`parameters_copy`@`0x100A8A538`、`parameters_to_context`@`0x100A8A544`、
`parameters_free`@`0x100A8A54C`）与 `av_malloc`（`0x100A8A5B8`/`0x100A8A650`）继续走 4.4.5。
⇒ 实现时必须保证 shim 的 `alloc_context3` 交出的影子**能被 legacy 代码继续填充**，且 `free_context`
对 legacy `av_malloc` 出来的 `ctx+0x58`(extradata) 的回收语义与 4.4.5 对齐（Task 3，须有证据）。

**UI 两个函数**（`sub_100A233C4`/`sub_100A237B0`，spec §1.1 曾列入）经核**贡献 0 站点**：它们只操作 app
自定义帧包装对象并调 `av_image_*` 与 sws，不碰 FFmpeg 结构（`field-surface` §5）。因此本单元不含它们。

- [x] **Step 1: 复核 spec §7.2（先做，未通过就停）** —— **已完成，结论：原假设被推翻，经用户裁定扩展站点集**

用 IDA（只读，`save:false`）复核 `sub_100A8A4F8`（`0x100A8A4F8`）建的临时源 ctx：

- ✅ 只读使用（4 个 consumer 零写入）；✅ 不进 app 对象；✅ 不进被移入的
  `open2`/`send_packet`/`receive_frame`/`close`/`flush_buffers`（那些站点一律取 `[obj+0x10]` 的另一个 ctx）。
- ❌ **"建与弃都在该函数内"不成立**：alloc 在 helper（`0x100A8A514`），free 落在 5 个 caller 上
  （`0x100A80F4C`/`0x100A81104`/`0x100A89880` + SPDIF 的 `0x100B300F0`/`0x100B63F88`）；
  caller 实为 5 个（原判 3 个）。按原站点表移动即构成"alloc 在 4.4.5、free 在 shim"，违反 spec §2.4。

**用户裁定（2026-09-27）：扩展站点集**（39 → 44，见上表新增列），使该对象生命周期整体落在本单元内。
证据与逐条指令见 `notes/ida-investigation-p2-site-attribution.md` 的追加 1/追加 2；spec 文末有同名修订小节。
本步的**门禁现在是**：修订后的站点集满足"同一对象 alloc/consume/free 同单元"，且不与既有四域重复认领。

- [ ] **Step 2: 写守卫测试**

`tests/test_codec_manifest.py`：结构与 `tests/test_demux_manifest.py` 相同，断言符号集合、逐符号站点、
并集恰为 **44**、与 `ffmpeg-demux`/`libswscale`/`libswresample`/`ffmpeg-subdecode` 无交集、`old_target` 与
`ffmpeg-core` 同名 API 一致，以及**排除断言**：`0x100A469FC`（MJPEG 编码器函数）的 13 站点、
`sub_100B94DA0`/`sub_100B98A38`/`sub_100B9A294`/`sub_100B9A534`（HLS/mux/编码）的全部站点都不在集合内。
（注意：`sub_100A8A4F8`、`sub_100B30054`、`sub_100B63EDC`、`sub_100B63FA4` 自 2026-09-27 起**是部分认领**的
——只含上表 helper/SPDIF 列里的那 5 个站点，其余站点仍属 4.4.5，别整函数排除。）

- [ ] **Step 3: 跑测试确认失败**

Run: `uv run pytest tests/test_codec_manifest.py -q`
Expected: FAIL（找不到 `ffmpeg-codec` 域）

- [ ] **Step 4: 写进 manifest**

新增 `ffmpeg-codec` 域并追加到 `dylib[ffmpeg-core902].domains`。**不动任何既有域。**

- [ ] **Step 5: 跑测试确认通过**

Run: `uv run pytest tests/test_codec_manifest.py tests/test_subdec_manifest.py tests/test_manifest.py -q`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add manifests/nplayer-3.13.0.json tests/test_codec_manifest.py notes/ida-investigation-p2-site-attribution.md
git commit -m "feat: add the ffmpeg-codec unit to the manifest"
```

---

## Task 2: 复用影子核心，导出 12 个入口

**Files:**
- Modify: `bridge/npa_ffmpeg_subdec_bridge.c`（把内部实现抽成共享函数，供两套导出复用）
- Modify: `bridge/ffmpeg-core902.exports`（追加 12 个 `_npa_codec_*`）

**Interfaces:**
- Consumes: P2-A 的 `ctx_of()`/登记表/ABI 常量。
- Produces: 12 个 `NPA_EXPORT` 入口 `npa_codec_*`，与 P2-A 共用同一张 ctx 登记表与同一套影子构造；
  P2-A 的 16 个入口行为不变。

- [ ] **Step 1: 抽出共享实现**

把 `npa_subdec_*` 里与单元无关的部分提成内部函数（`npa_shadow_alloc_ctx` / `npa_shadow_free_ctx` /
`npa_shadow_open2` / `npa_shadow_params_*` …），`npa_subdec_*` 与新的 `npa_codec_*` 都只是薄转发。
**P2-A 的导出签名与行为不得改变**（现有守卫测试与设备验收必须继续成立）。

- [ ] **Step 2: 新增 12 个入口**

先全部 `__builtin_trap()`（除已在 Task 1 之外尚未实现的），保证构建与导出齐全。

- [ ] **Step 3: 构建与校验**

Run: `make bridge && make verify`
Expected: `ffmpeg-core902` 导出从 36 变为 **48**，7 项检查全过

- [ ] **Step 4: 回归**

Run: `uv run pytest -q && uv run pytest dev/tests -q`
Expected: 全绿（P2-A 的守卫与测试不受影响）

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_subdec_bridge.c bridge/ffmpeg-core902.exports
git commit -m "refactor: share the shadow core and add the codec unit entries"
```

---

## Task 3: 解码器操作（`send_packet` / `receive_frame` / `flush_buffers` / `close`）

**Files:**
- Modify: `bridge/npa_ffmpeg_subdec_bridge.c`

**Interfaces:**
- Consumes: Task 2 的共享实现。
- Produces: 可用的 12 个入口（`close` 特殊：9.0.2 无此符号，由 shim 自实现）。

- [ ] **Step 1: 实现 `send_packet`**

按 legacy 布局读 app 的 packet（`pts 0x08 / dts 0x10 / data 0x18 / size 0x20 / flags 0x28 /
duration 0x40`），构造瞬时现代 packet（`buf=NULL` 指向 app 内存；9.0.2 内部会自行复制/引用），调 9.0.2
`avcodec_send_packet`，释放瞬时对象，返回原返回值。

- [ ] **Step 2: 实现 `receive_frame`（本单元的核心）**

调 9.0.2 `avcodec_receive_frame` 得到现代帧后**物化进 app 的 legacy 帧**：
`data[8] 0x00 / linesize[8] 0x40 / extended_data 0x60 / width 0x68 / height 0x6C / nb_samples 0x70 /
format 0x74 / pts 0x88 / interlaced_frame 0xFC / sample_rate 0x110 / channel_layout 0x118 /
best_effort_timestamp 0x198 / channels 0x1BC / pkt_duration 0x1A8`。
`format` **必须翻译回 4.4 编号**（spec §3.3）；帧缓冲用 shim 自建的 legacy `AVBuffer`/`AVBufferRef`
（`refcount` 手写，释放回调里 `av_frame_unref` 现代帧），使 app 的 `av_frame_unref/free/ref` 正确工作。
现代帧的所有权挂在这些 legacy 引用上，直到最后一个引用释放。

- [ ] **Step 3: 实现 `flush_buffers` 与 `close`**

`flush_buffers`：先把影子的 `skip_loop_filter/skip_idct/skip_frame`（`0x33C/0x340/0x344`）按 app 的值同步给
现代 ctx（app 在 flush 前清零），再调 9.0.2 同名函数。
`close`：9.0.2 已删除该符号 ⇒ shim 实现为"flush + 标记关闭"，**不释放** ctx（随后的 `free_context` 才释放）；
若 app 在 close 后再用该 ctx 解码 ⇒ `__builtin_trap()`。

- [ ] **Step 4: 构建与校验**

Run: `make bridge && make verify && uv run pytest -q && uv run pytest dev/tests -q`
Expected: 全过

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_subdec_bridge.c
git commit -m "feat: drive the playback decoders through the shadow context"
```

---

## Task 4: 设备验收（**用户停点 A**）

**Files:**
- Modify: `dev/acceptance.json`（**新增** `ffmpeg-codec` 条目）

- [ ] **Step 1: 打产物（我来做）**

选择 `libass + ffmpeg-core902`，记下 packaged main sha256 与 dylib 哈希。

- [ ] **Step 2: 请用户安装并跑行（停下来等）**

**这一步必须由用户操作。** 按 `notes/playcover-debug-path.md`：

1. **缩略图/poster**（天然走软解）：H.264、HEVC Main10、**AV1**（用本机 `libsvtav1` 自造样本）；
2. **播放（先把 app 解码器切到软解）**：H.264、HEVC、HEVC Main10、AV1、MP4/MKV 容器 + seek + 连续播放；
3. **音频软解**：选能落到 `FFmpegAudioDecoder` 的编码（Opus / Vorbis）；
4. **内挂 + 外部字幕各一条**（确认 P2-A 未回归）；
5. **状态字读取**：先读 `0`，驱动后 `ffmpeg-codec` 读 `2 (NEW)`，同时确认 `ffmpeg-subdecode` 仍为 `2`；
6. **硬解路径不变**：不切软解时播放 H.264/HEVC，确认走 VideoToolbox、行为与今天一致。

- [ ] **Step 3: 写条目并 Commit**

```bash
git add dev/acceptance.json
git commit -m "test: record the playback decode unit acceptance"
```

---

## Task 5: 整 dylib 回退 A/B

**Files:**
- Modify: `dev/acceptance.json`（同一批条目的回退行）

**做法：不另做产物，直接从已安装的包里抽掉桥 dylib**（同 P2-A Task 7）。

- [ ] **Step 1: 备份并抽掉 dylib**

```bash
APP=~/Library/Containers/io.playcover.PlayCover/Applications/com.newin.nplayer.basic.app
BK=/private/var/folders/rx/pf75s9k53vg45_mtzdbkh7hh0000gn/T/opencode
cp "$APP/Frameworks/LibFFmpegCore902Bridge.dylib" "$BK/LibFFmpegCore902Bridge.dylib.bak"
rm "$APP/Frameworks/LibFFmpegCore902Bridge.dylib"
```

- [ ] **Step 2: 重启 app、驱动、读状态字**

Expected: `ffmpeg-core902` 承载的**全部 5 个单元**（`ffmpeg-demux`/`libswscale`/`libswresample`/
`ffmpeg-subdecode`/`ffmpeg-codec`）读 `3 (OLD)`，`libass` 保持 `2 (NEW)`，播放与字幕行为等同 app 自带 4.4.5。

- [ ] **Step 3: 还原并写条目**

把 dylib 拷回 `Frameworks/`，确认状态字恢复 `2 (NEW)`，把两种状态的结果写进条目。

- [ ] **Step 4: Commit**

```bash
git add dev/acceptance.json
git commit -m "test: record the whole-dylib fallback after the codec unit landed"
```

---

## 收尾（不单独设任务）

- **AV1 复核**：若原生 `av1` 在设备上出现明显问题（画质/卡顿/崩溃），停下来向用户提"给闭包加
  `libdav1d ≥1.0.0`"（构建路径见 spec §3.1），**不得静默降级或绕过**。
- 解码器集合对账：把 `dev/tools/enable_set_diff.py` 对 `LibFFmpegCore902Bridge.dylib` 的差分收敛到只比
  **解码器名**，逐项给出处置（补齐 / 接受差异 / 记为不覆盖），结论写进同一验收条目。
- 全分支复跑：`uv run pytest -q`、`uv run pytest dev/tests -q`、`git status` 干净；临时诊断移除。
- 播放解码落地后，路线图 `2026-09-27-expand-ffmpeg902-wiring.md` 的 P2 关闭；P3（mux/录制）仍留待可测环境。
