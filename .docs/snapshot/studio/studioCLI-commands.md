# HarkenStudioCLI 命令总览

快照日期：2026-11-05。本文记录 `HarkenStudioCLI.exe`（macOS 为 `HarkenStudioCLI`）的独立无窗口入口、命令目录和会话约定。Studio 本体的命令接口见 [Studio 命令接口总览](studio-commands.md)。两端共享工程模型、创作逻辑、资源处理和打包服务。

## 产物与构建

| 项目 | 当前形态 |
|---|---|
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

## 命令目录与参数描述

| 统计项 | 当前数量与含义 |
|---|---|
| 实际注册业务命令 | 启用 LMDB 时为 184；以 `HarkenStudioCLI commands --json` 返回值为准 |
| 可选搜索后端 | `HARKEN_DISABLE_LMDB=ON` 时不注册 `search_fulltext`、`search_status`、`run_search_benchmark`，目录为 181 条 |
| 桌面专属命令 | 0；所有目录项的 `requiresDesktop` 均为 `false` |
| 顶层终端入口 | `init`、`exec`、`run`、`shell`、`build`、`list-targets` 等另行列出，不计入业务命令数量 |
| 执行覆盖 | Windows 下逐项验证了 184 条保留命令的至少一个有效场景；不代表覆盖所有参数组合或其他平台 |

CLI 只注册具有无窗口实现的命令；Studio 的窗口、布局、当前选择和画布上下文命令，以及当前运行时桥的空实现、废弃接口不进入此目录。未注册名称在描述和执行时均返回 `UNKNOWN_COMMAND`。命令存在仍要求工程、参数、运行时状态及内容满足对应前提。

工程能力按实际实现划分；历史名称带有 canvas 或 Inspector 不表示依赖界面。`import_audio_to_canvas` 提供共享批量导入，`get_inspector_schema` 读取共享属性定义，`audition_get_state` 读取共享试听状态，均可由 CLI 独立调用。

本轮从 CLI 排除 13 个旧入口，Studio 桌面目录仍保留这些声明：

| 旧入口 | 当前替代与边界 |
|---|---|
| `set_platform_profile`、`set_target_platforms` | 使用 `set_project_platform_profile` 及 `create_project_platform`、`apply_project_platforms` 等工程平台命令，以工程平台 UUID 指定目标 |
| `set_voice_language`、`set_preferred_format` | 通过工程语言、音源和平台转换配置生成内容；当前没有对应的逐 Voice 语言覆盖或运行时格式偏好入口 |
| `get_object_audio_status`、`get_cpu_budget_state`、`get_render_plan` | 使用已实现的 `get_performance`、`get_active_voices`、`get_engine_snapshot` 获取各自提供的观测；不提供这三个旧查询承诺的完整视图 |
| `audition_get_position`、`audition_get_speed`、`audition_set_speed` | 使用 `audition_get_state` 查询状态、`audition_set_position` 在有效播放中定位；当前没有速度控制或位置、速度查询替代 |
| `bind_attenuation_to_object`、`unbind_attenuation_from_object`、`set_attenuation_distance_scaling` | 使用空间字段组的 `attenuationRef`、`attenuationDistanceScaling`，通过共享字段组命令配置引用、清除引用和距离缩放 |

| 参数描述来源 `schemaSource` | 含义 |
|---|---|
| `declared` | 已声明对象形式的 JSON Schema |
| `legacy` | 旧参数描述，经转换提供 `inputSchema`；可能缺少必填项和约束 |
| `undocumented` | 未提供参数描述；不表示命令没有参数 |

每条命令提供 `parameters`、`inputSchema`、`requiresDesktop`、`readOnly` 等信息。`readOnly` 是分派层分类，不保证没有文件系统副作用。实际执行仍使用共享编辑逻辑的校验；不能仅凭旧描述生成完整合法性判断。类别沿用注册表原值，部分分派处理器没有 `category`。

## 已验证范围与限制

当前验证使用 Windows 的 RelWithDebInfo 产物。184 条保留命令均执行了至少一个有效场景，并核对返回值与实际工程、文件产物或运行时状态；仅返回成功而未产生预期效果的入口已经修复或排除。

