# HarkenStudioCLI 命令用法

快照日期：2026-11-24。独立无窗口程序 `HarkenStudioCLI.exe`（macOS 为 `HarkenStudioCLI`）与 [Studio 本体](studio-commands.md) 共享工程核心命令、创作模型、资源处理和打包服务。

## 入口组织

| 入口 | 用途 |
|---|---|
| `--help`、`<入口> --help` | 查看总体或入口专属语法、选项与示例 |
| `exec <name> --help`、`commands <name>` | 查看命令用途、必填/可选参数、类型、范围与默认值 |
| `commands --search <text> --category <name> --read-only` | 按名称/说明/分类搜索、精确分类筛选、只看查询；筛选可单用或组合 |
| `--version --json` | 查询程序版本 |
| `instances --json` | 查询当前用户的存活 Studio 实例，包含版本、`available` 和 `unavailableReason` |
| `--instance <id> commands/exec/run/shell ...` | 在指定同版本 Studio 会话发现或执行共享与 GUI 命令 |
| `commands [name] --json` | 查询实际命令目录或单条参数，委托共享 `list_commands` / `get_command_schema` |
| `init <directory> --json` | 在尚不存在的目录创建工程 |
| `exec <name> --project <path> --input <file> --save --json` | 打开工程、执行单条业务命令，成功后保存 |
| `run --project <path> --input <file> --save --json` | 在同一会话顺序执行请求数组，全部成功后保存 |
| `shell --project <path> [--json]` | 持续工程、运行时和撤销会话，显式保存和退出 |
| `list-targets --project <project.hsp> --json` | 只读加载已保存工程，查询工程、平台、语言和 Action 身份 |
| `build --project <project.hsp> --all --platform-id <UUID> --self-clean <on\|off> [--output <directory>] [--force] [--json]` | 只读加载已保存工程并构建；`--platform-id` 可重复 |

顶层负责发现、会话执行和离线构建，业务命令沿用统一的 `动词_对象` 标识。`init <directory>` 是创建工程；运行时初始化使用 `exec init_engine` 或会话中的 `init_engine`。

## 独立搭建工程的验收基准

凡是 Studio 能创作并保存的工程内容，都应能在仅启动 `HarkenStudioCLI` 的条件下完成。可以组合多条命令。验收从新建工程开始，按 GUID 或工程路径编辑，保存后用新进程重开、读回实际创作数据，再按目标平台导出 Pack。窗口、鼠标手势、面板焦点等交互可通过 `--instance` 驱动桌面。新增创作功能时，应同时核对独立宿主的命令发现、参数契约、读回和保存路径。

| 搭建环节 | 独立 CLI 入口 |
|---|---|
| 模板与工程设置 | `list_project_templates` 列出已安装模板。给 `create_project` 传 `template_id` 使用该模板，也可同时传 `template_data` 使用自带的模板内容。首次创建即保存模板的总线层级、对象及对象间引用。平台、语言、默认转换、发布目录及导出选项均可在工程会话配置 |
| 物理目录与对象层级 | `create_folder` 创建桶内目录。`create_item` 的 `folder` 接受已有目录（如 `Layers/Ambience`），`parent` 用于业务父对象。`move_item` 的 `folder` 可将对象移入目录或移回桶根，`new_parent` 用于调整父对象 |
| 分组与复制 | `create_parent {"items":["Footstep","Landing"],"type":"Container","name":"Movement"}` 将已有同级对象放入新父对象。`duplicate_items {"items":["Movement"]}` 在原位置复制整棵子树，两者均返回新对象的 GUID |
| 工程内剪贴板 | 在同一次 `shell` 或 `run` 中使用 `copy_item`、`cut_item` 和 `paste_item`。`items` 可指定多个对象。`paste_item {"target_bucket":"Variables"}` 将复制时的快照粘贴到 Variables 桶根，`target_path` 可将复制或剪切的对象粘贴到兼容的父对象 |
| 粘贴属性 | 复制单个来源后，`paste_properties {"targets":["Walk","Run"]}` 将剪贴板中的属性应用到多个兼容目标。显式传 `source_path` 时使用该来源此刻的属性 |
| 另存工程 | `save_project_as {"path":"../Variant/Variant.hsp"}` 保存当前会话中的工程和已登记的音频原件。目标可以是当前工程文件，制作副本时须选择尚无 `.hsp` 工程的目录。成功后同一会话的查询与后续编辑都使用目标工程 |
| Action | `create_action` 创建对象，`set_item_property(property=steps)` 编辑有序步骤。`get_action_definition.steps` 返回可直接写回的步骤数组，包括目标、延迟、Stop 淡出、SetVariable 值与作用域、Reset 作用域及适用的量化参数 |
| 播放和发布 | 工程内可试听并查询电平，`export_pack` 导出当前会话，`build` 按已保存的工程构建。下文列出资源、转换、总线、字段组与时间线的具体参数 |

