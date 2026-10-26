# PR 描述 / Description

## 摘要 / Summary

<!-- 这个 PR 改了什么？ / What does this PR change? -->

## 变更类别 / Change Type

<!-- 这个 PR 解决什么问题？可多选，关联 Issue 以 #编号 引用。
     What does this PR solve? Check all that apply; reference issues as #123. -->

- [ ] 问题修复 / Bug fix
- [ ] 新功能产出 / New feature
- [ ] 无功能变更的代码重构与优化 / Refactoring or optimization (no behavior change)
- [ ] 其他 / Other: <!-- 请说明 / please specify -->

## 变更范围 / Scope

<!-- 这个 PR 的"增删改"波及哪些区间 / Which areas does this PR touch? -->

- [ ] `HarkenStudio/*`
- [ ] `HarkenEngine/*`
- [ ] `HarkenHub/*`
- [ ] `HarkenIntegration/*`
- [ ] 公共文档与脚本 / Shared docs & scripts (`.agents` `.claude` `.codex` `CLAUDE.md` `AGENTS.md` `.docs` `.github` `.scripts`)
    > （波及本项时，需诚实说明改动内容与动机 / If checked, honestly describe what changed and why:）
- [ ] 其他根目录文件及 `cmake/*` / Other root files and `cmake/*`
    > （波及本项时，需诚实说明改动内容与动机 / If checked, honestly describe what changed and why:）

## 如何验证 / Verification

<!-- 这个 PR 在本地落实时的具体环境 / The local environment this PR was built and verified in. -->

- 本地构建、编译、测试时的平台 / Platform used for local configure, build, and test:
- 本地构建、编译的通道和预设 / Build channel and preset used locally:
- 本地测试的目标和内容 / Test targets run locally and what they cover:

## 敏感度报告 / Sensitivity Report

<!-- 勾选本次 PR 提交涉及的重要内容并逐项报告 / Check anything this PR involves and report details under each checked item. -->

- [ ] 包含字体、图像、图标、音频等版权敏感文件，或预编译库、测试夹具等任何二进制文件（无论是否已声明登记）
      / Copyright-sensitive assets (fonts, images, icons, audio) or any binaries such as prebuilt libraries and test fixtures, declared or not
    > 详细报告及登记地点 / Details & where registered:
- [ ] 新增或升级第三方依赖（无论是否固定了完整性哈希，无论是否已声明登记）
      / Third-party dependency added or upgraded, with or without pinned integrity hashes, declared or not
    > 详细报告及登记地点 / Details & where registered:
- [ ] 有关"网络 / 遥测 / 数据采集"等隐私敏感的功能代码（无论是否已声明登记）
      / Privacy-sensitive code involving networking, telemetry, or data collection, declared or not
    > 详细报告及登记地点 / Details & where registered:
- [ ] 包含任何需要许可证的数据内容（如 HRTF 数据集、脉冲响应、滤波器系数表），或专利敏感的算法、技术（无论是否已声明登记）
      / Data requiring a license (e.g. HRTF datasets, impulse responses, filter coefficient tables) or patent-sensitive algorithms and techniques, declared or not
    > 详细报告及登记地点 / Details & where registered:
- [ ] 改变了 HarkenHub 的行为语义导致版本升级（无论是否已升级 Schema 或 VERSION）
      / HarkenHub behavioral-semantics change that requires a version bump, whether or not Schema or VERSION was already upgraded
    > 详细报告及登记地点 / Details & where registered:
- [ ] 改变了 HarkenStudio、pack 包体的指纹或工程格式（无论是否相应完善了旧版迁移，无论是否已升级 Schema 或 VERSION）
      / HarkenStudio or pack artifact fingerprint or project-format change, with or without a completed legacy migration, and whether or not Schema or VERSION was already upgraded
    > 详细报告及登记地点 / Details & where registered:
- [ ] 改变了 HarkenIntegration 面向游戏侧的公开接口或其行为语义（无论是否已升级 Schema 或 VERSION）
      / Game-facing HarkenIntegration public API or behavioral-semantics change, whether or not Schema or VERSION was already upgraded
    > 详细报告及登记地点 / Details & where registered:
- [ ] 以上均不涉及 / None of the above

## 提交前检查 / Pre-submission Checklist

<!-- 合规 PR 应全部诚实勾选以下选项 / A compliant PR honestly checks every item below. -->

- [ ] 没有泄露凭证、令牌或授权文件 / No credentials, tokens, or authorization files are exposed
- [ ] 每个 commit 均使用 `git commit -s` 附带了 DCO 签名 / Every commit is signed off with `git commit -s` (DCO)
