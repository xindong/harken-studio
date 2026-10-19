# HarkenHub 现状快照

最近核对：2026-08-12

Hub 在旧设计图谱中零篇文档，整册属"未记载→补录"：以下表单本身即是它在设计层的
首次登记，预期栏留待设计补课后回填。定位：纯"分发 + 启动器"，不链接 harken_core，
与引擎完全解耦。技术栈 Qt6 Widgets/Sql/Network + minizip-ng + ed25519，仅
Windows/macOS；交付三件套：HarkenHub、HarkenHubSetup（提权引导器）、
HarkenHubUpdateBridge（特权更新桥）。

| 模块/能力 | 现状 | 成熟度 |
|---|---|---|
| 主窗口双页 UI | `Source/UI/HubWindow`：VersionsPage（版本管理）+ ProjectsPage（工程管理），无边框标题栏 | 可用 |
| 匿名资源访问 | 版本发现、下载与自更新均匿名，不实现登录、设备授权或身份 token 存储 | 可用 |
| 版本目录与更新检查 | `Source/Update/` ServerVersionCatalog / UpdateChecker / Coordinator / Queue / StateStore 全链路；共享状态文件以跨进程锁串行提交，条件更新与清除拒绝过时快照 | 可用 |
| 下载与包校验 | `Source/Download/`（重试退避、文件占用保护）+ `Source/Packaging/`（zip 读取校验）+ `Source/Security/` ed25519 清单签名与信任锚；Studio 手动与后台缓存隔离，占用保护延续到校验和安装交接复制完成 | 可用 |
| 事务化安装/修复/卸载 | `Source/Transactions/` 三类 Executor + Journal / Reclaimer / HandoffArea；安装索引共用有界原子写入重试，Windows 短暂占用后恢复，持续失败保留旧索引并沿用事务回滚；新安装目录使用版本、渠道和平台架构，重名时追加短序号；构建身份保存在元数据中，既有安装路径保持兼容 | 可用 |
| Hub 自更新 + 特权桥 | `Source/Update/HubSelfUpdate` + 更新桥（Windows 计划任务 / macOS LaunchDaemon），带健康检查与单测；安装包及其清单共用占用保护，覆盖下载、校验和安装交接，同步静默更新复用当前下载锁 | 可用 |
| 已装版本管理与启动链 | `Source/Versions/`（扫描/完整性/自检）+ `Source/Launch/`（预检/工程打开流/启动监控） | 可用 |
| 工程库与登记回流 | `Source/Projects/` ProjectStore（Qt Sql）+ ProjectProbe + IntakeConsumer（FS watcher 接收 Studio 侧登记） | 可用 |
| 引擎集成 SDK 来源选择 | 安装/更换 SDK 弹窗的源目录控件可直接下拉选择 Hub 已安装 Studio 附带的 SDK，按目标引擎筛选，悬停提示 Studio/SDK 版本；保留手动选文件夹，每次打开刷新候选 | 可用 |
| SDK 变更检查结果 | 默认显示版本变化及新增/更新/移除/保留数量，按动作分组列出文件，保留项折叠；完整日志独立页签，失败和待处理提醒优先展示，状态与建议分色强调 | 可用 |
| 合规与维护 | `Source/Legal/`（EULA + ConsentStore）+ `Source/Maintenance/`（缓存清理/日志导出/工程钉版本） | 可用 |
| 平台加固 | `Source/Platform/` Win+mac 双实现：提权、ACL 加固、登录自启、进程扫描、单实例守卫 | 可用 |
| Authenticode 二进制验证 | `Source/Security/BinaryTrustPolicy.cpp` 发布者/安装器验证两处显式 not implemented | 占位 |
| 服务端接入 | `Source/Update/ServerEndpoints.cpp` 默认硬编码内网明文 HTTP 地址，仅环境变量可覆写 | 半成品 |

## 备注

1. 预期中而未达的两项即表末两行：Authenticode 信任策略、正式（HTTPS/公网）分发服务端。
2. `HarkenHub/Docs/HarkenGuide.md` 是 Hub 目录下唯一文档，性质为三产品《入门指南》而非设计文档。
