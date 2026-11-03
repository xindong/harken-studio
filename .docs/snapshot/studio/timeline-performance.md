# Timeline 拖动性能基准

`harken_timeline_benchmark` 使用真实 `TimelineEditorPanel`、鼠标移动事件与 Qt 事件循环，测量 clip 拖动。目标默认不参与普通构建，在已开启 Editor 单元测试的开发配置中显式构建；它不是 GTest 测试，也不对业务细节写断言。

```powershell
cmake --build build-nc --config RelWithDebInfo --target harken_timeline_benchmark
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 200 --lanes 4 --frames 40 --warmup 5 --snap both --tag local --output .temp/timeline-perf-200.json
```

目标复用 Editor 的库、资源和部署目录。基准默认使用 Qt `offscreen` 平台；可通过 `QT_QPA_PLATFORM` 指定平台，但比较时必须保持平台一致。Windows 离屏字体默认来自系统 Fonts 目录。程序只创建临时项目、临时设置和合成的六秒单声道音频，不播放声音、不打开用户项目。

macOS 使用同一构建命令及无 `.exe` 后缀的可执行文件；基准单独发布 schema，避免依赖 Editor `.app` 内的路径。

## Windows 拖动延迟复核

首轮离屏优化没有覆盖用户报告的实际卡顿。用户确认默认网格吸附正确，问题是拖动时鼠标和 clip 同时出现明显延迟。Windows 平台对照将阻塞定位到 `TimelineCanvas::applyDragPreview()` 中每次移动调用的 `QToolTip::showText`。按照用户要求，已删除移动时的 Start 浮动提示，没有替换为其他提示或改变吸附行为。

独立临时工程使用 Qt `windows` 平台、4 个带波形 clip、1 条轨道、1200×540 面板；以 8 ms 定时器安排 80 次移动，每次前进 1 px，无预热。窗口使用 `Tool`、`WindowDoesNotAcceptFocus` 和 `WA_ShowWithoutActivating`，输入为进程内 `QMouseEvent`，不会操作用户工程。before 和 after 复用同一个测量对象文件，before 的临时 Canvas 副本只在 tooltip 调用外计时，产品代码没有计时埋点。

| 操作 | 吸附 | 鼠标处理 p95 | frame p95 | canvas 绘制 p95 | tooltip 创建次数 |
| --- | --- | ---: | ---: | ---: | ---: |
| 移动，移除前 | 开 | 147.25 ms | 157.92 ms | 1.33 ms | 80 |
| 移动，移除前 | Alt 关闭 | 143.96 ms | 152.91 ms | 1.36 ms | 80 |
| 拉伸，对照 | 开 | 1.48 ms | 4.87 ms | 1.15 ms | 0 |
| 拉伸，对照 | Alt 关闭 | 1.64 ms | 5.79 ms | 1.04 ms | 0 |
| 移动，移除后 | 开 | 1.60 ms | 6.91 ms | 1.10 ms | 0 |
| 移动，移除后 | Alt 关闭 | 1.26 ms | 6.98 ms | 1.07 ms | 0 |

移除前 `QToolTip::showText` 的同步耗时均值为 134.54 / 134.45 ms，p95 为 147.05 / 143.67 ms（吸附开 / 关），占据绝大部分鼠标事件处理时间。对照的拉伸操作不调用此函数，仍保持低延迟。移除前后每一步鼠标坐标、clip 预览位置与时长完全相同，均产生 80 次 Paint，实际位置变化为 9 / 80 帧（吸附开 / 关）。相关回归 68 项通过，1 项因符号链接环境限制跳过。

这些是 Windows 独立无焦点窗口的结果，能够确认该平台路径的阻塞与修复效果，不是用户完整编辑器的屏幕帧率；定时等待的 8 ms 不计入 `frame_ms`。优化前基线 `8722319` 也含同一 tooltip 调用，尚未确定用户此前主干版本为何没有触发相同阻塞，不将本次发现归因于某个新增 tooltip 调用。

本地原始数据：`.temp/timeline-native-review/before-step1.json`、`before-resize.json`、`after-step1.json`，修复前后二进制分别保留。Windows 复测命令：

```powershell
$env:QT_QPA_PLATFORM='windows'
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 4 --lanes 1 --frames 80 --warmup 0 --gesture move --interval-ms 8 --step-px 1 --output .temp/timeline-native-move.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 4 --lanes 1 --frames 80 --warmup 0 --gesture resize --interval-ms 8 --step-px 1 --output .temp/timeline-native-resize.json
```

入仓 benchmark 的同参数 Windows 复测：移动 frame p95 为 6.79 / 6.22 ms，拉伸为 6.27 / 5.39 ms；每次均 80 次 Paint、0 个 tooltip 创建，位置变化和步长保持 9 / 80 帧、10 / 1 px（吸附开 / 关）。20 clip 离屏冒烟与非法参数检查也通过，记录位于同目录 `persistent-*.json`。

