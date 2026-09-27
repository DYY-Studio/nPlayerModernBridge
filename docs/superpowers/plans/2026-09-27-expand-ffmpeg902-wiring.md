# nPlayer × FFmpeg 9.0.2 接线扩展：路线 A（先收敛再扩面）

> 本文件由 Plan 模式草案搬运并修正（草案曾在 `~/.opencode/plan/`）。修正点：原草案把
> §1.2 的像素格式漂移判为"活缺陷"并单列 P0.5b；实际该翻译**早已实现并设备验收**
> （`05423b2`，见 §4），故 P0.5b 关闭，路线表中移除。
> 本轮未修改 IDA 数据库（只读、未保存）。

- 工作树/分支：`nPlayerLibassBridge/.worktrees/ffmpeg-core-fullswap` @ `feat/ffmpeg-demux-class-a`
- 起点 HEAD：`22e24a8`（class-A demux 面已设备验收，结论 `partial`）
- 目标：把 9.0.2 的接线范围从"纯播放 demux 面"逐步扩到**单个 9.0.2 核心单元**

---

## 0. 收益的准确表述（避免错误期待）

1. **现代能力**：新 demuxer / decoder / 格式。
2. **减少"混版本交互面"**。今天存在两处跨版本交互：
   - 9.0.2 demux 产出的对象 → app 的 4.4.8 avcodec（已由 class-A 影子 + 枚举翻译处理）；
   - app 侧 4.4.x 的像素格式 → 9.0.2 swscale（**已在 sws 入口翻译**，见 §4）。
3. 最终可退役 4.4.8 闭包（维护/安全）。

**必须澄清一点**：app 二进制是固定的，它内嵌了 4.4.5 头编译出的结构体偏移与枚举常量。因此：

- **结构体布局影子是永久设施**（Confirmed）：app 直接按 4.4.x 偏移解引用字段，与 FFmpeg 是哪个版本无关。
- **枚举翻译至少在当前布局下是必需的，且方向是 app→FFmpeg**（Confirmed）：实证是 sws 入口必须
  把 app 传来的值翻译成现代值，否则 P010 缩略图崩溃（§1.2、§4）。P2 之后是否仍需要取决于
  "AVFrame 影子里的 format 按哪种编码写"，见 §6 的开放问题。

"整体 9.0.2"减少的是 FFmpeg **实例之间**的混版本面；而 app ↔ FFmpeg 的适配层会随覆盖面增加而**增多**
（每多一个面就多一组结构体影子）。

---

## 1. 已确认事实

### 1.1 现状（单元 → 闭包）

| 单元 id | basename | 版本 | 静态链接的库 | domain（API/站点） |
|---|---|---|---|---|
| `libass` | `LibASSBridge.dylib` | 0.17.5 | libass + FreeType/HarfBuzz/FriBidi/fontconfig/expat | libass（15/16） |
| `ffmpeg` | `LibFFmpegBridge.dylib` | 9.0.2 | avutil + swresample + swscale | libswscale（5/13）、libswresample（6/7） |
| `ffmpeg-core902` | `LibFFmpegCore902Bridge.dylib` | 9.0.2 | avcodec + avformat + avutil + swscale + swresample | ffmpeg-demux（10/25）、libswscale（5/13）、libswresample（6/7） |
| `ffmpeg-full` | `LibFFmpegFullBridge.dylib` | 4.4.8 | avcodec + avformat + avutil + swscale + swresample + **libdav1d 0.9.2** | ffmpeg-core（99/449）、libswscale（5/13）、libswresample（6/7） |
| `ffmpeg-core` | `LibFFmpegCoreBridge.dylib` | 4.4.8 | avcodec + avformat + avutil + **libdav1d 0.9.2** + swresample + swscale | ffmpeg-core（99/424） |

