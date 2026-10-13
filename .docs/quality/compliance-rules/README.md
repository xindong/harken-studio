# Harken Studio 合规巡检自动化

本目录记录合规巡检的运行方式。巡检主体面向完整已跟踪代码树；PR 事件还会检查 PR 描述是否保留目标分支模板的完整骨架，以及 PR 引入的 commit 是否带有效 DCO trailer。

规则标准见 [COMPLIANCE_AUDIT_RULES.md](COMPLIANCE_AUDIT_RULES.md)。新增或调整规则时，先更新规则目录，再同步 `.scripts/compliance_audit.py` 的实现。

## 触发方式

- GitHub Actions 手动触发：在 `Compliance Audit` workflow 页面点击 `Run workflow`。
- GitHub Actions PR / `main` push：任意已跟踪路径发生变更都触发 `Compliance Audit`，避免许可证、资源或构建文件变更绕过检查。
- GitHub Actions 定时触发：北京时间每天 09:00 执行完整合规巡检。
- 本地触发：

```powershell
python .scripts/compliance_audit.py `
  --json-out .temp/compliance/compliance-report.json `
  --markdown-out .temp/compliance/compliance-report.md
```

## PR 专属检查

在 `pull_request` 事件中，workflow checkout 完整 Git 历史，脚本从 event payload 读取 base SHA、head SHA 和 PR 描述。PR 描述以 base SHA 中的 `.github/PULL_REQUEST_TEMPLATE.md` 为准，因此同一 PR 修改模板不会改变自身判定标准。DCO 校验读取 `base.sha..head.sha` 的 commit trailer，不看模板勾选。

PR 专属规则：`GOV-001` 校验章节与条目条数是否完整，`GOV-002` 校验必答内容是否留空，`GOV-003` 仅提示条目文案疑似改写、不阻断，`GOV-004` 校验非 merge commit 是否带合法行首 `Signed-off-by: Name <email>`（冒号后一个空格，多出来的空格进 name；行长超过 512 跳过；trailer email 像邮箱即可，不比 author/committer 身份）。workflow 还监听 PR 描述编辑事件，确保描述规则在变更后重新检查；`synchronize` 会重跑 DCO。定时、手动触发及 `main` push 没有 PR 上下文，跳过这四条规则。

## GitHub 发布

巡检脚本先生成本地 JSON/Markdown 报告。GitHub Actions 随后以 `if: always()` 运行 `.scripts/publish_report_to_github.py`，因此即使高风险门禁把 job 标记为失败，报告发布步骤仍会执行：

- 将风险统计和全部 findings 写入 GitHub Actions Summary 页面。
- 上传完整报告 artifact。
- 同仓库分支 PR 会在 PR 对话区直接显示完整 Markdown 报告；问题按风险等级分组，每项默认只显示标题，点击后展开详情。后续提交原地更新同一条评论，不重复刷屏。fork PR 的 `GITHUB_TOKEN` 默认降为只读，因此不自动评论，仍保留 Summary 和 artifact。
- 手动触发时同步 GitHub Issues 问题台账，并在 `Compliance Audit` report-feed issue 下追加本次运行摘要；`main` push 只展示 Summary 和 artifact，避免重复写入台账。
- 当前流程不做 `@mention` 通知。

正式合规扫描先生成本地 JSON/Markdown 报告；报告发布完成后再统一判定扫描门禁结果。

## GitHub 权限

workflow 使用仓库内置 `GITHUB_TOKEN`，需要：

```yaml
permissions:
  contents: read
  issues: write
  pull-requests: write
```

这些写权限只用于同仓库分支 PR 的报告评论，以及维护者手动触发时的 Issue 台账同步。fork PR 默认使用只读 token，不执行写入。流程不需要外部 webhook、第三方应用凭据或飞书相关 secret。

## 配置异常时的行为

合规扫描脚本不依赖 GitHub Issue 发布配置；即使发布步骤失败，扫描产物仍会由 workflow 的 artifact 步骤上传，便于排查。

GitHub 发布步骤缺少 `GITHUB_TOKEN`、`GITHUB_REPOSITORY`、`issues: write` 或 `pull-requests: write` 权限时会失败，避免出现“Action 成功但没有通知/台账”的静默跳过。

## 边界

定时、手动触发和 `main` push 基于当前 checkout 的完整已跟踪树执行；PR 还读取 event payload 中的描述并按目标分支模板校验。扫描不包含未跟踪/已忽略文件、下载后的远程依赖内容或最终安装包；详细边界见规则目录的“覆盖边界”。

`Compliance Audit` 对任意分类的高风险或严重 finding 返回失败。GitHub Issues 台账仅在手动触发巡检时同步。