| 验证范围 | 已观察结果 |
|---|---|
| 构建与依赖 | Studio 与 CLI 联合构建、禁用 Qt 图形模块的独立 CLI 构建通过；111 项 CTest 通过；隔离目录只携带 CLI、共享运行库、Qt Core／Network／Concurrent 及 Schema，也能完成工程流程 |
| 工程与打包流程 | 创建、编辑、保存重开、转码、会话导出及已保存工程只读构建通过；两种入口生成的包体和伴随文件逐字节一致，未保存内容只进入会话导出；可用 [verify_studio_cli.py](../../../.scripts/verify_studio_cli.py) 复核 |
| 试听与属性发现 | 工程打开／运行时初始化的两种顺序、编辑后立即播放、输出重启、试听状态和有效播放中的定位，以及十种属性 Schema 查询通过 |
| 创作参数与持久化 | 语言、批量音频导入与转换、Send 增益及非法输入、容器属性持久化与撤销重做、工程平台编辑和导出设置均有实际验证 |
| 运行时与包生命周期 | 真实音频输出、总线与主输出增益、Voice 统计、Listener 写读，以及设备输出期间的包加载、查询、卸载、重载通过；内容和 Init 卸载后的在播 Voice 生命周期另有原生验证 |
| 搜索与文件比较 | 新工程自动建立 LMDB 索引，查询反映未保存重命名；索引不可用明确失败；`replay_verify` 核对相同文件、已知字节差异和缺失文件错误 |
| 持续会话 | 逐行管道响应、错误后继续、显式保存重开、未保存退出保护和 EOF 均通过；Windows 真实终端重复 Ctrl+C 后仍可执行命令，打包中断后释放输出锁并恢复会话 |

`load_pack`／`unload_pack` 遇到运行时尚未消费的发布所造成的短暂背压时，会在会话超时范围内等待并重试；实际错误、超时或中断才使调用失败。

上述覆盖按保留命令的有效场景计数，未穷举所有参数、工程规模和运行状态。本轮未验证 macOS 构建及完整硬件资格测试。

## 入口与业务能力

| CLI 入口 | 当前用途 |
|---|---|
| `--help` | 查看终端语法、选项和示例，输出文本 |
| `--version --json` | 查询程序版本 |
| `commands --json` | 枚举当前 CLI 宿主实际注册的业务命令 |
| `commands <name> --json` | 查询已注册 CLI 命令的参数描述；Studio 专属或未实现接口不在此目录 |
| `init <directory> --json` | 在尚不存在的目录创建工程 |
| `exec <name> --project <path> --input <file> --save --json` | 打开工程、执行单条命令，成功后保存 |
| `run --project <path> --input <file> --save --json` | 在同一会话中依次执行 JSON 命令数组，成功后保存 |
| `shell --project <path> [--json]` | 保持工程、运行时和撤销历史，逐行接受命令；显式保存和退出 |
| `list-targets --project <project.hsp> --json` | 只读加载已保存工程，查询工程、平台、语言及 Action 身份 |
| `build --project <project.hsp> --all --platform-id <UUID> --self-clean <on\|off> [--output <directory>] [--force] [--json]` | 只读加载已保存工程并构建；`--platform-id` 可重复以指定多个平台 |

| 业务范围 | 代表命令 | 当前边界 |
|---|---|---|
| 工程生命周期 | `create_project`、`open_project`、`save_project`、`close_project` | 共用工程格式与独占锁，不自动升级 Schema |
| 对象与属性 | `list_items`、`get_item_info`、`create_item`、`set_item_property`、`move_item` | 显式指定对象身份，共用对象查找、属性校验和编辑服务 |
| 属性定义 | `get_inspector_schema` | 使用 `schema_id` 指定类型，无需工程或 Inspector 面板；参数枚举见命令描述 |
| 音频导入 | `import_audio`、`import_audio_to_canvas` | 单文件使用 `path`；批量入口使用 `files` 和可选目标；导入和转码沿用共享资源管线 |
| 音频转换 | `transcode_asset`、`batch_convert_media` | 显式指定 `platform_id`；批量入口接受 `source_ids` 或 `sourceIDs`，等待各项转换完成并返回实际成功、失败及输出结果 |
| Action 配置 | `create_action`、`set_item_property`、`get_action_definition` | 通过 `steps` 配置播放逻辑，引用使用对应命令要求的身份字段 |
| 路由与发送 | `set_field_group_field`、`set_output_bus_routing`、`set_send` | 直接编辑输出与发送关系；画布手势保留在 Studio |
| 会话导出 | `export_pack`、`list_project_platforms` | `output_path` 指定输出目录；默认使用工程导出平台选择，可传 `platform_id` 或 `platform_ids`；等待实际完成后返回各平台输出 |
| 试听与监控 | `post_action`、`audition_play`、`audition_get_state` 等已注册运行时命令 | CLI 启动时不自动初始化音频设备；在同一 `run` 会话显式调用共享 `init` 及所需输出启动命令，播放前等待当前工程完成运行时同步 |

