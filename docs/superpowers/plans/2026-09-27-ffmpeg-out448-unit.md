# ffmpeg-out448 单元实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 在 `feat/ffmpeg-output448` 上落一个 4.4.8 的独立单元（三个域：HLS / SPDIF / 封面 MJPEG），与已验收的
9.0.2 输入侧组合成 `libass + ffmpeg-core902 + ffmpeg-out448`。

**Architecture:** 一个 4.4.8 dylib，入口点全部是汇编 tail-branch（`npa_<sym>` → `b _<sym>`），复用现有 4.4.8
闭包，不含任何翻译代码与结构体影子；站点按"所属函数"整块从 `ffmpeg-core` 域取，与 9.0.2 五域地址级零重叠，
与两个 4.4.8 全核故意重叠并由对称 `conflicts` 正当化。

**Tech Stack:** C（bridge 转发 TU）、Python（`npabridge` manifest/payload/打包工具链、pytest、站点表生成）、
arm64 iOS 13 静态闭包、ldid、Frida（进程内驱动）。

**Spec:** `docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md`（本计划从 spec 出发，执行者需同时阅读）

## Global Constraints

- 站点只从 `ffmpeg-core` 域的现成站点里取，不新增站点；站点按**所属函数**整块取，不按 API 语义切。
- 一个站点只归一个单元；与 9.0.2 五域（`ffmpeg-demux`/`libswscale`/`libswresample`/`ffmpeg-subdecode`/`ffmpeg-codec`）
  **地址级零重叠**，守卫测试只比地址、不比符号。
- 与 `ffmpeg-core` / `ffmpeg-full` 的故意重叠由对称 `conflicts` 正当化；`default_dylibs` 不变。
- 零翻译、零影子：入口点只有 tail-branch；跨版本相容性由 `bridge/ffmpeg-core-abi.h` 编译期断言。
- 复用 `build/deps/ffmpeg-core-closure.txt`，**不跑 `make deps`**、不改闭包、不引新依赖。
- 每个 API 的 `old_target` 必须与 `ffmpeg-core` 域同名 API 一致。
- 剔除项（留给 `ffmpeg-codec`）：`avcodec_free_context` @`0x100B300F0`、@`0x100B63F88`。
- 既有选择 `libass + ffmpeg-core902` 的 `packaged_main_sha256` 必须仍为
  `5a33aa13b5c5c2455c9be9f5fb2857c1e36bbc95b8a3de61a206ed6d819b6ca1`。
- 驱动纪律（`notes/playcover-debug-path.md` §6/§8）：只 attach/读内存优先；进程内驱动要有锚点字节校验并在
  加载期间**不得 kill** 目标；进程内驱动只在本地已安装 app 上做，不在用户设备上做。
- IDA 只读：不覆盖既有函数名/变量名/注释/原型/类型，`save: false` 收尾。

## Review Focus

1. **站点归属错**：某站点其实不在目标函数内（或在主播放音频路径上）⇒ 会静默改动不相干面的行为。
   预期行为：附录里每个站点都能指出所属函数，且它只出现在一个域里。→ Task 1 的函数边界核对 + Task 2 的 exclusion 断言。
2. **`old_target` 不一致**：修补会打到错误地址（app 崩溃或行为变化）。
   预期：逐符号与 `ffmpeg-core` 域相等。→ Task 2 的守卫测试逐符号断言。
3. **`canPassthru == 1` 时 mux 根本没跑**，却被当成"驱动成功"（假绿）。
   预期：驱动先断言 `impl+0x280 == 0`，否则报错退出。→ Task 4 的 HLS 前置断言。
4. **SPDIF 驱动在非 AC3/DTS 源上工厂不触发**（假绿）。
   预期：判读要求"状态字变 2"与"帧头断言通过"同时成立。→ Task 4 的 SPDIF 双重条件。
5. **载荷布局被本次改动影响**，既有选择的锚点静默变化。
   预期：重打 `libass + ffmpeg-core902` 的 `packaged_main_sha256` 与记录值逐字节相等，不等则停下来查。
   → Task 3 的锚点回归检查。
6. **驱动后会话/线程未收尾**：`.hls/` 与线程残留，下一次驱动结果被污染。
   预期：驱动结束前会话正常停止、线程可终止。→ Task 4 的收尾步骤。

---

## Task 1: 站点归属核对与站点表冻结（IDA，只读）

