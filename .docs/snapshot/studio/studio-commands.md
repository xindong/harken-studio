# HarkenStudio 本体命令接口总览

快照日期：2026-11-05。本文记录 `HarkenStudio.exe` 桌面宿主的启动参数、IPC 命令入口及注册边界。独立无窗口程序 `HarkenStudioCLI.exe` 的终端语法、构建方式与使用示例见 [StudioCLI 命令用法](studioCLI-commands.md)。

Studio 与 StudioCLI 使用同一套工程模型、编辑命令、校验、撤销、保存、资源处理和打包服务。桌面宿主增加窗口、面板、当前选择和画布交互适配；两端不维护不同的工程模型或打包实现。

## 调用入口

| 入口 | 当前用途 | 会话归属 |
|---|---|---|
| `HarkenStudio.exe --project <path>` | 启动桌面程序并打开工程 | Studio 桌面进程 |
| `HarkenStudio.exe --create-project <directory>` | 通过桌面启动流程创建工程；也可使用 `--project-name`、`--project-location` 和模板参数 | Studio 桌面进程 |
| 本地 TCP IPC | 向运行中的 Studio 发送业务命令或桌面上下文命令 | 当前 Studio 工程、运行时和界面 |
| `--self-test` 等诊断参数 | 程序身份、Schema 校验及工程打开诊断 | 独立诊断流程，不计入业务命令目录 |
| `HarkenStudioCLI.exe` | 独立执行无窗口工程自动化与打包 | CLI 自己的进程和会话 |

Studio 本体的业务命令由 IPC 接收。`commands`、`exec`、`run`、`shell`、`build`、`list-targets` 是 `HarkenStudioCLI.exe` 的子命令，`HarkenStudio.exe` 没有对应的终端子命令解析入口。

Studio 主窗口就绪后启动 IPC 服务，默认监听 `127.0.0.1:9527`。命令使用换行分隔的 UTF-8 JSON：

```json
{"cmd":"get_project_info","args":{},"request_id":"project-info-1"}
```

响应包含 `status`，成功数据位于 `result`；错误提供错误码与消息。请求提供 `request_id` 时，服务会将它带回响应。长连接客户端可先发送 `set_channel_role`，参数为 `{"role":"command"}`，使该连接只接收命令响应。通道设置属于传输控制，不计入下文注册目录。

仓库提供 [PowerShell IPC 客户端](../../../.scripts/studio/harken_ipc.ps1)。Studio 已运行且 IPC 启动成功时，可在仓库根目录调用：

```powershell
& .\.scripts\studio\harken_ipc.ps1 -Command ping
& .\.scripts\studio\harken_ipc.ps1 -Command get_project_info
& .\.scripts\studio\harken_ipc.ps1 -Command get_inspector_schema -ArgsJson '{"schema_id":"Sound"}'
```

IPC 编辑作用于当前桌面会话；断开连接不会结束工程会话。保存由 `save_project` 或桌面保存操作完成。异步操作须继续查询完成状态，例如 `export_pack` 返回构建已启动后，使用 `get_pack_build_status` 查询结果；具体状态与取消入口也由响应的 `operation` 字段提供。

## 注册目录统计

统计口径为 `IPCServer::allCommandNames()` 合并并去重的 ActionManager 与 IPC handler 名称。启动参数、传输控制、二进制监控协议和 CLI 顶层子命令不计入此数。

| 当前注册范围 | 数量 | 含义 |
|---|---:|---|
| Studio 桌面宿主总目录 | 348 | 包含共享业务、桌面上下文及保留的旧接口声明 |
| 同时进入 StudioCLI 的共享命令 | 184 | 具备无窗口实现；CLI 注册项的 `requiresDesktop` 均为 `false` |
| 要求桌面上下文的命令 | 58 | 依赖当前窗口、画布、选择或桌面会话；其中部分会查询或修改工程数据 |
| 其余未进入 StudioCLI 的声明 | 106 | 包含当前运行时桥的空实现、废弃入口和其他不适合 CLI 会话的接口 |

目录核对关系为 `348 = 184 + 58 + 106`，对应启用 LMDB 的构建；禁用 LMDB 时，CLI 不注册全文搜索、搜索状态和搜索 benchmark 三条命令，目录为 181 条。这是注册统计，不代表 348 条桌面接口均已实现完整行为。184 条 CLI 保留命令已在 Windows 下逐项验证至少一个有效场景，所有合法参数组合仍未穷举；工程、运行时状态及参数须满足对应前提。

桌面上下文与工程能力按实际行为区分。命令名称带有 canvas 或 Inspector，不足以判定它必须依赖 GUI：`import_audio_to_canvas` 是共享批量导入，`get_inspector_schema` 是共享静态属性 Schema 查询，`audition_get_state` 读取共享试听状态。`audition_get_position`、`audition_get_speed` 和 `audition_set_speed` 没有可靠的当前运行时实现，已从 CLI 目录排除。运行时桥尚未实现的声明也不能统称为 GUI 能力。