- 打补丁默认选中 `default_dylibs`（**`libass` + `ffmpeg-full`**）；备选之间用 `conflicts` 互斥。
- 同步 `main` 后（`38b5b97`），`ffmpeg-core` domain 与 `ffmpeg-full` 共用且必须保持完整（449 站点）。
- 进程内**已经存在 9.0.2 代码**：`ffmpeg-demux` 甚至已静态链入 9.0.2 的 `libavcodec`
  （`find_stream_info` 内部探测需要）。所以"扩大接线"不是引入新库，而是**把更多 app 调用点
  接到已有 9.0.2 库，并为 app 直接解引用的结构体补影子**。

### 1.2 sws/swr 的跨版本像素格式（**已处理**；此处保留实测与取值来源）

方法：对两棵头树分别编译探针打印枚举值后 join 比对；再对 sws/swr 调用站点做 IDA 取值追溯。

**枚举差异**

| 枚举 | 4.4.8 项 | 9.0.2 项 | 差异 |
|---|---|---|---|
| `AVPixelFormat` | 275 | 375 | **216 项不同**；**≤45 安全区不变**（`YUV420P=0`、`RGB24=2`、`YUVJ420P=12`、`NV12=23`、`RGBA=28` …） |
| `AVSampleFormat` | 13 | 13 | **0 项不同** |

漂移规律（与既有笔记一致）：4.4 枚举中部多出三个后续被删除的成员（`VAAPI_MOCO=44`、
`VAAPI_IDCT=45`、`XVMC=153`），故 legacy→modern 为 `0..45` 恒等、`46..153` 减 2、`154..197` 减 3。
样例：`YUV420P10LE 64→62`、`YUV444P10LE 70→68`、`P010LE 161→158`、`VIDEOTOOLBOX 160→157`。

**取值穿越（IDA，保留供 P2 参考）**

| 站点 | 函数/角色 | srcFormat 来源 | 判定 |
|---|---|---|---|
| `0x100A46A90` | `sub_100A469FC`：解码帧 → YUV420P → JPEG（poster/backdrop，调用者 `sub_100A3E1F8`） | **`AVFrame.format`（+0x74）**，帧由 app 自己的 4.4.5 软解路径 `sub_100A46774`（`avcodec_receive_frame`）产出 | Confirmed |
| `0x100A23428` | `sub_100A233C4`：帧 → RGB24 → `CGImageCreate`（UI 快照） | 自定义帧包装对象字段 `+0x2C` | Probable |
| `0x100A8A4A4` / `0x100A8A450` | `sub_100A8A3B4`：通用转换（调用者 `0x100A22F2C`、`0x100A3240C` 播放转换线程） | 同上 `+0x2C` | Probable |
| dstFormat | 同上 | 立即数 `0/2/12/23` | 全部安全值 |

其他确认项：

- app 自带的 `av_pix_fmt` 描述表在 `0x1016A58B8`（stride `0xA0`），编号即 **4.4.x**
  （`[64]=yuv420p10le`、`[161]=p010le`）——**P2 的关键输入**：它说明 app 侧持有的值按 4.4.x 编码。
- 解码器硬编码 `70/77/133/137`（`yuv444p10le`/`gbrp10le`/`yuv444p12le`/`gbrp12le`）与通用 10-bit
  4:2:0 的 `64`，**全在重编号区**（这些是 app 内嵌的 4.4.5 libavcodec 自己的常量）。
- **没有任何 `AVFrame*` 跨入 sws/swr**：只传整数格式值 + 平面指针/stride；数组布局跨版本一致，
  唯一随版本变的就是那个整数。
- app 代码中**不存在字符串驱动的格式查找**，所以不能靠"按名字解析"绕开。
- swr：sample format 无漂移；声道布局是**旧式 uint64 mask**，桥接已翻译（`npa_layout_from_legacy_mask`）。

---

## 2. 路线总览

