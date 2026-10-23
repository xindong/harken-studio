# 外部数据流与本机存储清单

本文档记录 Harken 桌面工具和 Runtime 的网络请求、本机存储及用途，供产品文档和法务文本核对事实。基准为 2026-09-09 的 main 提交 `9ca30410ecf3a31d582926a7184b9b4f8eaeb395`，以下路径均相对仓库根目录。

产品边界：Harken 不设用于接收用户信息的业务服务器，不进行用户信息采集、遥测或崩溃自动上传。用户自行配置的第三方 AI 请求直接发往所配置的服务；GitHub／对象存储用于分发公开的软件文件。本机正常使用所需的配置、工程、日志和系统权限信息应按实际用途描述，不能据此推断存在向 Harken 上传的行为。

维护要求：新增或变更功能性网络请求、本机持久化时同步更新本清单。采集用户信息、向 Harken 上传用户数据的代码不符合上述产品边界，应修复；不能通过修改本文档将其默认为许可行为。

## 总览

| 编号 | 产品 | 行为 | 触发方式 | 接收方 |
| --- | --- | --- | --- | --- |
| DF-1 | Studio | AI 对话与工具结果请求 | 用户配置服务和 API key 后，发起对话或测试连接 | 用户配置的第三方／本地模型服务 |
| DF-2 | Hub | 检查更新、下载清单与软件包 | 更新服务启动后定期检查，也可主动检查；安装下载由用户操作触发 | GitHub Releases，失败时按规则使用对象存储备用源 |
| DF-3 | Hub | 获取可安装版本列表、补取包清单 | 用户打开版本选择界面或导入需要补全清单的本地包 | 与 DF-2 相同的软件分发源 |
| DF-4 | Studio / Hub | 打开官网、在线文档、代码仓库 | 用户点击 Studio 帮助菜单或 Hub 侧栏链接 | 由系统浏览器访问对应网站 |
| DF-5 | Studio / Engine / SDK | Remote 发现、连接、运行控制与监控 | SDK 默认随音频启动通信，项目设置可关闭打包游戏通信；用户在 Studio 发起发现、连接和采集 | 用户选择的同机或局域网游戏进程 |

主动网络请求还包括用户发起的 Remote 联调。未发现独立的用户统计、遥测或崩溃上传通道。Remote 直接连接用户选择的游戏进程，不向软件分发源或 AI 服务发请求。

## DF-1：用户配置的 AI 服务

### 请求内容和去向

`HarkenStudio/Source/Core/AI/OpenAICompatibleLLMProvider.cpp` 向 `<base_url>/chat/completions` 发送 POST。地址、模型及请求参数来自 `AIAgentConfig.cpp` 的用户配置；默认地址／模型为空，未配置时不发送对话请求。没有 Harken AI 转发服务器。

请求体包含模型名、消息、工具定义及生成参数。消息可能包含用户输入、相关对话历史、检索到的本机持久记忆，以及工具执行返回的工程内容。`AIAssistantService.cpp` 负责组装上下文和工具执行循环；例如 `Core/IPC/IPCHandlers_Project.cpp` 返回的 `projectPath` 可能进入工具结果。这些内容直接发送给用户选择的模型服务，不应描述为“任何工程数据都不会离开本机”。

源码没有额外附加设备指纹、Windows 账号 SID 或用于用户统计的标识。用户输入及工程内容本身可能包含个人信息，因此也不能保证请求内容绝不含此类信息。

### 凭据和传输

- API key 存在操作系统凭据保险库：Windows Credential Manager 或 macOS Keychain，实现为 `HarkenStudio/Source/Platform/SecretStore_Win.cpp` 和 `SecretStore_Mac.cpp`。配置文件保存端点、模型等设置，不保存 API key。
- 请求使用 `Authorization: Bearer <key>`。provider 对错误文本中的 key 作脱敏，服务在结束当前请求轮次时清除所持有的 key。
- `AIEndpointValidation.h` 仅接受 HTTPS，要求 host 存在并禁止 URL 内嵌账号密码，使用 Qt／平台证书校验。对话和连接测试均在读取密钥及发送请求前拒绝 HTTP，包括本机 HTTP 地址。
- provider 禁用自动重定向，避免将请求直接转发到其他端点。自建或本地模型服务也需要提供可通过证书校验的 HTTPS 入口。

### 本机留存

AI 对话已经支持本机持久化，不能再写成“对话仅存内存”。数据根目录由 `QStandardPaths::AppDataLocation` 提供：

| 内容 | 相对数据目录的路径 | 实现 |
| --- | --- | --- |
| 对话历史 | `AI/Conversations/<scopeHash>.json` | `AIConversationStore.cpp` |
| 会话目录 | `AI/Conversations/<scopeHash>.sessions.json` | `AIConversationSessionStore.cpp` |
| 持久记忆 | `AI/Memories/<scopeHash>.json` | `AIDurableMemoryStore.cpp` |