**Files:**
- Modify: `docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md`（§3.2 / §3.3 / 附录）
- Create: `/Volumes/990EP/Work/Mac/nPlayer_Backup/notes/ida-investigation-out448-sites.md`（工作区 notes，不入库）

**Interfaces:**
- Produces: 冻结的三张站点表（append 的附录）、每个辅助层站点的"所属函数 + 调用者"结论、HLS/SPDIF 驱动入口的
  签名与实例来源（供 Task 4 的 JS 使用）。

- [x] **Step 1: 派 `ida-explorer`（只读）做四项核对**

问题清单（逐条要证据与 Confirmed/Probable/Hypothesis 标注；主库 `nPlayer.i64`，imagebase `0x100000000`，
`save: false`，不改任何名字/注释/类型）：

1. 附录里每个站点属于哪个函数（给出函数起点）；`sub_100B94DA0`(0x1B30) / `sub_100B98A38`(0x1680) /
   `sub_100B9A294`(0xF0) / `sub_100B9A534`(0x46C) / `sub_100A469FC`(0x1AC) 的实际边界是否与附录区间一致。
2. 16 个音频/SPDIF 辅助层站点（`0x100B63E5C`…`0x100B64D7C`）各属哪个函数、该函数的调用者是谁；
   是否与主播放音频路径（`0x100A89xxx` 的音频解码器、9.0.2 codec 面）共用 ⇒ 决定它们是否留在本单元。
3. `0x100B96C70`…`0x100B96D40` 是否同属一个（或哪几个）会话清理函数。
4. HLS 驱动入口：`sub_100B94360`（`MediaServer::CreateHLSSession`）的 `this` 从哪来（单例？）、
   `sub_100B944C8` / `sub_100B93A18` 的签名、`sub_100A52108`（SPDIFOutput 判定）与
   `sub_100B8A664`（音频解码器工厂）→ `sub_100B2FFA4` → `sub_100B3010C` 的调用关系。

- [x] **Step 2: 按结论更新 spec 的 §3.2 / §3.3 / 附录**

- 若某辅助层站点落在主播放音频路径 ⇒ 从本单元站点集移出（认领数相应下调），并在 §3.3 记为"不覆盖"；
- 附录表格里每个站点的"域"列改成核对后的结论，并在 §3.3 写明核对依据（函数边界 + 调用者）。

- [x] **Step 3: 复核数字自洽**

Run:
```bash
cd /Volumes/990EP/Work/Mac/nPlayer_Backup/nPlayerLibassBridge/.worktrees/ffmpeg-out448
python3 - <<'PY'
import json, re, pathlib
spec = pathlib.Path("docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md").read_text()
appendix = spec.split("## 附：站点表")[1]
sites = re.findall(r"`(0x100[0-9A-Fa-f]+)`(†?)", appendix)
print("附录站点", len(sites), "剔除", sum(1 for _, d in sites if d))
rows = re.findall(r"^\| `npa_", appendix, re.M)
print("API 行", len(rows))
PY
```
Expected: 附录站点数 = **155**、剔除 = **1**、API 行 = **67**，与 §3.2 表格（认领 154）一致。

- [x] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md
git commit -m "docs: freeze the output-side site table after attribution"
```

---

## Task 2: manifest 三个域 + 守卫测试

**Files:**
- Modify: `manifests/nplayer-3.13.0.json`
- Test: `tests/test_out448_manifest.py`（新）

**Interfaces:**
- Consumes: Task 1 冻结的站点表。
- Produces: dylib 条目 `ffmpeg-out448`（`basename` `LibFFmpegOut448Bridge.dylib`、`library_version` `4.4.8`、
  `conflicts` `["ffmpeg-core", "ffmpeg-full"]`、`build.source`/`build.exports` 指向 Task 3 要建的文件、
  `build.closure` `build/deps/ffmpeg-core-closure.txt`）与三个域 `ffmpeg-hls448` / `ffmpeg-spdif448` /
  `ffmpeg-mjpeg448`（API 行：`symbol` `npa_<sym>`、`call_sites`、`old_target` 与 `ffmpeg-core` 同名 API 相同）。

- [x] **Step 1: 写守卫测试**

`tests/test_out448_manifest.py`，结构照 `tests/test_codec_manifest.py`，断言：

