# HarkenStudio 打包输出快照

## 输出配置与产物

工程 `.hsp` 的 `content.audio.renderSampleRate` 和 `content.audio.channelConfig` 保存音频编译目标；`content.deployPlatforms` 保存发布平台定义，`content.deployLanguages` 保存发布语言列表，允许没有语言。`content.metadata` 仅保存工程名称。

Studio 不再提供、保存或编译对象／总线的 `makeUpGain`；旧工程加载时丢弃该字段及其 presence 位，保存时清除。Pack 包体及预览传输移除该字段，旧 Pack 需要重新打包。引擎内部保留属性编号和成员定义，预览、运行时播放和增益估算均不再叠加该值。压缩器效果器的 `makeupDb` 不受影响。

v7/v8 工程升级时同步转换历史输出路由字段及效果条目结构，保留平台覆盖及各对象独立效果链的语义；旧格式未设置效果的对象仍保持空效果链。效果类型仍受当前工程合同限制，目前仅支持 Limiter。

| 配置／产物 | 默认值或位置 | 当前内容与边界 |
|---|---|---|
| Publish Directory | 工程目录下 BuildPacks | 以工程目录为相对路径基准，无法相对化时保留绝对路径 |
| Waveform File Directory | BuildPacks/Waveforms | 与发布目录采用相同路径保存方式 |
| Pack 包体 | 平台目录下的 Init.bytes、Action 包等 | Init 始终包含，未变化时可复用 |
| 包伴随 JSON | 每个 .bytes 同目录、同名 .json | 必含 schemaVersion 和名称映射；可附加 metadata、waveforms |
| Export Project Information | 默认关闭 | 开启后在发布根目录输出 Project.json，与平台目录并列 |
| Export Pack MetaData | 默认关闭 | 在本次构建包的伴随 JSON 中写入 metadata |
| Export Wave Buffer | 独立开关 | 在波形目录输出按内容 SHA-256 命名的 `<hash>.vkd`，并在伴随 JSON 写入 waveforms |
| 配置提交 | 编辑完成即更新项目，支持撤销 | 写入工程文件仍需 Save；保存与发布先提交有效编辑，无效输入标红并恢复旧值 |

表注：Project.json 是供集成侧按需使用的可选项目元数据；元数据及波形开关不改变 .bytes 包体或原有名称映射。

## 导出范围

| 入口／选项 | 当前范围与结果 |
|---|---|
| Export Platforms | 指定常规及右键导出的目标平台；不限制表格观察或勾选范围 |
| Export Checked | 按各目标平台及语言的包勾选导出；未选平台时按钮禁用 |
| Export All | 导出全部包，不改变已有勾选状态 |
| Force Re-export | 默认关闭；开启时强制重新生成本次范围内的包，仅作用于常规 Export／Export All |
| Self-cleaning | 默认开启；所选平台全部成功后清理整个目标目录，仅保留本次有效产物；关闭时保留退出清单的文件 |
| 右键 Export／Force Re-export | 以点击的 Action 包及对应清单为内容更新目标；忽略底部两开关，不清理，保留其他包内容与既有 Init，缺少 Init 时创建 |
| 分语言导出 | 各语言按独立选择出包；同语言 PostAction 依赖被取消勾选时报错 |

表注：Self-cleaning 包含取消、删除或改名的平台目录，以及任意后缀文件和 Unity .meta。右键单包导出不自动导出依赖包，也不检查混合新旧包的共享内容兼容性。

## 独立构建入口

GUI、IPC 与 CLI 共用打包静态库，内部保留 `harkenizer_core`、`harkenizer_build` 等目标名称，覆盖工程构建计划、媒体准备、资源编译调度、共享逻辑编译、包体装配、直接写入、根目录 Project.json 和自清理。独立 Harkenizer 和 packc 程序均不再交付，不新增内核 DLL。主打包链直接调用资源编译库生成内存条目，不启动私有 worker。文件资源接口的私有 worker 由 `HarkenStudioCLI --resource-worker` 承接，程序定位覆盖变量为 `HARKEN_STUDIO_CLI_PATH`。

Studio 的 Publisher 窗口及操作命令与 CLI 命令目录分别注册；共享打包服务不要求 CLI 带入桌面交互。`validate_pack_build_readiness` 按工程平台复用实际打包及 Current World 的总线发布校验，非中性 pan／pitch、Listener 相对路由等不支持配置会出现在预检错误中。

两端命令目录分别见 [Studio 命令接口总览](studio-commands.md) 和 [HarkenStudioCLI 命令总览](studioCLI-commands.md)。

| 构建边界 | 当前分工 |
|---|---|
| GUI 输入 | 借用当前工程对象并持有工程生命周期，包含未保存编辑；全程模态构建窗口冻结编辑，不复制工程树，也不隐式保存 |
| IPC／CLI 会话输入 | `export_pack` 使用当前内存工程，包含未保存编辑；保留选择范围、进度和结果接口；构建期间拒绝工程修改命令，继续允许查询和取消 |
| CLI 已保存工程输入 | `build` 以 `ReadOnly` 模式完整读取磁盘工程，不创建编辑会话或音频运行会话；与会话导出汇合到同一构建服务 |
| 资源编译 | 衍生 PCM 直接生成媒体条目，与总线等资源进入最终组包；绑定在最终归属确定后生成；不写出再拆读 `.media`／`.resident` 中间包 |
| 资源故障 | 可归属的局部错误使使用它的包及依赖者失败，独立包继续；平台公共结构错误结束该平台，后续平台继续 |
| 产物写入 | 包格式、媒体算法和条目顺序保持；直接写最终包，每个完成包独立记成功 |

