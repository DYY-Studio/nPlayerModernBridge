# ffmpeg-out448 单元：把 mux / 编码 / HLS / SPDIF 接到 4.4.8

- 日期：2026-09-27
- 分支：`feat/ffmpeg-output448`（基线 `feat/ffmpeg-demux-class-a` @ `4642f09`）
- 目标产物：`libass + ffmpeg-core902 + ffmpeg-out448`（默认产物不变）
- 状态：设计已与用户逐节确认并冻结；本文即规格（plan is spec 的上一段，实施计划另出）

## 0. 一句话

把 app 的**输出侧**（HLS 转封装/转码、SPDIF/IEC 61937 封装、封面 MJPEG 编码）从"留在 app 自带 4.4.5"
改为接到 **4.4.8**，做成一个**零翻译、零影子**的独立单元（三个域），与已设备验收的 9.0.2 输入侧组合。
收益取同世代（4.4.5→4.4.8）的修复；代价与风险远低于把这几个面搬到 9.0.2。

## 1. 背景与实测证据

以下数字全部由 manifest 实测得出，不沿用记忆或旧记录。

1. **9.0.2 五域在输出侧只认领 2 个站点**：`avcodec_free_context` @`0x100B300F0`（SPDIF 面内）与
   @`0x100B63F88`（主播放音频路径里对共享 helper `sub_100A8A4F8` 临时 ctx 的释放，见 §3.3）。
   它们属于 P2-B"临时源 ctx 生命周期"的一部分，而该 ctx 由
   9.0.2 shim 分配，**只能由 9.0.2 释放** ⇒ 这两个站点必须继续留给 `ffmpeg-codec`，本单元剔除。
2. **今天这些调用全部落在 app 自带的 4.4.5 上**；而 4.4.8 全核（`ffmpeg-core` 域，99 API / 424 站点）
   已经覆盖输出侧函数的全部候选站点 —— 也就是说"同样的接线"已在 main 的 4.4.8 路线上存在并被验收，
   本设计只是把它**拆成一个可独立选择、独立回落的小单元**。
3. **收益对比**（路线图 §0）：P3 的收益是三条里最弱的；把输出侧搬到 9.0.2 反而会**新增**输出侧结构体影子
   （`AVFormatContext`(out) / `AVStream` / `AVOutputFormat` / encoder `AVCodecContext` / 输出 `AVIOContext`
   + `AVBSFContext`），与"减少混版本面"的目标相反。⇒ 选择同世代 4.4.8，**不做任何影子**。
4. **入口机制**（决定工作量）：4.4.8 单元的入口点不写 shim 代码，由汇编 tail-branch 生成
   （`npa_<sym>` → `b _<sym>`），无参数/结构体搬运、无手写原型；跨版本相容性由
   `bridge/ffmpeg-core-abi.h` 在**编译期**断言（与 main 的 `ffmpeg-core` / `ffmpeg-full` 同一个头）。
5. **abi 相容性不是新假设**：main 的 4.4.8 全核运行在同一假设上且已验收；本单元让它以更小的粒度成立。

## 2. 覆盖范围

| 面 | app 主体（地址，尺寸为 IDA 函数大小） | 用户动作 | 单元（域） |
|---|---|---|---|
| HLS 转封装/转码 | 会话 open/决策 `sub_100B94DA0`(0x1B30)、mux/encode 主体 `sub_100B98A38`(0x1680)、AAC 编码器 `sub_100B9A294`(0xF0)、fMP4 init 段 `sub_100B9A534`(0x46C)、会话骨架与清理 `sub_100B944C8` / `sub_100B93A18` / `0x100B96Cxx` | 投屏（Chromecast / AirPlay）播**本地非 mp4** 文件 | `ffmpeg-hls448` |
| SPDIF / IEC 61937 | `sub_100B30054` / `sub_100B3010C`(0x240) / `sub_100B303C0` / `sub_100B30430` / `sub_100B30684` | 开数字音频透传（S/PDIF / HDMI）+ 播 AC3 / DTS 类源 | `ffmpeg-spdif448` |
| 封面 MJPEG 编码 | `sub_100A469FC`(+0x1AC)，调用方 `sub_100A3E1F8`（3 处）/ `sub_100A421B4` 链 | 影片信息面板 / 海报显示 | `ffmpeg-mjpeg448` |