```python
# 1. 三个域存在，逐域站点集合等于冻结表
# 2. 并集恰为 154，且三域两两不相交
# 3. 与 9.0.2 五域按“地址”零重叠（不要用 (地址, 符号) 配对比较）
# 4. 每个 API 的 old_target == ffmpeg-core 域同名 API 的 old_target
# 5. exclusion：0x100B300F0 / 0x100A469C0 / 0x100A469E0 / 0x100A469E8，
#    以及 0x100B63xxx-0x100B64xxx 的 16 个（主播放音频路径）都不在集合内
# 6. ffmpeg-out448 的 conflicts 与 ffmpeg-core / ffmpeg-full 对称
```

- [x] **Step 2: 跑测试确认失败**

Run: `uv run pytest tests/test_out448_manifest.py -q`
Expected: FAIL（找不到 `ffmpeg-out448` dylib）

- [x] **Step 3: 写入 manifest**

新增 dylib 条目 + 三个域（**不动任何既有域/条目**）。

- [x] **Step 4: 跑测试确认通过**

Run: `uv run pytest tests/test_out448_manifest.py tests/test_codec_manifest.py tests/test_manifest.py -q`
Expected: PASS

- [x] **Step 5: Commit**

```bash
git add manifests/nplayer-3.13.0.json tests/test_out448_manifest.py
git commit -m "feat: add the ffmpeg-out448 unit to the manifest"
```

---

## Task 3: bridge 源 / forwards / exports，构建校验与锚点回归

**Files:**
- Create: `bridge/npa_ffmpeg_out448_bridge.c`
- Create: `bridge/npa_ffmpeg_out448_forwards.h`
- Create: `bridge/ffmpeg-out448.exports`

**Interfaces:**
- Consumes: Task 2 的域（72 个 API 名）。
- Produces: 可构建的 `build/LibFFmpegOut448Bridge.dylib`，导出恰好 72 个 `_npa_*`。

- [x] **Step 1: 生成 forwards 头与 exports**

按 `bridge/npa_ffmpeg_core_bridge.c` 与 `bridge/npa_ffmpeg_forwards.h` 的现成形状写两个文件，名与格式完全由现有文件决定：

- `bridge/npa_ffmpeg_out448_forwards.h`：三个宏（每面一个），每行是**裸 FFmpeg 符号名**（如 `F(avformat_open_input)`），前缀由 `.c` 的宏加；
- `bridge/ffmpeg-out448.exports`：每行 `_npa_<面前缀>_<符号名>`，共 **90** 行。

符号清单从 manifest 取，避免手抄（下面的命令只打印清单与条数，写文件时照抄输出）：

```bash
python3 - <<'PY'
import json
m = json.load(open("manifests/nplayer-3.13.0.json"))
apis = [a for d in m["domains"] if d["id"].endswith("448") and d["id"].startswith("ffmpeg-")
        for a in d["apis"]]
syms = sorted({a["symbol"][len("npa_"):] for a in apis})
print(len(syms), "个入口")
for s in syms:
    print(f"X({s})")
PY
```

Expected: **90**（与 spec §4 一致：hls 66 + spdif 11 + mjpeg 13）。

- [x] **Step 2: 写 bridge 源**

`bridge/npa_ffmpeg_out448_bridge.c`：`#include "ffmpeg-core-abi.h"` + `#include "npa_ffmpeg_out448_forwards.h"`
+ `NPA_FORWARD` 宏 + `NPA_OUT448_FORWARDS(NPA_FORWARD)`（内容的唯一决定项是宏名，其余照抄 `npa_ffmpeg_core_bridge.c`）。

- [x] **Step 3: 构建与校验**

Run: `make bridge && make verify`
Expected: `LibFFmpegOut448Bridge.dylib` 生成；`make verify` 全过（导出集精确 72、install name、依赖仅系统框架、
无初始化器、无宿主路径）。

- [x] **Step 4: 锚点回归检查（Global Constraint）**

Run:
```bash
uv run npa-patch /Volumes/990EP/Work/Mac/nPlayer_Backup/nPlayer_3.13.0.ipa \
  --dylibs-dir build --dylib libass --dylib ffmpeg-core902 -o /tmp/anchor-check.ipa
# 打印 packaged_main_sha256
```
Expected: `packaged_main_sha256 == 5a33aa13b5c5c2455c9be9f5fb2857c1e36bbc95b8a3de61a206ed6d819b6ca1`
（不相等 ⇒ **停下**，先查载荷生成为何受影响，不许改用新锚点）。

