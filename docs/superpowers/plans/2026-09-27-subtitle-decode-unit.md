# 字幕解码单元（P2-A）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 app 的字幕解码路径（`avcodec_*` 上下文对象图 + `AVSubtitle` 对象图）接到 9.0.2 核心，成为
`ffmpeg-core902` 的第 4 个可独立回落的单元 `ffmpeg-subdecode`。

**Architecture:** 复用 demux 单元已验证的影子机制——shim 拥有 9.0.2 的真实对象，app 只见 legacy 形状的
影子与 legacy 归属的缓冲；单元 = 一个 domain + 一个状态字 + 一组冻结站点，失败整单元回落。app 侧永远不出现
9.0.2 布局的对象，因此本单元与已 NEW 的 demux 单元之间靠 legacy 布局互换对象。

**Tech Stack:** C99（桥 dylib，arm64/iOS13）、Python ≥3.11 + uv（补丁工具链与测试）、LIEF、9.0.2 静态闭包
（`deps/ffmpeg-core902.lock.json` 已就绪）、PlayCover（本机设备验收）。

**Spec:** `docs/superpowers/specs/2026-09-27-p2-codec-object-layer-design.md`

## Global Constraints

- 站点**只从 `ffmpeg-core` 域现有站点里挑**；**不得从 `ffmpeg-core` 域移走任何站点**（默认产物载荷与
  `a5243f0a…`/`e84ef5b5…` 一类锚点必须不变）。
- 一个站点在整个 `ffmpeg-core902` dylib 内**只能被认领一次**：`ffmpeg-demux` 已占的 12 个站点（表见
  Task 1）不得重复认领；`tests/test_manifest.py` 的通用守卫必须继续通过。
- app 侧只允许 legacy 布局：`AVCodecContext` `sizeof=0x438`、`AVSubtitle` `0x20`、`AVSubtitleRect` `0xC8`、
  `AVCodecParameters` `0x90`、`AVFrame` `0x218`、`AVPacket` `0x58`。
- 格式值一律按 **4.4 编号**写（`pix_fmt`/`sample_fmt`）；**不改** swscale 入口既有翻译。
- 不引入新依赖、不新增第三方库；闭包与锁定文件不变。
- **不静默**：shim 未实现的入口必须显式失败（`__builtin_trap()`），不得原样放行。
- 临时诊断在设备验收前移除；不为一次性验证保留测试。

## Review Focus

1. app **自行字节解析**的四类字幕（TEXT / MOV_TEXT / SAMI / SUBRIP）**不经过** `avcodec_decode_subtitle2`
   （`sub_100A04BCC` 内的 codec_id 分支）——这几类编码的行为必须与今天**逐字一致**。
2. `num_rects==0 && format==0`（图形字幕的"清空"）与 `end_display_time == -1` 哨兵：影子必须让 app 走到
   同一分支（`0x100A05748` / `0x100A04D58`）。
3. 位图字幕（`rect->type==1`，PAL8、`nb_colors>=1` 触发调色板路径）：影子的 `data[0]/linesize[0]/w/h/
   nb_colors` 必须能被 app 的 4.4.5 `av_image_copy` 正确消费。
4. app 用 4.4.5 的 `av_freep`/`av_packet_unref` 释放**它自己**的内存：shim 绝不能把 9.0.2 自己分配的
   地址交给这些路径（影子缓冲一律 legacy 归属）。
5. 已 NEW 的 demux 单元产出的 legacy `AVCodecParameters` 流入本单元的 `parameters_to_context`：必须按
   legacy 布局读取（外部字幕文件的 `st->codecpar` 就是这条）。

---

## Task 1: `ffmpeg-subdecode` 域与站点表

**Files:**
- Modify: `manifests/nplayer-3.13.0.json`
- Create: `tests/test_subdec_manifest.py`

**Interfaces:**
- Consumes: 无（纯数据）。
- Produces: domain `ffmpeg-subdecode`（10 API / 16 站点），挂在 `ffmpeg-core902` 的 `domains` 末尾；
  符号命名 `npa_subdec_<name>`，`old_target` 取 `ffmpeg-core` 域 `npa_<name>` 的 `old_target`。

**站点表（冻结）**

