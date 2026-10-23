# HarkenSDK 现状快照

Unity SDK 最近核对：2026-10-09

2026-10-24 产品边界更新：删除 Unreal SDK 后，`HarkenSDK/Unity/Harken/package.json` 的 `version` 字段是 SDK 产品版本的唯一维护源，不再单独维护 SDK 的 `VERSION` 文件。独立 SDK 制品的清单版本须与对应源码一致；Studio 随附 SDK 的组件版本从实际随附清单读取。`HarkenSDK/Unity/Harken` 保留为 Unity 包根，`Tooling` 与 `Test` 分别保留集成工具和仓库测试；不将它们移入 Unity 包。后续仅调整 SDK 产品版本时，只修改该 `version` 字段，无需同步修改业务源码或测试。

本次核对 Unity 的公开 API、资源与宿主生命周期、Editor 服务、生成边界及随包说明。此表记录实现状态，不代表所有目标平台均完成设备验收。iOS 设备行为保留原核对结论，本轮未重新验证。

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 原语与宿主分层 | 原生音频语义由 Engine 实现，SDK 承担托管互操作及明确的宿主责任 | `Runtime/Generated/` 投影 ABI；Actions/Voice 等包装原语；PackBootstrap、回调桥及 Unity 组件管理会话、资源引用和生命周期，不是无状态的一次转发 | 一致 |
| Unity API 可见性 | 游戏和宿主使用公开包装，工具实现留在内部，Editor 服务单独公开 | `HarkenNative`、`HarkenAbi`、Snapshot/Monitor DTO、Bus 结构操作及内部生成助手已 internal；Remote 使用 `HarkenEngine` 的三个公开宿主方法，Editor 使用 `HarkenVariableCatalogGenerator`。Voice 参数和 `SetSend/ClearSend` 仍公开，后两者仍能修改路由；原生 ABI 未因此裁剪 | 已实现；C 对应投影未改 |
| Action Seek | 已支持的游戏能力有公开 Unity 入口 | `ActionFiring.Seek` 返回 `ActionSeekTicket`；Query 区分接受/应用/取消，终态由调用者 Release；原生限制和返回码不变 | 已补齐 |
| 同源生成与 API 分类 | 原生签名与远程通道同源；Unity 可见性由独立 API 分类决定 | `HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json` 提供原生声明，`HarkenEngine/Source/Tooling/Binding/sdk_api_surface.json` 的 `managed_generation` 决定生成符号的访问性；生成器检查遗漏或过期分类，不为托管可见性修改 wire 的 public/opcode/policy | 一致 |
| ABI 保真 | 编译期与托管期双侧证明"投影==表" | 生成的 abi_pin 头以 static_assert 校验原生布局，`HarkenAbi.g.cs` CheckLayout 校验托管布局，`HarkenSDK/Test/ContractTest` 提供独立 runner | 一致 |
| 零 GC 热路径 | 被测热路径的调用线程托管分配为零 | ContractTest 以 `GC.GetAllocatedBytesForCurrentThread` 和正向分配对照验证；缺原生库显示 SKIP，`--require-native` 下 SKIP 判 FAIL。该结论不涵盖资源准备或全部字符串入口，也不代表音频线程安全 | 一致 |
| 名称与稳定身份 | 名称和 GUID 均是公开调用方式，不以字符串缓存代替当前发布校验 | `Actions.Post/Resolve` 解析由发布清单绑定的独立 Action 目录，`Values.SetValue/Resolve` 解析 Init 的变量目录；支持显式项目校验，Action pin/resolved 调用保留原生一致性检查；退役 Action 名称可返回 `VN_GAMEPLAY_NAME_RETIRED`，旧名不自动成为别名 | 一致 |
| AOT 姿态 | 无运行时代码生成，库名编译期固化，回调走静态转发点 | `HarkenNative.g.cs` 按平台固化 LibraryName（iOS 系 `__Internal`）；静态 `[MonoPInvokeCallback]` thunk + id 键注册表 | 一致 |
| Unity 坐标边界 | 左手转右手、米制，Listener 与 Emitter 走同一转换 | `Runtime/Unity/UnityCoordinates.cs` + `UnitySpatial.cs` 共用 ToHarken，无状态；ContractTest 有坐标回归门 | 一致 |
| 托管状态归属 | 状态须有明确所有者和释放时机 | 回调注册、Pack 引用、Variable 目录保护及 Update 嵌套深度均有托管状态；Unity owner 负责会话关闭后的回收，回调内关闭等待最外层 Update 返回 | 一致 |
| 平台分槽交付 | 按平台分槽布局，动态加载与静态链接（iOS）双形态 | `Runtime/Plugins/` 分槽 + `harken-integration.json` 平台产物清单；`HARKEN_BUILD_SHARED` 出动态库，iOS preset 出静态 `.a` | 一致 |
| 运行时精简构建 | 嵌入构建只含运行时库，剔除工具/编辑器/测试 | 根 CMake `HARKEN_RUNTIME_ONLY` + 移动交叉编译 presets | 一致 |
| 客人姿态 | 宿主持有进程级音频归属的平台上不夺取会话 | iOS 上实际纪律是"负责激活、绝不反激活"（`DeviceBackend.cpp` 注明理由：宿主可能未激活会话），比"绝不激活"多走半步 | 偏差 |
| Variable 统一写入 | SetValue 由可选 Emitter 决定 Global/Emitter，结果由 Engine 决定 | Unity `Values.SetValue` 支持名称、GUID、typed 引用，以及数值/Label 名称/Label GUID；全局写入清除该 Variable 的所有 Emitter 覆盖。Unity 旧 setter/batch/reset 包装不再公开 | 一致 |
| 调用顺序与生命周期 | SDK 不缓存局部状态或重排跨 scope 的写入 | Unity 同步提交，显式无效 Emitter 拒绝，组件销毁真正结束 Emitter；无 Clear/ResetVariables 公共入口 | 一致 |
| 示例交付 | 随包示例使用当前公开 API | `Samples~/PlayableDemo/` 展示六类 Action、明确的启动准备和 `Demo_Choice` Variable；不是生产项目接入证明 | 已实现 |
| 集成安装校验流 | SDK 安装位置、身份和所有权可检查 | Hub > Engine Integrations 使用 `harken-integration.json` 识别 SDK，支持 Packages 或允许的 Assets 子目录；完整迁移、恢复和目标限制见随包 README | 已实现 |
| 宿主生命周期 | 暂停、恢复、关闭结果可观察，失败后显式重试 | `HarkenEngine.Suspend/Resume`、`HarkenAudioSystem.SetSuspended` 与 RuntimeEnded 保留不同阶段的结果；缺少 Content 的文件和字节启动保留可恢复会话，provider 异常转为 `VN_PACK_PROVIDER_FAILED` | 已实现；不代表移动设备验收 |

