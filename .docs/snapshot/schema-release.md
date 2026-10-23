# Schema 发布与归档回流

发布身份由发布分支冻结的 commit、该 commit 内的正式 schema 与 VERSION，以及由它构建的制品确定。仓库不保存独立的 schema 发布回执，也不通过回流 PR 让 main 重新证明发布分支的身份。

## 发布分支

1. 从 main 创建发布分支，独立决定 Editor、Pack 复用当前代际还是升级。
2. 将 `current/` 物化为正式数字档案，更新对应 manifest；准备本次发布需要的 VERSION 与工程迁移逻辑。
3. 提交上述变更，完成该 commit 的开发验证。归档完成不代表验证通过或已经发布。
4. 发布流程使用该 commit 的正式版本构建、检查并打包发货制品。发布后冻结该 commit，不再向版本化石追加业务代码。main 的后续提交不改变它的身份。

准备合同的命令如下。`same` 与 `next` 分别表示复用最新代际和新增下一代；Editor、Pack 分别选择。候选目录必须尚不存在。

```sh
python .scripts/studio_schema_lock.py stage --candidate-root .temp/schema-candidate --editor next --pack next
python .scripts/studio_schema_lock.py archive --candidate-root .temp/schema-candidate
```

`archive` 只写两个树的 manifest 和本次数字档案，不修改 VERSION、迁移或业务源码，也不生成验证回执。候选内容过期、历史档案被篡改或合同无效时拒绝归档；写入后的合同校验失败会回滚。

## 发布构建与检查

正式发布要求物化后的 `current/` 与最新数字档案一致，并保持已发布历史合同的兼容性。静态检查命令为：

```sh
python .scripts/studio_schema_release_audit.py --verify-current --head HEAD
```

`studio-artifact.yml` 使用原有 Release 配置构建发货制品，不开启测试构建选项、不准备测试数据、不下载历史测试安装包，也不运行工程往返、迁移或原生 Pack 测试。Windows 与 macOS 各自构建、打包，不因开发测试增加相互依赖。

正式打包检查合同、版本与二进制身份、生成代码和安装快照的一致性。二进制必须来自 HEAD 的干净构建，且工作树没有已跟踪文件的待提交变更。这些检查直接使用正式输入和实际制品，不依赖测试构建产物或先前保存的测试结论。

历史合同检查只读取发布目录和标签内的源码档案，不下载、构建或执行历史读写器。只有 GitHub Release 目录明确关联且已上传 Studio 安装包的标签才算 Studio 发布；Hub 标签不算。版本、commit 与 schema 信息从相应标签读取。

## 开发验证

Studio 工程往返、历史工程迁移、Pack 解析和原生加载属于开发验证。移除回执后，现有读写器校验保留为独立命令，由开发侧提供已构建的程序、测试数据与历史安装包目录；它不属于发布工作流的构建或打包依赖。

```sh
python .scripts/studio_schema_release_audit.py --verify-readers --reader <Studio可执行文件> --baseline-readers-file <历史标签与安装目录映射.json> --pack-reader <HarkenAbiFidelity.dll路径> --pack-root <Pack测试数据目录>
```

历史读写器范围依据 schema 声明的支持范围选择，目录映射只指定本次使用的安装包位置。命令直接执行检查，不复用之前的通过结论。发布输入检查与开发测试各自负责自己的边界，正式打包成功不代表已经执行了开发测试。

命令可以输出诊断报告，Actions 也保留运行日志；后续流程不读取它们作为通过凭据，报告不属于回流 main 的内容。

## 回流 main

回流 PR 携带正式 schema 档案及 manifest、相应 VERSION 和工程迁移逻辑。业务源码准备与测试能力建设应在升级前完成，不能夹带进正式升级 PR。

main 可以继续修改 `current/` 和业务源码。PR 检查验证合同与生成器有效性；数字档案或 manifest 发生变更时，另外检查历史发布合同没有被删除或发生不兼容改写。它不要求 main 的 `current/` 等于回流的数字档案，也不要求 main 的源码等于发布 commit。

这项流程调整移除了回执依赖，保留现有兼容规则与开发验证能力。是否具备某个新 schema 代际的发布条件，仍须在开发阶段完成实际构建验证；归档工具不会替代缺失的业务或迁移实现。
