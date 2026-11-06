# HarkenStudioCLI 命令用法

快照日期：2026-11-06。独立无窗口程序 `HarkenStudioCLI.exe`（macOS 为 `HarkenStudioCLI`）与 [Studio 本体](studio-commands.md) 共享工程核心命令、创作模型、资源处理和打包服务。

## 入口组织

| 入口 | 用途 |
|---|---|
| `--help`、`<入口> --help` | 查看总体或入口专属语法、选项与示例 |
| `exec <name> --help`、`commands <name>` | 查看命令用途、必填/可选参数、类型、范围与默认值 |
| `commands --search <text> --category <name> --read-only` | 按名称/说明/分类搜索、精确分类筛选、只看查询；筛选可单用或组合 |
| `--version --json` | 查询程序版本 |
| `commands [name] --json` | 查询实际命令目录或单条参数，委托共享 `list_commands` / `get_command_schema` |
| `init <directory> --json` | 在尚不存在的目录创建工程 |
| `exec <name> --project <path> --input <file> --save --json` | 打开工程、执行单条业务命令，成功后保存 |
| `run --project <path> --input <file> --save --json` | 在同一会话顺序执行请求数组，全部成功后保存 |
| `shell --project <path> [--json]` | 持续工程、运行时和撤销会话，显式保存和退出 |
| `list-targets --project <project.hsp> --json` | 只读加载已保存工程，查询工程、平台、语言和 Action 身份 |
| `build --project <project.hsp> --all --platform-id <UUID> --self-clean <on\|off> [--output <directory>] [--force] [--json]` | 只读加载已保存工程并构建；`--platform-id` 可重复 |

顶层负责发现、会话执行和离线构建，业务命令沿用统一的 `动词_对象` 标识。`init <directory>` 是创建工程；运行时初始化使用 `exec init_engine` 或会话中的 `init_engine`。

CLI 不注册 GUI 专属、未实现或废弃的接口；未知名称统一返回 `UNKNOWN_COMMAND`。`commands --json` 是当前产物的能力来源，不依赖静态条数。每项提供 `category`、`scope`、`readOnly`、`requiresDesktop`、`inputSchema` 等字段。所有公开命令使用显式参数契约，发现、普通执行和 AI 调用共享同一份约束。无参数命令也声明空对象并拒绝额外字段。

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

## 持续会话

```text
HarkenStudioCLI shell --project GameAudio/GameAudio.hsp
harken> create_action {"name":"PlayFootstep"}
harken> save_project
harken> exit
```

| 操作 | 约定 |
|---|---|
| `help`、`commands [name]` | 查看会话语法、命令目录或参数 |
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

## 共享业务约定

| 任务 | 命令与边界 |
|---|---|
| 业务发现 | `list_commands`、`get_command_schema {"command":"..."}`；与顶层 `commands` 完全相同 |
| 内容与身份 | 对象查询/编辑显式指定 GUID、路径或命令声明的对象身份；创作句柄与运行时 short ID 不可混用 |
| 批量导入 | `import_audio_files {"files":[...],"target_container_id":...}`；目标可为容器、Sound 或输出 Bus，省略则导入根；部分失败返回错误及成功项，不吞掉非法数组元素 |
| 属性定义 | `get_inspector_schema` 使用 `schema_id`，无需 Inspector 面板 |
| 字段组 | `definition_guid` 加 `scope:"Shared"` 或 `platform_ids`；根定义直接编辑字段，继承模式使用共享字段组命令 |
| 总线 | `set_bus_volume_db/get_bus_volume_db` 使用创作 dB；`set_output_bus_gain/get_output_bus_gain` 只读写 Bus 输出增益；路由目的地使用 Bus 层级或可播放定义字段组 |
| Send | `set_send/set_send_level` 的 `level` 是非负线性增益，1 为单位增益；对象查询中的 `sendLevel` 使用 dB |
| 播放 | 在同一会话调用 `init_engine`、所需输出启动及 `post_action/audition_play`；播放前等待当前工程运行时同步。`device_mode` 默认为 `real`；无物理设备的自动化会话可使用 `null` |
| Voice 查询 | `get_active_voices.count` 是原生活跃 Voice 数，`reported_count` 是公开明细行数，二者可能不同 |
| 电平 | `get_performance.master_peak_l/r` 是线性幅度；mixer/metering 快照使用 dB，测量位于主输出增益之前 |
| 搜索 | `search_fulltext` 查询当前工程索引；不可用返回 `SEARCH_UNAVAILABLE`，不伪装为空结果 |
| 文件比较 | `compare_files` 的 `reference_path/candidate_path` 为两个文件；结果 `identical/compared_bytes/different_bytes/length_match` 均按原始字节解释 |

