# HarkenStudio 命令接口

快照日期：2026-11-06。Studio 与独立 [HarkenStudioCLI](studioCLI-commands.md) 共享工程、创作、资源处理、撤销、保存及打包命令；Studio 额外提供窗口、选择、布局与控件命令。

## 产品边界

| 层次 | Studio | StudioCLI |
|---|---|---|
| 共享业务 | 通过本地 TCP IPC 调用 | 通过 `exec`、`run`、`shell` 调用 |
| 桌面状态 | 当前窗口、面板、选择、画布及桌面会话 | 无 GUI 上下文，不注册这些命令 |
| 工程归属 | 当前桌面会话 | 独立持有工程和运行时的无窗口会话 |
| 运行入口 | `HarkenStudio.exe --project <path>` | `HarkenStudioCLI.exe`，macOS 为 `HarkenStudioCLI` |
| 打包 | `export_pack` 使用当前内存工程 | 同一 `export_pack`，另有读取已保存工程的 `build` |

Studio 本体目前通过 IPC 接受业务命令，没有 `exec`、`run`、`shell` 等终端子命令。二者的传输形式不同，但共享命令的名称、参数描述与业务实现相同。CLI 会等待异步操作完成；IPC 返回 `result.operation` 后由客户端查询完成状态。

## 发现与调用

默认监听 `127.0.0.1:9527`。请求为换行分隔的 UTF-8 JSON：

```json
{"cmd":"list_commands","args":{},"request_id":"catalog"}
{"cmd":"get_command_schema","args":{"command":"import_audio_files"}}
```

`list_commands` 返回当前宿主实际支持的目录，`get_command_schema` 查询单条契约；工程选择器状态下也可调用。成功响应为 `status: "ok"` 与 `result`，错误为 `status: "error"`、`code`、`message`。提供 `request_id` 时会原样回传。连接可先发送 `set_channel_role`，参数 `{"role":"command"}`，避免接收广播事件。

目录项包含 `command`、`description`、`category`、`scope`、`readOnly`、`requiresDesktop`、`parameters`、`inputSchema` 和 `schemaSource`。`scope` 区分共享与桌面宿主能力；`requiresDesktop` 进一步表明是否需要实际窗口。`schemaSource: legacy/undocumented` 表示历史参数约束不完整，不能据此认定没有参数。

未实现、废弃的运行时桥接口在两端一致不注册，描述及执行均返回 `UNKNOWN_COMMAND`。不再把这些接口算作 Studio 的 GUI 扩展。内置 AI 工具开放策略仍是独立约束，不能据此推断底层业务能力。

本次 Windows 启用 LMDB 的产物实测为 246 条：186 条共享命令与 60 条桌面宿主命令。186 条共享描述逐字段相同。共享目录中 58 条有正式 Schema，78 条沿用历史参数描述，50 条尚未声明参数；这些数量用于区分目录一致性与参数合同完成度，不代表所有合法输入均已验证。

仓库 [PowerShell 客户端](../../../.scripts/studio/harken_ipc.ps1) 可调用：

```powershell
& .\.scripts\studio\harken_ipc.ps1 -Command list_commands
& .\.scripts\studio\harken_ipc.ps1 -Command get_command_schema -ArgsJson '{"command":"set_bus_volume_db"}'
```

## 共享业务组织

沿用 `动词_对象` 的命令标识，按目录 `category` 浏览；工程身份、运行时身份与 GUI 选择显式区分。

| 任务 | 共享入口与语义 |
|---|---|
| 工程生命周期 | `create_project`、`open_project`、`save_project`、`close_project`；创建要求目标目录不存在，不覆盖现有工程 |
| 工程状态 | `get_project_info`；包含 `project_open`、`closing`，打开成功返回相同工程信息 |
| 内容创作 | 对象、容器、Action、字段组、平台、语言命令共用工程模型与校验 |
| 批量音频导入 | `import_audio_files`；接收 `files` 和可选 `target_container_id`，不接收画布坐标；部分失败返回错误并保留成功项和计数 |
| 总线增益 | `set_bus_volume_db(bus_name, db)` 修改总线增益；`set_output_bus_gain(bus_name, db)` 修改总线输出增益，`get_output_bus_gain` 读回；不更换目的地 |
| 总线静音与独奏 | `set_bus_mute(bus_name, mute)`、`set_bus_solo(bus_name, solo)`；布尔参数须显式提供 |
| 可播放定义输出 | 使用字段组 `outputBusRef`、`outputBusVolume`；Bus 层级使用 `move_bus` 与层级查询 |
| 引擎与播放 | `init_engine`、`shutdown_engine`、`start_output`、`stop_output`、`post_action`；与 CLI 顶层创建工程的 `init` 区分 |
| 音频转码 | `transcode_asset`、`batch_convert_media` 共享转码作业、结果查询和取消协议；不以入队冒充完成 |
| 保存与打包 | `save_project`、`export_pack`；与桌面使用相同工程格式、校验和 `ProjectPackBuildService` |
| 文件比较 | `compare_files(reference_path, candidate_path)` 按字节比较，不解码音频；返回 `identical`、`compared_bytes`、`different_bytes`、`length_match` |
| 执行错误 | `get_exec_errors` 只读；清空使用 `clear_exec_errors`，可指定 domain |
| 衰减距离缩放 | `get_attenuation_distance_scaling` 默认读取 Shared，可用 `platform_id` 查询平台覆盖后的有效值 |