| 符号 | 站点 |
|---|---|
| `npa_subdec_avcodec_alloc_context3` | `0x100A03BE8` `0x100A03CF8` `0x100A03D68` `0x100AB4740` |
| `npa_subdec_avcodec_free_context` | `0x100A03B28` `0x100A03E38` `0x100AB4884` |
| `npa_subdec_avcodec_find_decoder` | `0x100A03D8C` |
| `npa_subdec_avcodec_open2` | `0x100A03DC8` |
| `npa_subdec_avcodec_parameters_alloc` | `0x100A03D98` |
| `npa_subdec_avcodec_parameters_free` | `0x100A03DB8` |
| `npa_subdec_avcodec_parameters_from_context` | `0x100A03DA4` |
| `npa_subdec_avcodec_parameters_to_context` | `0x100A03DB0` `0x100AB474C` |
| `npa_subdec_avcodec_decode_subtitle2` | `0x100A04D14` |
| `npa_subdec_avsubtitle_free` | `0x100A05788` |

**为什么只有这 16 个站点**（与 spec §2.5 的差异，已在实施前确认）：这 10 个符号是**唯一会产出/消费
shim 拥有对象**的入口（ctx 与 params 由 shim 分配，`AVSubtitle` 的内部缓冲由 shim 分配）。同一批函数内的
`av_packet_alloc/free/unref`（`0x100AB4908` `0x100AB4AC0` `0x100AB4AA8` `0x100AB4FCC` `0x100AB5150`
`0x100AB5148`）与 `av_dict_get`（`0x100AB4670` `0x100AB469C`）**保留在 4.4.5**：本单元不产出 packet/dict，
demux 单元交出的 legacy 归属 packet 与 opaque dict 已被 app 的 4.4.5 正常释放（P1 已设备验收），而 app 从不
解引用这些对象。`av_init_packet` 等 11 个策略保留站点同理（见 spec §2.5）。

`npa_subdec_avcodec_alloc_context3` 的 3 个站点在 `sub_100A03BC8`/`sub_100A03CB0` 内，第 4 个在外部字幕簇
`sub_100AB4580` 内；`free_context` 的 3 个站点与它们一一配对。**因此本单元的源 ctx 全部由被认领的 alloc
站点创建**，不存在 spec §7.2 的"legacy 分配的 ctx 流入被移入口"问题——那条前置只留给 P2-B（视频/音频的
`sub_100A8A4F8` 临时源 ctx）。

**闭包侧的既成事实（2026-09-27 用 `nm` 核过）**：9.0.2 闭包里有本单元需要的全部字幕解码器对象——
`_ff_ass_decoder`、`_ff_ssa_decoder`、`_ff_pgssub_decoder`、`_ff_dvdsub_decoder`、`_ff_dvbsub_decoder`、
`_ff_xsub_decoder`、`_ff_microdvd_decoder`、`_ff_webvtt_decoder`、`_ff_srt_decoder`、`_ff_text_decoder`、
`_ff_movtext_decoder`、`_ff_sami_decoder`，以及 `avcodec_decode_subtitle2`/`avsubtitle_free` 本身；
`libavcodec.a` 里也有对应的 `assdec.o` 等。**不需要改闭包或锁定文件。**

- [ ] **Step 1: 写守卫测试**

`tests/test_subdec_manifest.py` —— 照 `tests/test_demux_manifest.py` 的结构，断言：

```python
SUBDEC_FACE = { ... 上表 10 个符号 → 站点集合 ... }

def test_has_exactly_the_subdecode_symbols(self): ...   # 符号集合相等
def test_each_symbol_redirects_exactly_its_sites(self): ...  # 逐符号站点相等
def test_redirects_exactly_16_sites(self): ...          # 并集 == EXPECTED_SITES 且 len == 16
def test_no_site_is_already_claimed_in_this_dylib(self): # 与 ffmpeg-demux/libswscale/libswresample 无交集
def test_old_targets_match_the_ffmpeg_core_unit(self): ...   # 逐符号 old_target == core 同名 API
def test_excluded_functions_are_untouched(self): ...     # 编码器/编码路径站点不在集合内：
                                                         #   0x100A469FC 的 13 站点、avcodec_find_encoder/avcodec_send_frame/
                                                         #   avcodec_receive_packet/av_bsf_* 的全部站点
```

