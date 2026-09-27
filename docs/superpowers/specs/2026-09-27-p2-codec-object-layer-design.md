# P2 设计：把 codec 对象层接入 9.0.2 核心

> 分支 `feat/ffmpeg-demux-class-a`（worktree `.worktrees/ffmpeg-core-fullswap`）。这是**高实验性分支**，
> 稳定性由 `main` 承担；但"可回落、可验收、不改已验面"这三条不放松。
>
> 设计依据（RE 已落盘，本文件只引用不复述大段反编译）：
> - `notes/ida-investigation-p2-site-attribution.md` —— 解码区域站点 → app 路径归属
> - `notes/ida-investigation-p2-field-surface.md` —— `AVCodecContext`/`AVFrame`/`AVPacket` 逐字段读写表
> - `notes/ida-investigation-p2-subtitle-decode.md` —— 字幕解码链与 `AVSubtitle( Rect)` 消费面
> - 既有：`notes/ida-investigation.md` §7/§8、`notes/ida-investigation-5-decoder-switch-indicator.md`、
>   `notes/ida-investigation-8-thumbnail-paths.md`、`notes/playcover-debug-path.md`（怎么测）、
>   `notes/ffmpeg-pixfmt-abi-mismatch.md`
> - 路线与需求：`docs/superpowers/plans/2026-09-27-expand-ffmpeg902-wiring.md`（P2 的展开）

---

## 0. 目标与成功判据

把 9.0.2 的接线范围从「解复用 + 缩放 + 重采样」扩到**解码**：新增两个桥接单元承载 app 的字幕解码与
播放/probe 软解，复用与 demux 单元同构的影子机制。

本轮结束时必须同时成立：

1. 两个新单元在**本环境**（PlayCover，见 `notes/playcover-debug-path.md`）有可跑的验收行，且设备通过；
2. 已验收过的面（demux、字幕 demux、libswscale、libswresample、libass）行为**不变**；
3. 任一单元失败可**整单元回落**，且不破坏其它单元；
4. 默认产物 `libass + ffmpeg-full`（4.4.8）与其打包锚点**完全不变**。

---

## 1. 范围与单元边界

### 1.1 两个单元

| 单元（新 domain id） | 归属路径 | 依据 |
|---|---|---|
| `ffmpeg-subdecode` | 字幕**解码**（含外部/内嵌字幕文件簇的解码侧） | 站点归属 §A、字幕链笔记 §3–§7 |
| `ffmpeg-codec` | 播放软解（`FFmpegVideoDecoder` / `FFmpegAudioDecoder`）+ probe/poster 抽帧解码 + UI 帧转换 | 站点归属 §B–§E |

两域都挂在 **同一个** dylib `ffmpeg-core902` 上（现已有 `ffmpeg-demux` / `libswscale` / `libswresample`
三个单元）⇒ 每个新单元各自一个状态字、各自解析、各自回落。选择与冲突关系不变：
`ffmpeg-core902` 继续与 `ffmpeg` / `ffmpeg-core` / `ffmpeg-full` 互斥；默认选择不动。

### 1.2 站点集合按「函数」定，不按地址区间定

反例（已坐实）：视频打开函数 `sub_100A80F64` 的 9 个站点里有 6 个落在 `0xA81000` 以下；音频打开
`sub_100A8989C`、共享 helper `sub_100A8A4F8`、缩略图解码 `sub_100A46774` 与 `sub_100A233C4` 同样在
我最初划的区间之外/边界上。因此：

- 单元的归属单位是**包含函数**（站点归属笔记的 A–E 节），站点由「函数 × 符号策略」筛出；
- 筛出的表要逐条通过 §7 的两条一致性检查（**同单元内 alloc/free/consume 配套**、**不与既有域重复认领**）。

规模（按 manifest 站点在归属函数内的计数，含将被保留的符号）：字幕解码约 47 站点、播放/probe 约 92 站点；
最终认领数由实施计划第一步筛出后固定，不在本文件承诺。

### 1.3 明确排除（连同原因）