独立模式不注册 GUI 专属接口；实例模式使用目标 Studio 的完整目录。两种模式都不注册未实现或废弃的接口；未知名称统一返回 `UNKNOWN_COMMAND`。`commands --json` 是当前目标的能力来源，不依赖静态条数。每项提供 `category`、`scope`、`readOnly`、`requiresDesktop`、`inputSchema` 等字段。所有公开命令使用显式参数契约，发现、普通执行和 AI 调用共享同一份约束。无参数命令也声明空对象并拒绝额外字段。

### Effect 创作与验收（2026-11-12）

`create_item(type=Effect)` 创建预设，`set_item_property` 的 `ceilingDb`／`releaseMs` 修改参数，`rename_item` 重命名。Bus 使用 `set_item_property(property=effects,value=[...])`；Sound、Container、Track、Segment、Layer 使用 `replace_field_group_effects`。两种入口共用效果行合同，完整数组表示处理顺序，增删、重排、替换和 Active 均通过该数组提交；替换或转换保留行的 `guid` 与 `bypass`。预设行使用 `effectGuid`，Custom 行使用 `custom.type` 与 `custom.parameters`。

`get_item_info` 对可播放定义返回 `fieldGroups`，包含本地值、继承模式及平台覆盖，可区分未配置与显式空链；Bus FX 和预设参数在 `components` 中读取。

持续 `shell` 试听中，Voice／Bus 编辑前后的链均只含 Limiter（含空链）时，参数、Active 和链条增删、替换、重排即时生效，无需停止重播。涉及其他效果类型时，链结构与来源替换仍延后生效，已安装槽位的 Active 和来源兼容的参数可即时更新。`get_metering_snapshot` 与 `audition_get_state` 可验证输出和播放身份。

两种链写入入口均限制最多 8 槽；第 9 槽在参数校验阶段拒绝，原链不变。域内校验和编译也检查容量，手工篡改工程文件不能绕过导出限制，非法数据不会被自动截断。

独立 CLI 可组合创建预设、设置参数和替换链完成 Save As Preset 的最终结果；这个组合不承诺单次原子撤销。精确的 GUI Save As Preset、Detach、View 菜单及多实例窗口交互通过 `--instance` 的桌面命令测试。

## 输入与选项

`exec` 输入为 JSON 对象，`run` 输入为 `[{"cmd":"...","args":{...}}]`。可使用 `--args`、`--input <file>` 或 `--input -`；文件和 stdin 使用 UTF-8。`--args` 与 `--input` 互斥，`run` 必须明确提供输入；打开工程和执行首条请求前，先校验全部请求形状、命令名及参数契约。每条请求仅接受必填的 `cmd` 和可省略的 `args`，未知字段会报用法错误。命令参数的结构错误使用 `INVALID_ARGUMENTS`，包含参数路径和查询帮助的提示；域内错误（对象不存在、工程状态不允许等）在执行时检查。批处理预检失败返回失败的 `index` 和空 `results`，尚未执行任何请求。

选项按子命令校验，不再静默忽略：`commands` 不接受工程/保存/执行输入参数，`init` 不接受 `--save`，`shell` 不接受 `--save/--args/--input`。单值选项不能重复；`--platform-id` 是允许重复的构建选项。没有子命令却提供执行选项会报用法错误。

```text
HarkenStudioCLI init ./GameAudio --json
HarkenStudioCLI commands --search audio
HarkenStudioCLI commands import_audio_files
HarkenStudioCLI exec import_audio_files --help
HarkenStudioCLI exec list_items --project ./GameAudio/GameAudio.hsp --json
HarkenStudioCLI run --project ./GameAudio/GameAudio.hsp --input edit.json --save --json
```

`edit.json`：

```json
[
  {"cmd":"create_action","args":{"name":"PlayFootstep"}},
  {"cmd":"rename_item","args":{"path":"PlayFootstep","new_name":"PlayPlayerFootstep"}}
]
```

`exec/run` 只在 `--save` 或显式 `save_project` 时保存。无保存退出会丢弃内存编辑，并回退工程内未提交的音频源文件变更；创建工程、导出等操作仍会产生文件结果。批处理不是文件系统事务，后续失败不回滚之前已完成的保存或外部写入；省略 `--save` 不等于 dry run。

## 操作运行中的 Studio

