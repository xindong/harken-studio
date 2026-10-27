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
| 版本目录与更新检查 | `Source/Update/` ServerVersionCatalog / UpdateChecker / Coordinator / Queue / StateStore 全链路；关闭对应产品自动更新后停止后台检查、重试和下载，手动检查不重开自动任务；未安装 Studio 时，启动页面立即请求 Studio 自动检查，仍受 Studio 开关控制且不触发 Hub 检查；共享状态文件以跨进程锁串行提交，条件更新与清除拒绝过时快照 | 可用 |
| Windows 安装包 | 原生 Windows Installer API 与 CAB 工具生成中英文 MSI，EXE 引导器在提权前按启动用户的 Windows 显示语言选定内嵌安装包：简繁中文统一显示简体中文，其余显示英文，跨账号提权保留该选择；界面和法务正文同语种，许可协议与隐私政策左右分栏、独立滚动，勾选接受两份文档后继续；两种 MSI 的语言元数据各自一致，共用组件与 UpgradeCode，ProductCode 和 PackageCode 独立，切换语言沿用升级流程 | 可用 |
| 下载与包校验 | `Source/Download/`（重试退避、文件占用保护）+ `Source/Packaging/`（zip 读取校验）+ `Source/Security/` ed25519 清单签名与信任锚；Studio 手动与后台缓存隔离，占用保护延续到校验和安装交接复制完成 | 可用 |
| 事务化安装/修复/卸载 | `Source/Transactions/` 三类 Executor + Journal / Reclaimer / HandoffArea；安装索引共用有界原子写入重试，Windows 短暂占用后恢复，持续失败保留旧索引并沿用事务回滚；新安装目录使用版本、渠道和平台架构，重名时追加短序号；构建身份保存在元数据中，既有安装路径保持兼容 | 可用 |
| Hub 自更新 + 特权桥 | `Source/Update/HubSelfUpdate` + 更新桥（Windows 计划任务 / macOS LaunchDaemon），带健康检查与单测；安装包及其清单共用占用保护，覆盖下载、校验和安装交接，同步静默更新复用当前下载锁 | 可用 |
| 已装版本管理与启动链 | `Source/Versions/`（扫描/完整性/自检）+ `Source/Launch/`（预检/工程打开流/启动监控） | 可用 |
| 工程库与登记回流 | `Source/Projects/` ProjectStore（Qt Sql）+ ProjectProbe + IntakeConsumer（FS watcher 接收 Studio 侧登记） | 可用 |
| 引擎集成包来源选择 | 安装/更换集成包弹窗和快捷更换菜单按当前工程引擎显示“Unity 集成包”或“UE 集成包”；源目录控件可直接下拉选择 Hub 已安装 Studio 附带的集成包，按目标引擎筛选，悬停提示 Studio/集成包版本；保留手动选文件夹，每次打开刷新候选 | 可用 |
| 集成包变更检查结果 | 默认显示版本变化及新增/更新/移除/保留数量，按动作分组列出文件，保留项折叠；完整日志独立页签，失败和待处理提醒优先展示，状态与建议分色强调 | 可用 |
| Unity 工程占用检查 | Hub 的安装、更新、卸载、迁移与恢复共用自身 `Source/Integrations/Unity/UnityProjectAccess` 检测；Windows 检查独占打开，POSIX 检查文件锁与记录锁。未被占用的残留 `Temp/UnityLockfile` 不阻止操作，检测不改写或删除锁文件；访问失败单独报告，真实占用仍拦截 | 可用 |
| 合规与维护 | `Source/Legal/`（EULA + ConsentStore）+ `Source/Maintenance/`（缓存清理/日志导出/工程钉版本） | 可用 |
| 平台加固 | `Source/Platform/` Win+mac 双实现：提权、ACL 加固、登录自启、进程扫描、单实例守卫 | 可用 |
| Authenticode 二进制验证 | `Source/Security/BinaryTrustPolicy.cpp` 发布者/安装器验证两处显式 not implemented | 占位 |
| 服务端接入 | `Source/Update/ServerEndpoints.cpp` 默认硬编码内网明文 HTTP 地址，仅环境变量可覆写 | 半成品 |

## 备注

1. 预期中而未达的两项即表末两行：Authenticode 信任策略、正式（HTTPS/公网）分发服务端。
2. `HarkenHub/Docs/HarkenGuide.md` 是 Hub 目录下唯一文档，性质为三产品《入门指南》而非设计文档。

## 集成包命名与目录兼容（2026-10-26）

Hub 0.1.71 使用“集成 / Integration”和“集成包 / Integration Package”，Unity 专属操作使用 Unity Integration。历史发布说明保留当时名称。Studio 0.9.1 以 HarkenIntegration 为主分发目录，同时从同一来源生成 HarkenSDK/Unity/Harken 实体兼容副本，支持旧 Hub；打包前逐文件验证一致，避免两份内容漂移。新版 Hub 优先新目录，仅在新目录不存在时使用旧目录，既有来源记录继续可用。

自动发现支持 HARKEN_UNITY_INTEGRATION_PATH，并保留 HARKEN_UNITY_SDK_PATH。Unity 工程内 Packages/com.harken.core、既有 Assets/HarkenSDK、自定义位置、备份目录及持久化字段保持兼容，源目录更名不触发工程搬迁。