`init_engine` 返回实际的设备模式。运行时不可用时返回 `ENGINE_UNAVAILABLE`，不会把编辑器模拟模式当作初始化成功。已初始化后，无参数调用复用当前运行时；带配置再次调用会提示先 `shutdown_engine`，避免把未生效的设置当作已应用。

旧名称直接迁移，无兼容别名；完整映射见 [Studio 命令迁移表](studio-commands.md#命令迁移)。

`apply_template_from_data` 的旧入口只合并部分模板内容，已从公开命令移除；需要模板工程时使用 `create_project` 的模板输入。桌面 `batch` 的中止选项改为 `stop_on_error`（默认 true），不再用 `atomic` 暗示事务；与 CLI `run` 一样，执行前校验全部请求，运行时失败会让父响应失败，且不回滚先前的外部写入。CLI 会话不公开桌面 `batch`，顺序脚本使用顶层 `run`。

## 转码与异步结果

单条 `transcode_asset` 使用 `source_id`，批量 `batch_convert_media` 使用非空 `source_ids` 数组；二者均要求工程 `platform_id`。批量先校验全部身份及重复项，再创建共享作业，不在 CLI 内另行实现转换。

接受响应的 `result` 包含 `async: true`、`operation_id` 和 `operation`。IPC 客户端按 `operation.status_command/status_args` 查询 `get_transcode_status`，按 `cancel_command/cancel_args` 调用 `cancel_transcode`；两个入口都使用 `operation_id`，不再用资源/平台猜测作业身份。

CLI 自动等待同一作业的最终结果。单条结果含实际 `cache_path/output_size`；批量结果含逐项响应与 `completed/total/succeeded/failed`。任一转换失败令整体失败；取消保留已完成缓存，当前文件结束后不再开始余项。超时/中断会请求取消并等待后台释放资源，随后才继续 shell 或退出。

转码期间 IPC 写操作（包括工程切换）被拒绝，查询和取消仍可调用；从 GUI 关闭工程会取消余项，陈旧工程的结果不写回新工程。`get_pack_build_status/cancel_pack_build` 为导出对应的完成/取消入口。一般异步协议以响应中的 `result.operation` 为准，成功接收请求不等于任务完成。

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

未打开工程的运行时会话可 `load_pack/unload_pack`：先 Init 再内容包，卸载 Init 卸载集合；与工程 Current World 互斥。CLI 等待短暂背压消退，运行时错误、超时或中断才失败。

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

随 Studio 构建 `harken_editor` 会同时构建 CLI；独立构建启用 `HARKEN_BUILD_CORE/TOOLS/STUDIO_CLI`，关闭 Editor/Hub 及其不需要的测试。构建目标 `harken_studio_cli`，工具链要求见根 [README](../../../README.md)。私有 `--resource-worker` 只供资源编译调用，不属于公共终端命令。

[verify_studio_cli.py](../../../.scripts/verify_studio_cli.py) 验证命令发现/筛选、帮助、所有公开命令拒绝未知参数、批处理执行前校验、逐行协议、工程生命周期、保存重开、转换及包体一致性；目录一致性测试位于既有 `AICommandCatalogTest.cpp`。Windows 验证不代表 macOS、所有音频设备和全部参数组合均已验证。

本地验证脚本可使用 `--skip-audio-output` 跳过依赖物理音频设备的检查，并在仍需运行时的流程中使用 `device_mode:null`；默认仍验证真实输出和播放。

本轮 Windows 最终验证：Studio 联合产物使用真实音频设备通过完整 CLI 流程；关闭 Editor/Hub 并禁用 Qt Gui/Widgets/Svg/OpenGL 的独立产物使用无声设备通过相同流程。覆盖中文、重音字符和空格路径、文件与 stdin 输入、纯净发现输出及保存重开；PowerShell 文件参数单独验证通过。对应本地记录为 `.temp/cli-contracts-release-real.log`、`.temp/cli-contracts-release-null.log` 与 `.temp/cli-contracts-powershell-unicode.json`。
