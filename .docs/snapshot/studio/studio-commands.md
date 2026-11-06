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

目录项包含 `command`、`description`、`category`、`scope`、`readOnly`、`requiresDesktop`、`parameters`、`inputSchema` 和 `schemaSource`。`scope` 区分共享与桌面宿主能力；`requiresDesktop` 进一步表明是否需要实际窗口。所有公开命令的 `schemaSource` 均为 `declared`。无参数命令也明确声明空对象，并拒绝多余参数。

未实现、废弃的运行时桥接口在两端一致不注册，描述及执行均返回 `UNKNOWN_COMMAND`。不再把这些接口算作 Studio 的 GUI 扩展。内置 AI 工具开放策略仍是独立约束，不能据此推断底层业务能力。

参数契约由共享 `CommandSchemas` 与桌面 `DesktopCommandSchemas` 定义，发现接口、普通 IPC、CLI 和 AI 使用同一份规则。执行前递归检查必填项、类型、枚举、范围、互斥形状及嵌套对象/数组；不把字符串、布尔值或小数静默转成默认数值。错误返回 `INVALID_ARGUMENTS`，消息定位参数路径。对象存在性、工程状态和业务语义继续由相应处理器检查。只读属性显式声明，不按命令名前缀猜测。

完整目录受构建特性影响，例如禁用 LMDB 时不暴露全文检索命令；以运行产物的 `list_commands` 为准。共享项在两种宿主必须逐字段相同，此约束由通用回归检测保护。

本轮 Windows 启用 LMDB 的实际产物包含 244 条命令：185 条共享命令、59 条桌面宿主命令，全部声明参数契约。真实桌面会话与独立 CLI 的 185 条共享描述逐字段相同。

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
| 控件发现 | `list_ui_windows` 枚举实际顶层窗口，包括独立窗口与对话框；`find_widget` 支持名称、文字、类型、作用域及数量上限，返回总量与截断标志 |
| 控件写入 | `set_widget_value` 校验控件类型、数值范围、可见/启用/只读状态，并返回真实 `state`；支持 ToggleSwitch |
| 事件投递 | `click_widget`、`key_press`、`open_context_menu`、`trigger_menu_action` 返回作业身份；用 `get_ui_operation` 查询执行状态 |
| 对话框 | `answer_dialog` 只接受当前对话框支持的响应；非法响应报错并保持对话框，不默认接受或关闭 |
| 曲线编辑 | `open_curve_editor` 校验真实对象和打开结果，不再无条件报告成功 |

控件命令统一接收以下目标参数：

- `widget_id`：发现接口返回的会话内不透明身份，适用于匿名控件、同名行控件及多实例面板。控件销毁后身份失效，绝不重新匹配同名替代控件。
- `name`：精确匹配唯一 `objectName`。与 `widget_id` 二选一；多义名称返回 `AMBIGUOUS_WIDGET` 和包含身份的 `result.candidates`。
- `scope_widget_id`：可选，将名称查找限定到指定控件及其子控件。可直接使用面板创建响应、`list_panels` 或窗口树返回的身份。

`get_widget_value`、`get_widget_children`、表格/列表/树读取及截图使用相同解析规则。查询可以检查隐藏或禁用控件；交互要求目标可见、启用且不被模态对话框阻塞。`path` 仅供阅读，不是稳定选择器。`get_ui_tree` 未指定目标时默认主窗口；`key_press` 未指定目标时默认当前焦点控件，无焦点时使用主窗口。

```json
{"cmd":"find_widget","args":{"name":"sourceActive","visible_only":true}}
{"cmd":"get_widget_value","args":{"widget_id":"<discovered-widget-id>"}}
{"cmd":"click_widget","args":{"widget_id":"<discovered-widget-id>"}}
{"cmd":"get_ui_operation","args":{"operation_id":"<returned-operation-id>"}}
```

队列响应的 `queued: true` 只表示已接受。作业依次处于 `queued`、`running`，最终为 `succeeded` 或 `failed`；失败保留错误原因。投递前再次检查目标存活、界面状态及导出互斥，目标销毁、隐藏或出现模态阻塞均不会被静默跳过。`succeeded` 表示 UI 操作已返回，不表示由该事件触发的业务作业已完成；打开阻塞式对话框的操作可持续为 `running`，直到对话框处理结束。会话保留最近 256 个已完成作业，过期或不存在的身份返回 `UI_OPERATION_NOT_FOUND`。

`open_context_menu` 发送真正的上下文菜单事件，滚动控件使用其 viewport 的坐标；可省略 `x/y` 使用中心，也可同时指定两者。禁止上下文菜单的控件会报错。`trigger_menu_action` 只负责触发菜单项，参数为 `action_text` 和可选统一控件目标；省略目标时使用当前弹出菜单。重复菜单文字或重复表格匹配行会明确报错，不猜测第一项。