构建窗口显示平台、包、处理阶段和进度。取消按钮、Esc 和关闭请求均请求取消，直到后台构建退出才关闭窗口；Publisher 表格只保留构建结果，不再绘制行进度。工程文件和媒体文件的监视通知延后到构建结束，随后再处理外部修改与重载。

```text
HarkenStudioCLI list-targets --project <project.hsp> --json
HarkenStudioCLI build --project <project.hsp> --all --platform-id <UUID> [--platform-id <UUID> ...] --self-clean <on|off> [--output <directory>] [--force] [--json]
```

| 合同 | 当前规则 |
|---|---|
| 工程读取 | `build`、`list-targets` 只读完整已保存工程；无需启动 Studio；不保存、升级或恢复工程事务，遇恢复或升级需求时失败 |
| 工程占用 | Studio 已打开同一工程时拒绝执行，不激活 Studio 窗口 |
| 构建范围 | 显式指定全部包、平台 UUID 及自清理开关；沿用工程语言、元数据与波形设置 |
| 参数边界 | 已保存工程入口不接受编辑输入、`--save` 或 `--timeout`；多次 `--platform-id` 必须合法、无重复且属于当前工程 |
| 输出目录 | 默认使用工程配置；覆盖参数中的相对路径以工程目录为基准 |
| 只读范围 | 不改写创作文件；打包仍写出包体、伴随文件、`.cache/pack`、资产源状态及临时锁，不保证整个工程目录无写入 |
| 目标发现 | JSON 提供 projectId、平台 id/name/basePlatform、语言、Action stableId/name |
| JSON 输出 | `--json` 使用 CLI 的 `protocolVersion: 1` 与 `status` 外层结果；目标信息或构建结果位于 `result` |
| 返回值 | 成功 0；失败 1；用法错误 2；取消 130 |
| 取消边界 | Ctrl+C 请求取消；已完成包保留，不执行根目录项目信息与自清理 |
| 部署 | 发布 Studio 与 CLI 两个入口及运行库、schema；Windows CLI 为 HarkenStudioCLI.exe，macOS 位于应用包 Contents/MacOS/HarkenStudioCLI；CLI 也可单独构建部署 |

会话导出继续通过 `exec export_pack` 或 `run` 中的 `export_pack` 请求调用。JSON 参数 `all`、`force`、`self_clean` 均为布尔值，默认 `false`，分别控制包含未勾选包、强制重打及成功后的自清理。`platform_id` 与新增 `platform_ids` 互斥；列表须非空、无重复且全部为当前工程的平台 UUID，均省略时沿用导出选择。非空 `action_name` 不能搭配 `all=true` 或 `self_clean=true`；单 Action 导出保持不清理其他包。完整入口与参数对照见 [HarkenStudioCLI 命令总览](studioCLI-commands.md)。

## 构建输入与复用

GUI、IPC、CLI 会话导出和 CLI 已保存工程构建使用同一个 `ProjectPackBuildService`。一次请求借用冻结的工程对象，共用构建上下文、源输入和衍生媒体内存缓存，按平台顺序执行。每次请求重新建立计划；输入身份仅在需要比较或保存成功记录时派生。即使复用了快照对象，也清除上一请求的源捕获及内存转换结果。

| 情况 | 实际工作 |
|---|---|
| 冷打 | 建计划，准备媒体时识别源，编译并直接写目标包；保存成功记录时利用本请求已知源身份派生包输入，不提前串行识别全部源 |
| 内容不变重打 | 建计划、计算包输入身份，查询相关源及已知产物的原生文件状态；全部相同即返回 Skipped，不读音频载荷、PCM、包体或伴随 JSON |
| 内容变化重打 | 进入同一构建链；未变化的包保持原位，需要更新的包按需复用 PCM／旧包资源，再编译和覆盖写入 |
| 强制重打 | 禁用本次范围内包的相等跳过，省去没有消费方的前置输入比较；仍共用同一计划、媒体、编译、写入和记录流程，源内容身份与有效 PCM 可以复用 |

完整链路如下：

1. 检查输出位置与平台选择，取得输出根锁和项目缓存锁；进入每个平台时准备目录并取得平台锁。锁只防止并发构建互相覆盖。
2. 建立该平台的 Action 依赖闭包及语言选择。目标已有包记录且未强制重打时，派生包输入身份并与 `builds.json` 比较，查询相关源及已知产物的原生状态；完整命中即跳过该平台的编译。无历史记录或强制重打时直接进入媒体准备。
3. 对需要构建的包准备媒体。源状态相同直接使用资产登记中的摘要和音频元数据；变化或状态不可靠时，复制稳定源的同时计算摘要。只有转换或波形生成实际需要样本时才读取样本；已有有效转换 PCM 直接载入。
4. 编译同平台、同语言的共享逻辑和资源，确定包依赖与资源归属。包体直接写入最终 `.bytes`，再完成必需的伴随 JSON 及已开启的附加输出。每个成功包独立记入结果，失败包及依赖它的包报错，独立包和后续平台继续。
5. 未取消时处理可选 `Project.json`。只有全部选中平台成功且启用 Self-cleaning 时枚举目标目录、清理退出本次清单的文件；不清理请求不为收尾扫描整个输出目录。不重新读取所有包，不在平台结束或请求结束再次验证源文件。
6. 对尚未派生输入的成功包，使用本请求已知源身份补齐记录；部分失败只派生成功路径，取消时不再派生新输入。实际进入过构建时先回收无用 PCM，并在尚未读取到 version 2 状态时清理旧缓存，然后将构建记录和资产源状态各保存一次，未变不写。全部命中时不扫描转换目录。

包输入身份包含编译器身份、实际平台配置、公共编译选项、本包 Action 依赖闭包及祖先对象语义、源内容身份和有效转换设置；对象摘要在请求内复用。Init 另包含总线／变量／衰减配置和本次包输入集合。仍需遍历工程图、建立计划并计算这些身份；不声称重打完全没有计算。notes、color、编辑器位置、未引用资产和只影响 `Project.json` 的信息不作为包内容输入。