**明确不覆盖**

- 主播放路径的**输入侧**（demux / 解码 / sws / swr）：已由 9.0.2 五域负责；
- `0x100B63xxx`–`0x100B64xxx` 的 16 个站点（`media::ios::AudioToolboxDecoder`，**主播放音频路径**）：
  经 Task 1 核对后移出，见 §3.3；
- 未列入上表的一切 FFmpeg 调用：仍为 app 自带 4.4.5；
- `0x100A469C0` / `0x100A469E0` / `0x100A469E8` 等落在 **MJPEG 函数之外**的站点：属 9.0.2 的 probe/poster 面；
- 没有"转码/导出成另一种格式"的对外功能（`doExport` 只导出原文件，零 FFmpeg 调用）。

## 3. 单元边界与站点集

### 3.1 规则（同时是守卫测试的断言）

1. 站点按**所属函数**整块取，不按 API 语义切 ⇒ HLS 内部的解复用/解码也进本单元；同一函数内不出现两个世代。
2. 一个站点只归一个单元。
3. 与 9.0.2 五域**地址级零重叠**。守卫测试**只比地址**，不比符号：9.0.2 侧符号是
   `npa_codec_avcodec_free_context`、4.4.8 侧是 `npa_avcodec_free_context`，按 `(地址, 符号)` 比较会漏掉冲突
   （本轮做站点盘点时实测踩到，故写死为规则）。
4. 与 `ffmpeg-core` / `ffmpeg-full` 的**故意重叠**由对称 `conflicts` 正当化（工具已强制对称）。
5. 每个 API 的 `old_target` 必须与 `ffmpeg-core` 域同名 API 的 `old_target` 一致（同一条 app 调用点、
   同一个 4.4.5 目标地址）；守卫测试逐符号断言。
6. 守卫测试落在 `tests/test_out448_manifest.py`（结构照 `tests/test_codec_manifest.py`）：逐符号站点、
   并集恰为 **154**、与 9.0.2 五域地址级零不相交、`old_target` 一致性、以及 exclusion 断言
   （`†` 站点、`0x100A469C0/E0/E8`、以及 `0x100B63xxx`–`0x100B64xxx` 的 16 个必须**不在**集合内）。

### 3.2 冻结后的候选与认领（Task 1 核对结果）

| 域 | 候选站点 | 剔除 | 认领 |
|---|---|---|---|
| `ffmpeg-hls448`（含会话清理 `sub_100B96C48` 的 12 个） | 130 | 0 | **130** |
| `ffmpeg-spdif448`（`media::SPDIF` 的 mux 面） | 12 | 1 | **11** |
| `ffmpeg-mjpeg448` | 13 | 0 | **13** |
| 合计 | 155 | 1 | **154** |

剔除项（留给 `ffmpeg-codec`）：`avcodec_free_context` @`0x100B300F0`（SPDIF 面内，释放的是 9.0.2 shim
拥有的临时源 ctx）。去重后入口 **67** 个，无任何非公开符号。
`ffmpeg-mjpeg448` 的 13 个与既有记录"`0x100A469FC` 的 13 站点"一致（交叉校验通过）。

### 3.3 Task 1 的归属核对（已完成 2026-09-27；证据见 `notes/ida-investigation-out448-sites.md`）

