# iOS Matroska/PGS 静态排查（2026-09-30）

范围：当前 `nPlayerLibassBridge` 源码、本地 FFmpeg 4.4.5 / 9.0.2 源码、已有逆向记录，以及后续用户提供的 iOS 崩溃报告。用户观察：iOS 打开视频即崩溃，macOS 能显示字幕。本轮没有打开 IDA、修改实现、运行播放或测试。

环境：项目有 `.venv/`；根目录不是 Git 仓库，`nPlayerLibassBridge/` 是仓库，排查开始时工作区干净。

## 崩溃报告追加：extradata 悬空指针成为首要根因候选

**Confirmed（报告事实）**：iOS 17.7.2，Thread 17 的 `EXC_BAD_ACCESS / SIGSEGV`，ESR 为 byte read Translation fault。调用栈：

`_platform_memmove +52` ← `av_memdup +144` ← `npa_subdec_avcodec_decode_subtitle2 +164` ← app `0x100A04D18`（还原 ASLR 后）。这次实际崩溃在内存复制阶段，不是第 1 项的显式 `abort()`，也没有到达第 3 项位图交接。

报告中的 bridge UUID 为 `1BB830EE-9708-38F1-B15F-09CC957A9F99`，与 `xcrun dwarfdump --uuid build/LibFFmpegCore902Bridge.dylib` 完全一致。`nm` 的字幕解码入口偏移 `0x527C` 与报告 `0x5320 - 164` 一致；`av_memdup` 偏移 `0xB9A48C` 与报告返回地址偏移 `0xB9A51C`（+144）一致。未反汇编桥或打开 IDA。

寄存器 `x1=x20=0x144948000` 为不可读源地址，恰好位于前一 MALLOC_LARGE 区域末端后的空洞起点。`x2=x19=0x445B4`（279988 字节，约 273.4 KiB），目标地址 `x0=x21=0x141144000`。源地址非法读取有直接证据；报告本身不能证明源地址之前属于哪个已释放分配。

**Confirmed（源码生命周期缺口）**：

1. `npa_ctx_out` (`bridge/npa_ffmpeg_subdec_bridge.c:431`) 将 modern extradata 的地址 A 借给 shadow。
2. 每次字幕解码 (`:1046`) 调用完整 `npa_ctx_in`，其中 `npa_ctx_set_extradata` (`:352`) 把 shadow 的 A 复制到 B。
3. `npa_ctx_set_extradata_owned` (`:340`) 释放 A，modern 改指向 B。
4. 字幕解码路径不再发布 extradata，shadow 仍指向已释放的 A。
5. 下一次刷新用 A 调用 `av_memdup`，构成 use-after-free；若 A 已不可读，就能形成本次报告这种调用栈。

该链路要求非空 extradata 且 shadow 仍持有此前发布的别名。不能仅凭报告确认故障样本的 extradata 来源、是否 PGS、是否第一/第二次解码或有无其他写入。当前源码字幕入口的非 AV1 `av_memdup` 来源是上下文 extradata 刷新；packet 数据复制使用 `memcpy`。因此 **Probable（本次根因）** 是 extradata 的悬空别名，而不是先前列出的释放中止或位图矩形问题。

该 UAF 可随分配器是否保留/复用/解除映射已释放内存而改变表现，能够解释 macOS 可运行、iOS 非法读的可能差异；两平台的具体分配器行为尚未测量。

**最小修正方向（未实施）**：字幕数据路径只同步 app 在 open 后修改、且解码真正需要的 `pkt_timebase`；ASS 的 `sub_text_format` 已直接从 shadow 读取。避免逐包重复制/释放 extradata，同时保留 PGS 自行更新的画布宽高。配置入口仍承担完整同步。仅补一次 `npa_ctx_out` 虽可更新别名，但仍留下无必要的逐包配置覆盖和复制，不作为优先方向。

## 调用链与依据