字段组编辑用 `definition_guid` 指定对象，并明确 `scope: "Shared"` 或 `platform_ids`。有父定义时可设置 `Local`／`Inherit`；平台范围可用 `UseShared`。根定义直接编辑对应字段，无需先切换继承模式。`set_send` 和 `set_send_level` 的 `level` 是非负线性增益，`1` 为单位增益、`0.5` 约为 -6.02 dB；对象查询中的 `sendLevel` 使用 dB。

| 查询或操作 | 返回值与实际语义 |
|---|---|
| `get_bus_volume_db` | 读取工程中的创作增益（authored gain），包含当前会话编辑；不要求音频初始化，也不是运行时电平测量 |
| `batch_convert_media` | `succeeded`／`failed` 统计实际转换结果，`results` 保留逐项响应；成功项包含相对工程 `.cache/media` 的 `cache_path` 和实际文件大小；任一失败会使批次返回错误，超时范围覆盖整批转换 |
| `get_active_voices` | `count` 是原生活跃 Voice 总数；`reported_count` 是 `voices` 数组实际行数。运行时不公开部分由可播放对象持有的 Voice 明细，因此两者可能不同 |
| 主输出与总线电平 | `get_performance.master_peak_l/r` 使用线性幅度，`get_mixer_levels`／`get_metering_snapshot` 使用 dB；这些电平均在主输出增益（output trim）之前测量，不能用于回读 `set_master_volume` 作用后的最终输出 |
| `replay_verify` | 比较两个文件的原始字节，不解码音频；`total_samples`、`diff_samples` 沿用旧字段名，实际计数字节。文件不同由 `is_bitexact=false` 表示，读取失败返回错误 |
| `search_fulltext` | 查询当前工程索引；索引目录无法创建、后端不可用或重建失败时返回 `SEARCH_UNAVAILABLE`，不将失败伪装为空结果 |

GUI、会话导出和已保存工程构建使用同一套编译服务和包体格式，供 Harken Engine 及集成层消费。编辑对象句柄与运行时 short ID 是不同身份，应使用命令参数明确要求的字段。`exec init` 调用共享的运行时初始化命令，与用于创建工程的顶层 `init <directory>` 不同。

`load_pack`／`unload_pack` 支持 canonical 包集合；先加载 Init，再加载内容包，卸载 Init 会卸载整个集合。包集合与已打开工程的 Current World 使用互斥的运行时内容来源，应在未打开工程的会话使用此流程。CLI 等待短暂背压消退，`get_loaded_packs` 查询实际已加载集合；卸载包不会强制截断已经开始播放且仍持有内容的 Voice。

## 持续编辑会话

`shell` 使用普通终端，不提供定制终端界面。每行格式为命令名加可选 JSON 对象；没有参数时省略对象。所有业务命令与 `exec`、`run` 共用同一个分派器。

```text
HarkenStudioCLI shell --project GameAudio/GameAudio.hsp
harken> create_action {"name":"PlayFootstep"}
harken> rename_item {"path":"PlayFootstep","new_name":"PlayPlayerFootstep"}
harken> save_project
harken> exit
```

上述修改的批处理形式为 `HarkenStudioCLI run --project GameAudio/GameAudio.hsp --input edit.json --save`，其中 `edit.json` 内容为：

```json
[
  {"cmd":"create_action","args":{"name":"PlayFootstep"}},
  {"cmd":"rename_item","args":{"path":"PlayFootstep","new_name":"PlayPlayerFootstep"}}
]
```

| 会话操作 | 约定 |
|---|---|
| `help`、`commands [name]` | 查看会话用法、业务命令目录或参数 |
| 编辑、查询和试听 | 命令串行执行，期间持续处理 QtCore 事件；等待输入时音频和后台服务仍运行 |
| `save_project` | 显式保存；没有自动保存模式，`shell` 不接受 `--save`、`--args` 或 `--input` |
| `exit` | 工程已保存时退出；存在未保存修改时返回 `UNSAVED_CHANGES` 并继续会话 |
| `exit --discard` | 明确放弃未保存的工程修改并退出；此前导入或导出的文件不回滚 |
| 切换工程 | 未保存时拒绝 `open_project`、`create_project` 和 `close_project`，先保存或明确放弃会话 |
| 错误与 Ctrl+C | 单条命令失败后继续接收输入；Ctrl+C 请求取消当前可取消操作，返回提示符；空闲时保持会话，不代替 `exit` |
| 输入结束 EOF | 无未保存修改时正常退出；否则报告 `UNSAVED_CHANGES` 并以 1 退出，不隐式保存 |
| 管道与 `--json` | 可持续从标准输入逐行读取；每条响应为一行 UTF-8 JSON，并带 `command`；不等待 EOF 才执行 |
| 提示符与退出码 | 只有终端输入且未指定 `--json` 时向 stderr 输出提示符；已处理的命令错误不会使后续正常 `exit` 返回非零，调用方检查逐条响应 |