- [x] **Step 5: 全量测试**

Run: `uv run pytest -q && uv run pytest dev/tests -q`
Expected: 全绿

- [x] **Step 6: Commit**

```bash
git add bridge/npa_ffmpeg_out448_bridge.c bridge/npa_ffmpeg_out448_forwards.h bridge/ffmpeg-out448.exports
git commit -m "feat: forward the output-side faces into the 4.4.8 closure"
```

---

## Task 4: 进程内驱动与判读（HLS、SPDIF；我出结果，不产生仓库提交）

> **顺序修订（2026-09-27 执行时裁定）**：本任务依赖**用户安装新产物之后**的已安装 app——新增 dylib 不能靠
> 本地换 dylib 生效（主程序只弱加载选择里声明过的 dylib）。执行顺序 = Task 5 Step 1（打产物）→ 用户安装 →
> 本任务 → Task 5 Steps 3-4（状态字与回退 A/B）→ Task 6。SPDIF 行见 Task 5 Step 2 的说明。

**Files:**
- Create（仓库外，沿用既有约定）：`/Volumes/990EP/Work/Mac/nPlayer_Backup/nPlayerFridaHook/probe-out448-hls.js` / `probe-out448-hls.py`
- Create（仓库外）：`/Volumes/990EP/Work/Mac/nPlayer_Backup/nPlayerFridaHook/probe-out448-spdif.js` / `probe-out448-spdif.py`

**Interfaces:**
- Consumes: Task 3 的 dylib、Task 1 的入口签名与实例来源、`nPlayerFridaHook/state-words.py`（状态字读取；
  段 rva 与 9 个单元偏移按本次选择用 `npabridge.payload.unit_offsets` 重算）。
- Produces: 供 Task 6 写条目的测量结果（HLS 分片、SPDIF 帧头、状态字、调用计数）。

- [x] **Step 1: HLS 驱动**

按 `nPlayerFridaHook/probe-subtitle-seek-drive.js` 的模式（开头 10 个地址的**锚点字节校验**，不匹配即报错退出）：
驱动 `-[HTTPServer createHLSSessionWithURL:options:]` → `sub_100B94360` → `sub_100B944C8` → `sub_100B94DA0`
→ `sub_100B93A18` → 线程体 `sub_100B98A38`。素材用非 mp4/非 webm/无 DRM 的 H.264+AAC MKV（放 app 的 Documents）。

- [x] **Step 2: HLS 前置断言与判读**

- 断言 `sub_100B94328` 读出的 `impl+0x280` == 0（否则会话不走 FFmpeg ⇒ 报错退出）；
- 判读三层：状态字 `ffmpeg-hls448 == 2`；`.hls/` 下 playlist 与分片文件存在且非空；
  `ffprobe -v error -show_entries stream=codec_type,duration -of default=nw=1 .hls/<第一个分片>`
  必须输出至少一条 `codec_type=video` 且 `duration` 非 0（命令与期望写进驱动脚本注释，也写进 Task 6 条目）。

- [x] **Step 3: HLS 收尾**

让会话正常停止（`-[HLSSession stop]` 或等线程自然结束），确认无残留线程/文件；**不 kill 进程**。

- [x] > **2026-09-27 结果（HLS 步）**：驱动已实现、5 个锚点全部校验通过，但本环境的 app 在
> `MediaServer::CreateHLSSession` 之前命中自身断言（cast 前置状态过不去），该面**不可判读**；
> `ffmpeg-hls448` 仍为 0，只留站点/构建级证据。见 `dev/acceptance.json` 的 blind spots。

**Step 4: SPDIF 驱动**

通过 app 自身 API 打开 `MediaPlayerConfig.SPDIFOutput`，播 AC3（再补一条 DTS）源；hook 写回调 `sub_100B30390`
取输出字节。锚点校验同上。

- [x] **Step 5: SPDIF 判读（双重条件）**（实际：状态字 + 分支返回值双证；IEC 61937 字节级断言未取到，见验收条目）

- 状态字 `ffmpeg-spdif448 == 2`（证明工厂确实造了 SPDIF 对象并走到本单元）；
- 输出字节的 IEC 61937 帧头为 `0xF872` / `0x4E1F`，data-type 为 AC3=1 / DTS=11；
- 无崩溃，且音频路径行为与改动前一致（对照同一素材在改动前的表现）。

- [x] **Step 6: 记录结果**