先检查 `nPlayer.h`：存在 `Subtitle`、`SubtitleBitmap`、`BitmapSubtitleView`、`MediaPlayerView setSubtitleBitmap:`，位图输出会进入 ObjC 层。解码和 ABI 转换主体在 C/C++ 层。

`bridge/npa_ffmpeg_core902_bridge.c` 包含 demux、util 和 subdec 三个实现。PGS 是 FFmpeg 位图字幕分支；桥接库中的 libass ASS 改写只处理 `SUBTITLE_ASS`。

沿用 `../notes/ida-investigation-p2-subtitle-decode.md` 已有逆向证据（本轮未重新验证二进制）：

- 内挂字幕：`sub_100B981DC` → `sub_100A04BCC`。
- 解码站点 `0x100A04D14`，释放站点 `0x100A05788`，均被 manifest 的 `ffmpeg-subdecode` 域接管。
- 位图交接：`sub_100A05EEC`，PAL8 frame 分配与 `av_image_copy`，之后可能执行调色板转换。
- 既有记录描述 `sub_100A04BCC` 各出口调用 `avsubtitle_free`，桥接源码也依赖这一约定。

## 1. 无输出/失败解码与释放契约不一致

**Confirmed（源码缺口）**：`bridge/npa_ffmpeg_subdec_bridge.c:1082` 在 `ret < 0 || !got` 时只释放 modern subtitle 并返回，既不登记 app subtitle，也不初始化 app 输出结构。`:1101` 的释放入口找不到登记时直接 `abort()`。

4.4.5 的 `avcodec_decode_subtitle2` 在正常进入解码时先初始化字幕输出；`avsubtitle_free` 能释放零矩形的空对象。桥接层把“没有输出字幕”和“非法对象来源”混为一谈。

9.0.2 `libavcodec/pgssubdec.c:655` 只有处理 DISPLAY_SEGMENT 才设置 `got_sub_ptr`。仅带 presentation / palette / object 等状态更新的包可以成功消费但 `got=0`；缺失 palette 等错误也会进入桥接层不登记的路径。

**Probable（依赖既有调用点证据）**：如果这些返回路径按既有记录到达 `0x100A05788`，释放入口会主动中止进程。可在首批预解码包触发。

**未确认**：出问题的样本是否包含这些包、实际崩溃是否到达此 `abort`。相同桥接版本、相同包序列和相同释放路径应在两平台同样触发，因此这一项本身不能解释平台差异。

## 2. 逐包刷新覆盖 PGS 解码器持有的画布尺寸

**Confirmed（源码行为）**：`:1046` 每次字幕解码调用 `npa_ctx_in`；`:367` / `:368` 将 shadow width / height 写回 modern。字幕解码返回后没有 `npa_ctx_out`，因此 PGS 在解码中更新的尺寸未同步到 shadow。

9.0.2 `pgssubdec.c:405` 的 presentation segment 用 `ff_set_dimensions` 设置画布尺寸；`:294` 的 object segment 按 `avctx->width/height` 检查位图尺寸。

**Hypothesis（样本相关）**：若 shadow 仍是零尺寸或旧尺寸，且 presentation 与 object 在不同解码包中，第二个包会把正确画布尺寸覆盖掉，随后拒绝有效位图。presentation 与 object 在同一个包内时，presentation 可以先恢复尺寸，不能据此宣称所有 Matroska PGS 都受影响。

此缺口可导致无输出、空位图或错误返回；是否进一步触发第 1 项或播放器位图异常未确认。

## 3. 空位图矩形未经适配传给播放器

**Confirmed（源码行为）**：9.0.2 `pgssubdec.c:537` 先加入 `SUBTITLE_BITMAP` 矩形并分配 256 色 palette。缺失 object (`:552`) 或非致命 RLE 错误可保留 `w=h=0` 的矩形；缺失 object 时 `data[0]=NULL`。外层仍可得到 `got=1`。