- [ ] **Step 2: 跑测试确认失败**

Run: `uv run pytest tests/test_subdec_manifest.py -q`
Expected: FAIL（`StopIteration`/`assertEqual` 找不到 `ffmpeg-subdecode`）

- [ ] **Step 3: 把域写进 manifest**

在 `manifests/nplayer-3.13.0.json` 的 `domains` 里新增 `ffmpeg-subdecode`（结构与 `ffmpeg-demux` 同形：
`symbol` + `call_sites` + `old_target`），并把它追加到 `dylibs[ffmpeg-core902].domains` 末尾。**不动
`ffmpeg-demux` 域、不动 `ffmpeg-core` 域的任何一条**。

- [ ] **Step 4: 跑测试确认通过**

Run: `uv run pytest tests/test_subdec_manifest.py tests/test_manifest.py tests/test_demux_manifest.py -q`
Expected: PASS（新守卫通过，且 `test_no_call_site_is_shared_between_units`、`test_ffmpeg_alternatives_conflict`
等通用守卫仍通过）

- [ ] **Step 5: Commit**

```bash
git add manifests/nplayer-3.13.0.json tests/test_subdec_manifest.py
git commit -m "feat: add the ffmpeg-subdecode unit to the manifest"
```

---

## Task 2: ABI 守卫头 `bridge/ffmpeg-subdec-abi.h`

**Files:**
- Create: `bridge/ffmpeg-subdec-abi.h`

**Interfaces:**
- Consumes: 闭包头 `libavcodec/avcodec.h`、`libavutil/frame.h`、`libavutil/packet.h`（`include_root`）。
- Produces: legacy 布局常量与镜像结构（`NPA_LEGACY_CTX_*` / `NPA_LEGACY_SUB_*` / `NPA_LEGACY_RECT_*` /
  `NPA_LEGACY_PARAMS_*`）+ `_Static_assert` 守卫，供 Task 3–5 的 shim 使用。

- [ ] **Step 1: 写头文件（只有常量与断言，无逻辑）**

按 spec §2.2/§2.3 落值，至少覆盖：`AVCodecContext` `sizeof=0x438` 与
`codec_type 0x0C / codec_id 0x18 / codec_tag 0x1C / bit_rate 0x38 / extradata 0x58 / extradata_size 0x60 /
time_base 0x64 / width 0x74 / height 0x78 / pix_fmt 0x88 / sample_aspect_ratio 0xE8 /
color_primaries 0x174 / color_trc 0x178 / colorspace 0x17C / sample_rate 0x190 / channels 0x194 /
sample_fmt 0x198 / block_align 0x1A4 / channel_layout 0x1B0 / bits_per_coded_sample 0x2F8 /
thread_count 0x310 / thread_type 0x314 / skip_loop_filter 0x33C / skip_idct 0x340 / skip_frame 0x344 /
subtitle_header 0x348 / subtitle_header_size 0x350 / pkt_timebase 0x374 / hw_device_ctx 0x408 /
hw_frames_ctx 0x3F0 / get_format 0x98`；
`AVSubtitle` `sizeof=0x20`（`format 0x00 start_display_time 0x04 end_display_time 0x08 num_rects 0x0C
rects 0x10 pts 0x18`）；`AVSubtitleRect` `sizeof=0xC8`（`w 0x08 h 0x0C nb_colors 0x10 pict 0x18
data 0x78 linesize 0x98 type 0xA8 text 0xB0 ass 0xB8 flags 0xC0`）；`AVCodecParameters` `sizeof=0x90`
（`codec_type 0x00 codec_id 0x04 codec_tag 0x08 extradata 0x10 extradata_size 0x18 format 0x1C
bit_rate 0x20 channel_layout 0x68 channels 0x70 sample_rate 0x74`）。

- [ ] **Step 2: 让断言先失败一次（消融）**

把 `NPA_LEGACY_CTX_SIZE` 暂时改成 `0x430`，然后：

Run: `make bridge`
Expected: 编译失败，`static assertion failed`（证明守卫真的生效，不是装饰）