上述实现均位于 `HarkenStudio/Source/Core/AI/`。这些文件用于本机恢复会话和提供 AI 上下文，没有向 Harken 同步的通道；在继续使用 AI 时，相关上下文仍可能按本节所述进入第三方请求。第三方对收到内容的处理不由 Harken 仓库控制。

## DF-2：更新检查和下载

### 软件分发源

`HarkenHub/Source/Update/ServerEndpoints.cpp` 统一生成端点：

- 清单首选 `https://github.com/xindong/harken-studio/releases/latest/download/<product>-manifest.json`，产品为 `hub` 或 `harken`。
- 默认备用基址为 `https://venus-1459151234.cos.ap-shanghai.myqcloud.com/`，清单路径为 `release/<product>/manifest.json`。
- 备用基址可由 `HARKEN_CONTENT_BASE_URL` 覆盖。旧 `HARKEN_SERVER_BASE_URL`、内网 IP 和 `/api/harken/client/...` 路由已不适用于当前代码。
- 软件包地址来自已验证清单的 release tag／对象路径，优先 GitHub Release 资产，备用路径为 `release/<product>/<platform-architecture>/<version>/<build-directory>/<filename>`。构建目录使用 Hub 的 `run<number>.<commit>` 或 Studio 的 `run<number>-<commit>-<timestamp>`（重跑时为 `run<number>.<attempt>`）。Hub 文件名为 `HarkenHubSetup-<version>.exe/pkg`，Studio 为 `<version>-<channel>-<platform-architecture>.zip`；GitHub 资产使用相同文件名。当前客户端只接受此归档布局。

以上是公开文件的托管与下载，没有接收用户工程、统计或诊断上报的业务接口。

`UpdateChecker.cpp`、`UpdateCoordinator.cpp` 负责周期检查和更新调度，默认检查间隔为 1 小时、下限为 15 分钟。不可用响应按现有规则尝试备用源并退避；`--check-updates --product hub` 和 `--check-updates --product harken` 分别支持 Hub 与 Studio 单次检查，省略产品参数时仍检查 Studio。

Hub 和 Studio 更新下载失败后会清除内存中的清单缓存标记，后续检查可重新获取清单并重试。待安装文件丢失时，即使条件请求返回 304，也会重新请求已签名清单以恢复下载。校验拒绝后主动清理的坏包不按缓存丢失自动恢复；用户主动检查仍可重试。以上请求沿用原有分发源和校验流程。

### 实际发送的信息

检查清单和下载使用 GET；断点续传可先使用 HEAD，并携带 `Range`。清单条件请求可携带内存中保存的 `If-None-Match`。请求没有上传工程或日志的请求体，源码未附加 Harken 登录凭据或设备令牌，版本比较在本机进行。

下载接收方仍能看到正常网络访问所需的来源 IP、请求路径和协议头。路径可能表明所下载的产品、平台／架构、版本；不能写成“对方只看得到 IP”或“请求不涉及任何版本信息”。GitHub 重定向还可能提供资产下载所需的查询参数，它们不是 Harken 生成的用户标识。

### 验证和重定向

- 清单经 `Security/ManifestVerifier.cpp` 验签，包内容验证 SHA-256 与大小。
- `UpdateChecker.cpp`、`ServerVersionCatalog.cpp`、`Download/Downloader.cpp` 都采用手动重定向。GitHub 下载只按 `ServerEndpoints::isApprovedGithubRedirect` 的 HTTPS 主机、路径和次数限制继续请求，不是无限制自动跟随。
- 默认分发源为 HTTPS。测试／部署覆盖仍可能使用允许的 HTTP 地址；不能将默认配置描述成所有可配置场景下的强制加密保证。

## DF-3：版本列表

`HarkenHub/Source/Update/ServerVersionCatalog.cpp` 按软件分发源列表拉取清单。`ServerEndpoints::versionsUrl` 复用当前清单路径，不再访问旧版本列表 API。`UI/ServerVersionsDialog.cpp` 和 `UI/VersionsPage.cpp` 的用户操作触发查询，没有独立的版本目录后台轮询。

请求使用无上传请求体的 GET，遵循与 DF-2 一致的来源切换、受限重定向与签名验证。返回结果用于界面展示或匹配本地包，包下载继续走同一下载器。

## DF-4：系统浏览器访问

`HarkenStudio/Source/MainWindow.cpp` 的帮助菜单打开 `https://harken.xd.com/`、`https://harken.xd.com/#/docs/` 和 GitHub 仓库。访问由用户点击触发并交给系统浏览器；网站接收浏览器请求，不能据此声称软件自动上传了工程，也不能将“没有自动上报”扩写为“用户不会访问任何外部网站”。

`HarkenHub/Source/UI/HubWindow.cpp` 的侧栏链接打开 `https://harken.xd.com` 和 `https://github.com/xindong/harken-studio`，同样由用户点击后交给系统浏览器，不附带工程、安装事务或诊断数据。