**结论一：所谓"音频/SPDIF 辅助层"的 16 个站点不属于 SPDIF，而属于主播放音频路径。** 它们只落在四个函数里，
四个函数同属 `media::ios::AudioToolboxDecoder`（RTTI `N5media3ios19AudioToolboxDecoderE`，ctor
`sub_100B63DB8`、vtable `off_1016C5D90`）：`sub_100B63E48`(析构，含 @`0x100B63E5C`)、
`sub_100B63EDC`(含 @`0x100B63F88`)、`sub_100B63FA4`(含 6 个)、`sub_100B64228`(含 8 个，其中包括
`avpriv_mpegaudio_decode_header`)。主播放解码工厂 `sub_100B911F8` 的逻辑是"SPDIF 使能 → `sub_100B8A664`
（SPDIF 支路）；否则 → `new AudioToolboxDecoder`" ⇒ **SPDIF 关闭（默认主播放）时走的就是这一层**。
⇒ 16 个站点**全部移出本单元**（§3.1 规则 2），其中 @`0x100B63F88` 释放的是共享 helper `sub_100A8A4F8`
拥有的临时源 ctx，继续留给 `ffmpeg-codec`。真正的 SPDIF 站点在 `media::SPDIF`（RTTI `N5media5SPDIFE`，
vtable `off_1016C4200`），即 §3.2 表里的 12 个（剔除 1 个后 11 个）。

**结论二：HLS 内的解复用与解码站点确在 `sub_100B94DA0` / `sub_100B98A38` 内**（`find_decoder` 0x100B96078、
`alloc_context3` 0x100B96088/0x100B95D4C、`open2` 0x100B960A4、`close` 0x100B965A0/0x100B98CD8、
`send_packet` 0x100B99334、`receive_frame` 0x100B9934C/0x100B995F4、`flush_buffers` 0x100B98CC8），
与主播放解码器无关 ⇒ 按 §3.1 规则 1 整体留在 `ffmpeg-hls448`。函数边界：`sub_100B94DA0` 结束于
`0x100B968D0`，`sub_100B98A38` 结束于 `0x100B9A0B8`，`sub_100B96C48`(0x130) 是会话清理、
`0x100B96C70`–`0x100B96D40` 的 12 个站点全部落在它里面（无跨函数）。

**结论三：MJPEG 的 13 个站点确在 `sub_100A469FC`(0x1AC) 内。**
- 产出：三张**冻结**站点表（本节附录即候选表，核对后原地更新并注明核对依据）。

## 4. 构建与导出

| 项 | 值 |
|---|---|
| dylib id / 文件 | `ffmpeg-out448` / `LibFFmpegOut448Bridge.dylib` |
| `library_version` | `4.4.8` |
| `build.source` | 新建 `bridge/npa_ffmpeg_out448_bridge.c`：`#include "ffmpeg-core-abi.h"` + 新 forwards 头 + `NPA_FORWARD` 宏 + `NPA_OUT448_FORWARDS(NPA_FORWARD)` |
| `build.exports` | 新建 `bridge/ffmpeg-out448.exports`：恰好列出 `_npa_*` 共 **67** 个 |
| 新增 forwards 头 | `bridge/npa_ffmpeg_out448_forwards.h`：67 个入口；三个域**共用**同一批符号（解析按 `(dylib, 符号)` 走，允许同名共享） |
| `build.closure` | **复用** `build/deps/ffmpeg-core-closure.txt`（现有 4.4.8 闭包）⇒ 无需 `make deps`，`make bridge` 即可 |
| include/lib root | 同 `ffmpeg-core` |

**本单元没有一行翻译代码**：入口点全是汇编 tail-branch，无手写原型、无参数/结构体搬运。
"4.4.5 编译出的 app 结构体与 4.4.8 闭包相容"由 `ffmpeg-core-abi.h` 在编译期断言，不靠口头保证。

**记档**：本单元**不含任何非公开符号**——原候选里的 `avpriv_mpegaudio_decode_header` 随
`AudioToolboxDecoder` 那一层一起移出（§3.3）。因此不存在"内部符号在 9.0.2 是否仍有"的问题。

**入口名与 4.4.8 全核同名**：本单元的 `npa_avformat_open_input` 等与 `ffmpeg-core` 域的入口点同名。
这是允许的——解析按 `(dylib, 符号)` 走，且两个 dylib 互斥；代价是评审时必须靠 `old_target` 与站点表区分，
故 §3.1 规则 5 把"`old_target` 与 `ffmpeg-core` 一致"写成硬性断言。

**校验**：`make bridge` 后 `make verify` 的 7 项检查（导出集精确匹配、install name、依赖仅系统框架、
无初始化器、无宿主路径等）必须全过。