| 阶段 | 内容 | 新增影子对象 | 本环境可测 | 回退粒度 |
|---|---|---|---|---|
| **P0** ✅ | 形态收敛：新增 `ffmpeg-core902`（demux + swscale + swresample 三个 domain 一个 dylib），退役 `ffmpeg-demux` 条目；保留 `ffmpeg`（main 的 split 备选） | 无（接线点不动） | 是 | 整个 9.0.2 核心 |
| **P1** | 字幕 **demux** 面（`0x100AB4xxx`） | **带缓冲 `AVIOContext`**、`AVInputFormat` | 是（字幕矩阵已验） | 同上 |
| **P2** ✅ | **codec 对象层**：`AVCodecContext`/`AVCodec`/`AVFrame`/`AVSubtitle`(+rect)/`AVBSFContext`，并迁移 avutil 的对象分配 API。**实际落地**：两个单元 `ffmpeg-subdecode`（字幕解码，P2-A）与 `ffmpeg-codec`（播放/probe/poster 软解，P2-B）；`av_frame_*`/`av_packet_*`/`av_dict_*`/`av_image_*`/`av_samples_*` 经裁定**不迁移**（帧/包仍 app 自持，shim 就地物化），`AVBSFContext` 未纳入（BSF 站点在 HLS/mux 侧，属 P3） | ctx/params/subtitle/frame 影子（复用 P2-A 的共享核心） | 是（两轮设备验收已过） | 同上 |
| **P3** ✅ | mux/录制/HLS session/SPDIF | **同世代单元**：4.4.8 原样，零翻译零影子（asm 尾分支 + 编译期 ABI 守卫） | **是**（设备证据见 `dev/acceptance.json` 的 `ffmpeg-out448`） | 另一形态是探索分支的 9.0.2 输出侧；HLS/SPDIF 行的驱动条件见 §7 |

**统一"能稳稳吃下"判据**：阶段内有本环境可测的验收行；不改动已验面的行为；失败时整单元回退。

**为什么 P1 可以先做**：与现有 demux 面同构——9.0.2 只出 demux 对象（已有影子），解码仍归 4.4.8。
**为什么 P2 不能拆小**：`libavutil` 定义结构体布局（`av_frame_alloc`/`av_packet_*`/`av_dict_*`），
一旦有 9.0.2 实例产出，任何仍留 4.4.8 的 avcodec/avformat 都无法消费；只能成组推进，
用"冷的先上（字幕解码）"控制风险。

---

## 3. P0：形态收敛为 `ffmpeg-core902`（代码已完成，待设备复验）

### 3.1 目标与同步后的修订

把两块 9.0.2（demux 面 + sws/swr 面）并成**一个** dylib，**接线点、符号名、站点集合全部不动**：
它提供三个 domain（`ffmpeg-demux` 10 API/25 站、`libswscale` 5/13、`libswresample` 6/7），
共 **21 API / 45 站点**，导出 21 个符号。

同步 `main` 后这一目标必须重新表述（原稿在此已过时）：

- `ffmpeg-core` domain 与新的 `ffmpeg-full`（**当前默认**）共用且必须保持完整，因此**不能**把 25 个
  demux 站点从 core 域里移走（那会让默认产物不完整）；
- 于是改为：新增 `ffmpeg-core902`，与 `ffmpeg` / `ffmpeg-core` / `ffmpeg-full` **全部互斥**；
  退役 `ffmpeg-demux` 这个 **dylib 条目**（其 domain 仍"声明一次"，改由 `ffmpeg-core902` 提供）；
- **保留 `ffmpeg`**（9.0.2 sws/swr）——它是 main 已验收的 split 备选（`libass + ffmpeg + ffmpeg-core`）
  的一半，合并掉它等于退役一个已验收产物；
- 默认仍是 `libass + ffmpeg-full`；9.0.2 的选择是 `libass + ffmpeg-core902`。

### 3.2 实施（已提交）

