from __future__ import annotations

import io
import json
import tempfile
import unittest
import unittest.mock as mock
import urllib.error
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "slack_notify.py"
MODULE = load_script_module("slack_notify", SCRIPT)


PR_BODY = """# PR 描述 / Description

## 摘要 / Summary

<!-- 这个 PR 改了什么？ / What does this PR change? -->

- New Project 页读取并回填 Hub 已保存的工程 Location。
- Browse 取消或输入空路径时保留原有配置。

## 变更范围 / Scope

- [x] `HarkenStudio/*`
- [ ] `HarkenEngine/*`
- [x] `HarkenHub/*`
- [ ] `HarkenIntegration/*`
"""


class SlackNotifyTest(unittest.TestCase):
    def test_parse_checked_product_scopes_only(self) -> None:
        labels = MODULE.parse_scope_labels(PR_BODY)
        self.assertEqual(labels, ["Studio", "Hub"])
        self.assertEqual(MODULE.parse_scope_labels(""), [])
        self.assertEqual(MODULE.parse_scope_labels("- [ ] `HarkenStudio/*`"), [])

    def test_issue_body_does_not_invent_scope(self) -> None:
        body = "## 预期行为\n删除对象后 Inspector 页签关闭。\nHarkenStudio 里仍能看到旧页签。"
        self.assertEqual(MODULE.parse_scope_labels(body), [])

    def test_extract_summary_drops_comments_and_later_sections(self) -> None:
        summary = MODULE.extract_summary_section(PR_BODY)
        self.assertIn("New Project 页", summary)
        self.assertNotIn("这个 PR 改了什么", summary)
        self.assertNotIn("变更范围", summary)

    def test_sanitize_strips_mentions_and_secrets(self) -> None:
        # Split the sample so the compliance scanner does not treat this
        # fixture as a live token.
        sample = "".join(("sk", "-", "abcdefghijklmnopqrstuvwxyz"))  # REPLACE_WITH
        dirty = (
            "请 @channel 看一下 <@U01DFRZJCMC> 和 ＠Probus-XD\n"
            f"key={sample}\n"
            "<!here>"
        )
        cleaned = MODULE.sanitize_plain(dirty, 400)
        self.assertNotIn("@channel", cleaned)
        self.assertNotIn("<@U01DFRZJCMC>", cleaned)
        self.assertNotIn("Probus-XD", cleaned)
        self.assertNotIn(sample, cleaned)
        self.assertNotIn("<!here>", cleaned)

    def test_mention_uses_map_and_falls_back_to_login(self) -> None:
        mapping = {"Probus-XD": "U09ES4R8QLX"}
        self.assertEqual(MODULE.mention_for("Probus-XD", mapping), "<@U09ES4R8QLX>")
        self.assertEqual(MODULE.mention_for("unknown-dev", mapping), "unknown-dev")
        self.assertEqual(MODULE.mention_for("bad<script>", mapping), "bad&lt;script&gt;")
        self.assertEqual(MODULE.mention_for("x", {"x": "not-an-id"}), "x")

    def test_format_title_keeps_chinese_and_adds_translation(self) -> None:
        self.assertEqual(
            MODULE.format_title("支持 Hub 记忆新建工程路径", "ignored"),
            "支持 Hub 记忆新建工程路径",
        )
        original = "fix(studio): preserve rapid tree chevron toggles"
        translated = "修复 Studio：保留快速树形展开箭头切换"
        title = MODULE.format_title(original, translated)
        self.assertIn(original, title)
        self.assertIn(translated, title)

    def test_build_pr_and_issue_cards(self) -> None:
        pr = MODULE.build_card(
            {
                "kind": "pr",
                "number": "188",
                "url": "https://github.com/xindong/harken-studio/pull/188",
                "title": "修复 Studio：保留快速树形展开箭头切换（fix(studio): preserve rapid tree chevron toggles）",
                "author": "<@U09ES4R8QLX>",
                "merged_by": "<@U09ES4R8QLX>",
                "digest": "工程树左侧小箭头现在可以连点两次。",
                "prefix": "【Studio】",
            }
        )
        self.assertIn("<@U09ES4R8QLX>", pr)
        self.assertIn("https://github.com/xindong/harken-studio/pull/188", pr)

        issue = MODULE.build_card(
            {
                "kind": "issue",
                "number": "186",
                "url": "https://github.com/xindong/harken-studio/issues/186",
                "title": "删除对象后对应 Inspector 页签仍保留",
                "author": "<@U09F43VS5EH>",
                "digest": "删掉对象后，右边 Inspector 里对应页签还在。",
                "prefix": "",
            }
        )
        self.assertIn("<@U09F43VS5EH>", issue)

    def test_empty_digest_preserves_event_attribution(self) -> None:
        event = {
            "kind": "pr",
            "number": "183",
            "url": "https://github.com/xindong/harken-studio/pull/183",
            "title": "支持 Hub 记忆新建工程路径",
            "author": "<@U0AHFCDFK47>",
            "merged_by": "<@U0AHFCDFK47>",
            "digest": "",
            "prefix": "【Hub】",
        }
        card = MODULE.build_card(event)
        self.assertIn(event["author"], card)
        self.assertIn(event["url"], card)

    def test_collect_event_does_not_post_when_model_fails(self) -> None:
        env = {
            "EVENT_NAME": "pull_request_target",
            "PR_NUMBER": "183",
            "PR_TITLE": "支持 Hub 记忆新建工程路径",
            "PR_URL": "https://github.com/xindong/harken-studio/pull/183",
            "PR_AUTHOR": "liuxin2-sys",
            "PR_MERGED_BY": "liuxin2-sys",
            "PR_BODY": PR_BODY,
            "GH_SLACK_MAP": json.dumps({"liuxin2-sys": "U0AHFCDFK47"}),
        }
        with mock.patch.object(MODULE, "call_model", return_value=""):
            event = MODULE.collect_event(env)
        self.assertIn("Studio", event["prefix"])
        self.assertIn("Hub", event["prefix"])
        self.assertEqual(event["digest"], "")
        self.assertEqual(event["author"], "<@U0AHFCDFK47>")
        card = MODULE.build_card(event)
        self.assertIn("https://github.com/xindong/harken-studio/pull/183", card)

    def test_post_slack_is_not_retried_on_http_error(self) -> None:
        calls = {"n": 0}

        def boom(*_args, **_kwargs):
            calls["n"] += 1
            raise urllib.error.URLError("down")

        with mock.patch.object(MODULE.urllib.request, "urlopen", side_effect=boom):
            with self.assertRaises(SystemExit):
                MODULE.post_slack("token", "C0BLA6ALY6N", "hello")
        self.assertEqual(calls["n"], 1)

    def test_post_slack_rejects_non_object_json(self) -> None:
        class FakeResponse:
            def read(self) -> bytes:
                return b'["not", "ok"]'

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        with mock.patch.object(MODULE.urllib.request, "urlopen", return_value=FakeResponse()):
            with self.assertRaises(SystemExit):
                MODULE.post_slack("token", "C0BLA6ALY6N", "hello")

    def test_call_model_treats_unexpected_json_as_empty(self) -> None:
        class FakeResponse:
            def read(self) -> bytes:
                return b'["not", "an", "object"]'

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        with mock.patch.object(MODULE.urllib.request, "urlopen", return_value=FakeResponse()):
            text = MODULE.call_model(
                base_url="https://example.invalid",
                api_key="test-key",
                system="sys",
                user="hi",
                max_tokens=20,
                attempts=1,
            )
        self.assertEqual(text, "")

    def test_collect_event_reads_github_event_path(self) -> None:
        payload = {
            "pull_request": {
                "number": 188,
                "title": "fix(studio): preserve rapid tree chevron toggles",
                "html_url": "https://github.com/xindong/harken-studio/pull/188",
                "user": {"login": "Probus-XD"},
                "body": PR_BODY,
                "merged_by": {"login": "Probus-XD"},
            }
        }
        handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False)
        self.addCleanup(lambda: Path(handle.name).unlink(missing_ok=True))
        handle.write(json.dumps(payload))
        handle.close()
        event_path = Path(handle.name)
        env = {
            "EVENT_NAME": "pull_request_target",
            "GITHUB_EVENT_PATH": str(event_path),
            "GH_SLACK_MAP": json.dumps({"Probus-XD": "U09ES4R8QLX"}),
        }
        with mock.patch.object(
            MODULE,
            "call_model",
            side_effect=["修复 Studio：保留快速树形展开箭头切换", "工程树左侧小箭头可以连点两次。"],
        ):
            event = MODULE.collect_event(env)
        self.assertEqual(event["number"], "188")
        self.assertIn("Studio", event["prefix"])
        self.assertIn("Hub", event["prefix"])
        self.assertIn("连点两次", event["digest"])
        self.assertEqual(event["author"], "<@U09ES4R8QLX>")

    def test_main_dry_run_prints_card_without_slack(self) -> None:
        env = {
            "EVENT_NAME": "issues",
            "ISSUE_NUMBER": "186",
            "ISSUE_TITLE": "删除对象后对应 Inspector 页签仍保留",
            "ISSUE_URL": "https://github.com/xindong/harken-studio/issues/186",
            "ISSUE_AUTHOR": "mengyufan-byte",
            "ISSUE_BODY": "## 预期行为\n删除后页签关闭。",
            "GH_SLACK_MAP": json.dumps({"mengyufan-byte": "U09F43VS5EH"}),
        }
        buf = io.StringIO()
        with mock.patch.object(MODULE, "call_model", return_value="删掉对象后页签还在。"):
            with mock.patch.object(MODULE, "post_slack") as posted:
                with mock.patch("sys.stdout", buf):
                    code = MODULE.main(["--dry-run"], env=env)
        self.assertEqual(code, 0)
        posted.assert_not_called()
        self.assertIn(env["ISSUE_URL"], buf.getvalue())


if __name__ == "__main__":
    unittest.main()