## 5. 载荷、选择与冲突

| 项 | 设计 |
|---|---|
| `conflicts` | `ffmpeg-out448` ⇄ `ffmpeg-core`、`ffmpeg-full`（对称声明，工具强制）。理由：三者认领同一批输出侧站点。与 `libass`、`ffmpeg`、`ffmpeg-core902` 可共选 ⇒ 目标产物 `libass + ffmpeg-core902 + ffmpeg-out448`。 |
| `default_dylibs` | **不动**（仍 `libass + ffmpeg-full`）⇒ 新单元与 `ffmpeg-core902` 同为 opt-in。 |
| 载荷布局 | 三个新单元让 `__NPATCH_DATA` 变长 ⇒ 本选择需要**新的 `packaged_main_sha256` 锚点**，单元偏移 6 → 9 个。偏移一律用 `npabridge.payload.unit_offsets(manifest.units([...]))` 按当次选择算，不手推。 |
| 不扰动既有锚点 | 加 dylib **条目**不得改变其它选择的载荷：重打 `libass + ffmpeg-core902`，`packaged_main_sha256` 必须仍为 `5a33aa13b5c5c2455c9be9f5fb2857c1e36bbc95b8a3de61a206ed6d819b6ca1`；不等则先查清，不许静默换锚点。 |
| 弱加载与回落 | 选择里的每个 dylib 都弱加载 ⇒ 抽掉本 dylib 时三个单元各自读 `3 (OLD)`，其余单元不受影响。 |
| 验收记录 | 新增 variant `ffmpeg-out448`：产物锚点、dylib 哈希（as-built 与 IPA 内各一份，注明二者因重签名不同）、域与站点数、验证行、以及与 9.0.2 五域零重叠 / 与 4.4.8 全核故意重叠的对账结论。 |

## 6. 验证与驱动

### 6.1 HLS 面（`ffmpeg-hls448`）—— 进程内驱动（用户无法实测）

照 `nPlayerFridaHook/probe-subtitle-seek-drive.js` 的模式：开头对入口做**锚点字节校验**，不匹配即报错退出，
不静默继续。

1. 驱动序列按 app 自己的构造顺序：`-[HTTPServer createHLSSessionWithURL:options:]`（`0x100AA91D0`）→
   `MediaServer::CreateHLSSession`（`0x100B94360`）→ `HLSSessionImpl` 构造（`0x100B944C8`）→
   open/决策（`0x100B94DA0`）→ `-[HLSSession start]`（`0x100B93A18`）→ mux 线程体（`0x100B98A38`）。
   实施前先用 IDA 钉死这些入口的**签名与实例来源**（`MediaServer` / `HTTPServer` 是否单例、如何取得）。
2. **前置断言**：读 `canPassthru`（`impl+0x280`，`sub_100B94328`）必须为 `0`，否则会话根本不走 FFmpeg。
   素材选非 mp4 / 非 webm、非 DRM、非 HEVC tag 的 H.264+AAC MKV。
3. **判读三层**（弱 → 强）：单元状态字 = 2；`.hls/` 真实产出 playlist 与分片且非空；**用 ffprobe 解析第一个
   分片**确认是合法 mpegts/fMP4 ⇒ "mux 输出正确"的行为证据。
4. 收尾：让会话正常结束（线程收尾 + 清理），**不 kill 进程**。

### 6.2 SPDIF 面（`ffmpeg-spdif448`）—— 进程内驱动（用户无法实测）

1. 通过 app 自己的配置打开 `MediaPlayerConfig.SPDIFOutput`，播 AC3（与 DTS）源 ⇒ 音频解码器工厂
   `sub_100B8A664` 才会造 SPDIF 对象。
2. **字节级断言**：hook 写回调 `sub_100B30390`（app 冷函数）取实际输出，校验 IEC 61937 帧头
   `0xF872` / `0x4E1F` 与 data-type（AC3 = 1、DTS = 11）——这正是 4.4.8 `spdifenc.c` 应产出的字节。
3. 判读：单元状态字 = 2、帧头断言通过、无崩溃、音频路径行为与今天一致。**不需要真实透传接收端。**