```text
HarkenStudioCLI instances --json
HarkenStudioCLI --instance <id> commands --search widget
HarkenStudioCLI --instance <id> exec get_project_info --json
HarkenStudioCLI --instance <id> exec list_ui_windows --json
HarkenStudioCLI --instance <id> run --input edit.json --save --json
HarkenStudioCLI --instance <id> shell
```

Studio 正常打开工程并就绪后自动发布实例，无需特殊启动选项。CLI 与 Studio 沿用统一软件版本；发现列出各版本的存活实例，并注明是否可执行命令。执行连接再次握手，只有 `version` 完全相同时允许执行，其他版本返回 `VERSION_MISMATCH`。构建标识和渠道用于诊断，不参与版本匹配。实例 ID 每次启动重新生成，多实例必须明确选择。

`--instance` 与 `--project` 互斥，仅适用于 `commands/exec/run/shell`。命令在目标 GUI 线程通过原有分发器执行，使用当前内存工程、撤销栈和窗口。目录与参数预检来自目标实例，包含 GUI 命令。CLI 退出不会丢弃修改或关闭 Studio；`--save` 保存该实例已有及本次产生的未保存修改。实例 `shell` 的 `exit`/EOF 仅断开，拒绝 `exit --discard`。有未保存修改时，CLI 切换/关闭工程返回 `UNSAVED_CHANGES`，包括嵌套批处理。

连接使用当前用户的本地命名管道/套接字，各实例独立端点，不依赖 TCP 9527。超时或中断后已经提交的操作可能继续执行，CLI 不自动重试。共享异步作业复用独立 CLI 的状态查询等待；GUI 事件保留明确的排队结果，使用返回的 `get_ui_operation` 查询，避免对话框阻塞后续输入。

## 持续会话