无参数启动仍显示帮助并退出。双击后控制台关闭不表示崩溃；需要持续会话时，从终端运行 `HarkenStudioCLI shell`。

## 两种打包输入

| 边界 | 会话内 `export_pack` | 顶层 `build` |
|---|---|---|
| 输入 | 当前会话内存工程，包含本次流程尚未保存的编辑 | 磁盘上完整已保存工程 |
| 加载 | 使用 `StudioSession` 与共享编辑服务 | 使用 `ProjectLoader` 的 `ReadOnly` 模式，不创建编辑会话 |
| 编辑与保存 | 可在 `run` 或 `shell` 中先编辑后导出；`--save` 或 `save_project` 决定是否保存创作文件 | 不接受编辑输入、`--save` 或 `--timeout`；需要事务恢复或 Schema 升级时失败 |
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
| 旧批处理 | `batch` 不注册到 CLI；批量操作使用 `run`，统一处理会话及异步完成 |
| 保存 | `exec`、`run` 仅在 `--save` 或显式 `save_project` 时保存编辑；`--save` 在整个请求成功后保存一次。`shell` 通过 `save_project` 保存，退出规则见上表 |
| 外部写入 | 创建、导入、导出等操作可产生自身文件结果；批处理不是文件系统事务，不会因后续失败回滚之前的保存或导出，省略 `--save` 不等于 dry run |
| 工程占用 | 已由其他进程打开的工程返回 `PROJECT_IN_USE`；CLI 不激活桌面窗口代为处理 |
| 关闭 | `close_project` 完成关闭并释放会话的工程锁后才执行下一条；之后编辑须重新打开工程 |
| 标准输出 | `--json` 输出 UTF-8 JSON，包含 `protocolVersion: 1` 和 `status: "ok"` 或 `"error"`；一次性入口输出一个文档，`shell` 逐条输出一行；转发结果时保留未知字段 |
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
| [StudioCli/Shell.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/Shell.cpp) | 持续会话、逐行输入、事件循环、退出及中断处理 |
| [StudioCli/StudioSession.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/StudioSession.cpp) | 无窗口会话、工程占用和异步等待 |
| [StudioCli/SavedProjectBuild.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/SavedProjectBuild.cpp) | 已保存工程的只读加载、构建目标查询及打包入口 |
| [StudioCli/ResourceWorker.cpp](../../../HarkenStudio/Source/Tooling/StudioCli/ResourceWorker.cpp) | 私有资源 worker 协议，沿用共享资源编译库 |
| [EditorCommandRegistration.cpp](../../../HarkenStudio/Source/Core/EditorCommandRegistration.cpp) | 按宿主注册命令；共享业务处理器供 Studio 与 CLI 使用 |
| [EditorCommandCatalog.cpp](../../../HarkenStudio/Source/Core/EditorCommandCatalog.cpp) | 定义宿主支持边界，汇总已注册命令描述与参数 Schema |
| [Core/CMakeLists.txt](../../../HarkenStudio/Source/Core/CMakeLists.txt) | `harken_studio_core` 共享静态库；`harken_editor_foundation` 在其上提供桌面辅助实现 |
| [HarkenStudio/CMakeLists.txt](../../../HarkenStudio/CMakeLists.txt) | 双产物目标、独立 CLI 构建及 Qt 图形依赖检查 |
| [verify_studio_cli.py](../../../.scripts/verify_studio_cli.py) | CLI 目录与宿主隔离、协议、保存重开、转码及导出包一致性的流程验证 |

新增创作能力须通过共享命令暴露，参数描述随命令定义或目录维护；只写在 Widget 内的行为不会自动变成 CLI 能力。GUI 适配层负责控件、桌面命令、Qt Action 和通知展示，共享服务负责工程编辑、撤销历史、日志与异步工作。终端界面设计参考 [Command Line Interface Guidelines](https://clig.dev/)。