### 打包相关持久文件

`.cache/pack` 只有一份构建状态表及可复用转换媒体。源身份归既有资产登记，波形归编辑器与打包共用的波形缓存；不再为打包另存源表、显示表、逐目标结果或转换索引。

```text
.cache/
├── asset_registry/assets.json
├── waveforms/<source-relative-path>.vkd
└── pack/
    ├── builds.json
    ├── media/<conversion-key>.pcm
    └── .build.lock                  仅持锁期间存在
```

| 文件／字段 | 生产者 → 消费者 | 内容与目的 |
|---|---|---|
| `asset_registry/assets.json` | AssetRegistry／BuildSourceInputs → 编辑器、媒体准备 | 既有资产登记：sourceID、relativePath、contentHash、lastModified、metadata、settings、ownerObjectGuid、isOrphaned、platformCaches。打包在同一条目补充 sourceState、waveformState，不复制一张源表；absolutePath 从工程目录推导，不保存 |
| `sourceState` | 源识别 → 下次源识别 | 文件身份、大小及写入／变更时间的原生状态，十六进制保存；相同即可复用既有 contentHash 和 metadata |
| `waveformState` | 波形生成 → 波形读取 | 共享 .vkd 的文件身份、大小和修改时间；源内容变化时清空，用于判断已生成波形是否变化 |
| `builds.json.version` | PackBuildState → 后续构建、Publisher | 缓存格式版本，当前 2 |
| `targets[绝对平台输出目录].platform` | 同上 | 当前目标所属平台 UUID；同一输出目录一份当前记录 |
| `targets[...].packs[相对.bytes路径].key` | 同上 | 构建单元稳定键，连接当前计划和 Publisher 行 |
| `input` | 当前计划 → 下次比较 | 本包输入摘要，用于决定该包是否需要构建；不保存整棵工程树 |
| `state`、`companionState` | 成功写入 → 下次比较 | 包体和同名 JSON 的文件身份、大小和修改时间，检测缺失、替换和一般修改；权限变化不使内容失效，不保存全文件摘要 |
| `resourceFingerprint` | 资源编译 → 后续资源复用 | 仅在已知时保存；用于判断旧包内资源能否复用，与该包 state 绑定 |
| `packs[...].waveforms` | 启用波形导出时 → 下次比较、自清理 | 相对伴随文件目录的波形路径数组；仅保存本包引用，未导出波形则没有该字段 |
| `builds.json.waveforms` | 波形成功写入 → 下次比较 | 波形绝对路径到原生状态的唯一表；多个包／平台共享同一文件记录，空表不保存 |
| `media/<conversion-key>.pcm` | SoundSourceDerivative → 媒体编译 | 40 字节头、32 字节 SHA-256、交错 float PCM。头含 magic、格式版本、采样率、声道数、循环起止和媒体修订；帧数从载荷推导 |
| `waveforms/<source-relative-path>.vkd` | WaveformCache → 编辑器、波形导出 | 共用波形数据；不再附带 .vkd.source.json 或第二份源摘要 |
| `.build.lock` | 构建服务 → 其他构建请求 | QLockFile 进程锁记录，串行维护同一工程的 builds.json 和转换缓存；不是历史或成功标记 |

转换 key 只包含源内容摘要、有效裁剪／循环、量化到实际采样帧的调度偏移、实际重采样目标及转换格式版本；不含源 ID、平台 UUID 或不影响实际转换的参数。等价转换跨平台复用；实际载入 PCM 时读取并校验其摘要一次。资产既有 `platformCaches` 服务编辑器／预览；本链不借它额外生成转换 WAV。

`builds.json` 不保存 units、counts、warnings、完整结果、平台配置副本、逐包媒体引用历史或源摘要副本。名称、语言、统计、警告由当前计划推导，大小／时间由实际文件查询。Publisher 从这张状态表恢复产物显示，后续只维护内存，不再写 `pack-artifacts.json`。包勾选与 Export Platforms 保存到既有用户 QSettings 的 `PackExportSelection/<projectId>`，不作为构建缓存。

导出波形成功写入后立即记录一次文件状态；请求内按绝对路径共用查询结果，保存包记录时复用该结果。构建状态有变化而需要落盘时，按仍有效的包引用回收波形状态，避免不同平台持有同一文件的互相过期副本。

成功平台的对象／Action／媒体统计统一取本次计划的唯一实际依赖身份；对象包含所需全局定义和结构祖先。冷打、复用与强制重打采用同一口径，复用输入身份计算已有的集合，不另存一份统计快照。

### 必要读取、临时文件与清理

