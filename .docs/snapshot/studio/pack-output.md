# HarkenStudio 打包输出快照

## 输出配置与产物

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

GUI、IPC 与 Harkenizer 共用项目构建服务，覆盖预检、所选平台发布、根目录 Project.json 和自清理。packc 保持中间资源编译职责，不直接读取 `.hsp`。

```text
Harkenizer list-targets <project.hsp> --json
Harkenizer generate-packs <project.hsp> --all --platform-id <UUID> [--platform-id <UUID> ...] --self-clean <on|off> [--output <directory>] [--force]
```

| 合同 | 当前规则 |
|---|---|
| 工程读取 | 只读完整已保存工程；无需启动 Studio；不保存、升级或恢复工程事务 |
| 工程占用 | Studio 已打开同一工程时拒绝执行，不激活 Studio 窗口 |
| 构建范围 | 显式指定全部包、平台 UUID 及自清理开关；沿用工程语言、元数据与波形设置 |
| 输出目录 | 默认使用工程配置；覆盖参数中的相对路径以工程目录为基准 |
| 目标发现 | JSON 提供 projectId、平台 id/name/basePlatform、语言、Action stableId/name |
| 返回值 | 成功 0；失败 1；用法错误 2；取消 130 |
| 取消边界 | Ctrl+C 请求现有取消流程；已提交平台保留，未完成整次发布不执行根目录最终收尾和自清理 |
| 部署 | 随 Studio 的运行库、packc 和 schema 交付；Windows 为 Harkenizer.exe，macOS 位于应用包 Contents/MacOS/Harkenizer |

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
| 输出目录受保护 | 禁止发布到工程根、祖先目录、AudioSources、对象目录及其子目录；符号链接解析后同样检查 |
| 旧波形目录不安全 | 可加载并警告；启用波形导出时跳过不安全产物，修改和保存仍校验 |
| 媒体检查范围 | 仅本次选中 Action 的依赖、目标平台和语言；无关缺失音源不阻止发布 |
| 启动前检查 | 先恢复未完成发布，再检查确定要替换或删除的输出；全部所选平台通过后才构建 |
| 权限检查界面 | 模态等待窗口，无估算百分比；Stop、Esc、关闭均请求取消，后台退出后解除限制 |
| 必需产物不可替换 | 只读、权限不足或占用阻止发布，列出路径；不自动改权限 |
| 原样复用产物 | 只读不阻止复用；是否重写以实际内容与最终发布清单为准 |
| 部分包发布 | Init 须通过内容及发布身份校验才能复用；容量增长可能重写未选中包的容量信息，保留原内容与媒体 |
| 媒体转换失败 | 尝试默认参数或原始文件回退并保留警告；源文件缺失、变化或回退失败仍报错 |
| 可选波形失败 | 警告，主包继续；省略不完整 waveforms 索引，回收本轮新文件，保留已有文件 |
| 波形清理无法取得锁 | 保留该目录波形并警告，其余发布继续 |
| 空步骤独立 Action／全空包组 | Skipped 并警告；不生成包，不计入构建或复用，其余包继续 |
| 无效 PostAction 依赖 | 结构错误，不能作为空包跳过 |
| 取消 | 提交前可取消；正式提交须完成或回滚；Canceled 不计为失败或已导出 |
| 失败或取消 | 保留此前成功发布的产物及项目信息 |
| 关闭项目信息导出 | 下次成功发布删除根目录 Project.json；旧 ProjectData.json 同时清理 |
| 关闭包元数据导出 | 本次重建的伴随 JSON 移除 metadata；未选中包的伴随文件保留 |

表注：部分发布可能改变选中包以外的产物，交付范围以本次所有实际变化的 Pack、清单及依赖为准。

## Publisher 状态归属

| 数据 | 保存范围 | 当前含义 |
|---|---|---|
| Name、语言分区 | 当前工程 | 由 Action 和语言依赖派生 |
| 包勾选、Export Platforms | 项目本地缓存 | 不进入工程撤销栈；包勾选不改变导出平台选择 |
| Built、Size、Modified、File、Version | 项目本地产物缓存 | 最近成功发布的快照；外部删除或替换不自动同步，Self Cleaning 已知删除同步清理 |
| Result、Cost | 当前工程会话 | GUI 与 IPC 共用；工程关闭后清空，不写缓存或工程文件 |
| Cost 统计口径 | 每包资源准备与写包 | 不包含共享编译和发布耗时，表头与单元格提示说明口径 |
| 全局阶段 | 当前构建任务 | 表格上方显示检查输出、媒体准备、共享编译、写包、验证发布或根目录收尾，以及实际构建平台；任务结束后隐藏 |
| 行进度 | 当前工程会话 | 按主 Toolbar 当前观察平台和语言显示；只有实际 Preparing／Writing 显示活动进度，成功发布到 100%，失败或取消冻结 |
| 等待状态 | 本轮参与行 | Queued、Prepared、Ready to publish 为普通状态文字；准备或写入暂存完成不代表已发布成功 |
| 新一轮发布 | 本轮参与行 | 清零进度及旧 Result／Cost；未参与行保留 |
| 清缓存 | 本地选择及产物 | 包默认全选，产物列留空；Build 配置、平台及语言仍为工程数据 |

## 兼容范围

| 项目 | 当前结果 |
|---|---|
| Studio 0.9.1 → 0.9.2 | Editor Schema 9、Pack Schema 4 不变；工程可直接打开保存，既有 Pack 可由随附运行库读取 |
| Pack 跨合同代 | 须重新打包，不提供迁移；旧二进制版本号不能证明与当前合同兼容 |
| 未知版本或布局 | 读取端拒绝 |
