# HarkenIntegration 现状快照

## 能力与边界

| 能力域 | 预期 | 现状 | 状态 |
|---|---|---|---|
| 原语与宿主分层 | Engine 实现原生音频语义，Integration 承担托管互操作与宿主责任 | 原生 ABI 投影与公开包装分层；Unity 管理会话、资源引用、回调和组件生命周期 | 一致 |
| Unity API 可见性 | 游戏使用公开包装，工具实现内部化，Editor 服务独立公开 | 原生声明、ABI 校验、Snapshot / Monitor DTO 和 Bus 结构操作不公开；Voice 参数与 `SetSend/ClearSend` 仍公开，可修改路由；原生 ABI 未裁剪 | 已实现 |
| Action Seek | 已支持的游戏能力有公开 Unity 入口 | `ActionFiring.Seek` 返回 `ActionSeekTicket`；查询区分接受、应用、取消，终态由调用者释放 | 已实现 |
| 同源生成与分类 | 原生签名与远程通道同源，Unity 可见性独立分类 | 生成时检查遗漏或过期分类，托管可见性不改变原生操作及通道策略 | 一致 |
| ABI 保真 | 原生与托管双侧证明布局一致 | 原生编译时与托管运行时均校验布局，提供独立合同验证 | 一致 |
| 零 GC 热路径 | 被测热路径的调用线程托管分配为零 | 通过分配计量及正向分配对照验证；缺原生库时跳过，强制原生验证时视为失败 | 一致 |
| 名称与稳定身份 | 名称和 GUID 均可公开调用，解析基于当前发布 | Action 使用发布清单绑定的独立目录，Variable 使用 Init 目录；支持项目与解析身份校验；退役名称可返回 `VN_GAMEPLAY_NAME_RETIRED`，旧名不自动作为别名 | 一致 |
| Action 目录生命周期 | 目录与已激活的 Init、项目及 Action 归属一致 | 各语言清单共用 Init，目录由清单独立绑定；Content 加载、卸载与替换保留目录，Init 卸载清除目录；仍可读取旧二进制目录 | 一致 |
| AOT | 无运行时代码生成，库名编译时确定 | iOS 使用静态链接，其余平台按构建选择原生库；回调使用静态入口 | 一致 |
| Unity 坐标边界 | 左手转右手、米制，Listener 与 Emitter 转换一致 | 两者共用无状态转换，已有坐标回归验证 | 一致 |
| 托管状态归属 | 状态所有者与释放时机明确 | Unity owner 回收会话、回调、Pack 引用与目录状态；回调内关闭延迟到最外层 Update 返回 | 一致 |
| 平台交付 | 按平台分槽，兼容动态库与 iOS 静态库 | 提供平台产物清单；Android 限 arm64-v8a，包含 Linux x86_64 槽 | 一致 |
| 运行时精简构建 | 嵌入构建剔除工具、编辑器和测试 | 支持仅运行时构建与移动平台交叉编译 | 一致 |
| 客人姿态 | 不夺取宿主的进程级音频会话 | iOS 会负责激活会话，但绝不反激活 | 偏差 |
| Variable 统一写入 | 可选 Emitter 决定 Global / Emitter，结果由 Engine 决定 | `Values.SetValue` 支持 Variable 名称、GUID、typed 引用及数值、Label 名称、Label GUID；全局写入清除该 Variable 的全部局部覆盖 | 一致 |
| Variable 消费范围 | 局部值归 Emitter，Bus 只读全局 | 同一 Emitter 的不同 Action、Play、Layer 共享局部值；路由不改变范围；无公开旧 setter、batch、SetLabel、Clear / ResetVariables 入口 | 一致 |
| 调用顺序与生命周期 | 不缓存局部值或重排跨 scope 写入 | 同步提交，显式无效 Emitter 拒绝，组件销毁结束 Emitter 及其局部状态 | 一致 |
| 示例交付 | 示例使用当前公开 API | 展示六类 Action、启动准备与 `Demo_Choice` Variable | 已实现 |
| 集成安装校验 | 安装位置、身份和所有权可检查 | Hub 以集成包清单识别来源，支持 Packages 与允许的 Assets 子目录 | 已实现 |
| 宿主生命周期 | 暂停、恢复、关闭结果可观察，失败可显式重试 | `Suspend/Resume`、`SetSuspended` 与 `RuntimeEnded` 区分阶段结果；缺 Content 时保留可恢复会话，provider 异常返回 `VN_PACK_PROVIDER_FAILED` | 已实现 |

表注：零 GC 结论限于被测调用线程热路径，不覆盖资源准备、全部字符串入口或音频线程安全。示例与合同验证不等同于生产接入或移动设备验收。

## 分发与兼容

| 项目 | 当前状态 |
|---|---|
| 产品范围 | 仅维护 Unity Integration，Unreal SDK 已移除 |
| 产品版本 | Unity Integration 为 0.3.7，以 Unity 包清单为唯一版本维护源；独立制品与源码清单一致，Studio 随附版本以实际清单为准 |
| 制品名称 | 内部制品为 `harken-integration-bundle`，独立下载为 `HarkenIntegrationBundle` |
| Studio 随附布局 | 主目录为 `HarkenIntegration`，同时提供 `HarkenSDK/Unity/Harken` 实体兼容副本；两份内容逐文件校验一致 |
| Unity 身份 | 外层分发命名不改变 Unity 包标识、程序集、GUID 或工程安装位置 |
| 运行库升级 | 游戏工程需更新集成包才能获得原生运行库修正，单独升级 Studio 不会替换游戏工程运行库 |
| Schema 兼容 | Studio 0.9.2 的 Editor Schema 为 9、Pack Schema 为 4，与 0.9.1 一致；0.9.1 工程无需迁移，既有 Pack 不因升级而强制重打 |