## 审计结论与修改

- 坐标转换反复调用 `viewSpan()`，后者反复遍历全部 clip；每次 clip 查询又通过项目树查找所属轨道，使绘制和吸附出现嵌套扫描。轨道直接保留当前对象，内容起止范围合并为一次惰性计算，吸附循环复用跨度和视口宽度。
- clip target 的对象身份按 GUID 缓存，属性、移动、创建、删除通知同步失效；视图重建和项目关闭也清理缓存。名称、颜色、平台、语言、源范围等内容仍从当前对象读取，避免缓存过期的业务状态。
- 时间刻度按视图起点、跨度、宽度和字体度量复用；标题字体度量每次绘制只计算一次，标题遮挡按实际纵坐标分别处理，保留当前起点锚定和缩略规则。
- 不再为不可见轨道准备 clip 绘制内容；同帧重复 target 共用源信息。悬停提示按需生成，失败的波形缓存读取不再逐帧重试，收到缓存生成完成通知后重新加载。

性能优化本身保持波形采样精度与已确定的拖动、吸附、跨轨归属、批量撤销和标题绘制语义；界面统一和跨轨编辑的行为变化另见颜色快照。`Timeline` / `Theme` 回归共 69 项，68 项通过，1 项因环境不支持符号链接而跳过。新增回归覆盖模型通知后的坐标一致性、删除与撤销后的对象身份、轨道重排以及预览不修改模型、释放后单次可撤销提交。另检查实际 Qt 离屏截图的浅色／深色、重叠标题、窄 clip、紧凑轨道、滚动和波形幅度缩放，并验证波形负缓存能在生成完成后恢复。

## 参数与复测