| 对象 | 原因 |
|---|---|
| mux / HLS session / SPDIF / 编码器：`0x100B98A38` `0x100B94DA0` `0x100B9A294` `0x100B9A534` `0x100B96C48` `0x100B9E588` `0x100B9A384` `0x100B9AA64`、SPDIF 的 `0x100B30054` `0x100B3010C` `0x100B303C0` `0x100B30430` | 本环境不可驱动（路线图 P3）；`0x100B94DA0` 虽含解码 API，但属 HLS 会话，**列入未决问题**（§6） |
| poster 路径里的编码器 `sub_100A469FC`（`avcodec_find_encoder`=MJPEG @`0x100A46AC0`） | 它是**编码**入口，只因位置在 poster 路径内；不属解码单元 |
| 全局初始化 `sub_100A8A1E8`（`avformat_network_init`/`av_log_set_level`，8 个跨路径 caller） | 纯环境调用，无对象，被多单元共用 |
| 共享 helper `sub_100A8A4F8` 的全部站点（video+audio+**SPDIF** 共用） | 它建的是 4.4.5 的**临时源 ctx**，经 `parameters_from_context` **以参数形式**导出，建与弃都在该函数内 ⇒ 不移动即可，且避免把不可测的 SPDIF 拉进来（见 §2.5/§7） |
| 已被 `ffmpeg-demux` 域认领的站点（含落在解码函数内的 `av_read_frame@0x100A46964`、`avio_size@0x100A46874`、`av_seek_frame@0x100A46944`、`avformat_close_input@0x100A415B4`、`avformat_free_context@0x100A415D4` 等） | 同一 dylib 内一个站点只能被认领一次 |

### 1.4 不变量

- **app 侧永远只见 legacy 形状的对象与 legacy 归属的缓冲**（沿用 demux 单元已验证的机制）。
- **legacy 布局是单元之间的通用交换格式** ⇒ 任一单元回落都不会破坏另一个单元（包括已 NEW 的
  demux 单元与新解码单元之间的对象流）。
- 单元粒度 = 回落粒度；失败时该单元整个回落，绝不半迁移。

---

## 2. 对象模型与影子

### 2.1 四类对象

| 对象 | 权威持有者 | 交给 app 的形状 | 同步方向 |
|---|---|---|---|
| `AVCodecContext` | shim（`sizeof` 4.4=0x438） | legacy 影子 | **两向**：入口 legacy→modern；返回后 modern→legacy |
| `AVFrame` | shim（4.4 `sizeof=0x218`） | legacy 影子 | **单向**：`receive_frame` 后把现代帧物化进影子 |
| `AVPacket` | app 拥有栈上结构 | 原样（就是 app 的栈内存） | **单向**：`send_packet`/`decode_subtitle2` 入口按 legacy 布局读出并构造现代 packet |
| `AVSubtitle` / `AVSubtitleRect` | shim（4.4 `sizeof` = 0x20 / 0xC8） | legacy 影子 | **单向**：解码后物化；`avsubtitle_free` 被移动 ⇒ 释放归 shim |

依据：字段面笔记 §1.1/§2/§3 与字幕链笔记 §5/§7。

### 2.2 影子布局要点（全部有 `_Static_assert` 守卫）

- `AVSubtitleRect` 的 4.4 布局**含已废弃的 `AVPicture pict`**（`FF_API_AVPICTURE=1`，本 build 版本号 58）：
  `w@8 h@0xC nb_colors@0x10 pict@0x18 data[4]@0x78 linesize[4]@0x98 type@0xA8 text@0xB0 ass@0xB8 flags@0xC0`，
  `sizeof=0xC8`。9.0.2 已删 `pict`（`type@0x48` …）⇒ **必须做影子**。
- app **只读** `type/text/ass/w/h/nb_colors/data[0]/linesize[0]`，**不读** `x/y/pict.*/flags`（字幕链笔记 §5）⇒
  影子这些字段可以是 0，但 `data[0]/linesize[0]` 必须指向有效位图。
- app 对 `AVSubtitle` 只读 `format/start_display_time/end_display_time/num_rects/rects/pts`（`sizeof=0x20`）。

### 2.3 同步字段（来自字段面笔记，实现时逐条对照）

**`AVCodecContext` — app 写入，必须 legacy→modern**：
`codec_type(0x0C)`、`codec_id+codec_tag(0x18/0x1C)`、`extradata(_size)(0x58/0x60)`、`time_base(0x64)`、
`width/height(0x74/0x78)`、`pkt_timebase(0x374)`（字幕路径）；`bit_rate(0x38)`、`pix_fmt(0x88)`、
`sample_aspect_ratio(0xE8)`、`color_primaries/color_trc/colorspace(0x174/0x178/0x17C)`、
`sample_rate/channels(0x190/0x194)`、`sample_fmt(0x198)`、`block_align(0x1A4)`、
`bits_per_coded_sample(0x2F8)`（源 ctx 经 `parameters_from_context` 转出）；
`thread_count(0x310)=hw.ncpu`、`thread_type(0x314)=FF_THREAD_FRAME`（视频打开）；
flush 时清零 `skip_loop_filter/skip_idct/skip_frame(0x33C/0x340/0x344)`。