| 行为 | 触发条件与边界 |
|---|---|
| 源载荷读取 | 原生状态变化／不可复用时识别源；实际转换或生成波形时读取稳定副本。已知不变源需要副本时只复制，不再算一遍源摘要 |
| 稳定源副本 | 本请求内按真实文件共享，使用系统临时目录，释放输入时删除；Windows 读取句柄禁止同时写入／替换，读取完成不重复查状态；其他系统读取前后核对状态，避免把不同内容的头和样本组合使用 |
| 旧包资源读取 | 只有实际构建需要复用旧包资源时发生；完整相等跳过不读旧包 |
| 包体写入 | 直接打开目标 .bytes 并截断，按头、目录、条目和媒体顺序写；没有整平台暂存、复制保留包、发布证明、恢复标记或目录事务 |
| 单文件替换 | JSON、波形和 PCM 缓存仍使用单文件临时写入后替换，避免留下半份共享记录／缓存；不复制或回滚平台目录 |
| 锁文件 | 输出根 .harken-pack-build.lock、平台 .harken-pack-platform.lock、共享波形 .harken-pack-waveforms.lock；持有期间存在，防止同一输出被同时修改 |
| 平台大小写改名 | 必要时通过 .harken-pack-rename-* 中转目录完成一次改名；不是打包暂存区 |
| 转换回收 | 每次实际构建结束后、状态落盘前，按当前工程 Sound／平台／语言的有效源和转换设置推导所需 key，删除无用 .pcm 及遗留 .pcm.?????? 临时文件；不读音频载荷，不维护持久引用清单；删除失败只记日志、不重试 |
| 旧缓存清理 | 仅未读到 version 2 的首次／旧状态构建，在保存 version 2 前移除 pack/targets、pack/sources.json、pack-build、pack-inputs、media/pack-derivatives、pack-artifacts.json 和 .vkd.source.json。已读到 version 2 就不再扫描；没有额外迁移标记，删除失败只记日志、不重试 |
| 旧选择记录处理 | 读取旧 pack-export-selection.json 时，有效内容成功迁移到 QSettings 后删除，无效或版本不支持则直接清除 |
| 部分打包的保留包发现 | 仅部分打包实际进入构建时枚举最终平台目录，发现仍在磁盘上的保留包，用于容量合并与必要更新；成功缓存不是完整产物清单。完整复用不增加扫描，不保存失败清单或恢复标记 |
| 输出清理 | 仅 Self-cleaning 请求枚举并清理目标文件；删除成功后同步删掉状态表中退出本输出根的目标／包记录，其他输出根记录保留；删除失败报告错误 |
| 目标记录回收 | 读状态表时按当前平台定义移除已删除平台；状态表确有变更而需要保存时顺便移除已不存在的目标目录。完整命中不为此扫描其他目标，仍存在的其他输出根记录保留 |

源身份保留严格的变更状态：Windows 本地 NTFS／ReFS 使用卷／文件 ID、大小、LastWriteTime、ChangeTime；macOS 本地 APFS 使用设备、inode、大小、mtime、ctime；Linux 使用满足精度要求的相应状态。不能可靠获得源状态时不复用旧源摘要。生成的包体、JSON 和波形只保存文件身份、大小、mtime，设为只读不触发重写；这是一般变更检测，不承担内容完整性审计，也不检测人为原位修改后恢复大小和 mtime 的情况。

成功包立即获得构建记录；平台中的另一个包失败不撤销它。构建记录和源状态各在请求末尾保存一次，保存失败只影响后续复用并产生警告。目标包体打开并截断成功后，写入／flush 失败或写入期间取消会关闭并删除本次未完成文件；删除失败报告残留，打开失败保留旧文件，伴随文件后续失败不删除已完整写好的包体。进程崩溃或被强杀仍可能留下不完整文件，不建立恢复机制。独立包继续，没有平台回滚，也不在末尾增加整轮源复检。

### 构建期共享与并行

主计划只收集依赖闭包和媒体选择，不逐 Action 预编译播放逻辑。每种语言中的定义、Container 执行语义和 Action 各转换一次，局部错误按 stableId 归属到实际使用它的包；继承结构错误影响子定义使用者，Container 执行错误只影响执行该 Container 的闭包。选中 Track clip／Container row 的直属子项约束在依赖解析处校验；binding 在实际闭包与结构祖先上转换，宿主跨语言去重，错误按定义使用者分配。剔除失败范围后仅做一次共享编译，不按包重复编译或失败后重试。采样率、总线／身份、全局变量、总容量等平台公共结构错误仍使该平台失败，Bus binding 属于 Init 共享失败范围。

正式 Pack 链的 ExportAllPacks、Init 和规范运行时上下文不再执行全工程 playback 门禁；未选中的非法 binding／playback 不通过这些门禁阻断构建。全工程 playback 校验仍属于完整编译／预览入口。树结构与稳定身份、Bus 投影、共享字段组快照、共享容量及无法归属的共享编译错误仍保留平台公共失败边界。

媒体准备按源 ID 分组，最多八个工作线程且不超过逻辑处理器数量；同源请求保持原顺序，不同源可以并行。按转换预计输出设置 64 MiB 同时转换预算，单个超预算任务独占；预算不代表进程总内存上限。取消先停止并等待工作线程，再释放结果。

准备好的 PCM 直接交给资源编译，相同内容共享不可变样本和媒体修订；多声道只转换一次 planar 布局。绑定在最终归属确定后序列化一次，包顺序写入，不构造完整包内存映像。平台调度和包结果仍按确定顺序汇总。

### Wwise 参考边界

桌面 `wwise_lib` 与新增 `wwise_SoundBanksManagement` 的导出来源均为 2026.1.1.9196，后者补齐了实际 Bank 写入模块。可直接确认的缓存职责包括 `SoundBankInfoCache.dat`（配置／包／信息／媒体等记录）、按转换身份命名的转换文件，以及 LMDB 中的 AudioSourceData、WaveAnalysis 等表。2026 的 .dat 与 LMDB 同时存在；物理文件少不等于只有一种缓存职责。

以下路径相对 `wwise_SoundBanksManagement/SoundBanksManagement`，另注明的 `lib/` 路径属于 `wwise_lib`：