把两面的原始测量（状态字序列、分片清单与 ffprobe 输出、帧头字节与计数）整理好，供 Task 6 写进
`dev/acceptance.json`；驱动脚本留在 `nPlayerFridaHook/`（不进仓库）。

---

## Task 5: 打产物、用户行、状态字与回退 A/B（**用户停点**）

**Files:**
- Modify（产物）：`build/accept/out448-unit.ipa`（新增）

- [x] **Step 1: 打产物并记录锚点**

Run: `uv run npa-patch /Volumes/990EP/Work/Mac/nPlayer_Backup/nPlayer_3.13.0.ipa --dylibs-dir build --dylib libass --dylib ffmpeg-core902 --dylib ffmpeg-out448 -o build/accept/out448-unit.ipa`

记录：`packaged_main_sha256`、IPA 内三个 dylib 的 sha256、as-built `LibFFmpegOut448Bridge.dylib` 的
sha256 与 UUID（二者因重签名不同，条目里写明）。

- [x] **Step 2: 请用户安装并跑行（停下来等）**

用户跑：**影片信息面板 / 海报**（覆盖 MJPEG 编码路径）；以及 P0–P2 的既有行
（软解 H.264/HEVC/HEVC Main10、Opus/Vorbis、字幕、AV1 缩略图 + 软解 + 硬解）不得改变。

**SPDIF 行（执行时裁定）**：要跑到 `media::SPDIF` 需要活的 `media::MediaPlayer` 把 `+0x88`（SPDIFOutput）
置 1，Frida 侧拿不到该实例 ⇒ 这一行改为**由用户在设置里打开数字音频透传 + 播 AC3/DTS** 实测；
若用户也无法触发，则如实记为"本环境不可验证"（只保留站点/构建级证据，不假称通过）。

- [x] **Step 3: 读 9 个单元状态字**

用 `nPlayerFridaHook/state-words.py`（段 rva 与偏移按本次选择重算）驱动同一素材：先全 0，驱动后三个新单元
读 `2 (NEW)`，其余 6 个仍 `2`。

- [x] **Step 4: 整 dylib 回退 A/B**

抽掉 `LibFFmpegOut448Bridge.dylib`（先备份）→ 驱动 → 三个新单元读 `3 (OLD)`、其余 6 个不变；还原 → 再驱动
→ 六个 + 三个全 `2`。app 之间用优雅退出，不 `pkill`。

- [x] **Step 5: 记录结果**

原始读数整理好，供 Task 6 写条目。

---

## Task 6: 验收条目、路线图与收尾

**Files:**
- Modify: `dev/acceptance.json`（新增 variant `ffmpeg-out448`）
- Modify: `docs/superpowers/plans/2026-09-27-expand-ffmpeg902-wiring.md`（§7 P3 的最终形态）

- [x] **Step 1: 写验收条目**

variant `ffmpeg-out448`，内容：产物锚点与三份 dylib 哈希；域与站点数（130/11/13，认领 154，入口 90）；条目要点 =
零翻译零影子、复用 4.4.8 闭包、67 导出、`old_target` 与 `ffmpeg-core` 一致、两个剔除项留给 `ffmpeg-codec`、
与 9.0.2 五域地址级零重叠、与 4.4.8 全核故意重叠由 conflicts 正当化；行 = HLS（驱动 + `canPassthru==0` 断言 +
分片 ffprobe）、SPDIF（驱动 + IEC 61937 帧头断言）、封面（用户信息面板行）、状态字 9 项、整 dylib 回退 A/B、
P0–P2 回归；未实测/盲区照 spec §9 列出。

- [x] **Step 2: 更新路线图 §7**

把 P3 记为两种可选形态：**同世代单元（本设计，已落地）** 与 **输出侧也 9.0.2（探索分支
`feat/ffmpeg-demux-class-a`）**；写明默认产物不变、两者由 `conflicts` 隔离。

- [x] **Step 3: 全分支复跑**

Run: `uv run pytest -q && uv run pytest dev/tests -q && git status`
Expected: 全绿、工作区干净、无临时诊断残留（`rg -n "getenv|NPA_TRACE" bridge/*.c` 为空）。

- [x] **Step 4: Commit**

```bash
git add dev/acceptance.json docs/superpowers/plans/2026-09-27-expand-ffmpeg902-wiring.md
git commit -m "test: record the output-side unit acceptance"
```