参数：

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--clips` | 200 | clip 总数，每个 clip 有独立 Sound 对象 |
| `--lanes` | 4 | 轨道数；超过可见高度时覆盖纵向不可见轨道 |
| `--waveform` | on | `on/off`；开启时共用一个真实音频源和波形缓存 |
| `--hidden-clips` | 0 | 放在当前可见时间范围之外的末尾 clip 数 |
| `--frames` | 40 | 每次手势计入统计的移动事件数 |
| `--warmup` | 5 | 每次手势预热的移动事件数 |
| `--snap` | both | `on/off/both`；关闭吸附使用 Alt 修饰键 |
| `--gesture` | move | `move/resize`；移动或拉伸 clip 右边界 |
| `--interval-ms` | 0 | 每次事件前的定时等待；0 为连续发送，等待不计入 frame 耗时 |
| `--step-px` | 7 | 采样轨迹的像素步长；1 用于检查连续小步输入 |
| `--tag` | local | 报告标签 |
| `--output` | 标准输出 | JSON 文件路径 |

布局固定为 1200×540，轨道高度 72，左侧标题宽度 180，显示约十秒时间范围。每个轨道的普通 clip 分布在 2～8 秒、持续一秒，形成密集重叠。用于拖动的第一个 clip 从 0.5 秒开始，持续 0.7 秒；轨迹和预热固定。手势结束通过 Escape 取消，测量不包含提交、撤销、项目创建和首次波形加载。

建议比较矩阵：

```powershell
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 20 --lanes 4 --output .temp/timeline-perf-20.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 200 --lanes 4 --output .temp/timeline-perf-200.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 200 --lanes 4 --waveform off --output .temp/timeline-perf-200-plain.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 200 --lanes 4 --hidden-clips 180 --output .temp/timeline-perf-hidden.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 1000 --lanes 40 --output .temp/timeline-perf-1000.json
build-nc/bin/RelWithDebInfo/harken_timeline_benchmark.exe --clips 1000 --lanes 4 --waveform off --output .temp/timeline-perf-1000-plain.json
```

每个阶段输出毫秒计的 mean、p50、p95、max：`move_ms` 是同步鼠标事件处理，`event_loop_ms` 是随后处理排队事件的耗时，`frame_ms` 是两者之和；`canvas_paint_ms` 通过 `QApplication::notify` 包围实际 canvas Paint 事件，属于前两阶段内部耗时，不能再次相加。`paint_count`、`moving_frames` 和 `max_preview_step_px` 说明实际绘制与位置变化，拉伸时统计右边界；`tooltip_creates` 记录采样期间的 Qt tooltip 对象创建次数。

报告还记录配置、源码提交、dirty 状态、Qt 版本、操作系统与平台。采样前完成正式构建，避免同时编译或运行其他重负载程序；同一台机器、同一构建配置、同一参数比较前后数据，同时观察 clip 数量增加时的增长率。离屏结果不包含桌面合成、显卡呈现和完整主窗口其他面板的响应，不能直接等同于用户屏幕帧率。

## 2026-11-03 首轮优化前后测量

使用同一临时 Qt 基准程序、相同的对象与音频生成方式、视口、轨迹和事件循环，在 Windows / Qt 6.11.0 / RelWithDebInfo / offscreen 下比较。旧可执行文件独立保留，优化后重新链接产品静态库。20 和 200 clip 场景分别预热 5 帧、测量 40 帧；1000 clip 场景预热 3 帧、测量 12 帧。结果为单轮样本，没有多次独立运行的置信区间。

| 场景 | 吸附 | 优化前 frame p95 | 优化后 frame p95 | 优化后 paint p95 |
| --- | --- | ---: | ---: | ---: |
| 20 clip / 4 轨 / 波形 | 开 | 20.26 ms | 5.30 ms | 3.10 ms |
| 20 clip / 4 轨 / 波形 | Alt 关闭 | 18.53 ms | 4.22 ms | 2.42 ms |
| 200 clip / 4 轨 / 密集波形 | 开 | 2091.67 ms | 17.19 ms | 13.68 ms |
| 200 clip / 4 轨 / 密集波形 | Alt 关闭 | 1644.79 ms | 16.25 ms | 13.64 ms |
| 200 clip / 4 轨 / 无波形 | 开 | 未测 | 6.22 ms | 4.26 ms |
| 200 clip / 4 轨 / 180 clip 在可见时间之外 | 开 | 未测 | 4.79 ms | 2.18 ms |
| 1000 clip / 40 轨 / 可见区域含 175 clip | 开 | 未测 | 13.97 ms | 12.69 ms |

200 clip 的常规拖动均值从 1997.97 ms 降至 15.04 ms。其鼠标事件 p95 从 415.56 ms 降至 1.41 ms，canvas 绘制 p95 从 1692.87 ms 降至 13.68 ms。优化后可见 clip 的波形绘制仍占主要成本；1000 clip 场景只绘制可见轨道内的 175 clip，因此不能据此声称 1000 个同时可见的密集波形也达到相同耗时。

旧版 200 clip 已接近每帧两秒，未继续运行旧版 1000 clip 矩阵；表中空缺不使用外推值填补。`canvas_paint_ms` 是事件处理内部的实际 Paint 耗时，不能再加到 `event_loop_ms` 或 `frame_ms`。持久 benchmark 保留相同的负载生成、固定输入轨迹及采样定义，新增参数和单场景报告以方便后续复测。

本地原始报告为 `.temp/timeline-perf-review/before.jsonl` 与 `.temp/timeline-perf-review/after.jsonl`，旧版二进制为同目录 `TimelinePerfBenchmark-before.exe`；这些临时文件不入仓。

## 性能优化阶段复测

加入 target 身份缓存后，用入仓的 `harken_timeline_benchmark` 重新测量，构建配置、平台、视口和输入生成规则同上。所有场景均预热 5 帧、采样 40 帧，测量期间没有并发构建或测试。200 clip 场景首轮有较高尾延迟，因此追加两次独立运行；最终移除不再使用的逐 clip tooltip 字段后又补测一次，保留四轮全部结果。

| 场景 | 吸附开启 frame p95 | Alt 关闭 frame p95 | 实际绘制 clip 数 |
| --- | ---: | ---: | ---: |
| 20 clip / 4 轨 / 波形 | 8.48 ms | 5.19 ms | 20 |
| 200 clip / 4 轨 / 密集波形，四轮范围 | 16.78–29.48 ms | 17.56–19.30 ms | 200 |
| 1000 clip / 40 轨 / 波形 | 15.30 ms | 15.43 ms | 175 |
| 1000 clip / 4 轨 / 无波形 | 12.54 ms | 13.94 ms | 1000 |

200 clip 吸附开启的四轮 frame p95 依次为 29.48、17.16、16.78、19.62 ms，均值依次为 18.58、15.46、15.31、15.56 ms；对照旧版 1997.97 ms 均值，改善约 108–130 倍。四轮 paint p95 为 24.10、13.83、13.57、16.83 ms，鼠标处理 p95 为 2.46、2.51、2.40、1.30 ms。主要成本仍是可见波形绘制，不能声称严格满足每帧 16.7 ms。

全部报告 `complete=true`，每次手势记录 40 次 Paint。吸附开启时，20 / 200 / 1000 波形 / 1000 无波形的实际位置变化分别为 28 / 35 / 35 / 36 帧，Alt 关闭吸附均为 40 帧。报告为 `.temp/timeline-perf-review/final-{20-wave,200-wave,1000-wave,1000-plain}.json`，追加测量为 `final-200-wave-repeat-1.json`、`final-200-wave-repeat-2.json` 和 `final-200-wave-cleanup.json`。这些是本机离屏测量，未测量完整编辑器的桌面呈现延迟。