桥接层 `:831` 起原样镜像宽高、palette 和像素指针，没有过滤/适配这些空矩形。这和 `got=1,num_rects=0` 的正常清屏事件不同；正常清屏事件当前会登记并可由桥接释放。

与 4.4.5 的差异：9.0.2 将 palette 分配提前，异常空矩形仍可具有 `nb_colors=256` 和有效 palette。4.4.5 的部分相同错误出口发生在 palette 分配之前。

**Hypothesis（崩溃影响）**：已有逆向记录显示播放器将 BITMAP 交给 `sub_100A05EEC` 分配 PAL8 frame / 拷贝，再按条件进行 palette 转换。需核对它是否正确处理零尺寸、NULL 像素面和 frame 分配失败。现有记录不足以断言该函数一定解引用 NULL。

这一差异比 codec ID 更值得用于核对“macOS 显示 / iOS 崩溃”的位图路径，但仍缺本次调用栈证据。

## 已缩小的范围

- 两代 `AV_CODEC_ID_HDMV_PGS_SUBTITLE` 都为 `0x17006`，PAL8 都为 11；未发现这两项枚举错配。
- legacy `AVSubtitleRect` 的 `data@0x78`、`linesize@0x98`、`type@0xA8` 已由桥接结构和断言覆盖；与已有 app 字段访问记录一致。
- 像素面被借用，modern subtitle 保留到 app 释放时；当前释放入口先保存 `entry->modern` 再删除登记，历史上该位置的 use-after-free 已在源码中修正。
- `dev/acceptance.json` 明确记录 PGS/DVD/DVB 位图分支未被实际验收；ASS 验收不能证明此分支。

## 初轮建议取证（报告提供前）

优先取得 iOS 崩溃线程、异常类型和 bridge 偏移：若为 `SIGABRT` 且落在 `npa_subdec_avsubtitle_free`，先核对第 1 项；若落在 `sub_100A05EEC`、frame 分配/拷贝或 palette 转换，核对第 3 项及第 2 项是否制造空位图。

只需记录首批 PGS 解码的 `ret/got/num_rects`、shadow/modern 宽高及矩形 `w/h/data[0]/data[1]/linesize[0]`。同时确认 iOS 与 macOS 使用相同 bridge 版本和选中单元；否则平台差异不能直接用于归因。

不能从平台差异推断 iOS “内存模型更严格”；非法访问可能随分配器、内存布局、系统实现或线程时序不同而呈现不同结果，而显式 `abort()` 要按控制流定位。

## 范围消融审查

未添加防御框架、兼容层、依赖或测试。删去与当前 PGS 链路无直接证据联系的通用 side-data、OOM 和并发猜测，仅保留三个具体边界及其触发条件。实现是否需要改动留待本次崩溃证据确定。

报告追加后，优先收敛到 extradata 所有权和逐包刷新。此前三项保留为独立静态缺口，不再作为本次直接崩溃位置。未为取证扩展通用框架，也未改代码或打包；精确闭环只需核对故障调用的 shadow/modern extradata 指针、大小和上一轮释放地址。

## 修正与同类检查（设备复测通过）

用户授权修复后，修改 `bridge/npa_ffmpeg_subdec_bridge.c`：

- 字幕逐包入口只同步 `pkt_timebase`，不重新复制 extradata，不覆盖 PGS 画布。
- `npa_ctx_of` / `npa_params_of` 在同步后重新发布 shadow，保证上下文转参数、参数转上下文的源对象借用指针有效。
- 两个参数转换入口在 FFmpeg 返回错误时也发布实际对象状态；FFmpeg 转换函数可能先释放旧 extradata 再返回错误。
- flush 只同步调用点实际修改的三个 discard 字段；close 不进行配置刷新。

真实运行验证：`dev/subdec_extradata_probe.c` 包含生产 bridge 实现，链接同一份 FFmpeg 9.0.2 静态依赖；`tests/test_subdec_lifetime.py` 在临时 Catalyst 库中运行。测试使用与报告相同大小的 279988-byte extradata，检查借用指针所有权及内容，并调用实际 PGS decoder。没有模拟分配器或用源码文本匹配替代行为验证。