- [ ] **Step 3: 改回正确值**

恢复 `0x438`。

- [ ] **Step 4: 构建确认**

Run: `make bridge`
Expected: 成功（此时头文件还没被任何 shim 引用，可先只做语法检查：
`clang -fsyntax-only -I build/deps/ffmpeg-core902/include -target arm64-apple-ios13.0 bridge/ffmpeg-subdec-abi.h`）

- [ ] **Step 5: Commit**

```bash
git add bridge/ffmpeg-subdec-abi.h
git commit -m "feat: freeze the legacy subtitle-decode ABI in a guard header"
```

---

## Task 3: shim 骨架、导出与构建

**Files:**
- Create: `bridge/npa_ffmpeg_subdec_bridge.c`
- Modify: `bridge/npa_ffmpeg_core902_bridge.c`（加一行 `#include "npa_ffmpeg_subdec_bridge.c"`）
- Modify: `bridge/ffmpeg-core902.exports`（追加 10 个 `_npa_subdec_*`）

**Interfaces:**
- Consumes: Task 2 的 ABI 常量；manifest（Task 1）的符号名。
- Produces: 10 个 `NPA_EXPORT` 入口（签名与 4.4.5 头一致）：
  `npa_subdec_avcodec_alloc_context3(const AVCodec*)` → `AVCodecContext*`（legacy 影子）；
  `npa_subdec_avcodec_free_context(AVCodecContext**)`；
  `npa_subdec_avcodec_find_decoder(enum AVCodecID)` → `const AVCodec*`（**不做影子**）；
  `npa_subdec_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**)` → `int`；
  `npa_subdec_avcodec_parameters_alloc(void)` → `AVCodecParameters*`（legacy 影子）；
  `npa_subdec_avcodec_parameters_free(AVCodecParameters**)`；
  `npa_subdec_avcodec_parameters_from_context(AVCodecParameters*, const AVCodecContext*)` → `int`；
  `npa_subdec_avcodec_parameters_to_context(AVCodecContext*, const AVCodecParameters*)` → `int`；
  `npa_subdec_avcodec_decode_subtitle2(AVCodecContext*, AVSubtitle*, int*, const AVPacket*)` → `int`；
  `npa_subdec_avsubtitle_free(AVSubtitle*)`。
  另需模块内两张登记表：`shadow ↔ modern` 的 ctx 表与 `AVSubtitle* → modern` 表（Task 4/5 用）。

- [ ] **Step 1: 写 shim 骨架**

定义两个登记表结构与查找/登记/注销函数；10 个入口先全部 `__builtin_trap()`（**显式失败，不静默放行**），
只保留签名与 `NPA_EXPORT`。

- [ ] **Step 2: 接进聚合源与导出表**

`bridge/npa_ffmpeg_core902_bridge.c` 追加 include；`bridge/ffmpeg-core902.exports` 追加 10 行。

- [ ] **Step 3: 构建与校验**

Run: `make bridge && make verify`
Expected: 构建成功；`ffmpeg-core902` 的 `bridge.exports` 检查从 26 变为 **36**，7 项检查全过

- [ ] **Step 4: 回归全套**