| Commit | 内容 |
|---|---|
| `5f0f9f3` | `deps/ffmpeg-core902.lock.json` = demux 锁 + 启用 swscale、列入 `libswscale.a`；闭包构建与 `--verify-only` 通过 |
| `38b5b97` | 与 main 合并：core 域保持完整；demux 成为互斥备选；守卫改为"共享站点的单元必须互斥" |
| `0d2f51e` | 单元级聚合源 `bridge/npa_ffmpeg_core902_bridge.c`（引入两个 shim）+ 导出集 + manifest/Makefile/测试/README；删除 `ffmpeg-demux` 的 lock 与 exports |

- 多源文件方案：**单元级聚合源文件**（与 `ffmpeg-full`"一个 dylib 一个源文件"一致），工具链零改动。
- 两 shim 无同名文件级符号；合并后编译无警告（`NPA_EXPORT` 定义一致）。
- 枚举生成器改读新闭包头文件，**重生成结果字节一致**。
- 合并 dylib 19.9 MB < 两块之和 20.3 MB：去重了一份 `libavutil`。

### 3.3 验收

1. ✅ `make bridge` / `make verify`：五个 dylib 各 7 项检查全过；`ffmpeg-core902` 导出 21 个。
2. ✅ `uv run pytest`：86 passed / 1757 subtests；`dev/tests`：7 passed。
3. ✅ **设备复验**（`libass + ffmpeg-core902`，packaged main `34f19c62…`）：HEVC Main10 + FLAC Matroska、
   带章节 Matroska、缩略图、H.264、内嵌字幕 + Matroska 字体、https HLS 全部通过。这一次同时确认了：
   - 合并本身无行为变化；
   - **demux 面不再依赖 4.4.8 core**（该选择下其余面回落到 app 自带 4.4.5）——选项 A 的前提成立。
   矩阵其余行与状态字读取**未在本选择上重测**（其消费者由 4.4.8 变为 4.4.5），条目中已如实标注为"沿袭"。
4. ✅ 新增 `dev/acceptance.json` 条目 `ffmpeg-core902`（`result: partial`）：待做/未决项为 fallback 证明、
   `[Reference]` 分支、矩阵其余行，以及本环境不可测的录制路径。

### 3.4 风险

| 风险 | 处置 |
|---|---|
| 两桥接源文件 `static` 重名 | 已验证无重名，合并编译无警告 |
| 单元合并改变回退粒度 | 已由用户接受；README 写明 |
| 合并改变默认产物 | 未改变：默认仍是 `libass + ffmpeg-full` |
| 合并引入行为差异 | 以"行为等价"为验收标准；设备复验待做 |

## 4. 已关闭：sws 入口的像素格式翻译（原 P0.5b）

原草案据 §1.2 提出"在 sws 入口翻译像素格式"，**实际早已实现**：

- 实现：`bridge/npa_ffmpeg_util_bridge.c` 的 `npa_modern_pixfmt()`，在 `npa_sws_getContext` 与
  `npa_sws_getCachedContext` 内对 src 与 dst **双向都不回传**地翻译；三个被删成员与越界值一律
  `AV_PIX_FMT_NONE`（让 `sws_getContext` 失败而非静默误译）。
- 提交：`05423b2 fix: translate the legacy AVPixelFormat at the swscale boundary`（在 main）。
- 记录：`notes/ffmpeg-pixfmt-abi-mismatch.md`（含症状、逐值核对、重定向覆盖面复核、设备验收）。
- 设备验收：`--dylib ffmpeg` 下 P010/HEVC 缩略图正确且不崩溃；H.264/连续播放/HDR/音频正常。
- 本次独立复验：用两棵头树的探针数据穷举 275 个 legacy 名称，`npa_modern_pixfmt()` 的模型与
  头文件**零差异**（仅 `AV_PIX_FMT_VAAPI_VLD` 是同名改值别名，被模型正确映射到 `AV_PIX_FMT_VAAPI`）；
  demux 单元的生成表亦与头文件零分歧。

⇒ 无需新增代码。§9 的 R1 据此关闭。