**app 只读的 ctx 字段**（shim 负责填对，否则 app 用错值）：`codec_id(0x18)`、`codec_tag(0x1C)`、
`time_base(0x64)`、`width/height(0x74/0x78)`、`pix_fmt(0x88)`、`sample_aspect_ratio(0xE8)`、
`color_primaries/trc/colorspace(0x174/0x178/0x17C)`、`sample_rate/channels(0x190/0x194)`、
`sample_fmt(0x198)`、`channel_layout(0x1B0)`、`subtitle_header(_size)(0x348/0x350)`。

**`AVFrame` — app 零写入**，只读：视频 `format(0x74)`、`interlaced_frame(0xFC)`、
`best_effort_timestamp(0x198)`、`pkt_duration(0x1A8)`；音频 `pts(0x88)`、`extended_data(0x60)`、
`nb_samples(0x70)`、`channels(0x1BC)`。

**`AVPacket` — app 全量写**（栈上）：`pts(0x08)`、`dts(0x10)`、`data(0x18)`、`size(0x20)`、`flags(0x28)`、
`duration(0x40)`；字幕解码侧另只读 `pts/data/size/duration`。

**回调 / `priv_data` / hw 字段：app 全无写入**（`get_buffer2`/`draw_horiz_band`/`get_format`/`execute*`/
`priv_data`/`hw_device_ctx`/`hw_frames_ctx`/`refcounted_frames`；三处 `open2` 的 options 均为 NULL）。

### 2.4 缓冲归属与释放

- 跨 shim 的缓冲一律用 **9.0.2 自己的 `av_buffer_create(...)`** 表达（现代侧）；app 侧仍是 legacy 的
  `AVBuffer`/`AVBufferRef` 手工构造（demux 单元已实现，直接复用其做法）。
- 因此两个 shim **不需要共享状态**，只靠 legacy 布局互相理解。
- **释放归属跟随「谁被移动」**：`av_frame_unref/free`、`av_packet_unref/free`、`avcodec_free_context`、
  `avsubtitle_free` 在单元内的站点都由 shim 处理 ⇒ shim 交出的缓冲必须能被 shim 自己安全回收。
- **一致性硬约束**：同一个对象的 `alloc` / `consume` / `free` 站点必须落在**同一个单元**内；不满足就扩大
  该单元的站点集合，而不是让对象跨单元生命周期。这条由计划第一步逐条核对。

### 2.5 符号策略（移动 vs 保留 4.4.5）

判据：**会被调用在 shim 产出对象上的才移动**；只吃枚举/整数、或只写 app 自有内存的，保留 4.4.5。

- **移动**：`avcodec_alloc_context3`、`free_context`、`open2`、`close`、`flush_buffers`、`find_decoder`、
  `parameters_alloc|free|copy|from_context|to_context`、`decode_subtitle2`、`avsubtitle_free`、
  `send_packet`、`receive_frame`；`av_frame_alloc|free|unref|ref`；
  `av_packet_alloc|free|unref|ref|copy_props|move_ref`、`av_new_packet`。
- **保留 4.4.5（不认领）**：
  - 纯值 helper：`av_get_bytes_per_sample`、`av_sample_fmt_is_planar`、`av_get_default_channel_layout`、
    `av_get_channel_layout_channel_index`、`av_rescale_q`、`av_reduce`、`av_log2`、`avcodec_get_name`、
    `avcodec_descriptor_get`、`av_codec_get_tag`、`av_packet_rescale_ts`；
  - 只写 app 自有内存：`av_image_fill_arrays`、`av_image_get_buffer_size`、`av_image_copy`、`av_image_alloc`、
    `av_samples_get_buffer_size`、`avcodec_fill_audio_frame`；
  - `av_init_packet`（初始化的是 app 自己的栈上 legacy packet；真正的翻译发生在 send/decode 入口）；
  - `sub_100A8A4F8` 的全部站点（临时源 ctx，见 §1.3）；
  - `sub_100A8A1E8`（全局 init）；
  - demux 域已认领的 `avformat_*`/`avio_*`（含落在解码函数内的）。

### 2.6 双模读取与不静默