Run: `uv run pytest -q && uv run pytest dev/tests -q`
Expected: 全绿（不含新增守卫之外的期望变化）

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_subdec_bridge.c bridge/npa_ffmpeg_core902_bridge.c bridge/ffmpeg-core902.exports
git commit -m "feat: register the subtitle-decode shim entry points"
```

---

## Task 4: ctx 与 params 的对象图翻译

**Files:**
- Modify: `bridge/npa_ffmpeg_subdec_bridge.c`

**Interfaces:**
- Consumes: Task 3 的登记表与入口。
- Produces: 可用的 `alloc_context3` / `free_context` / `find_decoder` / `open2` /
  `parameters_alloc|free|from_context|to_context`。

- [ ] **Step 1: 实现分配与登记**

`alloc_context3(codec)`：用 9.0.2 的 `avcodec_alloc_context3` 建现代 ctx，另分配一个 `NPA_LEGACY_CTX_SIZE`
的零填充影子，双向登记后**返回影子**。`parameters_alloc` 同法（`0x90` 影子）。

- [ ] **Step 2: 实现双模读取**

写两个内部函数：`ctx_of(ptr)`（登记表命中 → 现代 ctx；否则按 legacy 布局构造**只读快照**）与
`params_read(ptr, legacy_out)`（登记表命中 → 现代 params；否则按 `NPA_LEGACY_PARAMS_*` 读出）。
**legacy 输入是正常用法**（demux 单元的 codecpar、源 ctx），不是回落。

- [ ] **Step 3: 实现入口同步**

- `open2(ctx, codec, options)`：先做 hw 守卫（`hw_device_ctx`/`hw_frames_ctx`/`get_format` 任一非空 ⇒
  `__builtin_trap()`）；把 spec §2.3 的 **ctx 写入清单**从影子搬进现代 ctx；调 9.0.2 `avcodec_open2`；把现代
  结果字段（`codec_id/codec_tag/time_base/width/height/pix_fmt/sample_aspect_ratio/color_* /
  sample_rate/channels/sample_fmt/channel_layout/subtitle_header(_size)/pkt_timebase`）搬回影子。
- `to_context(ctx, params)` / `from_context(params, ctx)`：按上面的双模读取取值后调用 9.0.2 同名函数。
- `free_context(pavctx)`：只注销并释放 shim 自己建的影子与现代 ctx；指针不在登记表 ⇒ `__builtin_trap()`。

- [ ] **Step 4: 构建与校验**

Run: `make bridge && make verify && uv run pytest -q`
Expected: 全过

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_subdec_bridge.c
git commit -m "feat: translate the subtitle decoder context object graph"
```

---

## Task 5: 字幕对象翻译（`AVSubtitle` 物化）

**Files:**
- Modify: `bridge/npa_ffmpeg_subdec_bridge.c`

**Interfaces:**
- Consumes: Task 4 的 `ctx_of()`。
- Produces: 可用的 `decode_subtitle2` / `avsubtitle_free`。

- [ ] **Step 1: 实现 decode_subtitle2**

按 legacy 布局读出 app 的 packet（`pts 0x08 / data 0x18 / size 0x20 / flags 0x28 / duration 0x40`），用 9.0.2 的
`av_packet_alloc` + 字段填充（`buf=NULL`，指向 app 的内存）构造**瞬时现代 packet**；调用 9.0.2
`avcodec_decode_subtitle2` 得到现代 `AVSubtitle`；再**在 app 传入的栈上 `AVSubtitle` 就地物化** legacy
形状（`0x20`）并分配 legacy 形状的 `rects[]`（`0xC8` 每个）与 `data[]/text/ass` 缓冲，把
`AVSubtitle* → modern` 登记起来。

- [ ] **Step 2: 实现 avsubtitle_free**

从登记表取现代对象并释放；同时按 legacy 布局释放影子自己的 `rects[]/data[]/text/ass`；未登记 ⇒
`__builtin_trap()`（app 只会 free 它从本入口拿到的对象）。

- [ ] **Step 3: 落地 Review Focus 的边界**

- 位图路径：第 1 条 rect 的 `type==1` 时把 `w/h/nb_colors/data[0]/linesize[0]` 物化完整（PAL8 调色板路径由
  app 自己跑）。
- `num_rects==0 && format==0`：把 `format=0 num_rects=0` 写进影子（app 据此发"清空"）。
- `end_display_time` 保留 FFmpeg 原值（含 `-1` 哨兵），不得归一化。
- 文本/ASS 路径：`type==2`/`type==3` 时 `text`/`ass` 必须是可被 app `memcpy` 的独立缓冲。

- [ ] **Step 4: 构建与校验**

Run: `make bridge && make verify && uv run pytest -q && uv run pytest dev/tests -q`
Expected: 全过

- [ ] **Step 5: Commit**

```bash
git add bridge/npa_ffmpeg_subdec_bridge.c
git commit -m "feat: materialise AVSubtitle shadows for the subtitle unit"
```

---

## Task 6: 设备验收（**用户停点 A**）

**Files:**
- Modify: `dev/acceptance.json`（**新增** `ffmpeg-subdecode` 条目）

**Interfaces:**
- Consumes: Task 1–5 的产物。
- Produces: 结论为 `partial`/`pass` 的验收条目 + 打包锚点。