`batch` 在任何子命令执行前校验所有子命令名称及参数；不支持嵌套批次。`stop_on_error` 默认 `true`，遇到业务错误后停止；设为 `false` 可继续其余命令。任一子命令失败时父响应为 `BATCH_FAILED`，并保留逐项结果、成功/失败/跳过计数。批次不会回滚已成功的操作，需要可回滚流程时使用 `preview_actions` 等事务命令。

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
| `batch_replace_media(items[].sourceID/new_path)` | 使用 `source_id/path`；无效、溢出及重复身份在替换前拒绝 |
| `set_container_property` 的空间属性、`string_value/object_value` | 标量入口只接受 `volume/pitch` 和 `value`；空间属性使用字段组命令 |
| `set_conversion_settings` 的未知 codec | 明确接受 `PCM_Float/ADPCM/Vorbis`，不再静默落到默认编码 |
| `key_press(widget)` / `get_ui_tree(root)` | 使用统一的 `name` 或 `widget_id`；可用 `scope_widget_id` 限定作用域 |
| `trigger_menu_action(emit_context)` | 使用 `open_context_menu`；原 `pos_x/pos_y` 改为 `x/y` |
| `trigger_menu_action(button)` | 使用统一控件目标 `name` 或 `widget_id` |
| `batch(atomic)` | 使用 `stop_on_error`，明确停止后续而非原子回滚 |
| `open_curve_editor(attenuation/attenuation_guid)` | 使用 `name/guid`，二选一 |
| `observe` / `unobserve` | 删除未投递观测结果的空接口；需要当前结果时使用 `query` |
| `ai_event` / `ai_response` | 删除没有接收端的旧注入入口 |
| `apply_template_from_data` | 删除仅完成部分行为的旧入口；创建时使用 `create_project`，成对传入 `template_id` 和 `template_data` |

废弃 Pack 手动管理、旧平台配置、未实现的运行时观测/空间/调制/流式/旧 HarkenLink 等接口不再公开。当前可用能力以运行产物的 `list_commands` 为准；工程平台使用 UUID 命令，空间衰减使用字段组，运行观测使用已实现的性能、Voice 与快照查询。

## 实现与验证

[EditorCommandCatalog.cpp](../../../HarkenStudio/Source/Core/EditorCommandCatalog.cpp) 定义宿主边界与统一描述；[CommandSchemas.cpp](../../../HarkenStudio/Source/Core/CommandSchemas.cpp) 与 [DesktopCommandSchemas.cpp](../../../HarkenStudio/Source/Core/IPC/DesktopCommandSchemas.cpp) 定义参数合同；[CommandArgumentValidator.cpp](../../../HarkenStudio/Source/Core/CommandArgumentValidator.cpp) 递归校验。[IPCServer.cpp](../../../HarkenStudio/Source/Core/IPC/IPCServer.cpp) 统一分派与状态限制。[DesktopWidgetTargets.cpp](../../../HarkenStudio/Source/Core/IPC/DesktopWidgetTargets.cpp) 统一桌面目标身份、交互限制和队列状态。

通用回归覆盖公开目录的完整声明、两宿主合同相等、所有命令拒绝未知参数、嵌套约束与精确身份规则。桌面生命周期回归覆盖重复名称、作用域、失效身份、隐藏/禁用/模态阻塞、入队后目标变化、作业状态及历史容量。CLI 流程验证位于 [verify_studio_cli.py](../../../.scripts/verify_studio_cli.py)，纳入 Windows/macOS 构建门禁。

本轮真实 Windows 桌面会话验证通过：请求外层结构及参数类型拒绝并保留 request_id、完整合同对齐、顶层窗口身份、隐藏控件读取与交互限制、同名候选身份、面板生命周期、队列完成查询、批次参数预检无先行副作用，以及真实未保存确认框阻塞背景交互、取消后原队列操作完成。最终临时报告为 `harken-desktop-contracts-t4kxs3y0/report.json`，仓库本地日志为 `.temp/desktop-contracts-release-smoke.log`。

最终相关原生回归 128/128 通过，结果保存在 `.temp/cli-contracts-release-tests.xml`。其中事务回归覆盖 27 组总线与容器属性的单独/组合修改，验证预览不修改数据、执行确实修改数据，以及回滚完整恢复 Shared 缺省、显式值、平台覆盖和试听状态。总线音高/输出增益及容器音量/音高的事务支持已与实际执行对齐；不会仅恢复当前数值而遗失原有覆盖关系。真实音频与无声设备两套 CLI 流程分别保存在 `.temp/cli-contracts-release-real.log` 和 `.temp/cli-contracts-release-null.log`。

本地验证环境为 Windows。联合构建、禁用 Qt 图形模块的独立 CLI 构建、原生测试和 CLI 流程分别执行；不能把 Windows 通过或 CI 配置存在等同于 macOS 和所有音频硬件组合已验证。
