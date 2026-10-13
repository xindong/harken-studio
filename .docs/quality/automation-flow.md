# Compliance Automation Flow

合规自动化流程分为发现问题、记录快照和按需维护台账三部分。

## 1. 触发

`Compliance Audit` 在北京时间每天 09:00、Pull Request、`main` push 或人工操作时触发。

## 2. 读取规则

流程读取 `.docs/quality/compliance-rules/` 定义的规则，并由 `.scripts/compliance_audit.py` 执行。常规规则读取完整已跟踪树；PR 事件还读取 PR 描述及 base 版本的 PR 模板。

## 3. PR 治理检查

仅在 `pull_request` 事件中，脚本使用 event payload 中的 base SHA 读取 `.github/PULL_REQUEST_TEMPLATE.md`，并据此校验 PR 描述。非 PR 事件跳过该规则；输入不可读取时失败关闭。

## 4. 生成 Findings

所有合规规则都输出统一 finding：

- `fingerprint`
- `title`
- `category`
- `severity`
- `file`
- `line`
- `evidence`
- `recommendation`
- `source`
- `report_type`
- `commit_sha`

## 5. 发布报告

每次运行生成 JSON 和 Markdown 报告。GitHub 发布步骤会：

- 将摘要写入 GitHub Actions step summary。
- 继续上传 `.temp/**` 作为 workflow artifact。
- 手动触发时，在 Compliance Audit report-feed issue 下追加本次运行摘要评论。

## 6. 同步台账

仅手动触发时同步 GitHub Issues。流程按 `fingerprint` 新增或更新 issue，只写机器字段和自动状态标签，不覆盖人工处理信息。PR 和 `main` push 只生成 summary 与 artifact。

## 7. 自动复查

当 issue 带有 `qa/manual/ready-for-recheck` 标签时，下一次手动巡检会根据是否复现更新自动状态：

- 本次仍存在：`qa/status/recheck-failed`
- 本次未复现：`qa/status/recheck-passed`

普通问题如果本次未复现，会标记为 `qa/status/not-reproduced`，由 QA 或负责人确认后手动关闭。
