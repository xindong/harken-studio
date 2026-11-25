# HarkenHub 现状快照

## 产品边界

| 项目 | 现状 |
|---|---|
| 定位 | 分发与启动器，不链接音频引擎 |
| 平台 | Windows、macOS |
| 交付组件 | HarkenHub、提权安装引导器 HarkenHubSetup、特权更新桥 HarkenHubUpdateBridge |

## 能力

| 模块 / 能力 | 现状 | 成熟度 |
|---|---|---|
| 主窗口 | 版本管理与工程管理双页，无边框标题栏 | 可用 |
| 命令行与离线文档 | `HarkenHub.exe --cli help` 列出当前版本的命令和参数。Hub 设置菜单的「CLI 使用指南」可打开随包用户指南，并从指南进入 AI 命令参考和 JSON 命令定义。GUI 与 CLI 共用设置，Windows 上尚未选择主题时使用深色 | 可用 |
| 匿名资源访问 | 版本发现、下载、自更新均匿名，无登录、设备授权或身份 token 存储 | 可用 |
| 自动更新检查 | Hub 与 Studio 开关独立；关闭后停止对应产品的后台检查、重试和下载，手动检查不重开自动任务；未安装 Studio 时，启动页立即发起受开关控制的 Studio 检查 | 可用 |
| 更新状态一致性 | 跨进程串行提交共享状态，条件更新与清除拒绝过时状态 | 可用 |
| Windows 安装语言 | 按启动用户显示语言选择中英文安装包；简繁中文均显示简体中文，其余显示英文；跨账号提权保留选择，界面与法务正文同语种 | 可用 |
| Windows 安装确认与启动 | 许可协议与隐私政策独立滚动，均接受后继续；成功页默认勾选启动 Hub，以普通桌面用户身份启动；卸载、失败、取消和静默自更新不触发启动 | 可用 |
| Windows 安装包完整性 | 共用中英文安装包重复内容，提权后还原所选语言的完整安装包并校验哈希，保留原始签名；切换语言沿用升级流程 | 可用 |
| 下载选源与续传 | GitHub 与对象存储片段测速选源；断点绑定来源、哈希、修改时间和大小，优先原源续传；重试耗尽后换源并提示从头下载 | 可用 |
| 包校验与占用保护 | 整包校验与 ed25519 清单验签；Studio 手动、后台缓存隔离；下载占用保护持续到校验和安装交接完成 | 可用 |
| 事务化安装 / 修复 / 卸载 | 原子更新安装索引，短暂占用时有界重试，持续失败保留旧索引并回滚；新目录由版本、渠道、平台架构命名，重名追加短序号；既有路径兼容 | 可用 |
| 安装索引校验 | 读写共用完整结构校验，拒绝错误字段类型、缺失身份、重复构建、未知状态和非法发布序号；损坏时不返回部分记录、不覆盖原件；兼容缺少历史可选哈希字段及安装目录缺失后的修复 | 可用 |
| 后台安装结果 | 更新桥按事务写入独立原子回执，Hub 保存成功或失败结果后才清理，重启后继续接收；成功还需索引状态为 Installed，Installing 不算完成；诊断日志失败单独报告，不改变安装结果 | 可用 |
| Hub 自更新 | Windows 计划任务与 macOS 特权服务执行更新，附健康检查；安装包及清单共享占用保护，静默更新复用下载锁 | 可用 |
| 已装版本与启动 | 支持版本扫描、完整性检查及启动预检；本地构建经 Studio 的 `--version --json` 识别版本、buildId 与渠道；发起 Studio 后不监听启动进度或结果，工程检查、升级确认与错误由 Studio 自行展示；保留工程列表及打开历史的静默同步 | 可用 |
| 工程库与登记 | 工程信息持久化，接收 Studio 侧工程登记；工程文件只读取头部的 `studioVersion` 用于推荐版本，按路径记录工程 | 可用 |
| 新建工程 | 选择 Studio 版本、名称与位置后交给 Studio 创建；CLI 可经所选 Studio 的 `--list-templates` 列出模板并以模板 ID 创建，Hub 不保存、不校验模板 | 可用 |
| 集成包来源选择 | 按工程引擎显示对应集成包名称；可下拉选择已安装 Studio 随附包，按目标引擎筛选并显示版本提示；支持手动选目录，每次打开刷新候选 | 可用 |
| 集成包变更结果 | 显示版本变化及新增、更新、移除、保留数量，按动作分组列出文件，保留项折叠；完整日志独立显示，优先展示失败和待处理提醒 | 可用 |
| Unity 工程占用检查 | 检查、安装、卸载、恢复之前的版本共用检查；Windows 检查独占打开，POSIX 检查文件锁；未占用的残留锁文件不阻止操作，也不改写或删除；访问失败与真实占用分别报告 | 可用 |
| Unity 集成包安装、卸载与恢复 | 安装、升级、重装均整目录替换，只需要新版本；替换前把旧集成目录完整备份到 Hub 数据目录并核验，成功即删除；失败或中断后可“恢复之前的版本”；卸载删除安装目录及其文件夹 `.meta`；目标须为空目录或已安装的集成包根目录 | 可用 |
| 独立文件清单 | 新 Studio 构建导出与 ZIP 内 `package-manifest.json` 字节一致的 `<version>-<buildId>.files.json`；签名绑定文件名、字节数与 SHA-256；Hub 验签时校验该声明，当前没有读取独立清单的功能 | 可用 |
| 合规与维护 | 许可协议与同意记录、缓存清理、日志导出、工程版本固定 | 可用 |
| 平台加固 | Windows / macOS 均支持提权、权限加固、登录自启、进程扫描与单实例守卫 | 可用 |
| Authenticode 二进制验证 | Windows 安装器默认校验签名；已安装目录默认不强制校验，非 Windows 无此验证能力 | 可用 |
| 服务端接入 | GitHub Releases 与 HTTPS 对象存储分发；对象存储地址可通过环境变量覆盖 | 可用 |

