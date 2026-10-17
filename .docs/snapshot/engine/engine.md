# HarkenEngine 现状快照

最近核对：2026-07-30。路径缩写：`Runtime/` = `HarkenEngine/Source/Runtime/`，
`Tooling/` = `HarkenEngine/Source/Tooling/`，`Tool/` = `HarkenEngine/Tool/`；测试指 `HarkenEngine/Test/`。

## 目录与开发边界

本节与远程通道于 2026-10-17 核对，其余条目保留各自核对日期。

| 目录 | 归属 |
|---|---|
| `HarkenEngine/Source/` | 引擎运行时、产品使用的库与共享代码生成。`Tooling/Preview` 是 Studio 使用的预览库；ABI、SDK Binding、Effects、VariableCatalog 等生成区域仍在此处 |
| `HarkenEngine/Tool/` | 内部开发工具：`Host` 提供最小引擎宿主与运行探针，`Qualification` 提供资格检测工具，`Remote` 提供宿主侧命名管道服务、Python 客户端及生成的协议代码 |
| `HarkenEngine/Test/` | 单元、端到端、性能等测试与夹具；工具自身测试放在 `UnitTest/`，管道验收入口为 `E2E/remote_acceptance_drive.py` |
| `HarkenEngine/ThirdParty/` | 随仓维护的第三方依赖 |
| `HarkenEngine/Examples/` | 面向引擎接入方的使用示例 |

目录按用途划分，不按语言划分。Python 管道客户端归入 `Tool/Remote/`，给出通过或失败结论的下混参考验收归入 `Test/E2E/downmix_null_reference.py`；全仓构建、发布和质量门禁编排仍由 `.scripts/` 承担。

`Tooling/Remote/wire/harken_wire_table.json` 同时供 ABI 与 SDK 生成使用，因此该表和 `gen_wire.py` 保留在 `Source/`。内部管道服务及其生成产物归入 `Tool/Remote/`，不把共享产品合同随开发工具一起迁走。

## 渲染与混音

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 三线程骨架与实时纪律 | 逻辑/音频/流式三线程，音频线程零分配零锁零 IO，命令块边界生效 | `api/src/DeviceBackend`→`render/MixerLoop` 直调，`core/CommandQueue`+`AllocGuard`+`BlockArena` | 一致 |
| 固定块节奏 | 恒渲染整块，游标跨回调拷帧，≤1 块延迟换 ramp 语义稳定 | `api/src/RenderSlicer` 纯索引算术，跨设备重建存活 | 一致 |
| 逐 voice 渲染级联 | 解码重采样→插入效果→滤波→增益→折叠汇入，尾音排空、采样精确启停、代际句柄 | `render/Voice`+`VoiceCascade`+`VoicePool`，golden 测试群钉死 | 一致 |
| 作业并行 | worker 只并行级联，折叠按槽位序串行，结果与线程数无关 | `render/RenderWorkerPool` 三段式，串/并行逐字节相同，默认串行 | 一致 |
| voice 容量与抢占 | 超容量如实拒绝；抢占列为"未来特性" | 完整容量模型+三种 steal 策略+优先级+受害者通知+declick（`core/VoiceLedger`） | 偏差 |
| 总线树与 send | master 为根的树+加性 send 边（bus→bus 与 voice→bus），环检测拒绝 | `render/BusTree`+`VoiceCascade` 双级 aux send，feeds-first 排序 | 一致 |
| 拓扑计划与换入 | 逻辑侧编不可变计划、块边界换入、预检背压零副作用 | `render/TopologyPlan` 如设计；另新增 `Vn_ApplyBusTopology` 整快照+revision 乐观并发+代际槽位复用 | 一致 |
| 多声道与下混 | 统一混音布局，master→设备与逐总线输出两处矩阵折叠 | `core/ChannelLayout`+8×8 矩阵+`Vn_SetBusDownmixMatrix/OutputLayout` | 一致 |
| 总线效果链与 fader 点 | 复用 voice 效果链，fader 点切前后两段，块边界原子换位 | `Vn_SetBusEffect/SetBusFaderPoint` 已公开 | 一致 |
| 计量与侧链 | 每总线块峰值入快照；计量变量+绑定拼出 sidechain | `core/MeterBlockRing`+`param/MeterSystem`，已扩为三 tap 可选点 | 一致 |
| 增益 ramp 契约 | 幅度域线性、整块切分、首设跳变、途中重设从当前值起步 | `dsp/GainRamp`，voice/bus 共用公式 | 一致 |
| 循环 | 内存内源级回绕无缝+流式生产者侧循环，一次/N 遍/无限 | `VoiceCascade` wrap 不重置重采样器+`media/StreamManager` 回绕，接缝容差 0 | 一致 |
| voice 插入效果链 | 固定槽、空槽零开销、干湿旁路仍处理、池化构造-移交-退役 | `render/EffectChain`+`EffectPool`+退役环 | 一致 |
| 内建效果集合 | 仅反馈延迟+三段 tone 两个原子 | 5 种（+gain/limiter/compressor），生成式注册表 `Tooling/Effects/`，参数可绑定实时驱动 | 偏差 |