- 被移入口收到**不在登记表里**的对象 ⇒ 按 **legacy 布局**读取。这是**正常用法**（源 ctx 由未移动的
  4.4.5 代码创建，`parameters_from_context` 必须以 legacy 布局读出它），不是静默回落。
- 但若该 legacy ctx 带 hw 字段（`hw_device_ctx`/`hw_frames_ctx`/`get_format` 任一非空）⇒ **直接失败**：
  9.0.2 无法消费 4.4.5 的 hw 帧，静默只会产出错帧。（已确认 app 不使用 FFmpeg hwaccel，所以这条现实中
  不应触发，保留为一行断言。）

---

## 3. 行为差异与处置

1. **AV1 → 9.0.2 原生解码器**（闭包已确认有 `_ff_av1_decoder`，无 `libdav1d`）。验收行：本机
   `libsvtav1` 自造 AV1 样本能解、能播、不崩。后备方案 = 给 9.0.2 闭包加 `libdav1d ≥1.0.0`
   （`deps/build_ffmpeg_core.py::build_dav1d` 路径现成，meson 在项目 venv 里），仅在原生不达标时启用，
   且必须经用户确认。
2. **解码器集合对账**：`avcodec_find_decoder` 从此在 9.0.2 自己的表里查找。现有
   `dev/tools/enable_set_diff.py` 对 `LibFFmpegCore902Bridge.dylib` 报出「app 有、闭包没有」= `libdav1d`
   等 4 项。计划里要把该差分**收敛到只比解码器名**，并逐项给出处置（补齐 / 接受差异 / 记为不覆盖）。
3. **格式编码方向 = legacy**：`AVFrame.format` 与 `AVCodecContext.pix_fmt/sample_fmt` 在 app 侧必须是
   4.4 编号（证据：app 自带格式表 `0x1016A58B8` 是 4.4 编号，poster 路径直接拿 `AVFrame.format` 去转换）。
   ⇒ shim 物化帧时把现代值翻回 4.4 编号；**sws 入口既有翻译保持不变**。路线图 §6 的开放问题据此关闭。
4. **音频路径**：`FFmpegAudioDecoder` 只覆盖子集（AudioToolbox / `DTSAudioDecoder` 优先）⇒ 验收的音频行
   必须选**能落到 `FFmpegAudioDecoder` 的编码**（如 Opus/Vorbis），否则等于没测。
5. **调优字段语义**：`thread_count=ncpu`、`thread_type=FF_THREAD_FRAME` 与 flush 时的 `skip_*` 清零必须
   在 modern ctx 上同样生效（否则解码并发/丢帧行为会变）。

---

## 4. 验收与回退

### 4.1 验收行（全部本环境可测）

| 面 | 行 |
|---|---|
| 缩略图/poster（天然走 FFmpeg 软解，`sub_100A46774`） | H.264、HEVC Main10、**AV1（自造样本）** |
| 播放**软解**（app 内切软解才打到：`ida-investigation-5-decoder-switch-indicator.md`） | H.264、HEVC、HEVC Main10、AV1；连续播放 + seek |
| 音频软解 | 选能落到 `FFmpegAudioDecoder` 的编码（Opus / Vorbis） |
| 字幕解码单元 | 内挂 ASS/SRT 渲染 + seek；外部 `.ass`（沿用既有字幕矩阵） |
| 状态字 | 两个新单元各读 `2 (NEW)`（先读 `0` 建立因果，再驱动） |
| 默认产物回归 | `libass + ffmpeg-full` 核心行——新域不从 `ffmpeg-core` 域移走任何站点，默认载荷与锚点不变 |

判读手法见 `notes/playcover-debug-path.md`（启动、喂文件、读状态字）。

### 4.2 回退证明

- **整 dylib 回落（沿用 `dev/acceptance.json` 的 `fallback-full` 配方）**：同一套素材，去掉
  `ffmpeg-core902` 再装一次 ⇒ 它承载的**全部**单元（含两个新单元）读 `3 (OLD)`，`libass` 保持 `2`，
  播放行为等同 app 自带 4.4.5。证明"不存在半迁移 + 无跨 dylib 连坐"。
- **单元级回落（可选，建议做）**：造一个只删一个导出符号的测试产物 ⇒ 只有该单元读 `3`，同 dylib 其余
  单元仍 `2`。证明粒度确实是单元。做法记在 `notes/playcover-debug-path.md` §9.2。

### 4.3 需要用户操作的停点

安装 IPA 必须由用户执行：至少 ① 含两个新单元的产物跑验收行；② 回退用产物（去 dylib / 删单符号）。
实施计划里要明确标出这两个停点。