修改前五项全部失败：字幕解码、上下文转参数、参数转上下文、flush、close 都失去 extradata 的有效所有权对应。修改后全部通过；额外验证连续 PGS 清屏、1920×1080 画布状态保留、open 后设置时间基生效、flush 的 discard 设置生效。

同类静态检查覆盖 context/params 全部同步调用点，以及 demux packet/extradata、decoded frame 和 subtitle plane 的所有权交接。demux extradata 独立复制，packet 保留 modern buffer 引用，frame 的最后一个 legacy 引用释放 modern frame，subtitle 像素面保留至 subtitle 释放；未在这些路径发现相同的“刷新释放所有者、shadow 留旧指针”缺口。这不等于对全部并发或内存问题的完整证明。

验证记录：

- `.venv/bin/python -m pytest -q --tb=short`：140 passed，1889 subtests passed（45.24 秒），仅运行一次全量。
- `.venv/bin/python -m npabridge.build_bridge --dylib ffmpeg-core902`：iOS arm64 / 48 exports 等全部构建检查通过。
- `.venv/bin/python -m npabridge.patch ../nPlayer_3.13.0.ipa --dylibs-dir build --dylib libass --dylib ffmpeg-core902 --dylib ffmpeg-out448 -o build/nPlayer_3.13.0-core902-pgs-extradata-fix.ipa`：35 项检查通过。
- 新 bridge UUID：`0EFE92E9-22E9-3793-A5BD-96F0F26B8F56`；main SHA256 仍为 `c0990ab70cf7c79cada52923fe889b7e4b3a801963abcdb59434aae88369b2b8`。

设备复测（用户反馈，2026-09-30）：使用本轮复测包，原崩溃 MKV 打开、PGS 显示、拖动进度、字幕切换，以及 ASS 显示均正常。本次原样本的 iOS 崩溃修复已验收；extradata 生命周期修正获得设备行为验证。

本次范围消融：删除无必要的逐包/flush/close 全量刷新，复用既有 shadow 发布函数；未增加依赖、生产辅助框架或兼容层。保留五项实测失败的生命周期回归验证。最初记录的无输出释放/异常空位图属于其他缺口，本次未混入修正。


## 后续指针检查：padding（2026-09-30）

**Confirmed（源码契约缺口）**：普通 context/params extradata、AV1 已有 record/新增 record、demux extradata 和无 buffer packet 共六条复制路径，原先只分配有效数据长度。4.4.5 `libavcodec/codec_par.h:70` 与 9.0.2 `libavcodec/codec_par.h:68` 均要求 extradata 额外具有 `AV_INPUT_BUFFER_PADDING_SIZE`（64）字节的零尾部；packet 的解码输入有相同要求。`av_memdup` 不添加 padding。9.0.2 AAC 初始化直接将 extradata 交给 bitstream reader；缺口确定，具体越界/崩溃仍取决于消费者及内存布局。

已修正六条复制路径，逻辑长度及 AV1 shadow 内部偏移保持原语义；新增 AV1 header 前检查 int 长度溢出。context/params/AV1 共用局部 padding 复制函数，demux 维持局部实现。extradata 分配失败明确 abort；无 buffer packet 分配失败沿用错误返回。

真实 Catalyst probe 使用实际 FFmpeg 分配，先以 `malloc_size` 检查容量，再读取 64-byte 零尾部，同时验证载荷和逻辑长度。六项回归修复前均以“buffer lacks capacity for FFmpeg padding”失败；修复后连同原有五项生命周期回归共 11 项通过。该失败/通过对照作为本步骤消融：去掉新增分配空间将恢复已观察到的容量失败。没有添加通用内存框架、依赖或导出接口。


## 后续指针检查：空字幕释放（2026-09-30）

