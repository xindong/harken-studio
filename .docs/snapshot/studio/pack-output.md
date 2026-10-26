# HarkenStudio 打包元数据导出

核对日期：2026-10-26。

Project Settings > Publish 的 `Export Directory` 与 `Waveform File Directory` 均以工程目录为相对路径基准；可转为相对路径时保存相对路径，跨盘符等无法相对化的情况保留绝对路径。留空时分别使用 `BuildPacks` 与 `BuildPacks/Waveforms`。

正常发布禁止输出到工程根、工程祖先目录、AudioSources、对象目录及其子目录，并对符号链接解析后的路径执行同样的保护，防止 Self-cleaning 删除工程数据。旧配置仍可加载以便修正，但导出入口、权限预检及最终清理均复查实际输出路径。媒体有效性按本次选中的 Action 依赖、目标平台及语言检查；未参与导出的 Sound 或未使用的基础源不会因缺失而阻止本次导出。

界面与 IPC 在启动打包前，先在平台输出锁保护下恢复未完成的发布，再检查本次已确定要替换或删除的路径，包括强制重建或无效的 Pack、已确定变化的伴随 JSON、根目录项目信息及确定待清理的输出。仅更新选中包时，Init 必须通过内容及发布身份校验才能作为保留项；仅有同名文件不代表可复用。直接导出到平台目录也遵循先恢复、后检查的顺序。

权限预检在后台线程执行，全部所选平台检查通过后才开始构建，并与构建共用强制重建、选择范围等选项。Studio 预检期间显示带等待进度条和 Stop 按钮的应用模态窗口，阻止其他 Studio 窗口的界面操作；检查结束后关闭窗口，恢复现有后台打包交互。Stop、Esc 或关闭窗口均只请求取消，后台确认退出后才解除模态限制。检查在目录扫描、Pack 校验和权限探测之间响应取消；中断发布的恢复必须完成后才能退出。进度条表示正在检查，不显示估算百分比。IPC 可继续查询或取消当前请求，预检失败通过异步结果返回 `PACK_BUILD_PREFLIGHT_FAILED`。

增量 Pack 是否复用取决于实际内容比较；确定需要重写后，在写入暂存包前检查目标文件。伴随 JSON 与按内容哈希命名的波形文件是否保留，以最终发布清单为准；只读但原样复用的文件不会阻止导出。正式发布前对实际替换、删除清单再次检查，既检查伴随文件和波形清理，也处理构建期间权限或占用状态变化。因复用决定依赖构建结果，这部分检查发生在结果确定后、修改已发布文件之前，并非全部发生在启动构建前。

必需产物发现只读、写保护、权限不足或文件占用阻止替换时，停止操作并列出受影响路径，提示先检查权限、关闭占用程序、签出或解锁。Windows 同时验证读取、删除访问权及共享删除限制，并检查目录创建权限；新建输出检查最近存在的父目录。检查不会解除只读或修改文件权限。可选波形的生成和发布失败只产生警告，不否决主包导出；对应包的伴随 JSON 省略完整的 `waveforms` 索引，避免引用缺失或不完整的数据。波形先暂存，发布失败或取消时回收本轮新建文件，保留此前已有的文件，外部波形目录同样适用。波形生成与清理共用目录锁；无法取得清理锁时保留该目录的波形并报告警告，其余发布继续。

GUI 与 IPC 的最终产物校验和目录清理均在后台执行；平台定义变化会在事务提交前停止收尾，平台编辑本身仍可使用。媒体处理、各语言编译、逐包写入、附属文件及清理准备之间检查取消；事务进入正式提交后必须完成或回滚。取消结果不记为包失败，也不计入已导出数量。无步骤的独立 Action 或全空包组按警告跳过，不生成包，不计入构建或复用数量，其余包正常发布；PostAction 的无效依赖仍属于结构错误。

Project Settings > Publish > Additional Material 的 `Export Project Information` 控制打包是否输出 `Project.json`，默认关闭。文件位于 `Export Directory` 根目录，与平台目录并列；它是 SDK 可选使用的项目元数据，不是引擎加载包的必要文件。

```text
BuildPacks/
├── Project.json
├── Windows/
│   ├── Init.bytes
│   ├── Init.json
│   ├── PlayMusic.bytes
│   └── PlayMusic.json
└── Android_High/
    ├── Init.bytes
    └── Init.json
```

| 字段 | 类型 | 来源与语义 |
|---|---|---|
| `schemaVersion` | 整数 | 当前 `HarkenSchema/Pack` 合同版本，与 Pack 伴随 JSON 共用版本来源 |
| `projectGuid` | 字符串 | Studio 项目稳定 GUID，规范小写形式 |
| `projectName` | 字符串 | Studio 项目名称 |
| `studioVersion` | 字符串 | 执行导出的 Studio 软件构建版本 |
| `studioSchemaVersion` | 整数 | 当前 `HarkenSchema/Editor` 项目合同版本 |
| `platforms` | 数组 | 项目配置的完整平台列表；每项仅含 `guid`、`name`、`basePlatform` |
| `languages` | 字符串数组 | 项目配置的完整语言列表，不含 SFX；未配置语言时为 `[]` |

