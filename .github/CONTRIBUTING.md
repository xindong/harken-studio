**English** | [简体中文](CONTRIBUTING.zh-CN.md)

# Contributing Guide

Contributions to this repository are subject to the following rules. Please read this guide before filing Issues or submitting Pull Requests.

## Development Environment

See the root [README.md](../README.md) for setup requirements and build/test steps.

## Filing Issues

- **Bug reports**: include the version in use (the build number from the Hub/Studio About dialog or the install directory), steps to reproduce, expected vs. actual behavior, and logs where possible (Hub logs can be exported with `--export-logs`).
- **Feature requests**: describe the use case and motivation so priority can be assessed.
- Do not file public Issues for security vulnerabilities — use the private reporting channel in [SECURITY.md](SECURITY.md).

## Submitting Pull Requests

1. Create a feature branch (`feature/<topic>`) from the latest `main`; never push to `main` directly.
2. Follow the repository coding and quality standards: root `CLAUDE.md` / `AGENTS.md` and `.docs/quality/`.
3. Keep changes focused — one PR solves one thing; provide the corresponding unit tests and make sure they pass locally.
   Title the result. If users see a change, say what the software can do now or what was fixed. For docs, process, or no-behavior-change refactors, say what rule or cleanup this PR lands; do not invent a product capability. Do not write frontend/backend. Do not stack module lists. `type(scope): plain summary` is a habit, not a form; a readable result is enough. If the current work is staged, the title may include the phase/pack id: `feat(scope): Monitor <phase/pack> summary`. Examples: `feat(studio): Monitor 期一 P1-2 时间序列图表控件`; `feat(runtime): Monitor M0-2 解码 record 封套与 State cut fixture`; `docs(studio): 写入 PR 标题规划由提示词起草`. Project prompts draft the title. Empty or shell titles are a human review matter, not a repository compliance gate.
4. All CI checks triggered by the PR (build gate, compliance audit, code review) must pass.
5. Respond to every review comment; all inline threads must be marked Resolved before merging.

## Intellectual Property Sign-off (DCO)

This project adopts the [Developer Certificate of Origin 1.1](https://developercertificate.org/) (DCO). By contributing you certify, per the DCO, that the contribution is your own work or that you have the right to license it to this project, and that it contains no unauthorized third-party code and does not infringe any copyright, patent, or trade secret. Every commit must be signed off with `git commit -s`, adding a `Signed-off-by: Name <email>` line to indicate your agreement to the DCO. Before introducing a new third-party dependency, confirm its license complies with the repository dependency policy (see `.docs/quality/DEPENDENCY_POLICY.md`) and register it in `NOTICE`.

For unsigned commits already merged into `main`, do not rewrite shared history. The original contributor must explicitly certify DCO 1.1 for a list of full commit SHAs, authorize their sign-off and acknowledge that the certification is public. A maintainer must verify the contributor's identity and the actual certification before recording it in [dco-attestations.json](dco-attestations.json). Approval of a remediation plan, company open-source authorization, or a maintainer's sign-off on the recording commit does not substitute for this certification.

Each completed entry in `attestations` contains `dco_version` (`"1.1"`), `signed_off_by` (one complete `Signed-off-by: Name <email>` line using the original contributor's email), `recorded_on` (the recording date, `YYYY-MM-DD`), `evidence` (the form and source of the confirmation), and `commits` (the full SHA list). Written confirmation should include its reference. For relayed verbal confirmation, explicitly record the relay and what was confirmed; do not describe it as a direct written sign-off by the contributor. `pending_commits` is an inventory only and never exempts a commit. Leave `attestations` empty until real confirmation is available; do not enter placeholders or sign on someone else's behalf.

The compliance audit checks the complete history, including on scheduled runs and `main` pushes, and fails on shallow clones or incomplete records. Automated checks validate the record structure and scope; reviewers must verify authenticity. Historical records do not exempt unsigned commits introduced by a new PR from the existing per-commit sign-off check.