其余工程目录、安装目录、日志目录等打开操作使用 `QUrl::fromLocalFile`，属于本机文件浏览。

## DF-5：Remote 游戏联调

2026-09-14 新增的 `HarkenEngine/Source/Runtime/remote/` 随正式 Engine 静态库和共享库交付，Debug、RelWithDebInfo 和 Release 均可用。Unity/Unreal SDK 默认随音频初始化启动通信；Unity `Project Settings > Harken`、Unreal `Project Settings > Plugins > Harken` 中的 `Enable Communication in Builds` 默认开启，关闭后打包游戏不启动通信，编辑器内运行不受此开关影响。加载 Engine 的进程监听 IPv4 TCP，并可按配置接收 UDP 发现请求，无需独立 host 程序。原生 C ABI 由集成方显式管理通信生命周期。

- Unity 项目配置保存为游戏项目内的 `Assets/Harken/Resources/HarkenProjectSettings.asset`，通过 Unity Resources 随包交付；Unreal 通过项目 `DefaultGame.ini` 随包交付。两者均在本地保存，不上传配置。

- 默认 TCP 端口由系统分配，默认绑定为 `0.0.0.0`；可指定地址和端口，`127.0.0.1` 将连接限制为同机。发现使用 UDP 端口 `37840` 和组播地址 `239.255.37.84`，按请求返回单播响应，不主动向公网广播。
- `HarkenStudio/Source/RuntimeBridge/RemoteConnection.cpp` 在用户发起发现时查询网络接口，在用户连接指定目标后发送握手、运行目录和控制请求；采集由用户单独启用。
- 发现和握手包含应用名、工程名、机器名、进程 ID、实例标识、监听端口、Engine 版本、构建 ID、平台、协议与能力。连接后的运行数据包含运行对象 ID／可用名称、音频状态、性能计数和用户请求的监控记录；本地实例地址字段在网络发送前清零。
- 当前链路不提供身份认证或传输加密；能够访问监听端口的客户端可操作运行时。本期不支持公网部署、云中继或资源热更新。旧 Monitor 测试端点的 ACL／认证状态不能代表正式 Remote 会话，未实现的 LAN 认证模式返回不支持。
- 这些数据直接在用户选择的 Studio 与游戏之间传输，不发送到 Harken 服务。连接窗口通过本机 `QSettings` 保存最近手动连接的地址和端口；不持久化发现目标列表、控制命令或采集数据。已有用户主动录制／导出功能的本机文件仍按其自身流程处理。
- 停止通信或引擎退出关闭套接字、通信线程与远程采集会话。通信启动失败单独报告，音频继续运行。

## 其他本机数据与通信

- Hub 的项目索引、更新状态、下载缓存、隔离目录、安装事务和日志用于本机运行与维护。用户数据位置见 `HarkenHub/Source/Platform/Windows/PlatformPaths.cpp` 和 macOS 对应实现。路径与错误文本可能包含本机用户名或工程名，不宜承诺“所有本机日志不含个人信息”。
- Hub 安装失败时，`transactions.log` 记录失败码和具体错误详情（`failureDetail`，可能包含安装路径），用于本次操作的错误弹窗和用户主动导出的诊断文件；没有自动上传通道。
- Hub 的 Studio 后台下载保存在 `downloads/`，手动下载保存在 `downloads/manual/`，分别保留软件包、清单和续传临时文件。`update-state.json.lock` 与缓存旁的 `*.download.lock` 用于本机跨进程互斥，锁内的进程标识等信息不上传；正常释放时删除锁文件，异常退出后可恢复。
- Hub 启动时清理过期缓存，会保留当前更新状态引用的安装包、清单及其下载临时文件，并跳过正在下载、校验或复制到安装交接区的缓存，避免删除仍在使用的内容。
- `HarkenHub/Source/Platform/Windows/UserSession.cpp` 获取的 SID 用于识别更新后应恢复的本机用户会话；更新桥接及 Monitor 的 SID／ACL 用于本机访问隔离。这些用途不等于采集用户画像，也未发现将其上传的代码。
- Studio 工具 IPC、Hub 单实例／健康检查属于本机进程间通信。旧 Windows Monitor 测试命名管道限制本机访问，正式 Remote 的同机／局域网链路见 DF-5。
- 本机日志、崩溃文件和用户主动导出的诊断文件没有自动上传通道。用户自行把文件发给他人的行为应与软件自动上传区分。
- CI 上传编译产物是开发发布流程，不是已安装软件上传终端用户数据。

## 法务文本对照边界

法务文本需同时准确说明：Harken 不采集或接收用户上报数据、第三方 AI 的直连请求、软件下载请求，以及必要的本机保存行为。不能套用注册账号、Harken 云端保存工程／对话、默认遥测或崩溃自动上传的服务模板。

最终协议措辞、主体和联系方式由法务确认；首启及安装器的协议文本应在正式文本确定后同步，不能先在产品内发布未经确认的承诺。