## 供给与媒体生命周期

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 流式供给 | 专属流线程唯一碰文件、每 voice 无锁环、预缓冲门、饥饿≠结束 | `media/StreamManager`（2ms 节拍、逐 chunk 校验）+`core/StreamRing` | 一致 |
| 媒体运行时 | 加载全有全无事务、引用闭包先集齐、常驻/内容两类包、来源归属、内容地址化身份 | `media/PackFormatV2`+`PackLogicImage`（15 类引用闭包）+cookie 归属+内容哈希 | 一致 |
| 热重载（代际） | 退旧代新触发走新版、每资产至多一个退役代、纯逻辑数据 tick 边界原地换 | PCM/stream/pack 三动词+attenuation/binding 就地换，金样钉死 | 一致 |
| 整包替换 | 暂存-间隙-固定激活点、事务性、漂移复查成建制丢弃 | `api/src/PackLogicStore` staged revision+diff 幂等+激活顺延 | 一致 |
| 可闻性停泊 | 滞回、游标镜像权威、虚拟时间按解析音高推进、返回即全新发声、默认关 | `core/VoiceLedger`+`SourceCursorMirror`+ParkRecord；流与有限多遍循环不停泊 | 一致 |
| 设备后端 | 编译期定候选集、空后端独立模式、范围外平台如实失败 | `api/src/PlatformBackend`（WASAPI/CoreAudio/AAudio→OpenSL/其余空集）+`render/NullBackend` | 一致 |

## 观测与远控

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 一致快照与阶段抽头 | 音频侧无等待发布、读者付代价、块内自洽、抽头只观测不介入 | `core/PinnedSnapshot3`（三槽 pin 变体，契约同）+StageTap，撕裂压测覆盖 | 一致 |
| trace 录制与回放 | — | `api/src/TraceRecorder`（全 API 调用流）+`harken_host trace-replay`；历史 CurrentWorld 仅支持有限诊断，明确拒绝 pre-#443 V5–V7 内嵌 Binding/Curve，范围与样本见[历史诊断约定](../../../HarkenEngine/Test/UnitTest/fixtures/current-world-pre443/README.md)（本项核对：2026-09-08） | 未记载 |
| 内部宿主管道 | 宿主侧工具通道、逻辑线程安全点执行、单一描述表生成+门禁防漂移 | `Tool/Remote/` + `harken_host remote-serve`；Windows named pipe 连接 Python 客户端，用于独立驱动引擎、验证 Pack 热替换、快照与可选 trace 回放；不进入游戏运行库 | 一致 |
| Studio / 游戏远程连接 | 游戏进程内提供通信，Studio 连接运行中的引擎进行观测与控制 | `Runtime/remote/` 提供 TCP 通信与 UDP 发现，SDK 调用 `Vn_RemoteStart` 启动，Studio `RemoteConnection` 使用独立的 `Protocol.h`；本地试听仍通过进程内 C API | 已实现 |

## 对象与参数（运行时侧）

