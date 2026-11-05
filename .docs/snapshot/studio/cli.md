# Studio CLI 现状总览

统计日期：2026-11-05。注册表统计基线：`9230ba7c`，Windows / RelWithDebInfo 的 `HarkenStudioCLI commands --json`。命令按名称去重，数量包含只可发现、不能在 CLI 中执行的桌面命令，不含 `build`、`list-targets` 等顶层子命令；后续变化以实际注册表为准。

## 产物与构建

| 项目 | 当前形态 |
|---|---|
| 桌面产物 | Windows：`HarkenStudio.exe`；macOS：`HarkenStudio.app` |
| CLI 产物 | Windows：`HarkenStudioCLI.exe`；macOS：`HarkenStudioCLI` |
| 使用入口 | 终端、脚本及具备终端能力的 Agent；无需运行中的 Studio、账号、MCP 客户端或特定模型插件 |
| 共享实现 | GUI 与 CLI 使用同一套工程模型、编辑命令、校验、撤销、保存和导出服务 |
| Qt 依赖 | CLI 使用 `QCoreApplication`；依赖 Core、Network、Concurrent，不依赖 Gui、Widgets、Svg、OpenGL 或显示平台插件 |
| 随附内容 | 保留共享运行库、必要依赖库及 `schema/`；独立 `Harkenizer` 程序已移除，CLI 仍需随附运行依赖 |
| 桌面包内位置 | Windows 与 `HarkenStudio.exe` 同目录；macOS 位于 `HarkenStudio.app/Contents/MacOS` |
| 程序发现 | 使用绝对路径或将程序目录加入 `PATH`；安装本身不会让所有 Agent 自动发现程序 |

| 构建方式 | 必要开关与目标 |
|---|---|
| 随 Studio 构建 | `HARKEN_BUILD_EDITOR=ON`、`HARKEN_BUILD_STUDIO_CLI=ON`；构建 `harken_editor` 会同时构建 CLI |
| 仅 CLI 构建 | `HARKEN_BUILD_EDITOR=OFF`、`HARKEN_BUILD_HUB=OFF`、`HARKEN_BUILD_STUDIO_CLI=ON`；构建 `harken_studio_cli` |
| 共同依赖 | `HARKEN_BUILD_CORE=ON`、`HARKEN_BUILD_TOOLS=ON` |
| 开发预设复用 | CLI 单独配置时，关闭预设继承的 Editor、Hub、Runtime 单元测试及 Runtime benchmark 开关 |
| 当前限制 | 桌面构建要求 CLI 开关开启；尚无专用 CLI 构建预设或独立 CI 构建任务 |
| 依赖边界检查 | `HarkenStudio/CMakeLists.txt` 在配置阶段检查共享核心和 CLI 的传递链接依赖，包含别名和条件分支中的目标引用；发现 Qt 图形模块即报错 |

独立构建可复用仓库开发预设，将上述开关写入单独构建目录；构建命令为 `cmake --build <build-dir> --config RelWithDebInfo --target harken_studio_cli`。通用工具链要求见根目录 [README](../../../README.zh-CN.md)。

## 命令数量与分布

| 统计项 | 数量 | 口径 |
|---|---:|---|
| 可发现命令 | 348 | `commands --json` 返回的全部命令 |
| 要求桌面环境 | 62 | `requiresDesktop=true`；执行返回 `DESKTOP_REQUIRED` |
| 未标记为桌面专属 | 286 | 包含运行时状态受限命令及旧 `batch` |
| 额外宿主限制 | 1 | `batch` 带 `hostRestriction`，CLI 拒绝执行；批处理使用 `run` |
| 未受上述两类限制 | 285 | 不代表无条件可执行；仍受工程、参数、运行时初始化和业务能力约束 |
| 分派层只读分类 | 125 | `readOnly=true`；不保证没有文件系统副作用 |

| 注册类别 | 总数 | 其中要求桌面环境 |
|---|---:|---:|
| `advanced` | 10 | 0 |
| `asset` | 32 | 0 |
| `audition` | 6 | 0 |
| `container` | 5 | 0 |
| `editor` | 17 | 9 |
| `meter` | 2 | 0 |
| `mixer` | 30 | 1 |
| `project` | 53 | 3 |
| `spatial` | 26 | 0 |
| `system` | 84 | 5 |
| `ui` | 18 | 17 |
| `ui_control` | 8 | 8 |
| `undo` | 2 | 0 |
| 未提供 `category` | 55 | 19 |
| 合计 | 348 | 62 |

类别沿用注册表原值，不根据名称推断是否可在 CLI 执行。部分命令只有分派处理器或目录描述，没有 Action 分类。

| 参数描述来源 `schemaSource` | 数量 | 含义 |
|---|---:|---|
| `declared` | 49 | 已声明对象形式的 JSON Schema |
| `legacy` | 145 | 旧参数描述，经转换提供 `inputSchema`；可能缺少必填项和约束 |
| `undocumented` | 154 | 未提供参数描述；不表示命令没有参数 |