Studio 本体也提供 `HarkenStudio --project <path> --shell`，复用相同逐行语法，在 GUI 进程内直接执行。`exit` 始终退出当前 shell 所属的进程：独立 CLI 退出 CLI，Studio `--shell` 退出 Studio（包含 GUI），实例模式退出 CLI 而保留另一个进程中的目标 Studio。Studio 的 `exit --discard` 明确放弃当前工程修改后退出；EOF 也请求关闭 Studio，未保存内容使用 GUI 确认。详见 [Studio 同进程 shell](studio-commands.md#studio-同进程-shell)。

Studio 拒绝关闭时继续同一输入会话，保留已读入的后续命令。Studio `--shell` 的响应由后台写出，单响应最多 64 MiB，输出阻塞期间 GUI 和实例接口仍可操作；断管道、中断或 300 秒写出超时结束 shell 并保留 GUI。成功退出的最后响应最多等待写出 5 秒。

```text
HarkenStudioCLI shell --project GameAudio/GameAudio.hsp
harken> create_action {"name":"PlayFootstep"}
harken> save_project
harken> exit
```

| 操作 | 约定 |
|---|---|
| `help` | 显示会话语法、退出约定和当前宿主的完整命令目录；终端按分类展示 |
| `help <name>`、`commands <name>`、`<name> --help` | 查看命令用途、必填/可选参数、类型、范围、默认值和互斥约束，不执行业务命令 |
| `help/commands --search <text> --category <name> --read-only` | 与外部 CLI 相同的搜索和筛选，可组合；包含空格的值使用引号 |
| `help commands`、`help exit` | 查看筛选语法或当前会话的退出行为 |
| `save_project` | 显式保存，无自动保存模式 |
| `exit` | 有未保存修改时返回 `UNSAVED_CHANGES` 并继续会话 |
| `exit --discard` | 放弃未保存编辑及工程内未提交的音频源文件变更后退出；已完成保存或导出的结果不回滚 |
| 切换工程 | 未保存时拒绝 `open_project/create_project/close_project`；成功关闭等待运行时退场及工程锁释放 |
| 单条错误 | 返回错误后继续接受输入；后续正常 `exit` 返回 0，调用方须检查逐条响应 |
| Ctrl+C | 请求取消当前可取消操作；清理结束后恢复输入，空闲时不替代 `exit` |
| EOF | 无未保存修改时正常退出，否则报告 `UNSAVED_CHANGES` 并返回 1 |
| 管道/重定向 | 输入或输出重定向时自动使用逐行 UTF-8 JSON，无需额外 `--json`；不等待 EOF 才执行 |
| 提示符 | 只在交互终端且未指定 `--json` 时向 stderr 输出 |

等待输入期间继续处理事件，音频和后台服务保持运行。无参数启动显示帮助并退出。

```text
harken> help
harken> help --search audio --read-only
harken> commands --search "current project"
harken> help create_action
harken> create_action --help
harken> help exit
```

独立 CLI、连接实例的 CLI 和 Studio `--shell` 使用相同的帮助格式与筛选规则。目录和参数实时来自当前宿主：独立模式仅显示核心能力，桌面会话包含 GUI 命令。交互终端显示可读文本和当前 shell 的调用语法；重定向或 `--json` 仍为逐行结构化响应，`help` 的 `result.commands` 包含实际目录，`result.help` 包含会话指南。错误筛选不会执行命令或退出会话；未知命令提示使用当前 shell 内的帮助。

## 共享业务约定

| 任务 | 命令与边界 |
|---|---|
| 业务发现 | `list_commands`、`get_command_schema {"command":"..."}`；与顶层 `commands` 完全相同 |
| 内容与身份 | 对象查询/编辑显式指定 GUID、路径或命令声明的对象身份；创作句柄与运行时 short ID 不可混用 |
| 批量导入 | `import_audio_files {"files":[...],"target_container_id":...}`；目标可为容器、Sound 或输出 Bus，省略则导入根；部分失败返回错误及成功项，不吞掉非法数组元素 |
| 属性定义 | `get_inspector_schema` 使用 `schema_id`，无需 Inspector 面板 |
| Variable | `create_item` 按 `Step` 或 `Smooth` 创建变量。`set_item_property` 支持 `kind`（0 为 Step，1 为 Smooth）、`range`、`defaultValue` 和 `labels`。修改范围时以 `range_policy` 指定 `stretch` 或 `fix`。标签使用 `anchor` 和 `name`，可从 `get_item_info` 读回并保留标签的 `id` |
| 变量绑定 | `get_inspector_schema` 的可绑定属性行提供 `binding`，包含地址、默认输出范围、过渡时长和曲线点及手柄。填入 `variable` 的 GUID 后可直接交给 `set_binding`。用 `get_modulator_binding` 读回，用 `reorder_bindings`、`remove_binding` 或 `clear_bindings` 调整顺序或移除绑定 |
| 效果链 | Studio 当前提供 Limiter。内联行使用 `custom.type="limiter"` 和 `custom.parameters=[ceilingDb,releaseMs]`，预设行使用 `effectGuid`。Bus 通过 `set_item_property` 写入 `effects`，可播放定义通过 `replace_field_group_effects` 写入 Shared 或平台效果链 |
| 衰减 | `set_attenuation_curve` 编辑 `volume` 或 `pitch` 的 `points` 与贝塞尔 `handles`，`get_attenuation` 读回完整定义。对象通过空间字段组的 `spatial.attenuationRef` 引用衰减预设 |
| 字段组 | `definition_guid` 加 `scope:"Shared"` 或 `platform_ids`；根定义直接编辑字段，继承模式使用共享字段组命令 |
| 总线 | `set_bus_volume_db/get_bus_volume_db` 使用创作 dB。`set_output_bus_gain/get_output_bus_gain` 读写 Bus 的 Shared 输出增益。用 `set_item_property` 设置 `outputBusVolume` 时，传入 `platform_ids` 可按平台写入 dB，传入同一组 ID 和 `override_operation="clear_platform_override"` 可恢复 Shared 值。父 Bus 的 `get_bus_properties.inputs[].level_db` 返回当前试听平台的有效增益。路由目的地使用 Bus 层级或可播放定义字段组。 |
| Send / Sidechain | `set_send`、`set_send_level` 和 `remove_send` 通过目标、`type`（`Send` 或 `Sidechain`）及布尔值 `duckTarget` 定位路由。默认操作 Send。Bus 命令的 `level` 是非负线性增益，1 为单位增益。路由行中的 `sendLevel` 使用 dB |
| Input / Receive 来源 | 在目标 Bus 上调用 `get_bus_properties {"bus_id":"<Bus GUID>"}`。`inputs` 列出子 Bus 和向该 Bus 主输出的可播放定义，`receives` 列出有效的 Send 和 Sidechain 来源。两组均使用当前试听平台，每行包含 `source_guid`、`source_name`、`source_authoring_handle` 和 `level_db`。`receives` 还包含 `type` 和 `duckTarget`。工程重开后可用 `source_guid` 定位同一来源 |
| 试听上下文 | `set_preview_context` 接受 `platform_id`、`all_platforms`、`language` 和 `media_mode` 中的一项或多项，`get_preview_context` 读回完整选择。`all_platforms` 控制工具栏的 All 编辑范围，`platform_id` 选择主试听平台。平台 ID 和语言取自 `list_project_platforms` 和 `list_project_languages`。媒体模式使用 `Original` 或 `Converted`。选择按工程保存在本机，无需 `--save` |
| 播放 | 在同一会话调用 `init_engine`、所需输出启动及 `post_action/audition_play`；播放前等待当前工程运行时同步。`device_mode` 默认为 `real`；无物理设备的自动化会话可使用 `null`（只在显式推进时前进）或 `null_realtime`（输出启动后按真实时间推进，电平与预览可观测） |
| 试听定位 | `audition_set_position` 等待所返回请求的实际 `applied/failed` 结果；`audition_get_seek_status` 使用 `audition_seek_request_id` 查询相同请求 |
| Voice 查询 | `get_active_voices.count` 是原生活跃 Voice 数，`reported_count` 是公开明细行数，二者可能不同 |
| 电平 | `get_performance.master_peak_l/r` 是线性幅度；mixer/metering 快照使用 dB，测量位于主输出增益之前 |
| Vorbis 性能 | `get_performance` 和 `get_engine_snapshot.performance` 提供 `vorbis_decode_last_us`（上一个音频块的常驻 Vorbis 解码工作时间，各渲染线程累加，单位为微秒）与 `vorbis_decoder_count`（当前持有解码状态的实例数） |
| 搜索 | `search_fulltext` 查询当前工程索引；不可用返回 `SEARCH_UNAVAILABLE`，不伪装为空结果 |
| 文件比较 | `compare_files` 的 `reference_path/candidate_path` 为两个文件；结果 `identical/compared_bytes/different_bytes/length_match` 均按原始字节解释 |

`init_engine` 返回实际的设备模式。运行时不可用时返回 `ENGINE_UNAVAILABLE`，不会把编辑器模拟模式当作初始化成功。已初始化后，无参数调用复用当前运行时；带配置再次调用会提示先 `shutdown_engine`，避免把未生效的设置当作已应用。

Studio 的对象试听、Action 播放与容器重置统一使用 AudioEngine 的当前创作版本就绪检查。GUI 对象与 Action 试听在同步期间排队，完成后重新校验项目、运行时和目标身份再播放；取消、目标删除、同步失败或等待超时会结束请求。GUI 重置在未就绪时明确拒绝执行。

独立 CLI、实例 CLI 与 Studio `--shell` 在共享分发器复用同一检查，再执行 `post_action/post_action_3d/audition_play/audition_reset`。只有明确尚未提交操作的 `RUNTIME_SYNC_PENDING` 会等待后重试；超时、中断、同步失败或等待期间切换工程都返回失败。已有旧世界不能放行新操作。已有播放实例的停止、定位和 File Viewer 原始音频试听不等待当前创作版本。

试听定位复用共享异步作业协议，成功接收与实际应用区分。完成查询保留全部进行中的请求及最近 128 条完成记录；已过期的身份返回 `NOT_FOUND`，引擎关闭或原生定位失败返回可查询的失败终态。附加会话超时/中断只停止等待，应按返回的 `operation.status_command/status_args` 查询结果后再决定下一步。

旧名称直接迁移，无兼容别名；完整映射见 [Studio 命令迁移表](studio-commands.md#命令迁移)。

`apply_template_from_data` 的旧入口只合并部分模板内容，已从公开命令移除；需要模板工程时使用 `create_project` 的模板输入。桌面 `batch` 的中止选项改为 `stop_on_error`（默认 true），不再用 `atomic` 暗示事务；与 CLI `run` 一样，执行前校验全部请求，运行时失败会让父响应失败，且不回滚先前的外部写入。CLI 会话不公开桌面 `batch`，顺序脚本使用顶层 `run`。

## 音频资源与 Sound 引用

Resources 命令的 `folder`、`path` 和 `paths` 均相对工程的 `AudioSources` 目录，子目录使用 `/`。省略 `folder` 或传空字符串表示根目录。导入参数 `files` 接受原始文件路径或需要递归扫描的目录。

| 操作 | 命令与用法 |
|---|---|
| 浏览与建目录 | `list_audio_resources` 返回当前目录直属的 `folders` 和 `files`，已登记文件附带 `source_id`。`create_audio_folder {name,folder?}` 返回实际创建的相对路径。`delete_audio_folder {path}` 删除空目录。 |
| 导入原件 | `import_audio_resources {files,folder?,replace?}` 将文件放入 Resources。相同内容复用，同路径不同内容需要显式传 `replace:true`。同一批次多个不同版本竞争同一文件名时，先选择一个版本再提交。 |
| 移动与改名 | `move_audio_resources {paths,folder?}` 批量移动文件或目录。`rename_audio_resource {path,name}` 返回实际改名后的路径，音频文件名须含原扩展名。已有 Sound 引用跟随资源 ID。 |
| 修复丢失文件 | `relink_audio_resource {source_id,file}` 使用外部文件补回或替换指定资源，保留原路径与 ID，支持撤销。`restore_audio_resource {source_id,missing_id}` 将同目录中的现有资源改名为缺失资源的文件名，并将缺失资源的全部引用转到保留的 `source_id`。 |
| 导入 Sound 引用行 | `import_audio_files {files,target_container_id,language?,entry_id?}` 的目标使用 Sound 的 `authoring_handle`。`language` 选定已配置的语言组，`entry_id` 选定具体行，可消除同名缺失引用的歧义。两者同时提供时必须指向同组。 |
| 编辑引用 | `get_item_info` 返回 `components.sound.sources`。`set_item_property(property=sources)` 提交完整的普通组和语言组，可以增删、排序、换源、指定共享或分平台 Active，以及编辑每行的裁剪、循环、淡入淡出。 |

```text
harken> create_audio_folder {"name":"Ambience"}
harken> import_audio_resources {"folder":"Ambience","files":["D:/Audio/Rain.wav"]}
harken> list_audio_resources {"folder":"Ambience"}
harken> rename_audio_resource {"path":"Ambience/Rain.wav","name":"HeavyRain.wav"}
harken> save_project
```

资源文件编辑与 Studio 共用撤销历史。独立 CLI 的 `exec/run` 使用 `--save` 或显式调用 `save_project` 提交，放弃会话会回退未提交的文件操作。批量导入的结果含成功、重复、失败计数，部分导入失败时可在会话内检查成功项并决定是否保存。

## Conversion 创作与转换

Conversion 是 Conversions 桶内的独立预设，按源文件大小为每条规则保存采样率、格式和适用于 Vorbis 的质量。对象的 Conversion 字段组在 Shared 或平台覆写中引用预设或继承父级，各平台默认值由 `set_default_conversion` 设置。共享命令同时供独立 CLI、Studio shell、IPC 与 AI 使用。

`init` 创建的默认工程包含三个内置预设。Windows 和 macOS 使用 Default Quality，Android 和 iOS 使用 Medium Quality，Multiple Quality 可手动选用。已有工程可用 `get_project_info` 查看各平台实际的 `default_conversions`。

| 命令 | 参数 |
|---|---|
| `create_conversion` | `name`；可选 `folder`，为桶内已有相对目录或 `Conversions/` 开头的路径 |
| `get_conversion_fields` | `conversion`。返回预设身份和完整 `rules`，可直接将该数组传给 `set_conversion_fields` |
| `set_conversion_fields` | `conversion` 和完整的 `rules` 数组 |
| `set_conversion_rule_range` | `conversion`、从 0 开始的 `rule_index`，以及 `min_bytes`、`max_bytes` 中至少一项。大小等于 `min_bytes` 的文件归前一行，等于 `max_bytes` 的文件归本行。首行从 0 开始，末行延伸到无穷，相应参数须省略 |
| `set_object_conversion` | `object`、`inherit`；可选 `settings` 为预设选择器或 `null`；可选 `platform_id` 改写该平台覆写，缺省为 Shared（写入 Shared 引用同时清除各平台覆写） |
| `set_default_conversion` | `platform_id`；`conversion` 为预设选择器或 `null` |
| `get_effective_conversion` | `object`；可选 `platform_id` |
| `get_conversion_results` / `convert` | `conversion`；可选 `platform_id` |

对象与预设选择器接受 GUID、无歧义路径或名称。查询与转换命令省略 `platform_id` 时使用当前主平台。`set_conversion_fields` 的 `rules` 完整替换规则表。每项包含 `max_bytes`、`sample_rate` 和 `format`。Vorbis 行还需要 `quality`（0–10 的整数，默认档位为 4），PCM 行省略质量。有限字节上限严格递增，最后一行的上限为 `null`（`Inf`）。每个 Source 按原始文件大小匹配首个包含该大小的上限，取该行的全部设置。格式和质量一致的预设也接受旧命令参数 `format/quality/sample_rate_tiers`。采样率为 `0/8000/11025/16000/22050/24000/32000/44100/48000`，`0` 或不低于原始采样率的目标保留原始采样率。

`set_object_conversion` 的 `inherit:true` 不接受 `settings`；`inherit:false,settings:null` 明确选择不转换，省略 `settings` 沿用字段组本地历史。预设与引用编辑共用工程撤销栈。一次 `set_conversion_rule_range` 可同时移动两个边界，并保留各行的编码设置。`convert` 只产生缓存，不进入撤销历史。以下流程在已导入 Sound 的工程会话执行，将 `<Sound selector>` 替换为目标对象身份：

```text
harken> create_conversion {"name":"PreviewPCM"}
harken> set_conversion_fields {"conversion":"PreviewPCM","rules":[{"max_bytes":1048576,"sample_rate":0,"format":"PCM"},{"max_bytes":null,"sample_rate":24000,"format":"Vorbis","quality":4}]}
harken> set_object_conversion {"object":"<Sound selector>","inherit":false,"settings":"PreviewPCM"}
harken> set_default_conversion {"platform_id":"<Platform ID>","conversion":"PreviewPCM"}
harken> get_effective_conversion {"object":"<Sound selector>"}
harken> get_conversion_results {"conversion":"PreviewPCM"}
harken> convert {"conversion":"PreviewPCM"}
harken> set_preview_context {"platform_id":"<Platform ID>","media_mode":"Converted"}
harken> save_project
```

`get_effective_conversion` 返回预设、来源对象与平台；Sound 另返回当前语言有效 Source 的身份、最终采样率、格式以及适用于 Vorbis 的质量。`get_item_info` 对可播放定义返回 `conversion` 与 `fieldGroups.shared.conversion`，预设在 `components.conversion` 读回原生配置；`get_project_info.default_conversions` 以平台 ID 为键列出各平台默认预设 GUID，无默认值的平台不出现。

`get_conversion_results` 仅检查现有缓存，不触发编码。返回 `sources`，每行包含 Sound、Source、语言、`original` 和 `converted`；未转换时 `converted:null`，已有结果带格式、采样率、声道、帧数、字节数、时长和路径。`convert` 使用相同范围，覆盖该平台各工程语言下实际引用预设的有效 Source，同步等待完成后返回 `conversion_result`（`converted/cached/canceled/errors`）与 `sources`。失败返回 `CONVERSION_FAILED`，成功生成的缓存保留。该同步命令不使用旧作业协议：独立 CLI 进程内执行不受异步作业等待期限约束；`--instance` 附加模式仍受 `--timeout` 的传输等待期限约束，等待超时不会取消 Studio 侧正在执行的同步转换。旧 `set_conversion_settings/transcode_asset/batch_convert_media/get_transcode_status/cancel_transcode` 已移除。

导出按每个 Source 的有效规则写入 PCM 或常驻 Vorbis。Vorbis 支持 1、2、4、6、8 声道，Converted 试听使用相同的编码块和 Engine 解码器。

## 可播放对象、容器和时间线

`create_item` 支持 Sound、Container、Track、Segment 和 Layer。`parent` 可填写父对象 GUID、路径或无歧义名称，父子类型沿用 Studio 工程树的约束。`get_inspector_schema` 可查询各类型的 Inspector 字段。`set_item_property` 编辑 `notes` 后，可用 `get_item_info.notes` 读回。可播放定义的 `playableProperties` 包含已设置的属性及按平台写入的覆盖，`fieldGroups` 包含输出、发送、空间、效果和转换设置。

| 编辑内容 | CLI 写入与读回 |
|---|---|
| 容器 | `set_item_property` 的 `mode` 可设为 0 Random、2 Sequence、4 Match、5 Blend。Match 和 Blend 分别保存自己的 `variableResponse`，0 为 Fixed，1 为 Live。在 `get_item_info.components.selection` 读取 `matchResponse`、`blendResponse`、`selections`。用 `selections` 编辑 Shared 或平台的子项、权重、Match 条件及 Blend 曲线；Match 条件行位于各表的 `matchRows`，按数组顺序排列，`target` 可为 null，无目标的行保留在工程中但不参与播放。 |
| Track | `components.tracktimeline.clips` 列出片段的 `clipGuid`、`targetGuid`、起点、时长及出现序号。`set_item_property(property=clips)` 接受 `{rows,occurrenceOrdinalHighWater}`，可增删片段或调整时长。保留既有行的身份，新行使用新 GUID 和新的出现序号。 |

用 `move_track_clips` 整体移动一个或多个片段。每行指定源 Track、目标 Track、片段 GUID 和目标起点，跨轨移动要求两个 Track 属于同一 Segment。移动后片段保留原 GUID。若源 Track 还有其他片段使用同一目标对象，Studio 会在目标 Track 创建副本，否则将该对象一同移入目标 Track。一条命令对应一次撤销。以下命令在已打开工程的 shell 中执行，片段 GUID 来自第一条查询。

```text
harken> get_item_info {"path":"Lead"}
harken> move_track_clips {"moves":[{"source_track":"Lead","destination_track":"AlternateLead","clip_guid":"<clipGuid>","start":480}]}
harken> save_project
```

## 两种打包输入

| 边界 | 会话 `export_pack` | 顶层 `build` |
|---|---|---|
| 输入 | 当前内存工程，含未保存编辑 | 磁盘上完整已保存工程 |
| 平台 | 默认工程导出选择；可传 `platform_id` 或 `platform_ids` | 显式一个或多个 `--platform-id` |
| 范围 | 导出选择、指定 Action 或 `all:true` | 要求 `--all` |
| 清理 | `self_clean` 默认 false，所有平台成功后才清理 | 显式 `--self-clean on/off` |
| 保存 | `--save` 或 `save_project` 决定是否保存创作文件 | 不接受编辑输入、`--save` 或 `--timeout` |
| 共同服务 | 同一工程格式、包体格式、校验与 `ProjectPackBuildService` | 同左 |

`export_pack` 的单个平台与平台数组互斥，平台数组须非空且无重复；`all:true/self_clean:true` 不能与非空 `action_name` 同用。支持 `output_path/language/force`。两种构建均会写出包体、缓存及临时锁；只读指创作文件。已被 Studio 或其他进程占用的工程返回 `PROJECT_IN_USE`。

`export_pack` 继续使用共享异步作业协议：IPC 按响应中的 `operation.status_command/status_args` 查询 `get_pack_build_status`，按 `cancel_command/cancel_args` 调用 `cancel_pack_build`。CLI 自动等待终态；成功接收请求不等于构建完成。

未打开工程的运行时会话可 `load_pack/unload_pack`：先 Init 再内容包，卸载 Init 卸载集合；与工程 Current World 互斥。CLI 等待短暂背压消退，运行时错误、超时或中断才失败。

`audition_stop` 返回停止请求的当前状态：`stopping` 表示仍在停止，`stopped` 表示目标试听已经结束，`stop_failed` 表示失败且保留控制记录。该命令不阻塞等待淡出；通过 `audition_get_state` 查看逐实例 `stop_state/stop_result`，直到目标实例消失。等待延迟、无声间隙和未来调度均属于试听生命周期。`stop_action` 同样区分 `stopping/stopped/rejected`，只在运行时确认结束后从播放列表移除。

## 输出与退出码

stdout 承载结果，stderr 承载诊断；`--verbose` 增加调试日志。一次性业务执行入口输出一个 JSON 文档，`--json` 紧凑输出；未带 `--json` 的 `commands` 目录/详情及 `--version` 使用文本，shell 重定向逐条一行。JSON 响应包含 `protocolVersion:1`、`status`，转发者应保留未知结果字段。帮助默认使用文本，添加 `--json` 后也返回标准 JSON 响应；`exec <name> --help --json` 返回该命令契约。

| 退出码 | 含义 |
|---:|---|
| 0 | 成功 |
| 1 | 执行失败 |
| 2 | 用法或输入错误 |
| 124 | 异步等待超时 |
| 130 | 中断 |

`--timeout` 默认 300 秒，接受 1–86400 的整数，约束每次异步等待；同步阶段与取消后的资源清理不是整个进程的硬期限。`build/list-targets` 不接受此选项。

## 产物与验证入口

CLI 使用 `QCoreApplication`，依赖 Qt Core/Network/Concurrent，无 Gui/Widgets/Svg/OpenGL 或显示平台插件依赖。Windows 与 Studio 同目录；macOS 位于 `HarkenStudio.app/Contents/MacOS`。仍需共享运行库、依赖库和 `schema/`，安装不会自动修改终端 PATH。

构建 `harken_studio` 会同时构建 CLI。独立构建启用 `HARKEN_BUILD_ENGINE_DEV`、`HARKEN_BUILD_STUDIO_CLI`，关闭 `HARKEN_BUILD_EDITOR`、`HARKEN_BUILD_HUB` 及相应产品的单元测试。构建目标为 `harken_studio_cli`。工具链要求见根 [README](../../../README.md)。

[verify_studio_cli.py](../../../HarkenStudio/Test/E2E/verify_studio_cli.py) 验证命令发现/筛选、帮助、所有公开命令拒绝未知参数、批处理执行前校验、逐行协议、工程生命周期、保存重开、转换及包体一致性；目录一致性测试位于既有 `AICommandCatalogTest.cpp`。Windows 验证不代表 macOS、所有音频设备和全部参数组合均已验证。

本地验证脚本可使用 `--skip-audio-output` 跳过依赖物理音频设备的检查，并在仍需运行时的流程中使用 `device_mode:null`；默认仍验证真实输出和播放。

本轮 Windows 最终验证：Studio 联合产物使用真实音频设备通过完整 CLI 流程；关闭 Editor/Hub 并禁用 Qt Gui/Widgets/Svg/OpenGL 的独立产物使用无声设备通过相同流程。覆盖中文、重音字符和空格路径、文件与 stdin 输入、纯净发现输出及保存重开；PowerShell 文件参数单独验证通过。对应本地记录为 `.temp/cli-contracts-release-real.log`、`.temp/cli-contracts-release-null.log` 与 `.temp/cli-contracts-powershell-unicode.json`。