- [ ] **Step 1: 造打包产物（我来做）**

Run: `make bridge && uv run python dev/tools/... ` 或既有打包入口，选择 `libass + ffmpeg-core902`；
记下 packaged main sha256 与两个 dylib 的 sha256。

- [ ] **Step 2: 请用户安装并跑行（停下来等）**

**这一步必须由用户操作**：安装 IPA，然后按 `notes/playcover-debug-path.md`：

1. 内挂 ASS 字幕视频（既有 `sample-20s-contained.mkv`）播放 + 拖进度；
2. 外部 `.ass`（`sample-20s-external.ass`）；
3. 内挂**位图**字幕（PGS/DVD，需新样本）——覆盖 Review Focus 第 3 条；
4. 内挂 SRT，以及**字节解析四类**（TXT/SAMI 等）各一条——覆盖 Review Focus 第 1 条；
5. 状态字读取：驱动后 `ffmpeg-subdecode` 读 `2 (NEW)`（先读 `0` 建立因果）。
6. **默认产物回归**：用 `libass + ffmpeg-full`（4.4.8）装一次，跑既有核心行（H.264 / HEVC Main10 / 缩略图 /
   内挂字幕）——新域没有从 `ffmpeg-core` 域移走任何站点，这个产物应当**完全不变**（锚点也与历史一致）。

- [ ] **Step 3: 写验收条目**

把锚点、dylib 哈希、逐行结果与"未做项"写进 `dev/acceptance.json` 的**新条目**（不改历史条目）。

- [ ] **Step 4: Commit**

```bash
git add dev/acceptance.json
git commit -m "test: record the subtitle decode unit acceptance"
```

---

## Task 7: 整 dylib 回退 A/B

**Files:**
- Modify: `dev/acceptance.json`（同一条目的回退行）

**Interfaces:**
- Consumes: Task 6 的素材与**已安装**产物。
- Produces: R7 的证明记录。

**做法：不另做产物，直接从已安装的包里抽掉桥 dylib**——弱加载使 app 仍能启动，而身份校验（dylib 在期望
路径、basename 与导出符号对得上）失败正是单元发布 `OLD` 的条件。

- [ ] **Step 1: 备份并抽掉 dylib**

```bash
APP=~/Library/Containers/io.playcover.PlayCover/Applications/com.newin.nplayer.basic.app
BK=/private/var/folders/rx/pf75s9k53vg45_mtzdbkh7hh0000gn/T/opencode
cp "$APP/Frameworks/LibFFmpegCore902Bridge.dylib" "$BK/LibFFmpegCore902Bridge.dylib.bak"
rm "$APP/Frameworks/LibFFmpegCore902Bridge.dylib"
```

- [ ] **Step 2: 重启 app、驱动、读状态字**

`open -b com.newin.nplayer.basic`，再按 `notes/playcover-debug-path.md` §3 喂一个媒体文件、§4 读数。

Expected: `ffmpeg-core902` 承载的**全部 4 个单元**（`ffmpeg-demux`/`libswscale`/`libswresample`/
`ffmpeg-subdecode`）读 `3 (OLD)`，`libass` 保持 `2 (NEW)`，字幕与播放行为等同 app 自带 4.4.5。

（若 app 因包签名失效拒绝启动：把备份拷回 `Frameworks/`；需要时改用"只选 `libass` 的产物"这条路，
并在条目里写明用的是哪种做法。）

- [ ] **Step 3: 还原并写条目**

把 dylib 拷回 `Frameworks/`，确认状态字恢复 `2 (NEW)`，把两种状态的结果都写进条目。

- [ ] **Step 4: Commit**

```bash
git add dev/acceptance.json
git commit -m "test: record the whole-dylib fallback for the 9.0.2 core"
```

---

## 收尾（不单独设任务）

- 全分支复跑：`uv run pytest -q`、`uv run pytest dev/tests -q`、`git status` 干净。
- 计划外的临时诊断/测试若产生，在 Task 6 之前移除。
- 播放/probe 解码单元（`ffmpeg-codec`）是**另一份计划**（P2-B），在 Task 6 设备验收通过后再写。