| 行为 | 直接证据 |
|---|---|
| 普通 Bank 直接写目标 | `SoundBankFileManager/GetOutputFilePath.c:68` 的临时路径开关等于 Source Control 开关；`SoundBankGenerator/WriteSoundBankToDisk.c:127` 据此选路径；`SoundBankFileWriter/Open.c:13` 以 WriteOvrwr 打开。Init 也采用相同路径选择 |
| Source Control 的逐文件临时输出 | 开启时使用同目录文件名追加 `~` 的路径；`global/anonymous_namespace_dynamic_initializer_for_k_tempNameAddOn.c:11` 定义后缀，`SoundBankFileManager/CopyFilesToOutput_L_class_std_map_L_struct_Sou_e81ea12760b7.c:59` 仅处理需写、无文件错误的项并逐文件 Move。此路径不构成整平台目录事务 |
| 元数据先写临时文件并比较 | `global/InfoFile_InfoFileGenerator_InfoFile_WriteMainB_15b3a8bd04fa.c:141` 写临时路径，`:367` 比较新旧内容，`:454` 调用 `HandleTempFile`；`SoundBankFileManagerPaths/HandleTempFile.c:16` 相同则删除临时文件，不同且无需 Source Control 处理时删除旧目标并 Move。不能概括成 Wwise 所有文件都直接覆盖 |
| 单 Bank 失败继续其他 Bank | `SoundBankEngine/GenerateSoundBanksInternal.c:988` 写失败退出当前 Bank 的语言循环，`:1026` 仅 ShouldStop 终止整轮，`:1043` 继续下一 Bank。该 Bank 剩余语言会跳过；平台前置失败仍可阻断该平台 |
| 失败文件清理 | `SoundBankGenerator/WriteSoundBankToDisk.c:247` 对错误或不允许的超限尝试删除本次选择的路径，没有恢复旧文件。`:154` 的打开失败也能进入该分支；Harken 只删除已成功打开并截断后的未完成文件，不照搬此边界 |
| 收尾保存缓存 | `SoundBankEngine/GenerateSoundBanksInternal.c:290` 统一调用缓存 Save；`lib/SoundBankInfoCache/Save.c:98` 直接覆盖写 .dat，并未先判断整个缓存是否变化；`:147` 起仅对更新的 Bank 补查写入时间。收尾没有重新读取所有 Bank 载荷的证据 |

`lib/SoundBankInfoCache/IsBankFileUpToDate.c` 先比较设置与文件时间，时间变化时只读 48 字节 Bank 头再核对逻辑身份。可借鉴的是职责清楚、按需读取、逐包处理；不能推导为完全不计算、不写缓存或不使用临时文件。Harken 保留未变缓存不写、独立语言继续的现有行为。真实 test-wwise 的 .wproj 是 2024.1.9，其当前仅见 CacheVersion 和 LMDB 文件的缓存不能作为完整 2026 文件清单。

## 项目信息字段

| 字段 | 类型 | 含义 |
|---|---|---|
| schemaVersion | 整数 | Pack 合同版本，当前为 4，与包体及伴随 JSON 一致 |
| projectGuid | 字符串 | 项目稳定 GUID，规范小写形式 |
| projectName | 字符串 | 项目名称 |
| studioVersion | 字符串 | 执行导出的 Studio 软件版本 |
| studioSchemaVersion | 整数 | Editor 合同版本，当前为 9 |
| platforms | 数组 | 完整平台配置，每项仅含 guid、name、basePlatform |
| languages | 字符串数组 | 完整语言名称，不含 SFX；未配置时为空数组 |

表注：basePlatform 可为 Windows、macOS、Linux、Android、iOS，不含编码等处理参数。部分导出仍记录完整项目配置，不能据此推断本次输出范围；语言不转换为 GUID 或 ISO 代码，也不另列默认语言。

## 包伴随 JSON 字段

| 字段 | 存在条件 | 内容 |
|---|---|---|
| schemaVersion | 必需 | 与包体相同的 Pack 合同版本 |
| name、guid | Action 名称映射 | Action 名称与稳定 GUID |
| variables | 有 Variable／Label 映射 | 变量及标签名称映射 |
| metadata.packStableId | 开启 Export Pack MetaData | 十进制字符串，保留 64 位整数精度 |
| metadata.platform | 同上 | 项目配置的平台名称 |
| metadata.language | 同上 | 实际构建语言；无语言时为空字符串 |
| metadata.objects | 同上 | 平铺、按 GUID 去重并稳定排序的依赖对象清单 |
| metadata.objects[].guid / name / type / path | 对象清单项 | Studio 对象身份、名称、类型和工程对象路径 |
| waveforms | 开启波形导出且完整成功 | 波形文件、Sound GUID 及裁剪范围索引；路径优先相对伴随 JSON，无法相对化时使用绝对路径 |

| 对象清单范围 | 当前规则 |
|---|---|
| 常规对象 | Action、Sound、Container、Track、Segment、Layer、Variable、Attenuation |
| Bus | 仅 Init 包导出；内置设备输出根不列入，项目主总线保留原身份 |
| 共享依赖 | 可能由其他包承载；列入清单不表示完整定义位于当前包 |
| 波形关联 | Sound 的 GUID 对应 waveforms 中的 soundGuid；仅导出元数据不生成波形 |

## 发布与失败边界