## 备注

Variable 局部值属于 Emitter，影响其不同 Action、多次播放和不同 Layer 下使用同一 Variable 的消费者；路由不改变该范围，Bus Binding 始终读取全局值。正式边界与三行例子见 [Game Modulate](../../philosophy/game-modulate-system.md#emitter-上下文与消费边界)，调用示例见 [Unity](../../../HarkenSDK/Unity/Harken/README.md)。

2026-09-28 核对：Native C++ 与 Unity 的 Label 赋值统一为 `SetValue` 重载，值参数接受数值、Label 名称或 Label GUID，不保留 `SetLabel` 兼容入口。按变量名称调用支持 `SetValue(variableName, labelName)`，Emitter 仍为可选参数。C ABI 使用 `Vn_SetValueByLabelId` / `Vn_SetValueByLabelName` 承接 Label 重载。Label 映射、写入权限、作用域与既有目录校验策略沿用原行为；本次未治理其他公开 API。

2026-10-18 核对：新导出的 Init 不再携带 Action 发布目录，语言清单共用同一 Init。Unity bootstrap 验证清单和 Init 后，通过 `Vn_SetGameplayCatalog` 将 JSON 目录编码并绑定到已激活的 Init；Content 后续加载、卸载和替换保留目录，Init 卸载时清除。目录绑定校验项目、Init 版本和已加载 Action 的归属；原有二进制目录仍可读取。

1. 客人姿态一条宜以代码为准回写预期：实际纪律是"负责激活、绝不反激活"，而非"绝不激活"。
2. 当前 `harken-integration.json` 与 `Runtime/Plugins/README.md` 均限定 Android 为 arm64-v8a，并列出 Linux x86_64 槽；此前记录的平台清单差异已不存在。
3. 依赖方向已验证成立：引擎运行时源码不引用 HarkenSDK，仅工具链生成器向其写入产物。