---

## 5. P1：字幕 demux 面（概要）

目标站点聚集在 `0x100AB4xxx`（`media::FFmpegSubtitle`）：
`avformat_alloc_context 0x100ab4df4`、`avformat_open_input 0x100ab451c / 0x100ab4e1c`、
`avformat_find_stream_info 0x100ab4e2c`、`av_read_frame 0x100ab4934 / 0x100ab4fe0`、
`av_find_input_format 0x100ab4d74`、`av_probe_input_buffer 0x100ab4dec`、
`avio_alloc_context 0x100ab4dcc`、`avio_read 0x100ab4b00`、`avio_seek 0x100ab4b14`。

- 复用：`AVFormatContext`/`AVStream`/`AVCodecParameters`/`AVPacket` 影子与枚举翻译。
- 新增影子候选：**带缓冲的 `AVIOContext`**（与 demux 面的零缓冲不同，需确认 app 读哪些字段）、
  `AVInputFormat`（若 app 解引用其字段）。
- 解码仍归 4.4.8，故与本阶段同构、可测（字幕矩阵已验）。

## 6. P2：codec 对象层（已关闭 2026-09-27）

**结局**：拆成两个单元落地并通过设备验收——`ffmpeg-subdecode`（字幕解码；`dev/acceptance.json` 的
`ffmpeg-core902-subdecode` 条目）与 `ffmpeg-codec`（播放/probe 软解；同文件 `ffmpeg-codec` 条目，
两者都带整 dylib 回退行）。与原概要的偏离都是经裁定的：

- `av_frame_*`/`av_packet_*`/`av_dict_*`/`av_image_*`/`av_samples_*` **不迁移**：帧与包仍由 app 自持，
  shim 就地物化，让 app 自己的释放路径驱动引用计数（spec §2.5）；
- `AVBSFContext` 未纳入：app 的 BSF 站点都在 HLS/mux 会话侧，属 P3；
- `AVFrame` 影子的 `format` 定为 **legacy（4.4 编号）**，与 §4 的入口翻译一致（spec §3.3）；
- AV1 的收尾决定：闭包加 **libdav1d 1.5.4** 并 `--disable-decoder=av1`（9.0.2 原生 av1 解码器是硬解专用），
  下一条"`libdav1d 0.9.2` 不能用于 9.0.2"因此由升级 dav1d 解决。

以下为立项时的概要，保留作历史：

**门槛**：先建影子，再迁站点。需覆盖 `AVCodecContext`（app 读写字段多）、`AVCodec`、`AVFrame`、
`AVSubtitle`(+`AVSubtitleRect`)、`AVBSFContext`；并随之迁移 avutil 的对象分配 API
（`av_packet_*`、`av_frame_*`、`av_dict_*`、`av_image_*`、`av_samples_*` 等），因为它们定义布局。

- 首个使用者：**字幕解码**（`avcodec_decode_subtitle2 0x100a04d14`、`avsubtitle_free 0x100a05788`，
  以及 `0x100a03xxx` 的 `avcodec_find_decoder/alloc_context3/open2/free_context`）——冷路径、可测。
- 其次：**播放软解**（`0x100a81xxx`/`0x100a89xxx`）与 **BSF**（`av_bsf_* 0x100aef144/0x100aef164/…`、
  `0x100b9593c/0x100b95954`）。
- **开放设计问题（P2 必须先定）**：`AVFrame` 影子里的 `format` 按哪种编码写？
  - 若按 legacy：§4 的 sws 入口翻译必须保留（app 的 4.4.x 编号表 `0x1016A58B8` 支持这一选择）。
  - 若按 modern：需要先证明 app 不拿该值做索引/比较（目前未证），否则会打乱 app 自己的表。
- 注意：`libdav1d 0.9.2` 无法用于 9.0.2（要求 ≥1.0.0）；AV1 将走 9.0.2 原生解码器
  （或届时升级 dav1d），需在 P2 前定。

