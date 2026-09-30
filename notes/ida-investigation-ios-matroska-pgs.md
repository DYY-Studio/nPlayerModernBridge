# iOS Matroska/PGS 静态排查（2026-09-30）

范围：当前 `nPlayerLibassBridge` 源码、本地 FFmpeg 4.4.5 / 9.0.2 源码、已有逆向记录。用户观察：iOS 打开视频即崩溃，macOS 能显示字幕。本轮没有打开 IDA、修改实现、运行播放或测试；不将源码缺口等同于本次已确认根因。

环境：项目有 `.venv/`；根目录不是 Git 仓库，`nPlayerLibassBridge/` 是仓库，排查开始时工作区干净。

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

## 下一步最小取证

优先取得 iOS 崩溃线程、异常类型和 bridge 偏移：若为 `SIGABRT` 且落在 `npa_subdec_avsubtitle_free`，先核对第 1 项；若落在 `sub_100A05EEC`、frame 分配/拷贝或 palette 转换，核对第 3 项及第 2 项是否制造空位图。

只需记录首批 PGS 解码的 `ret/got/num_rects`、shadow/modern 宽高及矩形 `w/h/data[0]/data[1]/linesize[0]`。同时确认 iOS 与 macOS 使用相同 bridge 版本和选中单元；否则平台差异不能直接用于归因。

不能从平台差异推断 iOS “内存模型更严格”；非法访问可能随分配器、内存布局、系统实现或线程时序不同而呈现不同结果，而显式 `abort()` 要按控制流定位。

## 范围消融审查

未添加防御框架、兼容层、依赖或测试。删去与当前 PGS 链路无直接证据联系的通用 side-data、OOM 和并发猜测，仅保留三个具体边界及其触发条件。实现是否需要改动留待本次崩溃证据确定。