| 情况 | 当前结果 |
|---|---|
| 输出目录受保护 | 禁止写到工程根、祖先目录、AudioSources、对象目录及其子目录；符号链接解析后同样检查 |
| 媒体检查范围 | 仅本次选中 Action 的依赖、目标平台和语言；无关缺失音源不阻止构建 |
| 必需产物不可写 | 实际写入时报告只读、权限或占用错误；不预先反复试写，不自动改权限 |
| 原样复用产物 | 只读不阻止复用；相等文件保持原位，不为复用再复制 |
| 部分包更新 | 保留未选中包及已有 Init；共享容量需要增长时可能更新其他包容量信息，具体以实际输出为准 |
| 媒体转换失败 | 该包失败；不换默认参数或原始媒体冒充成功 |
| 已开启的波形导出失败 | 该包失败；不省略用户要求的波形后报告成功 |
| 波形清理无法取得锁 | 保留该目录波形并警告，其余清理继续 |
| 空步骤独立 Action／全空包组 | Skipped 并警告；不生成包，不计入构建或复用，其余包继续 |
| 无效 PostAction 依赖 | 结构错误，不能作为空包跳过；依赖失败的包不成功 |
| 选中 Track／Container／binding 错误 | 局部错误归属实际 owner 及使用它的包；定义继承与跨语言共享依赖按真正使用者传播 |
| 平台公共结构错误 | 采样率、总线／身份、全局变量、总容量等错误结束当前平台；后续平台继续 |
| 共享字段组解析失败 | Output／Send／Spatial 引用等错误仍会使共享字段组快照失败并结束该平台；此边界尚不按包隔离 |
| 独立失败 | 独立包及后续平台继续；已完成包保留，不回滚整个目录 |
| 取消 | 停止后续工作，保留此前完成包；未完成文件不取得成功记录；不执行根目录项目信息／自清理 |
| 未取消但部分失败 | 仍处理项目元数据并保存成功包状态；不执行 Self-cleaning |
| 关闭项目信息导出 | 未取消的请求删除根目录 Project.json；旧 ProjectData.json 同时清理 |
| 关闭包元数据导出 | 本次重建的伴随 JSON 移除 metadata；未选中包的伴随文件保留 |

## Publisher 状态归属

| 数据 | 保存范围 | 当前含义 |
|---|---|---|
| Name、语言分区 | 当前工程 | 由 Action 和语言依赖派生 |
| 包勾选、Export Platforms | 既有用户 QSettings，按 projectId 分组 | 不进入工程撤销栈；包勾选不改变导出平台选择 |
| Built、Size、Modified、File、Version | builds.json 的启动投影及会话内存 | 从已知成功包和实际文件恢复，不另写显示缓存；构建成功与 Self-cleaning 已知删除同步更新 |
| Result、Cost | 当前工程会话 | GUI 与 IPC 共用；工程关闭后清空，不写缓存或工程文件 |
| Cost 统计口径 | 每包资源准备与写包 | 不包含共享编译和请求末尾处理耗时，表头与单元格提示说明口径 |
| 全局阶段 | 当前构建任务 | 显示媒体准备、共享编译、写包及输出整理；不再显示暂存验证或事务发布阶段 |
| 行进度 | 当前工程会话 | 只有实际 Preparing／Writing 显示活动进度；成功到 100%，失败或取消冻结 |
| 等待状态 | 本轮参与行 | Queued、Prepared 为普通状态文字；准备完成不代表包已生成 |
| 新一轮构建 | 本轮参与行 | 清零进度及旧 Result／Cost；未参与行保留 |
| 清缓存 | 本地产物 | 构建记录清空后产物列留空；包选择属于用户偏好，工程配置仍由工程保存 |

## Pack 二进制合同（2026-11-05）

`HarkenSchema/Pack/current/PackManifestSchema.json` 的 `x-harken-format.binaryLayout` 描述当前有效生产写入路径的 `.bytes` 布局，是 Engine 和 Studio 共用的布局来源。合同包含字段顺序、类型、固定值、保留字段、行数与章节顺序、偏移、对齐以及目录 kind／vintage；C++ 类型和成员映射单独保存在 `.scripts/pack-wire-bindings.json`。

| 覆盖范围 | 当前内容 |
|---|---|
| 文件框架 | 文件前缀、包头、媒体文件头、目录条目 |
| 资源条目 | Media(1)、Bus(2)、BusSend(3)、Attenuation(4)、BindingDefinition(7)、BindingAttachment(8)、BusEffect(12)、EffectDynamicRoute(13) |
| 低层资源写入 | Container(9)、VoiceRecipe(11)；虽不由常规最终组包直接生成，仍有有效生产写入路径 |
| 组合与依赖 | CanonicalDefinitionPartition(21)、RequirementBlock(22)、DependencyEdge(23)、BusDefinitionSnapshot(24) |
| 变量 | VariablePolicyOverlay(26)、VariableRequirements(27) |
| 分区 body | shipping V18，包括可播放属性、时间表、容器、转换与 Action 章节 |

有效目录 kind 共 16 个。DiscreteVariable(5)、MeterWiring(6)、Action(10)、InitRoutingCatalog(20)、GameplayCatalog(25)、VariableNameCatalog(28) 共 6 个列入 `retiredKinds`，读取端不再接纳；其中 Action 的当前生产数据位于分区 body。独立的 `Vn_SetGameplayCatalog` 接口与伴随 JSON 的变量名称映射继续保留，不作为 Pack 目录条目。

`.scripts/pack_wire_codegen.py` 从合同和绑定表生成四份入仓头文件：Engine 的 `Runtime/media/include/harken/media/generated/` 与 Studio 的 `Tooling/PackBuild/include/harken_studio_build/generated/` 下各有 `harken_pack_wire_constants.g.h` 和 `harken_pack_wire.g.h`，目录均相对于各端 `Source`。前置常量头提供 `binaryLayout.constants`，供域结构的定长数组等声明使用；布局头生成定长记录访问及变长章节顺序。域结构、分配和字段语义校验仍由调用方负责。生成的定长读写直接操作调用方内存，变长读取通过 visitor 使用调用方存储，不新增中间容器。

修改布局后运行 `python .scripts/pack_wire_codegen.py`，或显式构建 `harken_pack_wire_regen`。普通构建直接使用入仓产物，不运行 Pack 布局生成器，也不新增 Engine 对 Studio 的依赖。`pack_wire_generated_check` 运行 `--check`，核对生成结果并扫描布局源码中的手写字节访问；Core 和 Studio PR 门禁覆盖合同、生成器及绑定表的改动。发布审计范围保留既有 Pack 范围并加入生成器、绑定表和四份产物，迁移期间增加的手写布局文件列表已移除。