## 与 Studio 的接口

Hub 自己维护“Studio 版本 → 可用启动入口”的对应关系（`Versions/StudioInterface`），不读取 Studio 的能力声明：0.7.0 起可从 Hub 新建工程，高于 0.9.4 的版本可列出模板。Studio 一侧的入口见 [Studio 命令接口](../studio/studio-commands.md#hub-使用的启动接口)。发布包中的 `capabilities.json` 已退役，Hub 不再读取或校验；旧发布包仍带有该文件，安装与修复时按包内元数据原样接受。

Studio 使用的 Hub 接口同样只新增、不改含义、不删除：

| 入口 | 行为 |
|---|---|
| `--show-new-project`、`--show-projects`、`--edit-integration` | 打开 Hub 对应页面 |
| `--register-local-build=<Studio 可执行文件>`、`--reveal-local-build` | 登记并显示本地构建的 Studio |
| `--harken-project=<工程文件>` | 与上述页面一起指定工程 |
| `<Hub 数据目录>/project-intake/<任意名>.json` | Studio 打开工程后写入 `{"projectPath","buildId","openedAt"}`，Hub 读取后删除 |

## 集成包兼容

| 项目 | 现状 |
|---|---|
| 产品术语 | 使用“集成 / Integration”和“集成包 / Integration Package”，Unity 专属操作使用 Unity Integration |
| 随附目录 | Studio 使用 `HarkenIntegration` 主目录，并提供内容一致的 `HarkenSDK/Unity/Harken` 实体副本兼容旧 Hub |
| 来源发现 | Hub 优先新目录，仅在其不存在时使用旧目录，既有来源记录继续可用 |
| 环境变量 | 未选择来源时读取 `HARKEN_UNITY_INTEGRATION_PATH`；不按源码仓库目录结构查找 |
| 工程安装位置 | `Packages/com.harken.core`、`Assets/Plugins/Harken`、既有 `Assets/HarkenSDK` 或 Assets 下的自定义子目录 |
| 对集成包的认知 | 只读 `package.json` 的 `name` 与 `version`；重复与冲突检测依据包身份和 Package Manager 引用，不识别包内的程序集、原生库或设置文件 |
| 安装实现归属 | Unity 集成包安装、卸载和恢复之前的版本由 Hub 内的 `Integrations/Unity` 负责 |

## 发布协议

新增签名字段 `packageManifest` 包含 `fileName`、`size`、`sha256`。文件名固定为 `<version>-<buildId>.files.json`，最大 16 MiB；下载位置只从原发布包所在对象目录和同一 GitHub Release 推导。清单还需匹配产品、版本、构建和渠道；拒绝越界路径、设备名、ADS、大小写／Unicode 别名、重复文件与文件目录冲突。清单与 ZIP 共用 `Packaging/PackageInventory` 解析，下载过程中限制长度，缓存每次重新核验。

构建脚本将独立清单加入发布证据与 CI 产物。独立调用 `.scripts/release/sign_release_manifest.py sign` 时传入 `--package-manifest`，脚本核对清单与 ZIP 字节及身份后纳入签名。正式发布入口位于 **audio-harken-managing-tool** 仓库的 `publishing/core.py`：从最终 ZIP 提取独立清单、签名、双源上传并验证，之后才更新公开版本目录。部署更新的发布器和 Hub 后，新发布启用节省下载量的路径；历史发布保持原签名与完整包回退行为。
