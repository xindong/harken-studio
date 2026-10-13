# GitHub Integration

合规自动化只使用 GitHub 原生能力发布报告和维护问题生命周期。

## 产物

- GitHub Actions step summary：显示本次运行摘要、风险统计和最高优先级 finding。
- Workflow artifact：保存完整 JSON/Markdown 报告。
- Report-feed issue：手动触发时向固定的 Compliance Audit issue 追加摘要评论。
- Finding issue：每个确认 finding 一条 issue，用 `fingerprint` 作为生命周期主键。

## Issue 标签

自动流程维护这些标签：

- `qa`
- `qa/report/compliance`
- `severity/critical`
- `severity/high`
- `severity/medium`
- `severity/low`
- `severity/info`
- `qa/status/new`
- `qa/status/persistent`
- `qa/status/not-reproduced`
- `qa/status/recheck-passed`
- `qa/status/recheck-failed`

人工流程使用：

- `qa/manual/ready-for-recheck`

## 状态流转

- GitHub 没有、本次有：创建 issue，标记 `qa/status/new`。
- GitHub 有、本次仍有：更新 issue，标记 `qa/status/persistent`。
- GitHub 有、本次没有：标记 `qa/status/not-reproduced`。
- 人工加 `qa/manual/ready-for-recheck`，本次仍有：标记 `qa/status/recheck-failed`。
- 人工加 `qa/manual/ready-for-recheck`，本次没有：标记 `qa/status/recheck-passed`。

Issue 状态只在手动触发巡检时流转。自动流程不会自动关闭 issue，QA 或负责人确认后手动关闭。

## 查看报告

手动触发时，`publish_report_to_github.py` 会向 report-feed issue 写评论，但 workflow 不做 `@mention` 通知。查看结果时进入对应 workflow run 的 summary、artifact，或打开 report-feed issue。

## 审核请求通知

`.github/workflows/pr-review-notify.yml` 监听 `pull_request_target` 的 `review_requested` 事件。GitHub 因 CODEOWNERS 或人工操作请求个人审核后，workflow 会立即发送飞书私聊卡片，不等待 CI 检查完成。

发送阶段会：

1. 将本次请求的 GitHub 审核人登录名映射到已登记的工作邮箱；
2. 使用 `FEISHU_APP_ID` 和 `FEISHU_APP_SECRET` 获取飞书 tenant access token；
3. 通过飞书通讯录 API 用邮箱查找审核人的 `open_id`；
4. 解析本次改动中由该审核人负责的 CODEOWNERS 区域；
5. 向审核人的飞书私聊发送 interactive 卡片，卡片包含仓库、PR、提交者、提交 SHA、审核区域和打开 PR 的按钮。

Workflow 不 checkout、执行或读取 PR 分支代码，因此使用 `pull_request_target` 时不会把不可信 PR 代码暴露给飞书凭据。仓库需要配置以下 Actions secrets：

- `FEISHU_APP_ID`
- `FEISHU_APP_SECRET`

飞书应用需要具备获取 tenant access token、按邮箱查询用户和发送 IM 消息的权限；管理员还需要允许应用向目标用户发送私聊消息。可从 Actions 页面手动运行该 workflow，选择 `Probus-XD`，以真实测试邮箱寻人和卡片发送，而不必先创建 PR。真实的 `review_requested` 事件仍需在 GitHub 中请求一次审核。

## Workflow 权限

合规审计流程使用仓库内置的 `GITHUB_TOKEN`，最小权限为：

```yaml
permissions:
  contents: read
  issues: write
```

审核通知流程另外声明 `pull-requests: read`，用于读取 PR 文件并解析审核区域。该流程使用仓库 Actions secrets 访问飞书；合规审计本身不依赖外部 webhook 或飞书凭据。

当前流程不需要外部 webhook。
