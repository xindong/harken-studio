from __future__ import annotations

import json
import os
import runpy
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class CompliancePullRequestGovernanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        audit_module = runpy.run_path(str(REPO_ROOT / ".scripts" / "compliance_audit.py"))
        cls.audit_module = audit_module
        cls.detail_answered = staticmethod(audit_module["detail_answered"])
        cls.count_section_structure = staticmethod(
            audit_module["count_section_structure"]
        )
        cls.section_entries = staticmethod(audit_module["section_entries"])

    def test_pr_governance_matches_detail_prompts_by_item_identity(self) -> None:
        body_item = self.audit_module["BodyItem"]
        match_template_items = self.audit_module["match_template_items"]
        aliases = (
            ("无需说明",),
            ("需要说明",),
            ("其他",),
        )
        items = [
            body_item(False, "需要说明", "", False, []),
            body_item(False, "无需说明", "", False, []),
            body_item(False, "其他", "", False, []),
        ]

        matches, duplicates = match_template_items(items, aliases)

        self.assertEqual(matches, [1, 0, 2])
        self.assertEqual(duplicates, [])

    def test_pr_governance_rejects_duplicate_item_identity(self) -> None:
        body_item = self.audit_module["BodyItem"]
        match_template_items = self.audit_module["match_template_items"]
        aliases = (
            ("无需说明",),
            ("需要说明",),
            ("其他",),
        )
        items = [
            body_item(False, "无需说明", "", False, []),
            body_item(False, "无需说明", "", False, []),
            body_item(False, "其他", "", False, []),
        ]

        matches, duplicates = match_template_items(items, aliases)

        self.assertEqual(matches, [0, 0, 2])
        self.assertEqual(duplicates, [0])

    def test_pr_governance_accepts_nested_list_as_field_content(self) -> None:
        lines = [
            "- Platform: Windows 11",
            "- Build preset: windows-ninja-clang-dev",
            "- Test targets:",
            "  - `compliance_audit.py` -> highest=none",
            "  - `harken_editor_tests` -> 4 passed",
        ]

        items, fields = self.section_entries(lines)

        self.assertEqual(self.count_section_structure(lines), (0, 3))
        self.assertEqual(items, [])
        self.assertEqual(
            fields,
            [
                "Windows 11",
                "windows-ninja-clang-dev",
                "`compliance_audit.py` -> highest=none `harken_editor_tests` -> 4 passed",
            ],
        )

    def test_pr_governance_sensitivity_contract_items_have_distinct_aliases(self) -> None:
        template = (REPO_ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(
            encoding="utf-8"
        )
        specs = self.audit_module["pull_request_section_specs"](template)
        sensitivity = next(
            spec
            for spec in specs
            if "敏感度报告" in spec.title or "Sensitivity Report" in spec.title
        )
        alias_hit = self.audit_module["alias_hit"]
        markers = (
            "HarkenHub",
            "HarkenStudio",
            "HarkenSDK",
        )
        contract_items = [
            aliases
            for aliases in sensitivity.item_aliases
            if any(any(marker in alias for alias in aliases) for marker in markers)
        ]

        chinese_only = [aliases[0] for aliases in contract_items]
        english_only = [aliases[1] for aliases in contract_items]

        body_item = self.audit_module["BodyItem"]
        match_template_items = self.audit_module["match_template_items"]
        for language, labels in (
            ("zh", chinese_only),
            ("en", english_only),
        ):
            with self.subTest(language=language):
                items = [
                    body_item(False, label, "", False, []) for label in labels
                ]
                matches, duplicates = match_template_items(
                    items, tuple(contract_items)
                )
                self.assertEqual(matches, list(range(len(contract_items))))
                self.assertEqual(duplicates, [])

        for index, aliases in enumerate(contract_items):
            with self.subTest(index=index, aliases=aliases):
                others = [
                    other
                    for other_index, other in enumerate(contract_items)
                    if other_index != index
                ]
                for alias in aliases:
                    hits = [
                        other
                        for other in others
                        if alias_hit(alias, other)
                    ]
                    self.assertEqual(
                        hits,
                        [],
                        f"alias {alias!r} also matches another contract item",
                    )

    def test_pr_governance_ignores_prompt_formatting_changes(self) -> None:
        prompts = (
            "波及本项时,需诚实说明改动内容与动机 / If checked, honestly describe what changed and why:",
        )
        unanswered = (
            "波及本项时，需诚实说明改动内容与动机 / If checked, honestly describe what changed and why",
            "波及本项时，需诚实说明改动内容与动机/If checked, honestly describe what changed and why:",
        )
        answered = (
            "波及本项时，需诚实说明改动内容与动机 / If checked, honestly describe what changed and why: 修改审计规则",
            "直接说明修改审计规则",
        )

        for content in unanswered:
            with self.subTest(content=content):
                self.assertFalse(self.detail_answered(content, prompts))
        for content in answered:
            with self.subTest(content=content):
                self.assertTrue(self.detail_answered(content, prompts))


class ComplianceDcoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audit_module = runpy.run_path(str(REPO_ROOT / ".scripts" / "compliance_audit.py"))
        cls.parse_sign_offs = staticmethod(cls.audit_module["parse_sign_offs"])
        cls.validate_dco_commit = staticmethod(cls.audit_module["validate_dco_commit"])
        cls.dco_exempt_reason = staticmethod(cls.audit_module["dco_exempt_reason"])
        cls.looks_like_email = staticmethod(cls.audit_module["looks_like_email"])
        cls.rule_pull_request_dco = staticmethod(cls.audit_module["rule_pull_request_dco"])
        cls.PullRequestContext = cls.audit_module["PullRequestContext"]
        cls.DcoCommit = cls.audit_module["DcoCommit"]

    def _commit(
        self,
        *,
        sha: str = "a" * 40,
        parents: tuple[str, ...] = ("b" * 40,),
        author_name: str = "Alice",
        author_email: str = "alice@xd.com",
        committer_name: str = "Alice",
        committer_email: str = "alice@xd.com",
        message: str = "feat: example\n",
    ):
        return self.DcoCommit(
            sha=sha,
            parents=parents,
            author_name=author_name,
            author_email=author_email,
            committer_name=committer_name,
            committer_email=committer_email,
            message=message,
        )

    def test_no_pull_request_context_skips_gov004(self) -> None:
        findings: list = []
        self.rule_pull_request_dco(None, findings)
        self.assertEqual(findings, [])

    def test_valid_signoff_matching_author_passes(self) -> None:
        commit = self._commit(
            message="feat: example\n\nSigned-off-by: Alice <alice@xd.com>\n"
        )
        self.assertEqual(self.validate_dco_commit(commit), [])
        self.assertIsNone(self.dco_exempt_reason(commit))

    def test_missing_trailer_fails(self) -> None:
        errors = self.validate_dco_commit(self._commit(message="feat: example\n"))
        self.assertEqual(errors, ["No Signed-off-by trailer."])

    def test_identity_alias_qiming_passes(self) -> None:
        commit = self._commit(
            author_name="zqm-web",
            author_email="zqm@xd.com",
            committer_name="zqm-web",
            committer_email="zqm@xd.com",
            message="feat: example\n\nSigned-off-by: zhuqiming <zhuqiming@xd.com>\n",
        )
        self.assertEqual(self.validate_dco_commit(commit), [])

    def test_identity_alias_dengyun_passes(self) -> None:
        commit = self._commit(
            author_name="huangdengyun-xd",
            author_email="huangdengyun@xd.com",
            committer_name="huangdengyun-xd",
            committer_email="huangdengyun@xd.com",
            message="feat: example\n\nSigned-off-by: huangdengyun <huangdengyun@xd.com>\n",
        )
        self.assertEqual(self.validate_dco_commit(commit), [])

    def test_identity_alias_huahua_passes(self) -> None:
        commit = self._commit(
            author_name="wanhuahua0108",
            author_email="huahua@xd.com",
            committer_name="wanhuahua0108",
            committer_email="huahua@xd.com",
            message="feat: example\n\nSigned-off-by: huahua-creator <huahua@xd.com>\n",
        )
        self.assertEqual(self.validate_dco_commit(commit), [])

    def test_merge_commit_without_trailer_is_exempt(self) -> None:
        commit = self._commit(parents=("b" * 40, "c" * 40), message="Merge pull request #1\n")
        self.assertEqual(self.dco_exempt_reason(commit), "merge commit")

    def test_two_spaces_after_colon_still_count(self) -> None:
        sign_offs = self.parse_sign_offs("Signed-off-by:  Alice <alice@xd.com>\n")
        self.assertEqual(len(sign_offs), 1)
        self.assertEqual(sign_offs[0].name, " Alice")
        errors = self.validate_dco_commit(
            self._commit(message="feat: example\n\nSigned-off-by:  Alice <alice@xd.com>\n")
        )
        self.assertEqual(errors, [])

    def test_trailer_email_not_like_email_fails(self) -> None:
        errors = self.validate_dco_commit(
            self._commit(message="feat: example\n\nSigned-off-by: Alice <not-an-email>\n")
        )
        self.assertTrue(errors)
        self.assertIn("not a valid email address", errors[0])

    def test_one_valid_trailer_among_invalid_passes(self) -> None:
        commit = self._commit(
            message=(
                "feat: example\n\n"
                "Signed-off-by: Alice <not-an-email>\n"
                "Signed-off-by: Alice <alice@xd.com>\n"
            )
        )
        self.assertEqual(self.validate_dco_commit(commit), [])

    def test_indented_trailer_is_rejected(self) -> None:
        self.assertEqual(
            self.parse_sign_offs(" Signed-off-by: Alice <alice@xd.com>\n"),
            [],
        )
        errors = self.validate_dco_commit(
            self._commit(message="feat: example\n\n Signed-off-by: Alice <alice@xd.com>\n")
        )
        self.assertEqual(errors, ["No Signed-off-by trailer."])

    def test_body_signoff_before_paragraph_still_counts_like_dco_app(self) -> None:
        commit = self._commit(
            message=(
                "feat: example\n\n"
                "Signed-off-by: Alice <alice@xd.com>\n"
                "More description after the sign-off.\n"
            )
        )
        self.assertEqual(self.validate_dco_commit(commit), [])

    def test_oversized_line_is_not_treated_as_signoff(self) -> None:
        huge = "Signed-off-by: " + ("A" * 600) + " <alice@xd.com>"
        self.assertEqual(self.parse_sign_offs(huge + "\n"), [])

    def test_control_character_in_message_fails_closed(self) -> None:
        parse_git_log_commits = self.audit_module["parse_git_log_commits"]
        sep = self.audit_module["DCO_GIT_LOG_FIELD_SEP"]
        rec = self.audit_module["DCO_GIT_LOG_RECORD_SEP"]
        sha = "a" * 40
        parent = "b" * 40
        poisoned = sep.join(
            (
                sha,
                parent,
                "Alice",
                "alice@xd.com",
                "Alice",
                "alice@xd.com",
                f"feat: example{sep}Signed-off-by: Alice <alice@xd.com>\n",
            )
        ) + rec
        with self.assertRaises(RuntimeError) as raised:
            parse_git_log_commits(poisoned)
        self.assertIn("dco-parse-error", str(raised.exception))

    def test_control_character_range_emits_gov004(self) -> None:
        findings: list = []
        original = self.audit_module["rule_pull_request_dco"].__globals__["run_git"]

        class Result:
            returncode = 0
            stdout = (
                self.audit_module["DCO_GIT_LOG_FIELD_SEP"].join(
                    (
                        "a" * 40,
                        "b" * 40,
                        "Alice",
                        "alice@xd.com",
                        "Alice",
                        "alice@xd.com",
                        f"feat: example{self.audit_module['DCO_GIT_LOG_FIELD_SEP']}"
                        "Signed-off-by: Alice <alice@xd.com>\n",
                    )
                )
                + self.audit_module["DCO_GIT_LOG_RECORD_SEP"]
            )
            stderr = ""

        self.audit_module["rule_pull_request_dco"].__globals__["run_git"] = lambda args: Result()
        try:
            context = self.PullRequestContext(
                base_sha="b" * 40,
                body="synthetic",
                head_sha="a" * 40,
            )
            self.rule_pull_request_dco(context, findings)
        finally:
            self.audit_module["rule_pull_request_dco"].__globals__["run_git"] = original
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule_id, "GOV-004")
        self.assertIn("dco-parse-error", findings[0].evidence or "")
        self.assertEqual(findings[0].title, "无法读取 PR commit 范围做 DCO 校验")

    def test_missing_head_sha_emits_gov004_without_raising(self) -> None:
        findings: list = []
        context = self.PullRequestContext(
            base_sha="a" * 40,
            body="- [x] 每个 commit 均使用 `git commit -s`",
            head_sha=None,
        )
        self.rule_pull_request_dco(context, findings)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule_id, "GOV-004")
        self.assertEqual(findings[0].severity, "high")

    def test_template_checkbox_does_not_satisfy_dco(self) -> None:
        findings: list = []
        context = self.PullRequestContext(
            base_sha="a" * 40,
            body="- [x] 每个 commit 均使用 `git commit -s`",
            head_sha=None,
        )
        self.rule_pull_request_dco(context, findings)
        self.assertEqual(findings[0].rule_id, "GOV-004")

    def test_company_author_emails_look_valid(self) -> None:
        for email in (
            "huahua@xd.com",
            "huangdengyun@xd.com",
            "zqm@xd.com",
        ):
            with self.subTest(email=email):
                self.assertTrue(self.looks_like_email(email))


class ComplianceDcoGitRangeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.audit_module = runpy.run_path(str(REPO_ROOT / ".scripts" / "compliance_audit.py"))
        self._original_run_git = self.audit_module["run_git"]
        self.temp_dir = Path(tempfile.mkdtemp(prefix="gov004-dco-"))
        env = os.environ.copy()
        env["GIT_AUTHOR_NAME"] = "Alice"
        env["GIT_AUTHOR_EMAIL"] = "alice@xd.com"
        env["GIT_COMMITTER_NAME"] = "Alice"
        env["GIT_COMMITTER_EMAIL"] = "alice@xd.com"
        self.env = env
        subprocess.run(
            ["git", "init", "-b", "main"],
            cwd=self.temp_dir,
            check=True,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Alice"],
            cwd=self.temp_dir,
            check=True,
            env=self.env,
        )
        subprocess.run(
            ["git", "config", "user.email", "alice@xd.com"],
            cwd=self.temp_dir,
            check=True,
            env=self.env,
        )

        def run_git(args: list[str]):
            return subprocess.run(
                ["git", *args],
                cwd=self.temp_dir,
                text=True,
                encoding="utf-8",
                errors="replace",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

        # runpy.run_path 返回的是 globals 副本；函数仍从 __globals__ 取名。
        self.rule_fn = self.audit_module["rule_pull_request_dco"]
        self.rule_fn.__globals__["run_git"] = run_git
        self.history_rule = self.audit_module["rule_history_dco"]
        self.history_rule.__globals__["REPO_ROOT"] = self.temp_dir
        (self.temp_dir / ".github").mkdir()
        self._write_attestations([])
        self.addCleanup(self._restore_run_git)
        self.addCleanup(lambda: shutil.rmtree(self.temp_dir, ignore_errors=True))

    def _restore_run_git(self) -> None:
        self.rule_fn.__globals__["run_git"] = self._original_run_git
        self.history_rule.__globals__["REPO_ROOT"] = REPO_ROOT

    def _write_attestations(self, records: list) -> None:
        (self.temp_dir / ".github/dco-attestations.json").write_text(
            json.dumps({"schema_version": 1, "attestations": records}), encoding="utf-8"
        )

    def _attestation(self, sha: str) -> dict:
        return {
            "dco_version": "1.1",
            "signed_off_by": "Signed-off-by: Alice <alice@xd.com>",
            "recorded_on": "2026-01-01",
            "evidence": "https://example.com/review/1",
            "commits": [sha],
        }

    def _commit(self, message: str, *, signoff: bool) -> str:
        (self.temp_dir / "note.txt").write_text(message + "\n", encoding="utf-8")
        subprocess.run(["git", "add", "note.txt"], cwd=self.temp_dir, check=True, env=self.env)
        command = ["git", "commit", "-m", message]
        if signoff:
            command.append("-s")
        subprocess.run(
            command,
            cwd=self.temp_dir,
            check=True,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        sha = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=self.temp_dir,
            text=True,
            env=self.env,
        ).strip()
        return sha

    def test_signed_range_has_no_gov004(self) -> None:
        base = self._commit("base", signoff=True)
        head = self._commit("feat: signed", signoff=True)
        findings: list = []
        context = self.audit_module["PullRequestContext"](
            base_sha=base,
            body="- [x] 每个 commit 均使用 `git commit -s`",
            head_sha=head,
        )
        self.rule_fn(context, findings)
        self.assertEqual(findings, [])

    def test_unsigned_range_emits_gov004(self) -> None:
        base = self._commit("base", signoff=True)
        head = self._commit("feat: unsigned", signoff=False)
        findings: list = []
        context = self.audit_module["PullRequestContext"](
            base_sha=base,
            body="- [x] 每个 commit 均使用 `git commit -s`",
            head_sha=head,
        )
        self.rule_fn(context, findings)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule_id, "GOV-004")
        self.assertEqual(findings[0].severity, "high")
        self.assertEqual(findings[0].title, "PR commit 缺少有效 DCO 签名")
        self.assertIn(head[:8], findings[0].evidence or "")

    def test_history_checks_unsigned_ancestors_without_pr_context(self) -> None:
        base = self._commit("unsigned original contribution", signoff=False)
        self._commit("signed follow-up", signoff=True)
        (self.temp_dir / ".github/dco-attestations.json").write_text(json.dumps({
            "schema_version": 1, "attestations": [], "pending_commits": [base],
        }), encoding="utf-8")
        findings: list = []
        self.history_rule(findings)
        self.assertEqual([item.rule_id for item in findings], ["GOV-005"])
        self.assertIn(base, findings[0].evidence)

    def test_complete_signed_history_passes(self) -> None:
        self._commit("signed root", signoff=True)
        self._commit("signed follow-up", signoff=True)
        findings: list = []
        self.history_rule(findings)
        self.assertEqual(findings, [])

    def test_attestation_covers_only_listed_commits(self) -> None:
        first = self._commit("first unsigned", signoff=False)
        second = self._commit("second unsigned", signoff=False)
        self._write_attestations([self._attestation(first)])
        findings: list = []
        self.history_rule(findings)
        self.assertEqual(len(findings), 1)
        self.assertNotIn(first, findings[0].evidence)
        self.assertIn(second, findings[0].evidence)
        record = self._attestation(first)
        record["commits"].append(second)
        self._write_attestations([record])
        findings.clear()
        self.history_rule(findings)
        self.assertEqual(findings, [])

    def test_attestation_does_not_exempt_new_pr_commits(self) -> None:
        base = self._commit("signed base", signoff=True)
        head = self._commit("unsigned PR", signoff=False)
        self._write_attestations([self._attestation(head)])
        findings: list = []
        self.rule_fn(self.audit_module["PullRequestContext"](
            base_sha=base, head_sha=head, body=""), findings)
        self.assertEqual([item.rule_id for item in findings], ["GOV-004"])

    def test_invalid_or_unconfirmed_attestations_fail_closed(self) -> None:
        sha = self._commit("unsigned", signoff=False)
        for field, value in (
            ("dco_version", None), ("signed_off_by", None),
            ("signed_off_by", "Signed-off-by: Bob <bob@xd.com>"),
            ("recorded_on", None), ("recorded_on", "9999-01-01"),
            ("recorded_on", "2026-02-30"), ("evidence", None),
            ("evidence", "   "),
            ("commits", []), ("commits", [sha[:8]]),
            ("commits", ["a" * 40]), ("commits", [sha, sha]),
        ):
            with self.subTest(field=field, value=value):
                record = self._attestation(sha)
                record[field] = value
                self._write_attestations([record])
                findings: list = []
                self.history_rule(findings)
                self.assertEqual([item.rule_id for item in findings], ["GOV-005"])
                self.assertEqual(findings[0].title, "无法完成全历史 DCO 校验")

    def test_shallow_history_does_not_hide_unsigned_ancestors(self) -> None:
        self._commit("unsigned root", signoff=False)
        self._commit("signed tip", signoff=True)
        with tempfile.TemporaryDirectory() as clone:
            subprocess.run(["git", "clone", "--depth=1", self.temp_dir.as_uri(), clone],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.history_rule.__globals__["run_git"] = lambda args: subprocess.run(
                ["git", *args], cwd=clone, text=True, encoding="utf-8",
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            findings: list = []
            self.history_rule(findings)
            self.assertEqual([item.rule_id for item in findings], ["GOV-005"])
            self.assertIn("Full Git history is required", findings[0].evidence)


if __name__ == "__main__":
    unittest.main()