Action、Variable 与 Emitter 消费边界于 2026-09-21 重新核对；其余行保留原核对日期。

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 容器·选择 | 随机/轮询/权重/变量匹配，(容器,发射器) 独立游标与避免重复窗口 | `param/ContainerTable` 两段式选择管线+嵌套深度 8+每对状态 LRU | 一致 |
| 容器·扇出 | 全部子项同时发声、采样级对齐 | 代码无全播容器，只有单选管线；多分量叠层需上层多次 Play | 缺失 |
| Action | Play、Stop、PostAction、Set Variable、Reset；先解析后执行 | CurrentWorld Action program 的 SetVariable 步骤独立携带 Global/Emitter；Emitter 来自此次触发，缺失时拒绝；Global 不受触发携带的 Emitter 限制 | 一致 |
| Variable 写入 | 定义无 scope，统一 GlobalWithEmitterOverride；全局赋值同时清除该 Variable 全部局部覆盖 | `param/VariableService` 在同一应用边界提交全局值与覆盖清除；相同全局值仍有效；保留未来局部写入、Action 顺序及原子批次，失败恢复旧投影 | 一致 |
| Binding 消费上下文 | 局部值以 (Emitter, Variable) 共享；Layer 按后代播放 Emitter 求值，Bus 只读全局 | `ParamSystem` 的 voice 实例关联 Emitter；Layer 附着下放到后代实例；`BindingTable` 的 Bus 求值不携带 Emitter。路由、Layer、Action 和 Play 不新增局部 bank | 一致 |
| 曲线 | 分段曲线带形状标签（线性/等功率/指数/对数/S 形/阶梯） | `param/Curve` 已换代为 ≤8 控制点分段三次 Bezier+LUT，无形状枚举、无阶梯段 | 偏差 |
| Variable 类型 | Step / Smooth，range/default 描述数值；Labels 提供稳定身份与名称到当前整数位置的映射 | `VariableService` 校验数值域，按当前 Pack 映射解析名称或身份后复用 Global/Emitter 写入；Kind 不门控 Label 与消费者 | 一致 |
| 空间化与衰减 | 衰减=属性模型作用于距离；逻辑侧求值，音频线程不做几何 | `param/Spatializer` 逐 tick 求值（3 种 rolloff+空气吸收+声锥+近场+宽度） | 一致 |

## 公开表面

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| ABI 纪律 | 单头 C 原语、只增不改、尺寸字段自述版本、三类身份、诚实结果码族 | `api/include/harken/Harken.h`（98 函数）全 structSize 版本化+AbiPin/AbiHistory 测试 | 一致 |
| 窄腰不破 | 上层丰富性编译降级为冻结的音频侧原语 | M4VocabularySceneTest 直接断言新特性不新增音频侧 opcode | 一致 |
| 无文档归宿的公开原语 | — | Bus Mute/Solo（树作用域）、`Vn_Suspend/Resume`、能力发现族、`core/RtProbe`、快照约 90 个 profiling 字段、`core/Utf8Path` 硬化、packc 增量缓存 | 未记载 |

## 备注

1. 旧概览的"刻意留在引擎之外"清单五条里四条已失效：抢占、总线效果链、公开绑定组装、Action/容器/离散变量原语均已落地，"仅立体声"也已被多声道+逐总线下混取代。
2. 容器"扇出"是预期与现实差距最大的单项：文档将其与"选择"并列为容器两大规则，引擎侧没有对应原语。
3. Variable 局部值的正式边界、Layer/Bus 消费表与三行赋值例子见 [Game Modulate](../../philosophy/game-modulate-system.md#emitter-上下文与消费边界)。公开局部 Clear 已退出，Emitter 销毁仍结束其局部状态。
4. 曲线模型是破坏性换代（pack schema vintage 2、remote 协议 major 2 联动），旧"形状标签"叙事整体作废。
5. 媒体格式真实边界：运行时零编解码（仅 pcm-f32 planar、64B 对齐）、PcmRegistry 全量常驻不逐出、packc 只吃 wav；设备丢失无自动恢复。
6. trace 录制+确定性回放是"确定性可被观测"的关键闭环，预期层从未覆盖，宜尽快补课。
7. 结果码数值、Preview ABI 与 Studio 日志映射见 [engine 结果码](engine/README.md)。