---

## 5. 风险登记（沿用路线图 §9 编号续写）

| 编号 | 风险 | 状态 | 处置 |
|---|---|---|---|
| R4 | P2 影子落在热路径（视频/音频每帧都过 shim） | **本轮必须面对** | 判据是"行为等价 + 无可见卡顿"：验收行含连续播放与 seek；同步只做字段搬运，不复制帧数据（缓冲用 `av_buffer_create` 转移所有权） |
| R5 | 9.0.2 无法使用 dav1d 0.9.2 | **已决** | 本轮走原生 AV1；后备加 `libdav1d ≥1.0.0`（需用户确认） |
| R7 | 单元自身的回落证明 | **本轮关闭** | §4.2 的整 dylib A/B（+ 可选的单符号变体） |
| R8（新） | 影子与 4.4 布局不一致导致静默错帧 | 新增 | 全部影子偏移用 `_Static_assert` 守卫；app 只读字段必须由 shim 填对；hw 字段一律失败 |
| R9（新） | 站点划分遗漏导致对象跨单元生命周期 | 新增 | 计划第一步的一致性检查（§2.4）；未通过就不动代码 |
| R10（新） | `enable_set_diff` 显示的解码器集合差被忽略 | 新增 | §3.2 的逐项对账是计划的一步 |

---

## 6. 未选项与未决问题

**未选（本轮不做，记录理由）**

- 方案 B（app 拥有对象 + shim 挂现代 side-car）：新模式、未移动站点有隐患、与 demux 单元不同构。
- 给 9.0.2 加 `libdav1d ≥1.0.0`：先看原生 AV1 的表现。
- 把 mux/encode/HLS/SPDIF 一起接入（路线图 P3）：本环境不可测。
- 把 `ffmpeg`（9.0.2 的 sws/swr 拆分单元）退役：不影响本轮，独立决定。

**未决（需要时再定，不阻塞本轮实现）**

1. `sub_100B94DA0`（HLS 会话内含解码 API）是否要在将来纳入解码面。
2. `sub_100A897D4` 写 `request_channel_layout=0x60000000` 但该字段不经 `parameters_from_context` 传递，
   是否有效存疑（可能是无效写）。
3. `sub_100A3240C`（`sub_100A8A3B4` 的第二个 caller）的完整角色；`sub_100A237B0` 的调用者。
4. `sub_100A3E1F8` 读 `AVCodecParameters` 的 `channels/sample_rate`（Probable）。

---

## 7. 对实施计划的输入（约束，不是步骤）

1. **第一步必须是站点表的机械化筛出与逐条核对**：按 §1.2 的「函数 × 符号策略」筛出两个单元的站点，
   逐条通过 §2.4 的一致性检查，并确认不与 `ffmpeg-demux`/`libswscale`/`libswresample` 域重复认领。
   未通过则不进入实现。
2. **第二步复核 §1.3 的源 ctx 不外溢**（对 `sub_100A8A4F8` 及其 caller 的反编译证据）；若被推翻，
   停下来重新设计（要么让 shim 收养 legacy ctx，要么把 SPDIF 一起纳入——两者都要回到用户面前）。
3. 每个单元一个 domain + 一个状态字；导出符号用新前缀（如 `npa_codec_*` / `npa_subdec_*`），
   避免与 `npa_core_*`/`npa_demux_*` 冲突。
4. 影子布局与偏移守卫放进新的 ABI 头（沿用 `bridge/ffmpeg-demux-abi.h` 的 `_Static_assert` 做法）。
5. manifest：新域挂到 `ffmpeg-core902`；**不从 `ffmpeg-core` 域移走任何站点** ⇒ 默认选择载荷与锚点不变。
6. 测试只加能防回归的（站点归属守卫、影子偏移断言、单元一致性检查）；临时诊断在验收前移除。
7. `dev/acceptance.json` **新增**条目（`ffmpeg-codec` / `ffmpeg-subdecode`），不改写历史条目。
8. 计划里标出设备安装/验收的**用户停点**（§4.3），以及每步的 Commit 切分。
9. 不引入新依赖（原生 AV1）；`libdav1d ≥1.0.0` 仅在原生不达标时提出，并需用户确认。
10. **落地顺序**：先字幕解码单元（冷路径、字幕矩阵已验），再播放/probe 单元（热路径）。前者通过设备验收
    后才动后者；两个单元共享影子基础设施，但各自独立回落，因此顺序推进不会互相阻塞。
