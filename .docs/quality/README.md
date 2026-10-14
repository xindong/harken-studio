# Harken Compliance Automation

本目录定义 Harken Studio 的自动合规检查体系，覆盖合规规则、GitHub 报告和 GitHub Issues 问题台账。

## 子目录

- `DEPENDENCY_POLICY.md`：第三方依赖的许可证准入、引入登记与发布义务政策正文。
- `DATA_FLOW_INVENTORY.md`：全部对外网络数据流的事实清单，隐私影响评估的输入。
- `compliance-rules/`：合规检查规则和运行说明，覆盖 License、隐私、遥测、发布边界、第三方依赖和密钥风险。
- `data-contracts/`：自动化数据结构说明，例如 finding、报告 JSON、fingerprint 规则和字段含义。
- `github-integration/`：GitHub 原生发布说明，包括 Actions summary、Issues 台账和状态流转。
- `samples/`：示例文件，帮助开发者理解输出数据。

当前唯一报告类型是 `compliance`，用于合规、隐私、License、发布边界和供应链巡检。

Studio 的工程与 Pack sidecar 合同在 [`HarkenSchema/`](../../HarkenSchema/README.md) 分树维护。普通 PR 可修改各树的 `current/`，只检查合同与生成器有效性，不以 `VERSION` 或整树哈希冻结开发内容。

正式发布会独立决定各树复用当前代际还是换代。相同代际的兼容发布可以更新数字目录；发布门禁检查实际读写器的往返行为及候选输入。Studio 制品还会复验源码、生成代码、二进制与安装快照的一致性。Git tag 与已生成制品不得覆盖，安装包不携带 `current/`。发布命令与兼容边界见合同目录说明。

## 维护原则

- 仓库内文档定义“检查什么”和“如何解释结果”。
- `.scripts/` 中的脚本负责执行检查、生成 finding、发布 GitHub 报告和同步 GitHub Issues。
- GitHub Actions summary 和 workflow artifact 记录每次运行快照。
- 手动触发巡检时，GitHub Issues 记录问题生命周期。
- 自动流程不覆盖人工状态、修复人和人工备注；人工状态通过 issue label、assignee 和评论维护。