### 6.3 封面面（`ffmpeg-mjpeg448`）

- 用户实测行：影片信息面板 / 海报显示（覆盖 MJPEG 编码路径）。
- 可选补充：我这边驱动 `sub_100A3E1F8` 的 metadata 链，并做回退检查。

### 6.4 三面共同的仪器

- 零 hook 状态字读数（三个新单元；用 `nPlayerFridaHook/state-words.py`，段 rva 与偏移按当次选择重算）；
- hook **自家冷导出**做调用计数（"真的被调用"证据，强于 liveness）；
- **整 dylib 回退 A/B**：抽掉 `LibFFmpegOut448Bridge.dylib` ⇒ 三个单元读 `3 (OLD)`、其余 6 个不受影响、
  行为等同自带 4.4.5；还原后回 `2`；
- `make bridge` / `make verify`（导出集恰好 67 等 7 项）/ `pytest` 全绿（含新守卫测试）；
- **P0–P2 的行在新选择下重跑不得改变**（软解 H.264/HEVC/HEVC Main10、音频、字幕、AV1 缩略图+软硬解）；
- §5 的"既有锚点不扰动"检查。

### 6.5 顺序与停点

1. Task 1：IDA 归属核对（只读）→ 冻结三张站点表；同时钉死 HLS/SPDIF 驱动的入口签名。
2. Task 2：manifest 三个域 + 守卫测试（含地址级零重叠断言、exclusion 断言）。
3. Task 3：bridge `.c` + forwards + exports；`make bridge && make verify`。
4. Task 4：进程内驱动（HLS 先、SPDIF 后）+ 字节级断言（我出结果）。
5. Task 5：打产物 → **用户安装并跑封面/信息面板行**；我读状态字与回退 A/B。
6. Task 6：写 `dev/acceptance.json` 条目 + 更新路线图 §7（P3 的最终形态）。

## 7. 风险与不变量

**R1（最关键）跨边界对象 = 零，且有机械证明。** 四个面自封闭：HLS 会话用自己的
`AVFormatContext`/`AVStream`/解码与 AAC 编码 ctx/`AVBSFContext`/输出 `AVIOContext`（全在 `sub_100B94DA0`
与 `sub_100B98A38` 内建内消）；SPDIF 用自己的 output ctx/stream/IO；MJPEG 用自己的 encoder ctx/frame/packet。
与 9.0.2 五域的唯一接触点是 SPDIF 消费主音频路径的解码结果——那已是 P2-B 物化到 **app 自有 legacy
`AVFrame`** 的对象，不是 9.0.2 内部对象。机械证明 = **站点地址级零重叠**：同一批站点不可能同时走两个世代。
已核对：原先的例外项（16 个辅助层站点）属主播放音频路径，已按 §3.3 移出。

**R2 "4.4.5 编译的 app ↔ 4.4.8 闭包"相容性**：非新假设（main 的 4.4.8 全核同假设已验收），
且由 `ffmpeg-core-abi.h` 编译期断言。

**R3 驱动纪律**：进程内驱动会造真实会话（起线程、写 `.hls/`、打开 SPDIF 输出）⇒ 必须能正常收尾；
只在**本地已安装 app** 上跑，绝不在用户设备上做进程内驱动；全程不 `pkill`
（笔记 `notes/playcover-debug-path.md` §6/§8 已两次栽过）。

**R4 非公开符号**：本单元不含任何 `avpriv_*` / 非公开符号，见 §4 记档。

**R5 已知盲区（不属本单元范围，不阻塞）**：`canPassthru` 稠密位图语义
（驱动用 §6.1 的实测断言绕过）、SPDIF 位图里另 2 个 codec_id、`fileType == 5` 枚举名、
`decoder` 4/5/6 符号名、HLS 分片时长 / 是否 SAMPLE-AES / `.hls` 清理策略、`GoogleCastPlayerViewV2`
（在 SDK 内）是否另有 mux 路径。本单元只把这些调用接到 4.4.8，语义不变。

