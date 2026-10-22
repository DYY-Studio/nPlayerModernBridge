# PP-HDR 缩放管线 Alias 检查

日期：2026-10-23。检查基于 `1038ebd`，仅检查源码与生成的 MSL，未修改实现或 IDA 数据库。

## 结论

当前两阶段缩放没有正确实现水平滤波。水平预处理必定提前退出，最终 fragment 却仍按水平处理已完成来吸附横坐标。相较原始 bilinear，这直接丢失水平方向的插值和缩小低通，足以解释明显增加的锯齿与摩尔纹。即使单独修复提前退出，水平入口仍调用垂直函数，问题会继续存在。

这里的基线是同一 PP Metal 管线的原始 bilinear 采样。用户正在运行的二进制是否对应此提交，以及 PP 关闭后的系统 renderer 行为，本次未测量。

## 已确认的主因

### 1. drawableSize 返回值被丢弃

`bridge/npa_renderer_pp_hdr_bridge.c:1118–1125` 初始化 `drawable={0,0}` 后调用 `drawableSize`，没有赋值。因此 `npa_prepass_run` 必定返回 0。

`:1279–1282` 随后继续执行已经替换过的垂直 fragment，原始输入纹理仍留在 renderer 槽位。`:240` 的 `keep` 把未滤波轴吸附到单个 texel 中心，`:250` 等位置使用该横坐标，所以水平方向变成单 texel 选择，且有向左取整的相位偏差。

验证：临时 Objective-C harness 直接包含当前 bridge，实现返回 `128×8` 的 layer，并提供通过前置检查所需的 renderer 状态。实际调用输出：

```text
drawableSize returned 128 x 8
[npa-pp-hdr] separable pre-pass has no drawable size (status=-1)
actual npa_prepass_run result=0
```

### 2. npaHorizontal 调错函数

`:668–681` 生成水平实现 `npa_h_tap` 后撤销名称宏，并恢复 `NP_HORIZONTAL=0`。随后插入的 `npaHorizontal` 在 `:303` 调用 `npa_tap`，实际绑定垂直实现。水平实现虽然存在，却没有被这个入口使用。

验证：使用现有 `tests/pp_scale_rewrite_driver.c` 和捕获的 `/tmp/np-hdr-shader/host-0.metal` 生成当前 MSL。Lanczos3、AA Quality、Anti-ringing 关闭。在 Apple M4 的 Metal GPU 上，把逐列黑白交替的 `256×8` R32Float 输入渲染为 `128×8` RGBA32Float，排除边缘后读回：

| fragment 入口 | 均值 | 相对理想 0.5 的 RMS 误差 |
| --- | ---: | ---: |
| 原始 linear sample | 0.5 | 0 |
| 当前 npaHorizontal | 0 | 0.5 |
| 临时入口仅改用 npa_h_tap | 0.5 | 0 |

这是只改变函数调用的消融验证。当前入口把高频竖线错误折叠成常量黑色，直接证明水平低通缺失。验证没有运行完整应用的 draw wrapper。

## 其他已确认缺陷

### 3. 两处 attachment 下标调用缺参

`:1018` 和 `:1203` 使用无参数的 `npa_msg_id` 调用 `objectAtIndexedSubscript:`，未传入下标 0。调用 ABI 错误。修复 drawableSize 后会暴露该路径，可能造成错误 attachment、pipeline 失败或异常，实际结果取决于未传参数所在寄存器的值。本次未触发该未定义调用。

### 4. 色度半宽与最终横坐标吸附矛盾

`:1157–1158` 把 4:2:0 的中间色度宽度保留为 drawable 的一半。最终 fragment 的输出宽度却是完整 drawable，因而色度横向 footprint 是 0.5，代码注释所说的两个平面均为 1:1 不成立。

最终垂直函数仍通过 `keep.x` 吸附单个色度 texel，跳过半宽色度到输出宽度的水平重建。这在前述两项主因修复后仍会保留色彩边缘台阶。实际影片上的可见程度尚未测量。应明确色度在哪个阶段重建到输出横向网格。

### 5. Anti-ringing 的中心样本偏了半个 texel

`:271–274` 取 `base/size` 和 `(base+1)/size`，而正常 tap 在 `:248` 使用 `(base+0.5)/size`。启用 linear sampler 后，前者取的是相邻 texel 的混合值，不能表示注释中宣称的两个中心 texel 的 min/max。

例如内部区域的逐 texel 黑白交替信号，在 1:1 的 texel 中心采样时，这两个边界样本均为 0.5，clamp 会把本应为 0 或 1 的结果强制变成 0.5。该问题改变局部细节与相位，不能当作正常防振铃。

### 6. Spline36/64 首段多乘了一次 ax

`:125` 和 `:144` 写成 `1 + ax*ax*(a + ax*(b + ax*c))`，应为 `1 + ax*(a + ax*(b + ax*c))`。当前首段从三次变为四次。

与 [libplacebo 上游 filters.c](https://github.com/haasn/libplacebo/blob/master/src/filters.c) 的 `spline36`、`spline64` 对照后确认。`x=0.5` 的未归一化权重：

| 核 | 当前 | 上游 |
| --- | ---: | ---: |
| Spline36 | 0.7993421053 | 0.5986842105 |
| Spline64 | 0.8001760563 | 0.6003521127 |

当前测试中的 C 镜像也复制了同一公式错误。归一化能保持常量图像，却不能恢复正确的核形状。该偏差对真实影片 Alias 的独立贡献尚未量化。

## 配置、精度与性能检查

- `PPVideoAntiAliasing` 缺失时返回 0，即不扩宽缩小滤波核。Balance 仅扩宽至 2，超过 2:1 的缩小仍有低通不足的风险。这是现有选项语义，无法修复水平预处理失效。
- 中间纹理沿用源像素格式（`:1141`、`:1173`）。源为 UNorm 时会量化并截断负瓣产生的越界值，因此“intermediate is lossless”的注释不成立。其画质贡献需要在恢复正确管线后单独评估。
- 当前失败路径每帧输出两条日志。60 fps 下约 120 条/秒，属于可移除的持续日志开销。
- 水平入口调错后，若预处理被恢复，会额外执行一次垂直滤波与中间纹理写入，却仍不做水平低通。两阶段本身无需推翻，优先修复入口、尺寸与采样网格。
- 未进行全量测试或性能基准。本次只运行两个针对主因的临时原生探针，以及公式数值对照。临时文件位于 `/tmp/npa-pp-alias-review/`，未增加项目测试或依赖。

## 建议修复顺序

1. 接收 drawableSize 返回值，补齐两处 attachment 下标，并让水平入口调用 npa_h_tap。
2. 明确色度重建阶段，修正 Anti-ringing 中心坐标和 Spline 首段。
3. 使用同一 PP 路径的 bilinear 与修复后路径，验证高频竖线、斜线及色度边缘。恢复正确管线后再决定中间格式与 AA 默认值。

尚未验证旋转、裁剪或非满屏情况下复用宿主顶点缓冲两次是否重复应用几何变换，不能将其列为已确认根因。
