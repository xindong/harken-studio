[English](CONTRIBUTING.md) | **简体中文**

# 贡献指南

本仓库的贡献活动适用以下规则。提交 Issue 或 Pull Request 前，请阅读本指南。

## 开发环境

部署要求、构建与测试步骤见根目录 [README.md](../README.md)。

## 提交 Issue

- **缺陷报告**：请包含所用版本（Hub/Studio 的关于框或安装目录中的构建号）、复现步骤、预期与实际行为，并尽量附上日志（Hub 可用 `--export-logs` 导出）。
- **功能建议**：请说明使用场景与动机，便于评估优先级。
- 安全漏洞请勿提交公开 Issue，走 [SECURITY.zh-CN.md](SECURITY.zh-CN.md) 的私下报告渠道。

## 提交 Pull Request

1. 从最新 `main` 创建特性分支（`feature/<主题>`），不直接向 `main` 推送；
2. 遵循仓库编码与质量规范：根目录 `CLAUDE.md` / `AGENTS.md` 与 `.docs/quality/`；
3. 保持改动范围聚焦，一个 PR 解决一件事；相应的单元测试须一并提供并在本地跑绿；
   标题写结果。有用户可见变化就写软件多了什么，或修了什么现象。文档、流程、无行为变化的重构，写这张单改了什么规则或清理了什么，不要虚构软件能力。不要写前端/后端，不要堆模块。`类型(范围): 人话` 只是习惯，不是填空；只要结果可读即可。如果当前开发任务有分阶段进行，可以带有期/包号，写成 `feat(范围): Monitor <期/包> 人话`。例：`feat(studio): Monitor 期一 P1-2 时间序列图表控件`；`feat(runtime): Monitor M0-2 解码 record 封套与 State cut fixture`；`docs(studio): 写入 PR 标题规划由提示词起草`。开 PR 时由项目提示词起草标题。空标题和空壳标题由人审，不进仓库检查。
4. PR 触发的全部 CI 检查（构建门禁、合规审计、代码评审）必须通过；
5. 评审意见逐条回应，全部行内会话标记 Resolved 后方可合并。

## 知识产权确认（DCO）

本项目采用 [Developer Certificate of Origin 1.1](https://developercertificate.org/)（DCO）。提交贡献即表示你按 DCO 作出承诺：贡献内容为本人创作或你有权授权本项目使用，不包含未经授权的第三方代码或侵犯他人版权、专利、商业秘密的内容。每个 commit 须使用 `git commit -s` 附带 `Signed-off-by: 姓名 <邮箱>` 签署行，表明你同意 DCO。引入新的第三方依赖前，须确认其许可证符合仓库依赖政策（见 `.docs/quality/DEPENDENCY_POLICY.md`），并在 `NOTICE` 中登记。

已合入 `main` 的历史漏签不重写共享历史。原贡献者须针对完整 commit SHA 清单明确作出 DCO 1.1 确认，授权登记签署行，并知悉确认记录会公开。维护者核实贡献者身份及真实确认后，才可登记到 [dco-attestations.json](dco-attestations.json)。接受补正方案、公司批准开源、维护者对登记提交的签署，均不能替代贡献者的真实确认。

`attestations` 中每条已完成记录包含：`dco_version`（`"1.1"`）、`signed_off_by`（完整的 `Signed-off-by: 姓名 <邮箱>` 行，邮箱须属于原贡献者）、`recorded_on`（登记日期，`YYYY-MM-DD`）、`evidence`（确认形式及来源）以及 `commits`（完整 SHA 清单）。书面确认附来源引用；经转述的口头确认应明确记载转述关系和确认内容，不得描述为原贡献者亲自书面签署。`pending_commits` 仅记录待补正项，不产生豁免。没有真实确认时保持 `attestations` 为空，不填写占位签名或代签。

合规巡检在定时运行和 `main` push 等场景也检查完整历史；浅克隆或不完整记录会导致检查失败。自动化仅核对记录格式与覆盖范围，真实性由评审者核验。历史补正记录不豁免新 PR 引入提交的逐条签署要求。