Pack 分区仅接受 shipping V18；V19 的 Trace 扩展和 V20 的预览／Trace 扩展不进入 `.bytes` 合同，保留各自入口并复用生成的公共布局。`.stream`、独立 Gameplay catalog 数据及 World artifact 外层封装不在本次合同内；Effect 参数定义仍来自既有 registry，指纹和校验和算法保持原有职责。

`current` 已补齐实际布局，与已发布的 Pack 4 档案不同。此次只更新当前合同和实现，不修改 `HarkenSchema/Pack/3`、`HarkenSchema/Pack/4` 档案或发布版本，不执行 Pack 5 发布。发布审计仍要求不同合同换代，后续由发布者完成；当前版本号保持 4 不表示本次合同变化已经完成发布。

## 兼容范围

| 项目 | 当前结果 |
|---|---|
| Studio 0.9.1 → 0.9.4 既有发布 | Editor Schema 9、Pack Schema 4 不变；工程可直接打开保存，既有 Pack 可由对应发布的随附运行库读取；本次合同治理的发布边界见上节 |
| Pack 跨合同代 | 须重新打包，不提供迁移；旧二进制版本号不能证明与当前合同兼容 |
| Pack 3 | 文件头即拒绝；历史档案保留 |
| 非当前条目布局 | 有效 kind 的非当前 vintage 返回 `VN_PACK_INCOMPATIBLE`；退役或未知 kind 返回 `VN_BAD_PACK` |
| 非 shipping 分区 body | Pack 路径拒绝；独立预览／Trace 入口不受此限制 |
| 未知版本或布局 | 读取端拒绝 |

## 二进制合同验证快照（2026-11-05）

迁移基线为 `30530109e57603225e68b26dc800c80ee3069954`，最终 C++ 实现为 `1164686ff93cb8694590a4f82bdbb707d9bfdb7c`；Integration 变量 fixture 的伴随名称迁移为 `7edf10f6`。验证环境为 Windows x64、clang-cl 19.1.5、Ninja Multi-Config、RelWithDebInfo、local／dev 通道。

| 验证范围 | 实际结果 |
|---|---|
| 构建 | `harken_editor_tests`、`harkenizer`、`harken_host`、`harken_runtime_tests`、`harken_core` 及资格测试目标编译通过 |
| Runtime | 执行 6457 项，6451 通过、6 跳过、0 失败；跳过集合与基线一致 |
| Editor PR／daily 范围 | 执行 622 项，620 通过、1 跳过、1 失败；另有 1 项禁用。失败为 `EditorDockLayoutTest.ColorRefreshPreservesBlueprintEditorsAndStructureRefreshReplacesThem`，基线同样失败，基线单独重跑通过 |
| 导出 | 22 个产物文件与基线逐字节一致，167 项打包回归通过 |
| Integration | 完整原生合同通过，包含变量、Pack 生命周期、伴随名称及调用线程零 GC 检查；托管测试项目构建为 0 警告、0 错误 |
| 源码合同 | 首轮 25／25 通过；最终轮 24 项通过，`schema_archive_unit` 遇到 Windows `publish_lock` flush 的 `PermissionError`，单独重跑通过。生成器 19 项、schema 22 项及生成物 `--check` 通过 |
| 门禁反例 | 只改合同、只改生成物、加入手写记录／body 字节访问均被拒绝；发布审计检测到当前合同差异并要求换代 |
| Core 独立构建 | 关闭 Studio、tools 与 shared 目标并禁用 Python 查找后，配置和构建通过；构建图无 Python／Studio 命令。iOS 仅核对依赖条件，未在 Windows 实编 |
| 资格检查 | 64 个常驻项完成，组合索引 2080 字节、常驻占用 14544 字节，释放后 retiring 数为 0。256／1024 场景因既有 fixture 容量 64 失败，旧基线已复现，未作为通过项 |

Editor 范围排除了 `ExportedPackLifecycle.WasapiImmediateSoundHarness`、`ExportedPackLifecycle.RelWithDebInfoQualificationReport` 和 `Historical/ProjectDocumentVersionRoundTripTest.*`。导出样本实际覆盖 kind 1、2、21、22、23、24、26、27；kind 3、4、7、8、9、11、12、13 由临时新旧编解码器探针补充比较。各批还验证了 body／snapshot 的完整字节、截断与尾随数据拒绝、保留字段、分配和异常边界；这些探针保留为本地迁移证据，不新增逐字段回归测试。

旧 kind 20 已无当前数据来源，资格报告移除了 `--catalog` 及旧 catalog 专用指标，保留生命周期、回滚和资源预算检查。变量 fixture 改用现有 V2 激活接口附带名称，替代退役 kind 28；名称查找沿用伴随数据既有的 `VN_UNKNOWN_ID` 结果，不修改运行时语义。

本地证据在 `.temp/pack-wire/`：`final-validation-summary.json` 汇总最终结论，引用 `final-head-build.log`、`final-head-runtime.json`、`final-head-editor.json`、`final-head-export/report.json`、`final-native-complete.log`、`final-head-schema-recheck.log`、`core-only-build.log` 和 `final-qualification-64-absolute.json`。`baseline/coverage.json` 与各批 `*-equivalence`、`body-stage/`、`snapshot-stage/` 保存覆盖清单和补充探针。原 `final-validation.json` 保留 fixture 修复前的变量失败及总体 false；原 `final-qualification-64.json` 保留相对 fixture 路径失败，绝对路径重跑后完成，不覆盖原始记录。

复核修正（2026-11-06）：恢复 body 编码端在迁移中丢失的 Action 步数累计上限，以及 Engine 对 V19 专属量化时机的拒绝；历史 tag 的 `current` 不含 `binaryLayout`，审计加载它们时不再报错，`binaryLayout` 只对代际大于 4 的 Pack 合同强制，`current` 由 `pack_wire_codegen.py --check` 保证；移除运行时已不可达的 V1 总线快照分支及其类型。重建后 Runtime 执行 6457 项，6451 通过、6 跳过、0 失败；167 项打包回归通过，22 个产物文件与基线逐字节一致。Integration 原生合同未在本次复核中重跑。