**Confirmed（真实 decoder + bridge 回归）**：合法的仅 presentation PGS 包产生 `ret=14,got=0`；未知 segment 配合 `AV_EF_EXPLODE` 产生 `AVERROR_INVALIDDATA,got=0`。按 app 各出口释放的既有契约调用桥接 free，修复前两条路径均 SIGABRT（进程返回 -6）。正常清屏 `got=1,num_rects=0` 对照路径原本通过。该证据确认空对象登记缺口，不依赖损坏媒体样本或平台内存布局。

已让无输出/失败路径先释放 partial modern subtitle 内部资源，再将空对象登记并物化；free 仍统一通过原有登记路径回收。返回码和 got 保持实际 decoder 结果。来源不明的字幕对象仍 abort。测试预填 legacy 输出为非零数据，每个场景重复 decode/free 三次，验证输出为空、释放后登记消失及结构归零。修复后与 padding/原生命周期回归合计 14 项通过。失败/通过对照完成本步骤消融；复用了已有登记/物化/释放函数，没有宽容未知对象的回落路径。

### 暂缓：缺少实机触发证据的指针风险

- **Hypothesis（并发触发）**：registry find 返回指针前已解锁，若另一线程同时销毁同一对象则可能 UAF；尚无 app 同对象并发释放证据，不加锁体系或引用计数。
- **Confirmed（输出形态），Hypothesis（app 非法消费）**：异常 PGS 可返回空 bitmap rect；当前桥接原样镜像。是否引发 app 指针消费异常未证实，不过滤矩形。
- **Confirmed（残留字段），未确认调用路径**：frame wipe 释放 legacy buffer 后未清除 data/linesize；如果 app 不先 unref、直接再次 receive 且返回 EAGAIN，会留下无 buffer 的旧 data。现有流程说明 app 通常先 unref，未确认实机存在该复用路径，暂不改动。
- **Confirmed（回收缺口），所有权待细分**：legacy helper 可向 shim context shadow 写入自有 extradata；bridge 导入复制/销毁只回收 modern 缓冲，无法回收此类外部自有分配。descriptor 路径又可能是借用数据，不能据此统一 free；本轮不推测释放，也不新增所有权标记。

本轮只修复 padding 与空字幕释放两项，不重做上轮所有权审计，也不将暂缓问题混入修改。


### 本轮交付验证（设备复测待完成）

- 修复提交：`84769c3`（padding）、`49fb0b1`（空字幕释放）。
- `.venv/bin/python -m npabridge.build_bridge --dylib ffmpeg-core902` 成功；arm64 iOS 13.0，48 exports，7 项 bridge 检查通过。新 dylib UUID：`D1A1F049-EE24-3B00-82FB-A39EC3AD816C`。
- 完整测试仅运行一次：`149 passed, 1889 subtests passed in 44.93s`。其中新增六项 padding 和三项空字幕回归已分别观察到修复前失败、修复后通过；清屏场景是既有正常路径对照。
- 复测 IPA：`build/nPlayer_3.13.0-core902-pointer-fixes.ipa`；使用 libass + ffmpeg-core902 + ffmpeg-out448，35 项打包检查通过。
- 包内 core902 SHA-256：`f5de02fabf8242742057b1820ea468478bf2a21a047db7fa2d764a747cb56ed8`；主程序 SHA-256：`c0990ab70cf7c79cada52923fe889b7e4ffea47cc0481d37b00bfd22285d3fa5`。

本轮本地验证完成；设备验收待用户反馈：原 MKV 打开、PGS 显示、拖动进度、字幕切换、ASS 显示，以及已有 AAC/AV1 样本打开与播放。上轮设备通过结果仅覆盖上轮产物，不代替本轮验收。

最终范围消融复核：生产修改仅两处 bridge 文件，无新依赖、导出、并发机制或未知对象回落；测试复用现有临时 Catalyst dylib/loader，未加入源码匹配测试。新复制仍先取得副本再释放旧 extradata，空字幕仍由 app 原释放站点驱动回收；未触碰四类暂缓风险。