## 桌面上下文命令

| 范围 | 代表命令 | 依赖的桌面状态 |
|---|---|---|
| 当前对象选择 | `select_item`、`select_project_item`、`select_bus`、`select_folder` | Project Explorer、总线或面板当前选中项 |
| 画布操作与视图 | `connect_nodes`、`disconnect_nodes`、`get_blueprint`、`get_node_graph`、`set_canvas_flow` | 当前画布、节点类型、视图位置和交互解析 |
| Inspector 与编辑器面板 | `get_inspector_context`、`create_inspector_panel`、`set_inspector_pinned`、`create_editor` | 面板实例、当前展示对象及固定状态 |
| 布局与外观 | `get_current_layout`、`get_layout_state`、`set_theme`、`get_theme` | 工作区布局与桌面主题 |
| 控件与对话框 | `get_ui_tree`、`click_widget`、`set_widget_value`、`list_open_dialogs`、`answer_dialog` | 实际窗口、控件树和打开的对话框 |
| 桌面会话信息 | `get_recent_logs`、`get_remote_connection_status` | Studio 控制台缓冲与当前远程连接面板 |

58 条命令并非全部只操作控件。例如画布连线可能修改对象关系，图查询同时返回业务结构和画布坐标。共享业务能力通过显式对象、容器、路由及字段组命令提供；桌面入口负责将当前交互上下文转换为这些业务操作。

## GUI 操作与共享业务入口

| Studio 操作 | 两端共享的业务入口 | 适用边界 |
|---|---|---|
| 画布拖入、连线与断线 | `move_item`、`add_child_to_container`、`remove_child_from_container`；总线用 `move_bus`；输出和发送用字段组、`set_send`、`remove_send` | 共享入口直接表达对象关系或路由，无需画布节点坐标 |
| 查询工程关系与信号链 | `get_item_info`、`get_container_info`、`get_signal_chain`、`get_bus_hierarchy` | `get_blueprint`／`get_node_graph` 保留桌面视图信息；与共享查询的 JSON 字段不逐项相同 |
| 一次导入多个音频文件 | `import_audio_to_canvas` | 历史名称保留；接收 `files` 与可选 `target_container_id`，不要求窗口或画布坐标 |
| 输出覆盖与继承 | `set_field_group_mode`、`set_field_group_field`；Bus 属性用 `set_item_property` | 现代可播放定义的 Output 使用字段组；旧 `set_node_override` 布尔值不能代替字段组模式 |
| 输出路由与增益 | 字段组 `outputBusRef`／`outputBusVolume`；支持对象上的 `set_output_bus_routing` | `set_output_bus_routing` 调整输出增益，不负责更换目的总线；字段组定义使用字段组命令 |
| 试听状态与定位 | `audition_get_state`、`audition_set_position` | 状态读取和有效播放中的定位无需试听控件；CLI 当前不提供位置、速度查询或速度修改入口 |
| 属性发现与编辑 | `get_inspector_schema`、`get_item_info`、属性及字段组命令 | Schema 来自同一共享实现；业务命令显式指定对象，面板选择由 Studio 管理 |
| 保存与打包 | `save_project`、`export_pack`、平台与导出选择命令 | 使用同一工程格式、校验与 `ProjectPackBuildService`；打包产物契约见 [打包输出](pack-output.md) |

## 实现与核对位置

| 内容 | 源码或工具 |
|---|---|
| 本体启动参数 | [StartupArguments.cpp](../../../HarkenStudio/Source/Core/StartupArguments.cpp)、[Main.cpp](../../../HarkenStudio/Source/Main.cpp) |
| 桌面 IPC 启动 | [MainWindow.cpp](../../../HarkenStudio/Source/MainWindow.cpp) |
| JSON 协议、注册目录合并与分派 | [IPCServer.cpp](../../../HarkenStudio/Source/Core/IPC/IPCServer.cpp) |
| 共享命令注册 | [EditorCommandRegistration.cpp](../../../HarkenStudio/Source/Core/EditorCommandRegistration.cpp) |
| 桌面动作适配 | [DesktopActions.cpp](../../../HarkenStudio/Source/Core/Actions/DesktopActions.cpp)、[IPCHandlers_DesktopEditor.cpp](../../../HarkenStudio/Source/Core/IPC/IPCHandlers_DesktopEditor.cpp) |
| CLI 宿主过滤与参数描述 | [EditorCommandCatalog.cpp](../../../HarkenStudio/Source/Core/EditorCommandCatalog.cpp) |

Studio 桌面 IPC 目录、StudioCLI 实际注册目录与内置 AI 可用工具集是不同统计范围。AI 的开放策略不决定某个 IPC 业务命令是否已实现；独立 CLI 的参数发现与执行方式以 [StudioCLI 命令用法](studioCLI-commands.md) 为准。