每条命令同时提供 `parameters`、`inputSchema`、`requiresDesktop`、`readOnly` 等信息。实际执行仍使用共享编辑逻辑的校验；不能仅凭旧描述生成完整合法性判断。

## 入口与业务能力

| CLI 入口 | 当前用途 |
|---|---|
| `--help` | 查看终端语法、选项和示例，输出文本 |
| `--version --json` | 查询程序版本 |
| `commands --json` | 枚举共享注册表 |
| `commands <name> --json` | 查询单条命令的参数描述与宿主限制 |
| `init <directory> --json` | 在尚不存在的目录创建工程 |
| `exec <name> --project <path> --input <file> --save --json` | 打开工程、执行单条命令，成功后保存 |
| `run --project <path> --input <file> --save --json` | 在同一会话中依次执行 JSON 命令数组，成功后保存 |
| `list-targets --project <project.hsp> --json` | 只读加载已保存工程，查询工程、平台、语言及 Action 身份 |
| `build --project <project.hsp> --all --platform-id <UUID> --self-clean <on\|off> [--output <directory>] [--force] [--json]` | 只读加载已保存工程并构建；`--platform-id` 可重复以指定多个平台 |

| 业务范围 | 代表命令 | 当前边界 |
|---|---|---|
| 工程生命周期 | `create_project`、`open_project`、`save_project`、`close_project` | 共用工程格式与独占锁，不自动升级 Schema |
| 对象与属性 | `list_items`、`create_item`、`set_item_property`、`move_item` | 共用对象查找、属性校验和编辑服务 |
| 音频导入 | `import_audio` | `path` 指向输入音频；导入和转码沿用共享资源管线 |
| Action 配置 | `create_action`、`set_item_property`、`get_action_definition` | 通过 `steps` 配置播放逻辑，引用使用对应命令要求的身份字段 |
| 路由与发送 | `set_output_bus_routing`、`set_send` | 显式操作工程数据；部分蓝图手势仍为桌面命令 |
| 会话导出 | `export_pack`、`list_project_platforms` | `output_path` 指定输出目录；默认使用工程导出平台选择，可传 `platform_id` 或 `platform_ids`；等待实际完成后返回各平台输出 |
| 试听与监控 | 注册表中的运行时命令 | CLI 启动时不自动初始化音频设备；可显式调用共享 `init` 命令，运行时操作另受初始化状态及业务能力约束 |
| 窗口与布局 | 标记 `requiresDesktop` 的命令 | 可查询描述，不能在 CLI 执行 |

GUI、会话导出和已保存工程构建使用同一套编译服务和包体格式，供 Harken Engine 及集成层消费。编辑对象句柄与运行时 short ID 是不同身份，应使用命令参数明确要求的字段。`exec init` 调用共享的运行时初始化命令，与用于创建工程的顶层 `init <directory>` 不同。

## 两种打包输入

| 边界 | 会话内 `export_pack` | 顶层 `build` |
|---|---|---|
| 输入 | 当前会话内存工程，包含本次流程尚未保存的编辑 | 磁盘上完整已保存工程 |
| 加载 | 使用 `StudioSession` 与共享编辑服务 | 使用 `ProjectLoader` 的 `ReadOnly` 模式，不创建编辑会话 |
| 编辑与保存 | 可在 `run` 中先编辑后导出；`--save` 或 `save_project` 决定是否保存创作文件 | 不接受编辑输入、`--save` 或 `--timeout`；需要事务恢复或 Schema 升级时失败 |
| 范围 | 默认使用导出选择；可指定单 Action、语言及平台，或使用 `all=true` | 要求 `--all`、一个或多个 `--platform-id` 及显式 `--self-clean on/off` |
| 输出与缓存 | 写出包体、伴随文件、构建缓存和临时锁 | 同左；只读约束针对创作文件，工程目录并非完全无写入 |
| 工程占用 | 独占当前工程；另一个进程占用时拒绝打开 | 沿用同一独占规则；GUI 已打开同一工程时拒绝构建 |
| 取消 | 按共享异步状态约定等待及取消 | Ctrl+C 请求取消；已完成包保留，不执行根目录项目信息与自清理 |

`list-targets` 使用与 `build` 相同的只读加载入口，只返回目标信息。`build` 和会话导出最终调用同一 `ProjectPackBuildService`，内部继续保留 `harkenizer_core`、`harkenizer_build` 等库目标名称；程序入口合并不产生第二套工程模型或打包实现。

| `export_pack` JSON 参数 | 类型与默认值 | 约束 |
|---|---|---|
| `platform_id` | UUID 字符串，可省略 | 与 `platform_ids` 互斥；必须属于当前工程 |
| `platform_ids` | UUID 字符串数组，可省略 | 必须非空、无重复，所有平台属于当前工程；两个平台参数均省略时沿用导出选择 |
| `all` | 布尔值，默认 `false` | `true` 包含未勾选包；不能与非空 `action_name` 同用 |
| `force` | 布尔值，默认 `false` | 强制重新生成本次范围内的包 |
| `self_clean` | 布尔值，默认 `false` | 所有请求平台成功后清理退出本次输出清单的文件；不能与非空 `action_name` 同用 |
| `output_path`、`action_name`、`language` | 字符串，可省略 | 保留原输出目录、单 Action 和语言选择语义；不接受其他 JSON 类型 |

