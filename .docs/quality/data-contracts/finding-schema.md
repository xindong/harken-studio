# Finding Schema

finding 是自动合规系统的问题最小单元。所有合规规则都输出统一 finding。

```json
{
  "fingerprint": "REL-001|.claude|",
  "title": "个人 Claude 自动化被纳入版本跟踪",
  "category": "release-boundary",
  "severity": "high",
  "file": ".claude",
  "line": null,
  "evidence": "matched path .claude",
  "recommendation": "从公开发布分支移除个人自动化，或迁移到私有工具仓库。",
  "source": "compliance:REL-001",
  "report_type": "compliance",
  "commit_sha": "abcdef123456"
}
```

## 字段说明

- `fingerprint`：问题生命周期主键，应尽量稳定，不应只依赖行号。
- `title`：问题标题，面向人工阅读。
- `category`：问题类型，例如 `privacy`、`license`、`performance`。
- `severity`：严重级别，使用 `critical`、`high`、`medium`、`low`、`info`。
- `file` / `line`：可定位时填写。
- `evidence`：证据摘要，不得泄露真实 secret。
- `recommendation`：修复建议。
- `source`：检测来源，例如 `compliance:REL-001`。
- `report_type`：当前固定为 `compliance`。
- `commit_sha`：本次检查对应的 Git commit。