## 验证快照（2026-11-03）

基线为 `a3c55d350612f29fbe1db357ccbee90c1618cb9c` 的保留二进制；本次 Harkenizer 的 SHA-256 为 `5dfb098d76abc23a7a84248055e624920326529aa7a7f9815c4dd954bc3fef6e`。167 项回归、14 项补充用例通过，22 个黄金样本产物与基线逐字节一致。真实工程比较共 36 次 CLI 调用，均成功且产物与对应输入的基线逐字节一致；原工程输入未改变。单平台产生 11 包／22 文件，四平台产生 44 包／88 文件。

下表为各三轮中位数，单位为秒，统计完整 CLI 启动、工程加载、构建／复用及收尾。每次都是新进程；冷打使用新复制的工程缓存与输出，未重置 OS 或全局应用缓存。四个平台均在 Windows 主机生成；Self-cleaning 与可选项目元数据、包元数据、波形导出关闭。增量场景修改一个 WAV 的 64 帧并保留大小和 mtime，验证源变更状态能使相关包失效。日志阶段边界不作为 build-only 计时。

| 场景 | 基线 | 本次 | 耗时变化 |
|---|---:|---:|---:|
| 单平台冷打 | 8.850 | 8.679 | −1.9% |
| 单平台原内容复用 | 2.919 | 2.783 | −4.7% |
| 单平台强制重打 | 4.301 | 3.319 | −22.8% |
| 单平台增量 | 6.581 | 3.543 | −46.2% |
| 四平台冷打 | 14.845 | 16.064 | +8.2% |
| 四平台原内容复用 | 4.902 | 3.934 | −19.7% |

单平台冷打与复用差异较小，且前一轮构建的比较方向相反，未证明稳定提速；四平台冷打本轮仍回退。不能将所有耗时变化归因于移除资产表复制，也不能用 CPU／I/O 下降替代耗时结论。单平台冷打 CPU 为 5.688 → 4.500 秒，读取 366.33 → 97.18 MiB，写入 309.28 → 195.45 MiB；这些指标同样包含完整 CLI。

以下为最终第 3 轮副本的实际 `.cache` 盘点，字节数包含完整资产登记，路径长度会影响 JSON 大小。四份缓存均有 367 个 PCM，共 54,316,896 字节；本次没有波形输出，因此未生成波形缓存／状态。

| 范围／版本 | 总文件数 | 包状态字节 | 源状态所在文件／字节 | 缓存总字节 |
|---|---:|---:|---|---:|
| 单平台基线 | 369 | 37,847 | sources.json／120,481 | 54,475,224 |
| 单平台本次 | 369 | 5,060 | assets.json／626,556 | 54,948,512 |
| 四平台基线 | 372 | 151,447 | sources.json／125,619 | 54,593,962 |
| 四平台本次 | 369 | 20,215 | assets.json／626,556 | 54,963,667 |

包状态已收敛，但本 fixture 的冷 CLI 原本没有编辑器资产登记，复用该登记需要创建完整 assets.json。因此单平台元数据总量为 158,328 → 631,616 字节，四平台为 277,066 → 646,771 字节；不能宣称总缓存占用下降。当前候选只有 `asset_registry/assets.json`、`pack/builds.json` 和 `pack/media/*.pcm`，无未知文件、旧缓存或临时锁残留。builds.json 为 version 2，单／四平台分别有 1／4 个 target，每个 11 条包记录；每条仅含 key、input、state、companionState、resourceFingerprint。

三轮比较结束后，另在两个第 3 轮候选副本各启动一次完整 CLI 复用审计。单平台的 369 个缓存文件与 22 个输出、四平台的 369 个缓存文件与 88 个输出，前后路径、大小、SHA-256、mtime、文件 ID 全部一致；分别复用 11／44 包。这两次审计不计入上表。结论是持久文件未改写；不声称进程无写 I/O，构建期间仍有瞬时锁文件。

本地证据位于 `.temp/pack-v3-20261102/`：`validation-verified/report.json`、`additional-verified.json`、`comparison-verified/comparison-summary.json`、`comparison-multiplatform-verified/comparison-summary.json` 与 `cache-audit-verified.json`。可选波形／元数据的正确性由回归测试覆盖，以上真实工程性能与持久写审计不覆盖这些开启选项。

### 主干同步验证（2026-11-03）

分支同步至主干 `8722319a`，保留 Studio 0.9.4、Hub 0.2.3、pitch binding 范围与旧曲线值修复，以及运行时包读取和媒体所有权优化。主干与分支没有同路径修改，语义复核未发现业务冲突。上述性能表仍为同步前的测量，不作为同步后耗时结论。

同步后在 Windows x64、Qt 6.11.1、clang-cl、Ninja Multi-Config、RelWithDebInfo、local／dev 通道重新构建 Studio、Harkenizer 和编辑器测试目标。167 项打包回归及 15 项补充测试全部通过，后者包含主干新增的旧 pitch 曲线保留用例；22 个黄金产物与同步前逐字节一致。本地证据位于 `.temp/pack-v3-main-sync-20261103/validation/report.json` 和 `additional.json`。

同时重新构建运行时与包存储测试目标：包格式／循环媒体加载替换 45 项、CanonicalPackSetStorage 37 项全部通过，分别记录于同目录 `runtime.json` 和 `pack-storage.json`。本地样式归属检查及包含 PR 正文、DCO 的合规审计通过。以上验证基于同步后代码 `5e476140`；随后只补充本段验证记录和兼容版本说明。