**R6 明确不做（防过度工程）**：不做输出侧结构体影子（同世代不需要）；不做这三个面的 9.0.2 版
（那是探索分支的活）；不挪用/拆分 `ffmpeg-core`/`ffmpeg-full` 的站点；不改闭包、不引新依赖；
不把新单元放进默认产物。

### 不变量清单（实施与评审的依据）

1. 站点只从 `ffmpeg-core` 域的现成站点里取，不新增站点。
2. 一个站点只归一个单元。
3. 与 9.0.2 五域**地址级零重叠**（守卫测试只比地址）。
4. 与两个 4.4.8 全核**故意重叠**，由对称 `conflicts` 正当化。
5. **零翻译、零影子**：入口点只有 tail-branch。
6. 既有选择的 `packaged_main_sha256` 不变。
7. 每个单元独立状态字、独立回落；默认产物不变。

## 8. 与其它路线/分支的关系

- **本分支**：4.4.8 输出侧单元，可验证、准备进主线（形态 = 新 dylib + 三个域 + 守卫测试 + 验收条目）。
- **探索分支** `feat/ffmpeg-demux-class-a`：原样保留，继续探索"输出侧也搬到 9.0.2"的完整替换。
- **main 的 4.4.8 全核**（`ffmpeg-core` / `ffmpeg-full`）：不受影响，也不被挪用站点。
- 路线图 `2026-09-27-expand-ffmpeg902-wiring.md` §7 在 Task 6 更新为两种可选形态：**同世代单元（本设计）**
  与**输出侧也 9.0.2（探索分支）**。

## 附：站点表（经 Task 1 归属核对后冻结；按 API 分组）

- 候选（本单元三个面的函数区间内）：**155**；剔除（留 `ffmpeg-codec`）：**1**；本单元认领：**154**（`ffmpeg-hls448` 130 + `ffmpeg-spdif448` 11 + `ffmpeg-mjpeg448` 13）
- 去重后入口（API）：**67**
- **不在本表内**：`0x100B63xxx`–`0x100B64xxx` 的 16 个站点 —— 经核对属主播放音频路径
  （`media::ios::AudioToolboxDecoder`），整体不进本单元，见 §3.3 与
  `notes/ida-investigation-out448-sites.md`。