`close_project` 成功表示关闭已被接受，`closing` 表示运行时退场是否仍在进行；IPC 可查询 `get_project_info`，CLI 在关闭完成并释放锁后才执行下一条。

## 桌面扩展

| 范围 | 命令及完成语义 |
|---|---|
| 当前选择 | `select_item`、`select_project_item`、`select_bus`、`get_selected_bus`、`select_folder`；总线选择同步完成，查询返回实际选中状态 |
| 画布 | `get_blueprint`、`set_blueprint_layer`、`get_blueprint_layers`、`connect_nodes`、`disconnect_nodes`；画布数据和手势归桌面，内容关系使用共享业务入口 |
| 布局 | `switch_layout`、`get_current_layout`、`reset_layout`；统一使用 `layout` 参数；命名布局使用 `list_named_layouts` 等命令 |
| 面板几何 | `get_panel_rect` 读取真实可见面板；名称由 `list_panels` 提供，可用 name/typeId/title；多义名称返回 `AMBIGUOUS_PANEL`，隐藏面板返回 `PANEL_NOT_VISIBLE` |
| 控件写入 | `set_widget_value` 校验控件类型、数值范围、启用及只读状态，并返回真实 `state`；支持 ToggleSwitch |
| 事件投递 | `click_widget`、`key_press`、`trigger_menu_action` 的 `queued: true` 仅表示事件入队；必要时 `flush_ui_events` 后读取控件或界面状态 |
| 对话框 | `answer_dialog` 只接受当前对话框支持的响应；非法响应报错并保持对话框，不默认接受或关闭 |
| 曲线编辑 | `open_curve_editor` 校验真实对象和打开结果，不再无条件报告成功 |

`click_widget` 只匹配唯一可见控件；没有可见目标返回 `WIDGET_NOT_VISIBLE`，多义目标返回 `AMBIGUOUS_WIDGET`。其余低层控件查询/写入仍按 `objectName` 首匹配，应使用唯一名称；它们尚不具备共享业务命令的 GUID 身份寻址能力。

## 命令迁移

本轮直接统一命名，不保留旧别名。

| 移除的名称 | 当前入口或替代 |
|---|---|
| `init` / `shutdown`（业务命令） | `init_engine` / `shutdown_engine`；CLI 顶层 `init <directory>` 保留 |
| `trigger_action` | `post_action` |
| `import_audio_to_canvas` | `import_audio_files`；删除 `drop_x/drop_y` |
| `set_mixer_volume` | `set_bus_volume_db`；参数 `bus/volume_db` → `bus_name/db` |
| `set_mixer_mute` / `set_mixer_solo` | `set_bus_mute` / `set_bus_solo`；参数 `bus` → `bus_name` |
| `set_output_bus_routing` / `get_output_bus_routing` | `set_output_bus_gain` / `get_output_bus_gain`，仅 Bus；参数 `path/volume_db` → `bus_name/db` |
| `replay_verify` | `compare_files`；`replay_path` → `candidate_path`，样本数与伪音频指标改为字节结果 |
| `get_node_graph` / `set_nodegraph_layer` / `get_nodegraph_layers` | `get_blueprint` / `set_blueprint_layer` / `get_blueprint_layers` |
| `switch_view` / `get_current_view` | `switch_layout` / `get_current_layout`；`view` → `layout`，包括 `reset_layout` |
| `get_exec_errors(clear:true)` | `clear_exec_errors`；查询不再携带清空副作用 |
| `get_transcode_status(platform_id,source_id)` | 使用转码响应的 `operation_id`，取消同样按作业身份执行 |
| `batch_convert_media(sourceIDs)` | 使用 `source_ids`，重复及非法身份在开始前统一拒绝 |

废弃 Pack 手动管理、旧平台配置、未实现的运行时观测/空间/调制/流式/旧 HarkenLink 等接口不再公开。当前可用能力以运行产物的 `list_commands` 为准；工程平台使用 UUID 命令，空间衰减使用字段组，运行观测使用已实现的性能、Voice 与快照查询。

## 实现与验证

[EditorCommandCatalog.cpp](../../../HarkenStudio/Source/Core/EditorCommandCatalog.cpp) 定义宿主边界与统一描述；[EditorCommandRegistration.cpp](../../../HarkenStudio/Source/Core/EditorCommandRegistration.cpp) 注册共享动作；[IPCServer.cpp](../../../HarkenStudio/Source/Core/IPC/IPCServer.cpp) 统一分派、状态限制与完成协议。桌面适配位于 `Actions/DesktopActions.cpp` 和 `IPCHandlers_Desktop*.cpp`。

既有目录测试检查共享集合、同名参数描述一致性、未打开工程时的发现能力和未知命令；[CLI 流程验证](../../../.scripts/verify_studio_cli.py) 检查协议、工程锁、保存重开、转码和导出一致性。目录一致性不等同于穷举所有参数、硬件状态和平台组合。

本轮通过 Studio/CLI 联合构建、禁用 Qt 图形模块的独立 CLI 构建、两套产物的 CLI 流程验证，以及 79 项相关原生测试。临时工程实测覆盖编辑撤销与保存重开、活跃运行时工程切换、桌面目录和控件状态、批量转码结果及取消，以及真实未保存确认框的非法响应拒绝与取消；未验证 macOS 与全部硬件组合。
