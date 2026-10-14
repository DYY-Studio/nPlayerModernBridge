# Renderer 原函数定位：LiveContainer 与 Plus（2026-10-14）

## 结论与证据

Confirmed：本次故障路径是 renderer 对非接管格式的原函数转发。用户随后确认其他格式也复现，并选择标准 YUV420P8 作为设备验收输入。

- 崩溃日志 main UUID `B556D1F9-5E89-39B7-81D0-C27A07CAFFF2` 对应 Plus。
- LR `0x106206D38` 去 slide 后为 `0x1001C2D38`，前一条是 renderer manifest 调用点 `0x1001C2D34`；原目标 `0x1001C294C`。
- 普通版对应调用点 `0x100A23314`，原目标 `0x100A22F2C`。
- 日志 renderer UUID `15D92C8E-3734-377D-AE39-986FE366037E` 与本地旧构建一致。旧反汇编调用 `_dyld_get_image_header(0)`，加 `0xA22F2C` 后 `br x2`。
- LiveContainer 基址 `0x102380000 + 0xA22F2C = 0x102DA2F2C`，等于日志 PC、FAR、x2。系统报告 `CODESIGNING 2 Invalid Page`，目标处于只读文件映射。

因此旧实现同时存在宿主基址选择错误和普通版偏移误用于 Plus 两项缺陷。精确像素格式未从原日志读出，但不是定位修正的前提。

首次核对发现现有 `nPlayer.i64` 对应普通版，未将其地址套用于 Plus；Plus 调用点随后以本地 Plus 可执行文件的原始指令验证。IDA 只读会话已关闭且不保存。

## 排查边界与实现

排除用户明确搁置的 curl 分支。其余生产 bridge 中只有 renderer 直接按 dyld 第 0 个镜像定位原主程序函数。FFmpeg/libass 桥没有同类地址计算；payload 使用所选 manifest 的相对分支及 dlsym/dladdr stub，patch 入口按主程序 pin 选择 manifest。

渲染器入口保留调用者返回地址，用 dladdr 获取其镜像基址，按返回偏移匹配两个宿主记录后调用正确原函数。未知调用者或 dladdr 失败明确记录错误并返回 NULL。pin 由打包阶段校验，运行时只匹配已冻结调用点偏移，不执行文件哈希。

`__TEXT,__npa_renderer` 为 96-byte 只读表，从两份 manifest 生成。构建验证两个版本，打包验证所选版本；缺失表的旧桥以及损坏偏移都拒绝。导出和 ABI 不变，高位深转换实现不变，无新依赖或并发机制。

## 本地验证

- 原生 probe 包含生产 renderer 和 plane-copy 实现，控制 dladdr 与返回地址输入，实际调用两个正确类型的原函数，验证 YUV420P8 的参数和返回值、普通版/Plus 不同基址、未知调用者、失败查询及 NULL 基址。
- 同一 probe 针对旧生产代码运行，返回 -6（断言失败）；新代码通过。该对照验证转发逻辑，不代替真机 loader 验收。
- 定向测试：9 passed、13 subtests passed；涵盖绑定、转发、破坏绑定拒绝打包、plane-copy、高位深 manifest、Plus preflight。增加旧表缺失场景后只重跑对应检查：1 passed。
- iOS renderer 构建通过 8 项检查，Plus 记录同时通过验证；反汇编确认 caller 在 dladdr 前保留且两版本偏移正确。
- 普通版、Plus 两份 IPA 的整个 patch/packaging 流程均成功，四个 dylib 为 libass、renderer-highbit、ffmpeg-core902、ffmpeg-out448。

范围消融与性能复核：只替换原函数定位，未引入镜像扫描或缓存；转发路径一次 dladdr、最多两次记录比较，高位深路径不增加定位工作。绑定表 96 bytes，未签名 renderer 从 69880 增至 69888 bytes。缺失/损坏绑定的失败对照验证校验确实阻止旧产物进入包。

## 复测产物

新 renderer UUID：`66F953D4-6A0F-3ED7-8B61-4978282CAE35`。

构建 dylib SHA256：`a6dc7d8f1aaead8eb5c6005790de21275a9179d33c74e14d19f2ded548bdb754`。
两份 IPA 中重签后的 renderer SHA256 均为 `18132ad688d9cbfcefdfc5c392d310f3551edc41cf0979c108493232aa19d9af`。

| 产物（build/） | IPA SHA256 | 包内 main SHA256 |
| --- | --- | --- |
| nPlayer-standard_3.13.0-renderer-host-fix.ipa | `8c823e75d0b3195dc1d2fa892509692c2fef94796baf7e77ac941f0339c854ee` | `63bedc52d2f88d49f8e0805854abb6832a180349b3919d1b886b3e0da614e369` |
| nPlayerPlus_3.13.0-renderer-host-fix.ipa | `332d29d2802855dd35f7eb39268d7dd50e7e27163fac5fb96caf794ed05097f3` | `2164d650b13bee5d741f96bdf14adbcd84f71239d4c98734d2ca98e6ad755e1f` |

实现提交：`3110f7d`。复测包及日志在忽略的 build/ 中，源码和记录提交到当前分支。

## 设备复测通过（2026-10-14）

用户反馈：“设备复测通过，未崩溃”。此前约定以标准 YUV420P8 检查原渲染器路径；本次反馈确认设备复测未再发生崩溃。

用户未逐项列出普通版、Plus 和高位深样本结果，本记录不补充逐项通过结论。