私有资源 worker 入口为 `HarkenStudioCLI --resource-worker`，供文件资源接口使用，程序定位覆盖变量为 `HARKEN_STUDIO_CLI_PATH`。它在正常会话创建前分流；主打包链直接调用资源编译库，不启动该 worker。

## 输入输出与会话约定

| 项目 | 当前约定 |
|---|---|
| 单条输入 | `exec` 接收 JSON 对象；来源为 `--args`、`--input <file>` 或 `--input -`（标准输入）；文件及标准输入使用 UTF-8，可避免 shell 引号转义 |
| 批量输入 | `run` 接收 `[{"cmd":"...","args":{...}}]`；先校验全部请求形状，再按顺序执行；首个错误停止，结果带命令名和从 0 开始的索引 |
| 旧批处理 | `batch` 在执行任何内部请求前被拒绝，不能绕过会话与异步完成约定 |
| 保存 | 仅在 `--save` 或显式 `save_project` 时保存编辑；未保存的内存编辑在进程退出时放弃 |
| 外部写入 | 创建、导入、导出等操作可产生自身文件结果；批处理不是文件系统事务，不会因后续失败回滚之前的保存或导出，省略 `--save` 不等于 dry run |
| 工程占用 | 已由其他进程打开的工程返回 `PROJECT_IN_USE`；CLI 不激活桌面窗口代为处理 |
| 关闭 | `close_project` 完成关闭并释放会话的工程锁后才执行下一条；之后编辑须重新打开工程 |
| 标准输出 | `--json` 输出一个 UTF-8 JSON 文档，包含 `protocolVersion: 1` 和 `status: "ok"` 或 `"error"`；转发结果时保留未知字段 |
| 诊断 | 写入 stderr；`--verbose` 包含调试诊断；帮助始终为文本 |
| 异步等待 | CLI 根据 `operation.status_command`、`status_args` 查询完成状态，读取 `in_progress` 和最终 `result`；支持时通过 `cancel_command` 请求取消 |
| 超时与中断 | 会话入口的 `--timeout` 按每次异步等待计时，默认 300 秒，接受 1–86400 的整数；超时或中断时尽可能取消，退出前等待后台任务释放文件；不强制终止同步命令，也不构成整个进程的硬期限；`build`、`list-targets` 不接受该选项 |

| 退出码 | 含义 |
|---:|---|
| 0 | 成功 |
| 1 | 执行失败 |
| 2 | 用法或输入错误 |
| 124 | 异步等待超时 |
| 130 | 中断 |

## 实现归属与核对入口

| 位置 | 职责 |
|---|---|
| [StudioCli/Main.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/Main.cpp) | 终端语法、输入输出和退出码 |
| [StudioCli/StudioSession.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/StudioSession.cpp) | 无窗口会话、工程占用和异步等待 |
| [StudioCli/SavedProjectBuild.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/SavedProjectBuild.cpp) | 已保存工程的只读加载、构建目标查询及打包入口 |
| [StudioCli/ResourceWorker.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/ResourceWorker.cpp) | 私有资源 worker 协议，沿用共享资源编译库 |
| [EditorCommandRegistration.cpp](../../../HarkenStudio/Source/Core/EditorCommandRegistration.cpp) | GUI 与 CLI 共用的命令注册入口 |
| [EditorCommandCatalog.cpp](../../../HarkenStudio/Source/Core/EditorCommandCatalog.cpp) | 汇总命令描述、参数 Schema 和能力标记 |
| [Core/CMakeLists.txt](../../../HarkenStudio/Source/Core/CMakeLists.txt) | `harken_studio_core` 共享静态库；`harken_editor_foundation` 在其上提供桌面辅助实现 |
| [DesktopActions.cpp](../../../HarkenStudio/Source/Core/Actions/DesktopActions.cpp) | `registerDesktopEditorCommands` 注册共享定义后，通过 `ActionManager::setActionHandler` 绑定桌面操作 |
| [IPCDesktopHost.cpp](../../../HarkenStudio/Source/Core/IPC/IPCDesktopHost.cpp) | `IPCServer::setMainWindow` 安装桌面 IPC 处理器 |
| [HarkenStudio/CMakeLists.txt](../../../HarkenStudio/CMakeLists.txt) | 双产物目标、独立 CLI 构建及 Qt 图形依赖检查 |
| [verify_studio_cli.py](../../../.scripts/verify_studio_cli.py) | CLI 协议、桌面隔离、保存重开、转码和导出包一致性的流程验证 |

新增创作能力须通过共享命令暴露，参数描述随命令定义或目录维护；只写在 Widget 内的行为不会自动变成 CLI 能力。GUI 适配层负责控件、桌面命令、Qt Action 和通知展示，共享服务负责工程编辑、撤销历史、日志与异步工作。终端界面设计参考 [Command Line Interface Guidelines](https://clig.dev/)。