`basePlatform` 使用 `Windows`、`macOS`、`Linux`、`Android`、`iOS`。平台不导出编码、重采样、质量等处理参数；语言保留项目配置的名称，不转换为语言 GUID 或 ISO 代码，也不输出默认语言。

项目身份直接铺在顶层，不使用 `project` 子对象。`Project.json` 随同现有 Pack 合同定义和验证，不建立独立的 Schema 版本线；`schemaVersion` 与 `.bytes` 文件头版本统一取自 `HarkenSchema/Pack/manifest.json` 的最新发布代。Studio 写入端与引擎读取端共同使用该来源，当前为 `3`，版本升代由正式发布流程决定。

Studio 软件版本、Editor 当前合同版本和 Pack 当前合同版本统一由 `cmake/BuildInfo.cmake` 读取各自权威文件，并通过已有 `harken_buildinfo` 构建目标提供。两个合同版本使用编译定义注入，Pack 格式头不依赖包含时间戳的构建信息头。当前合同版本的读取不需要 Python；Studio 的合同内容、历史支持列表和校验器仍由现有 Schema 工具生成。

Pack 跨代必须重新打包，不提供迁移或升级。统一版本前生成的二进制版本 `4` 要求重新导出。现用的 36 字节纯音频布局与 44 字节容器布局均保留，版本都取自同一合同；读取端先检查版本，再按文件头长度选择对应解析器，未知版本或布局直接拒绝。

本次只导出部分平台或 Action 时，`platforms` 与 `languages` 仍表达构建快照中的完整项目配置，不表示本次选择或输出目录中已有文件的清单。元数据只在整次请求成功后更新；失败或取消时保留上次成功发布的项目信息。

关闭开关后，下一次成功打包会删除输出根目录中的旧 `Project.json`。之前版本生成的 `ProjectData.json` 也会在下一次成功打包时移除。全量清理会保留当前启用导出的项目信息，更新和删除均使用现有发布事务及中断恢复机制。平台目录中的同名 Action 伴随文件不受根目录元数据开关影响。内部运行预览 bundle 沿用自己的输出协议。

## Pack 元数据

每个 `.bytes` 伴随 JSON 的最小形态包含顶层 `schemaVersion` 和名称到 GUID 的映射。Action 使用 `name`、`guid`；Variable 及 Label 映射使用 `variables`。即使关闭元数据与波形导出，顶层版本仍为必需字段。

Project Settings > Publish > Additional Material 的 `Export Pack MetaData` 默认关闭，独立于项目信息与波形导出。开启后，在本次构建的每个 `.bytes` 同目录、同名 JSON（如 `Init.json`、`Play_Footstep.json`）中追加 `metadata`，不在其中重复版本：

```json
{
  "schemaVersion": 3,
  "name": "Play_Footstep",
  "guid": "371e86db-0e67-4e50-bbd4-cf4e236d8d9a",
  "metadata": {
    "packStableId": "14996040238435073281",
    "platform": "Windows",
    "language": "",
    "objects": [
      {
        "guid": "d4084eef-94bc-45c3-9a70-ec956a7fe829",
        "name": "Footstep",
        "type": "Sound",
        "path": "\\Character\\Footstep"
      }
    ]
  }
}
```

`schemaVersion` 与包体使用同一 Pack 合同版本来源，不再输出 `packFormatVersion`。`packStableId` 是十进制字符串，防止 64 位整数在 JSON 使用方丢失精度。`platform` 使用项目配置的平台名称；`language` 使用实际构建语言，无语言时为空字符串。

`objects` 是平铺、按 GUID 去重并稳定排序的依赖对象清单，每项只含 `guid`、`name`、`type`、`path`。`type` 使用 Studio 对象类型，不对 Pack 添加类型字段；`path` 原样使用构建快照中的对象路径。清单依据本次包体中编译后的定义与引用收集，包括 Action、Sound、Container、Track、Piece、Layer、Variable、Attenuation。共享依赖可能由其他包承载，清单不表示每个对象的完整定义都驻留在当前包体中。Bus 仅从承载全局信息的 Init 包中导出，不加入各 Action 的清单。

清单只列 Studio 对象；总线编译器追加的内置设备输出根没有 Studio 对象身份，不列入清单。项目自身的主总线保留原有 GUID、名称和路径。

同组的 `Export Wave Buffer` 会将波形数据写入 `Waveform File Directory` 下按内容 SHA-256 命名的 `<hash>.vkd` 文件，并在包的同名 JSON 中写入 `waveforms` 索引。索引中的 `file` 优先使用相对于伴随 JSON 的路径，无法相对化时使用绝对路径。同时开启元数据输出时，Sound 条目的 `guid` 对应既有 `waveforms[].soundGuid`，进而关联 VKD 文件及裁剪范围。仅开启元数据不会生成波形；开关不改变 `.bytes` 内容或原有名称映射。

关闭后，本次重新构建的伴随文件移除 `metadata`。增量打包保留未选中包的原伴随文件；失败或取消保留之前成功发布的文件。元数据随包体通过既有发布事务提交，校验覆盖统一版本、包身份、语言、依赖清单去重与完整性。