| API | 域 | 站点（`†` = 剔留给 `ffmpeg-codec`） |
|---|---|---|
| `npa_av_bsf_alloc` | | `ffmpeg-hls448`: `0x100B9593C` |
| `npa_av_bsf_free` | | `ffmpeg-hls448`: `0x100B96CB8` |
| `npa_av_bsf_get_by_name` | | `ffmpeg-hls448`: `0x100B95930` |
| `npa_av_bsf_init` | | `ffmpeg-hls448`: `0x100B95954` |
| `npa_av_bsf_receive_packet` | | `ffmpeg-hls448`: `0x100B999E0` |
| `npa_av_bsf_send_packet` | | `ffmpeg-hls448`: `0x100B999CC` |
| `npa_av_codec_get_tag` | | `ffmpeg-hls448`: `0x100B99078` |
| `npa_av_dict_copy` | | `ffmpeg-hls448`: `0x100B98F2C`, `0x100B99094` |
| `npa_av_dict_free` | | `ffmpeg-hls448`: `0x100B95114`, `0x100B99140` |
| `npa_av_dict_get` | | `ffmpeg-hls448`: `0x100B95F94`, `0x100B95FE0` |
| `npa_av_dict_set` | | `ffmpeg-hls448`: `0x100B950A4`, `0x100B950C0`, `0x100B99100`, `0x100B9911C` |
| `npa_av_frame_alloc` | | `ffmpeg-hls448`: `0x100B960B8`, `0x100B960E4`<br>`ffmpeg-mjpeg448`: `0x100A46A3C` |
| `npa_av_frame_free` | | `ffmpeg-hls448`: `0x100B96D38`, `0x100B96D40`<br>`ffmpeg-mjpeg448`: `0x100A46B88` |
| `npa_av_freep` | | `ffmpeg-hls448`: `0x100B96C90`, `0x100B96D08`<br>`ffmpeg-mjpeg448`: `0x100A46B80`<br>`ffmpeg-spdif448`: `0x100B303FC` |
| `npa_av_get_default_channel_layout` | | `ffmpeg-hls448`: `0x100B9A330` |
| `npa_av_guess_format` | | `ffmpeg-hls448`: `0x100B98EC8` |
| `npa_av_image_alloc` | | `ffmpeg-mjpeg448`: `0x100A46A6C` |
| `npa_av_init_packet` | | `ffmpeg-hls448`: `0x100B954EC`, `0x100B99290`, `0x100B99590`, `0x100B99968`, `0x100B99B48`, `0x100B99D68`<br>`ffmpeg-mjpeg448`: `0x100A46B10`<br>`ffmpeg-spdif448`: `0x100B30458` |
| `npa_av_malloc` | | `ffmpeg-hls448`: `0x100B958D4`<br>`ffmpeg-spdif448`: `0x100B301C4` |
| `npa_av_new_packet` | | `ffmpeg-hls448`: `0x100B99B5C`, `0x100B99D7C` |
| `npa_av_opt_set` | | `ffmpeg-hls448`: `0x100B99F94`, `0x100B99FCC`, `0x100B99FE8` |
| `npa_av_packet_copy_props` | | `ffmpeg-hls448`: `0x100B99B68`, `0x100B99D88` |
| `npa_av_packet_move_ref` | | `ffmpeg-hls448`: `0x100B99B8C`, `0x100B99DAC` |
| `npa_av_packet_ref` | | `ffmpeg-hls448`: `0x100B99974`<br>`ffmpeg-mjpeg448`: `0x100A46B48` |
| `npa_av_packet_rescale_ts` | | `ffmpeg-hls448`: `0x100B99BC0`, `0x100B99EA0` |
| `npa_av_packet_unref` | | `ffmpeg-hls448`: `0x100B958C0`, `0x100B99358`, `0x100B999D4`, `0x100B99B80`, `0x100B99C00`, `0x100B99C08`, `0x100B99DA0`, `0x100B99EB4`<br>`ffmpeg-mjpeg448`: `0x100A46B50` |
| `npa_av_read_frame` | | `ffmpeg-hls448`: `0x100B954FC`, `0x100B9929C` |
| `npa_av_rescale_q` | | `ffmpeg-hls448`: `0x100B99408` |
| `npa_av_samples_get_buffer_size` | | `ffmpeg-hls448`: `0x100B99428`, `0x100B99548` |
| `npa_av_seek_frame` | | `ffmpeg-hls448`: `0x100B98C54`, `0x100B98CA4`, `0x100B98CBC` |
| `npa_av_write_frame` | | `ffmpeg-hls448`: `0x100B99BF8`, `0x100B99EAC`, `0x100B9A65C`, `0x100B9A700`, `0x100B9A7D8`<br>`ffmpeg-spdif448`: `0x100B304B0` |
| `npa_av_write_trailer` | | `ffmpeg-hls448`: `0x100B98E84`<br>`ffmpeg-spdif448`: `0x100B303DC` |
| `npa_avcodec_alloc_context3` | | `ffmpeg-hls448`: `0x100B95D4C`, `0x100B96088`, `0x100B9A2BC`<br>`ffmpeg-mjpeg448`: `0x100A46ACC` |
| `npa_avcodec_close` | | `ffmpeg-hls448`: `0x100B965A0`, `0x100B96D10`, `0x100B96D28`, `0x100B98CD8` |
| `npa_avcodec_fill_audio_frame` | | `ffmpeg-hls448`: `0x100B99568` |
| `npa_avcodec_find_decoder` | | `ffmpeg-hls448`: `0x100B96078` |
| `npa_avcodec_find_encoder` | | `ffmpeg-hls448`: `0x100B9A2B0`<br>`ffmpeg-mjpeg448`: `0x100A46AC0` |
| `npa_avcodec_flush_buffers` | | `ffmpeg-hls448`: `0x100B98CC8` |
| `npa_avcodec_free_context` | | `ffmpeg-hls448`: `0x100B95DC4`, `0x100B96D18`, `0x100B96D30`, `0x100B98CE0`, `0x100B9A36C`<br>`ffmpeg-mjpeg448`: `0x100A46B70`<br>`ffmpeg-spdif448`: `0x100B300F0`† |
| `npa_avcodec_open2` | | `ffmpeg-hls448`: `0x100B960A4`, `0x100B9A358`<br>`ffmpeg-mjpeg448`: `0x100A46B04` |
| `npa_avcodec_parameters_copy` | | `ffmpeg-hls448`: `0x100B9594C`, `0x100B98FCC` |
| `npa_avcodec_parameters_from_context` | | `ffmpeg-hls448`: `0x100B98FB0` |
| `npa_avcodec_parameters_to_context` | | `ffmpeg-hls448`: `0x100B95D58`, `0x100B96094` |
| `npa_avcodec_receive_frame` | | `ffmpeg-hls448`: `0x100B9934C`, `0x100B995F4` |
| `npa_avcodec_receive_packet` | | `ffmpeg-hls448`: `0x100B995A8`<br>`ffmpeg-mjpeg448`: `0x100A46B30` |
| `npa_avcodec_send_frame` | | `ffmpeg-hls448`: `0x100B99578`<br>`ffmpeg-mjpeg448`: `0x100A46B24` |
| `npa_avcodec_send_packet` | | `ffmpeg-hls448`: `0x100B99334` |
| `npa_avformat_alloc_context` | | `ffmpeg-hls448`: `0x100B95010` |
| `npa_avformat_alloc_output_context2` | | `ffmpeg-hls448`: `0x100B98EE4`<br>`ffmpeg-spdif448`: `0x100B30174` |
| `npa_avformat_close_input` | | `ffmpeg-hls448`: `0x100B96C70` |
| `npa_avformat_find_stream_info` | | `ffmpeg-hls448`: `0x100B95130` |
| `npa_avformat_free_context` | | `ffmpeg-hls448`: `0x100B96CE4`, `0x100B98E8C`<br>`ffmpeg-spdif448`: `0x100B30404` |
| `npa_avformat_init_output` | | `ffmpeg-hls448`: `0x100B99128` |
| `npa_avformat_new_stream` | | `ffmpeg-hls448`: `0x100B98F6C`<br>`ffmpeg-spdif448`: `0x100B30180` |
| `npa_avformat_open_input` | | `ffmpeg-hls448`: `0x100B950F8` |
| `npa_avformat_write_header` | | `ffmpeg-hls448`: `0x100B99150`<br>`ffmpeg-spdif448`: `0x100B30248` |
| `npa_avio_alloc_context` | | `ffmpeg-hls448`: `0x100B95008`<br>`ffmpeg-spdif448`: `0x100B30234` |
| `npa_avio_close` | | `ffmpeg-hls448`: `0x100B9A69C`, `0x100B9A818` |
| `npa_avio_close_dyn_buf` | | `ffmpeg-hls448`: `0x100B9A67C`, `0x100B9A7F8` |
| `npa_avio_closep` | | `ffmpeg-hls448`: `0x100B96CDC`, `0x100B9A70C` |
| `npa_avio_flush` | | `ffmpeg-hls448`: `0x100B99138`, `0x100B9A668`, `0x100B9A7E4`<br>`ffmpeg-spdif448`: `0x100B304F8` |
| `npa_avio_open2` | | `ffmpeg-hls448`: `0x100B9924C`, `0x100B9A648`, `0x100B9A768` |
| `npa_avio_open_dyn_buf` | | `ffmpeg-hls448`: `0x100B990DC`, `0x100B991B4`, `0x100B9A6A8`, `0x100B9A824` |
| `npa_avio_size` | | `ffmpeg-hls448`: `0x100B98C28` |
| `npa_avio_wb32` | | `ffmpeg-hls448`: `0x100B9A780`, `0x100B9A7AC` |
| `npa_avio_wl32` | | `ffmpeg-hls448`: `0x100B9A790`, `0x100B9A7A0`, `0x100B9A7BC`, `0x100B9A7CC` |
| `npa_avio_write` | | `ffmpeg-hls448`: `0x100B9A68C`, `0x100B9A808` |
