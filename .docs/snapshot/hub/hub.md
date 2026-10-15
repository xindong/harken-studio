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
| 版本目录与更新检查 | `Source/Update/` ServerVersionCatalog / UpdateChecker / Coordinator / Queue / StateStore 全链路 | 可用 |
| 下载与包校验 | `Source/Download/`（重试退避）+ `Source/Packaging/`（zip 读取校验）+ `Source/Security/` ed25519 清单签名与信任锚 | 可用 |
| 事务化安装/修复/卸载 | `Source/Transactions/` 三类 Executor + Journal / Reclaimer / HandoffArea；新安装目录使用版本、渠道和平台架构，重名时追加短序号；构建身份保存在元数据中，既有安装路径保持兼容 | 可用 |
| Hub 自更新 + 特权桥 | `Source/Update/HubSelfUpdate` + 更新桥（Windows 计划任务 / macOS LaunchDaemon），带健康检查与单测 | 可用 |
| 已装版本管理与启动链 | `Source/Versions/`（扫描/完整性/自检）+ `Source/Launch/`（预检/工程打开流/启动监控） | 可用 |
| 工程库与登记回流 | `Source/Projects/` ProjectStore（Qt Sql）+ ProjectProbe + IntakeConsumer（FS watcher 接收 Studio 侧登记） | 可用 |
| 合规与维护 | `Source/Legal/`（EULA + ConsentStore）+ `Source/Maintenance/`（缓存清理/日志导出/工程钉版本） | 可用 |
| 平台加固 | `Source/Platform/` Win+mac 双实现：提权、ACL 加固、登录自启、进程扫描、单实例守卫 | 可用 |
| Authenticode 二进制验证 | `Source/Security/BinaryTrustPolicy.cpp` 发布者/安装器验证两处显式 not implemented | 占位 |
| 服务端接入 | `Source/Update/ServerEndpoints.cpp` 默认硬编码内网明文 HTTP 地址，仅环境变量可覆写 | 半成品 |

## 备注

1. 预期中而未达的两项即表末两行：Authenticode 信任策略、正式（HTTPS/公网）分发服务端。
2. `HarkenHub/Docs/HarkenGuide.md` 是 Hub 目录下唯一文档，性质为三产品《入门指南》而非设计文档。