## 7. P3：输出侧（两种形态，默认产物不变）

输出侧影子：`AVFormatContext`(out)/`AVStream`/`AVOutputFormat`/encoder `AVCodecContext`/
`AVIOContext`(out)；站点在 `0x100B30xxx`（SPDIF）、`0x100B95-9Axxx`（MediaServer HLS session / mux）
与封面编码（`0x100A469FC`）。

P3 有两种可选形态，由 manifest 的 `conflicts` 隔离，**默认产物不变**：

1. **同世代单元（本分支 `feat/ffmpeg-output448`，已落地）**：一个 4.4.8 dylib
   `LibFFmpegOut448Bridge.dylib` 承载三域 `ffmpeg-hls448`/`ffmpeg-spdif448`/`ffmpeg-mjpeg448`
   （154 站点 / 90 入口），可与 9.0.2 输入侧四域（`libass + ffmpeg-core902`）共选，余下仍是
   app 自带 4.4.5。输出侧**不下沉到 shim**：asm 尾分支直跳 4.4.8 闭包，ABI 由编译期守卫锁定。
   与 `ffmpeg-core`/`ffmpeg-full` 互斥（它们带 4.4.5 核，符号相同）。证据见
   `dev/acceptance.json` 的 `ffmpeg-out448` 条目。
2. **输出侧也 9.0.2（探索分支 `feat/ffmpeg-demux-class-a` 的 `ffmpeg-full`）**：整核替换路线的后续，
   需要为输出侧写影子翻译；本分支不采用。

环境限制（两形态共有）：

- 录制/HLS 会话需要 AirPlay/cast 的前置状态，本环境**不可驱动**：app 自身的断言发生在
  `MediaServer::CreateHLSSession` 之前，故 `ffmpeg-hls448` 只能留站点/构建级证据。
- SPDIF 面需要 **HDMI 路由**才会被 app 主动启用（`-[nPlayerView setSPDIFOutput:]` 与路由相与），
  本环境用"在音频解码器工厂入口（`sub_100B911F8`）置位 `player+0x88`"的进程内驱动取到行为证据
  （见 `nPlayerFridaHook/drive-spdif-flag.py` 与验收条目）。
- 封面（MJPEG）面由用户在信息面板/海报行验证。

## 8. 不变量与统一验收判据

- 4.4.8（当前 `ffmpeg-core`）在全部阶段保持可出货；9.0.2 为实验面。
- 每阶段必须：有本环境可测验收行；不改动已验面行为；失败可整单元回退。
- 每阶段的设备证据按 sha 记入 `dev/acceptance.json`，**新增**条目而非改写历史。
- 反工程发现按 `AGENTS.md` 记入笔记（地址/符号/角色/证据，不放大段反编译）。
- 临时诊断在验收前移除；不为一次性验证保留测试。

## 9. 风险登记

| 编号 | 风险 | 状态 | 处置 |
|---|---|---|---|
| R1 | 4.4.x `AVPixelFormat` 进入 9.0.2 sws | **已关闭** | `npa_modern_pixfmt()` 已翻译并设备验收（§4）；本次独立复验零差异 |
| R2 | 9.0.2 删改 API 签名（如 `swr_alloc_set_opts`） | 已知且有先例 | 延续"桥接内翻译"模式 |
| R3 | 单元合并使回退粒度变粗 | 已接受 | README/验收条目写明 |
| R4 | P2 的 `AVFrame`/`AVCodecContext` 影子在热路径 | 未评估 | P2 立项时先做成本评估 |
| R5 | 9.0.2 无法使用 dav1d 0.9.2 | 已知 | P2 前决定：原生 AV1 解码器或升级 dav1d |
| R6 | class-A 未驱动项：`[Reference]` 分支 | 未驱动（非能力缺口） | 有可测条件时补 |
| R7 | 单元自身的 fallback 证明 | 有意推迟 | 后续补 |
