from __future__ import annotations

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

from script_loader import load_script_module


RUNNER = load_script_module("run_test_groups", Path(__file__).resolve().parents[1] / "run_test_groups.py")
FIXTURE = r'''
import fnmatch
import json
import os
from pathlib import Path
import subprocess
import sys
import time

if "--show-only=json-v1" in sys.argv:
    if os.environ.get("FAKE_DISCOVERY_DIAGNOSTIC"):
        print(os.environ["FAKE_DISCOVERY_DIAGNOSTIC"], file=sys.stderr)
    print(Path(sys.argv[1]).read_text(encoding="utf-8"))
    raise SystemExit(0)
mode = os.environ.get("FAKE_MODE", "pass")
if mode == "fail":
    sys.stdout.buffer.write(b"Failure diagnostic: \xff \xe2\x86\x92\n")
print(json.dumps({"cwd": os.getcwd(), "env": os.environ.get("FAKE_VALUE")}), flush=True)
if mode == "hang":
    if os.environ.get("FAKE_CHILD_MARKER"):
        subprocess.Popen([sys.executable, "-c", "import os,time; from pathlib import Path; time.sleep(0.8); Path(os.environ['FAKE_CHILD_MARKER']).write_text('survived')"])
    time.sleep(30)
if mode == "exit":
    raise SystemExit(0)
positive, _, negative = next(arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--gtest_filter=")).partition("-")
filters = positive.split(":")
selected = sorted(name for name in json.loads(os.environ["FAKE_TEST_NAMES"])
                  if any(fnmatch.fnmatchcase(name, pattern) for pattern in filters)
                  and not any(fnmatch.fnmatchcase(name, pattern) for pattern in negative.split(":")))
destination = next(arg.split("json:", 1)[1] for arg in sys.argv if arg.startswith("--gtest_output="))
cases = []
for index, name in enumerate(selected):
    case = {"name": name.split(".", 1)[1], "status": "RUN", "result": "COMPLETED"}
    if mode == "skip":
        case["result"] = "SKIPPED"
    if mode == "fail" and index == 0:
        case["failures"] = [{"failure": "expected failure"}]
    if mode == "fail" and index > 0 and "--gtest_fail_fast=1" in sys.argv:
        case.update(status="NOTRUN", result="SUPPRESSED")
    cases.append(case)
if mode == "omit":
    cases.pop()
report = {"tests": len(selected), "failures": int(mode == "fail"), "errors": 0,
          "testsuites": [{"name": selected[0].split(".", 1)[0], "testsuite": cases}]}
Path(destination).write_text(json.dumps(report), encoding="utf-8")
raise SystemExit(int(mode == "fail"))
'''


class TestGroupRunnerTest(unittest.TestCase):
    def setUp(self):
        scratch = RUNNER.ROOT / ".temp"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="test-groups-", dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = self.root / "fixture.py"
        self.fixture.write_text(FIXTURE, encoding="utf-8")
        self.output = self.root / "output"
        self.output.mkdir()
        self.enterContext(redirect_stdout(io.StringIO()))
        self.enterContext(redirect_stderr(io.StringIO()))
        self.enterContext(mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(self.root / "job-summary.md")}))

    def entry(self, name, mode="pass", disabled=False, native=False, labels=None):
        command = [sys.executable, str(self.fixture)]
        if not native:
            command.extend(["--gtest_filter=" + name, "--gtest_also_run_disabled_tests"])
        return {"name": "Prefix." + name, "command": command, "properties": [
            {"name": "WORKING_DIRECTORY", "value": str(self.root)},
            {"name": "ENVIRONMENT", "value": ["FAKE_MODE=" + mode, "FAKE_VALUE=preserved"]},
            {"name": "DISABLED", "value": disabled}, {"name": "LABELS", "value": labels or []}]}

    def discover(self, rows):
        names = []
        for row in rows:
            for arg in row.get("command", []):
                if arg.startswith("--gtest_filter="):
                    names.append(arg.split("=", 1)[1])
                elif arg.startswith("TEST_FILTER="):
                    names.append(arg.split("=", 1)[1])
        for row in rows:
            for prop in row.get("properties", []):
                if prop["name"] == "ENVIRONMENT":
                    prop["value"].append("FAKE_TEST_NAMES=" + json.dumps(names))
        inventory = self.root / "inventory.json"
        inventory.write_text(json.dumps({"tests": rows}), encoding="utf-8")
        return RUNNER.discover(self.root, "Release", self.output,
                               ctest=[sys.executable, str(self.fixture), str(inventory)])

    def plan(self, rows=None, mode="pr", **kwargs):
        rows = rows if rows is not None else [self.entry("Fast.One"), self.entry("Fast.Two"),
                                               self.entry("Heavy.Smoke"), self.entry("Heavy.Full")]
        return RUNNER.make_plan(self.discover(rows), "editor", mode, self.output,
                                budget_seconds=20, **kwargs)

    def test_discovery_groups_suites_and_preserves_runtime_environment(self):
        plan = self.plan()
        self.assertEqual([["Fast.One", "Fast.Two"], ["Heavy.Full", "Heavy.Smoke"]],
                         [group["tests"] for group in plan["groups"]])
        result = RUNNER.run_plan(plan, self.output)
        self.assertEqual("passed", result["status"])
        log = json.loads((self.output / (plan["groups"][0]["id"] + ".log")).read_text(encoding="utf-8"))
        self.assertEqual(str(RUNNER.ROOT), log["cwd"])
        self.assertEqual("preserved", log["env"])

    def test_discovery_keeps_stderr_diagnostics_separate_from_json(self):
        with mock.patch.dict(os.environ, {"FAKE_DISCOVERY_DIAGNOSTIC": "Qt offscreen diagnostic"}):
            self.discover([self.entry("Fast.One")])
        self.assertEqual("Qt offscreen diagnostic\n",
                         (self.output / "discovery.stderr.log").read_text(encoding="utf-8"))
        self.assertEqual(["Prefix.Fast.One"],
                         [test["name"] for test in json.loads(
                             (self.output / "discovery.log").read_text(encoding="utf-8"))["tests"]])

    def test_pr_excludes_daily_and_keeps_qualification_out(self):
        rows = [self.entry("Daily.One", labels=["harken-daily"]), self.entry("Fast.One"),
                self.entry("Qual.One", labels=["harken-qualification"])]
        plan = self.plan(rows, exclude_labels=("harken-daily",))
        statuses = {row["test"]: row["status"] for row in plan["inventory"]}
        reasons = {row["test"]: row["selection_reason"] for row in plan["inventory"]}
        self.assertEqual("not-selected", statuses["Daily.One"])
        self.assertEqual("qualification", statuses["Qual.One"])
        self.assertEqual("excluded-label", reasons["Daily.One"])
        self.assertEqual(["Fast.One"], plan["groups"][0]["tests"])
        self.assertIn("--gtest_fail_fast=1", plan["groups"][0]["command"])

    def test_daily_selects_only_daily_labels(self):
        rows = [self.entry("Daily.One", labels=["harken-daily"]), self.entry("Fast.One"),
                self.entry("Qual.One", labels=["harken-qualification"])]
        plan = self.plan(rows, mode="full", include_labels=("harken-daily",))
        self.assertEqual([["Daily.One"]], [group["tests"] for group in plan["groups"]])
        statuses = {row["test"]: row["status"] for row in plan["inventory"]}
        self.assertEqual("not-selected", statuses["Fast.One"])
        self.assertEqual("qualification", statuses["Qual.One"])
        self.assertNotIn("--gtest_fail_fast=1", plan["groups"][0]["command"])

    def test_qualification_mode_uses_native_label_only(self):
        rows = [self.entry("Daily.One", labels=["harken-daily"]), self.entry("Fast.One"),
                self.entry("Qual.One", labels=["harken-qualification"])]
        plan = self.plan(rows, mode="qualification")
        self.assertEqual([["Qual.One"]], [group["tests"] for group in plan["groups"]])
        self.assertEqual([], plan["qualification"])
        self.assertEqual({"Daily.One", "Fast.One"},
                         {row["test"] for row in plan["inventory"] if row["status"] == "not-selected"})

    def test_label_filters_are_exact_and_repeatable(self):
        rows = [self.entry("Case.One", labels=["alpha"]), self.entry("Case.Two", labels=["beta"]),
                self.entry("Case.Three", labels=["alpha", "beta"])]
        plan = self.plan(rows, mode="full", include_labels=("alpha", "missing"), exclude_labels=("beta",))
        self.assertEqual([["Case.One"]], [group["tests"] for group in plan["groups"]])
        self.assertEqual(["alpha", "missing"], plan["include_labels"])
        self.assertEqual(["beta"], plan["exclude_labels"])

    def test_partial_same_fixture_labels_use_exact_filter(self):
        rows = [self.entry("Shared.One", labels=["harken-daily"]),
                self.entry("Shared.Two", labels=["regular"])]
        plan = self.plan(rows, mode="full", include_labels=("harken-daily",))
        self.assertEqual(["Shared.One"], plan["groups"][0]["tests"])
        self.assertIn("--gtest_filter=Shared.One", plan["groups"][0]["command"])

    def test_same_fixture_daily_and_qualification_cases_stay_in_their_selected_runs(self):
        entries = self.discover([
            self.entry("Shared.Regular"),
            self.entry("Shared.Daily", labels=["harken-daily"]),
            self.entry("Shared.Qualification", labels=["harken-qualification"])])
        for title, mode, included, excluded, expected in (
                ("pr", "pr", (), ("harken-daily",), ["Shared.Regular"]),
                ("daily", "full", ("harken-daily",), (), ["Shared.Daily"]),
                ("qualification", "qualification", (), (), ["Shared.Qualification"]),
                ("full", "full", (), (), ["Shared.Daily", "Shared.Regular"])):
            with self.subTest(mode=title):
                output = self.root / title
                output.mkdir()
                plan = RUNNER.make_plan(entries, "editor", mode, output, budget_seconds=20,
                                        include_labels=included, exclude_labels=excluded)
                result = RUNNER.run_plan(plan, output)
                self.assertEqual("passed", result["status"], result)
                self.assertEqual(expected, result["groups"][0]["tests"])

    def test_daily_excludes_source_checks_and_missing_label_fails(self):
        rows = [self.entry("Daily.One", labels=["harken-daily"]),
                self.entry("Source", native=True, labels=["harken-source-contract"])]
        plan = self.plan(rows, mode="full", include_labels=("harken-daily",))
        self.assertEqual("not-selected", plan["inventory"][1]["status"])
        self.assertEqual([], RUNNER.run_plan(plan, self.output)["prebuild"])
        with self.assertRaises(RUNNER.TestRunError):
            self.plan(rows, mode="full", include_labels=("missing-label",))

    def test_source_contracts_are_prebuild_and_disabled_entries_do_not_run(self):
        rows = [self.entry("Native", native=True), self.entry("Source", native=True,
                                                               labels=["harken-source-contract"]),
                self.entry("Disabled.One", disabled=True)]
        plan = self.plan(rows, mode="full")
        statuses = {row["test"]: row["status"] for row in plan["inventory"]}
        self.assertEqual("prebuild", statuses["Prefix.Source"])
        self.assertEqual("disabled", statuses["Disabled.One"])
        self.assertEqual(["Prefix.Native"], plan["groups"][0]["tests"])
        result = RUNNER.run_plan(plan, self.output)
        self.assertEqual(["Prefix.Source"], result["prebuild"])

    def test_disabled_gtest_cases_are_excluded_from_wildcard_fixture_runs(self):
        rows = [self.entry("Heavy.Smoke"), self.entry("Heavy.Full", disabled=True)]
        plan = self.plan(rows, mode="full")
        self.assertEqual(["Heavy.Smoke"], plan["groups"][0]["tests"])
        self.assertIn("--gtest_filter=Heavy.*-Heavy.Full", plan["groups"][0]["command"])
        self.assertEqual("passed", RUNNER.run_plan(plan, self.output)["status"])

    def test_google_test_launcher_is_supported_and_rejects_custom_executor(self):
        launcher = self.root / "GoogleTest" / "LaunchTest.cmake"
        launcher.parent.mkdir()
        launcher.write_text("", encoding="utf-8")
        rows = []
        for name in ("Fast.One", "Fast.Two"):
            row = self.entry(name)
            row["command"] = [sys.executable, "-D", "TEST_EXECUTABLE=" + sys.executable,
                              "-D", "TEST_EXECUTOR=", "-D", "TEST_FILTER=" + name,
                              "-D", "TEST_XML_OUTPUT=", "-D", "TEST_EXTRA_ARGS=", "-P", str(launcher)]
            rows.append(row)
        entries = self.discover(rows)
        self.assertTrue(all(entry["kind"] == "gtest" for entry in entries))
        rows[0]["command"][4] = "TEST_EXECUTOR=custom"
        with self.assertRaises(RUNNER.TestRunError):
            self.discover(rows)

    def test_native_ctest_unsupported_properties_and_invalid_inventory_fail(self):
        row = self.entry("Native", native=True)
        row["properties"].append({"name": "WILL_FAIL", "value": True})
        with self.assertRaises(RUNNER.TestRunError):
            self.discover([row])
        invalid = [[], [{"name": "missing"}],
                   [{"name": "x_NOT_BUILT", "command": [sys.executable],
                     "properties": [{"name": "DISABLED", "value": True}]}],
                   [{"name": "missing-executable", "command": [str(self.root / "missing.exe")]}]]
        for rows in invalid:
            with self.subTest(rows=rows), self.assertRaises(RUNNER.TestRunError):
                self.discover(rows)

    def test_artifact_and_native_contracts_run_before_regular_groups(self):
        rows = [self.entry("Regular.One"), self.entry("Artifact.One", labels=["harken-artifact-contract"]),
                self.entry("Native", native=True), self.entry("Source", native=True,
                                                               labels=["harken-source-contract"])]
        plan = self.plan(rows, mode="full")
        self.assertEqual(["Artifact", "Prefix.Native", "Regular"],
                         [group["suite"] for group in plan["groups"]])
        self.assertEqual(["Prefix.Source"], RUNNER.run_plan(plan, self.output)["prebuild"])

    def test_native_qualification_skip_fails(self):
        row = self.entry("Native", native=True, labels=["harken-qualification"])
        row["properties"].append({"name": "SKIP_RETURN_CODE", "value": 0})
        plan = self.plan([row], mode="qualification")
        result = RUNNER.run_plan(plan, self.output)
        self.assertEqual("failed", result["status"])

    def test_fail_fast_report_retains_suppressed_cases(self):
        plan = self.plan([self.entry("Fast.One", "fail"), self.entry("Fast.Two", "fail")])
        result = RUNNER.run_plan(plan, self.output)
        self.assertEqual("failed", result["status"])
        self.assertEqual(["Fast.Two"], result["groups"][0]["not_run"])

    def test_partial_group_command_length_is_rejected_without_splitting_fixture(self):
        first = self.discover([self.entry("Huge.One")])[0]
        entries = []
        for index in range(600):
            row = copy.deepcopy(first)
            row["name"] = row["test"] = "Huge." + "LongName" * 10 + str(index)
            row["command"] = [sys.executable, str(self.fixture), "--gtest_filter=" + row["test"]]
            row["properties"]["LABELS"] = ["partial" if index < 599 else "other"]
            entries.append(row)
        with self.assertRaises(RUNNER.TestRunError):
            RUNNER.make_plan(entries, "editor", "pr", self.output, budget_seconds=20,
                             include_labels=("partial",))

    def test_numeric_validation_rejects_non_positive_and_non_finite_values(self):
        entries = self.discover([self.entry("Fast.One")])
        for kwargs in ({"budget_seconds": 0}, {"budget_seconds": float("inf")},
                       {"budget_seconds": 20, "group_timeout_seconds": 0}):
            with self.subTest(kwargs=kwargs), self.assertRaises(RUNNER.TestRunError):
                RUNNER.make_plan(entries, "editor", "full", self.output, **kwargs)

    def test_no_policy_files_are_needed_by_plan_only(self):
        entries = self.discover([self.entry("Fast.One")])
        output = self.root / "plan-only"
        with mock.patch.object(RUNNER, "execution_context", return_value={}), \
                mock.patch.object(RUNNER, "discover", return_value=entries), \
                mock.patch.object(RUNNER, "run_plan") as run:
            result = RUNNER.main(["--build-dir", str(self.root), "--config", "Release", "--product", "editor",
                                  "--mode", "full", "--budget-seconds", "20", "--output-dir", str(output),
                                  "--plan-only"])
        self.assertEqual(0, result)
        plan = json.loads((output / "plan.json").read_text(encoding="utf-8"))
        self.assertNotIn("select_all", plan)
        self.assertNotIn("changed_paths", plan)
        self.assertNotIn("policy", json.dumps(plan))
        run.assert_not_called()

    def test_main_requires_positive_finite_budget(self):
        for value in (None, "0", "inf"):
            argv = ["--build-dir", str(self.root), "--config", "Release", "--product", "editor",
                    "--mode", "full", "--output-dir", str(self.root / ("bad-" + str(value)))]
            if value is not None:
                argv.extend(["--budget-seconds", value])
            with self.subTest(value=value), self.assertRaises(SystemExit) as raised:
                RUNNER.main(argv)
            self.assertEqual(2, raised.exception.code)

    def test_existing_output_directory_is_rejected(self):
        (self.output / "stale.gtest.json").write_text("{}", encoding="utf-8")
        with self.assertRaises(SystemExit) as raised:
            RUNNER.main(["--build-dir", str(self.root), "--config", "Release", "--product", "editor",
                         "--mode", "pr", "--budget-seconds", "20", "--output-dir", str(self.output)])
        self.assertEqual(2, raised.exception.code)

    def test_successful_process_requires_complete_matching_report(self):
        for mode in ("omit", "exit"):
            with self.subTest(mode=mode):
                plan = self.plan([self.entry("Fast.One", mode), self.entry("Heavy.Smoke")])
                result = RUNNER.run_plan(plan, self.output)
                self.assertEqual("failed", result["status"])

    def test_pr_fail_fast_and_full_continues_after_failure(self):
        pr = self.plan([self.entry("Fast.One", "fail"), self.entry("Heavy.Smoke")])
        result = RUNNER.run_plan(pr, self.output)
        self.assertEqual("not-run", result["groups"][1]["status"])
        full = self.plan([self.entry("Fast.One", "fail"), self.entry("Heavy.Smoke", "skip")], mode="full")
        result = RUNNER.run_plan(full, self.output)
        self.assertEqual("failed", result["status"])
        self.assertEqual("passed", result["groups"][1]["status"],
                         result)

    def test_failure_evidence_survives_logs_unrepresentable_in_stdout_encoding(self):
        entries = self.discover([self.entry("Fast.One", "fail"), self.entry("Heavy.Smoke")])
        output = self.root / "encoded-output"
        buffer = io.BytesIO()
        with io.TextIOWrapper(buffer, encoding="gbk") as stdout, redirect_stdout(stdout), \
                mock.patch.object(RUNNER, "execution_context", return_value={}), \
                mock.patch.object(RUNNER, "discover", return_value=entries):
            result = RUNNER.main(["--build-dir", str(self.root), "--config", "Release", "--product", "editor",
                                  "--mode", "pr", "--budget-seconds", "20", "--output-dir", str(output)])
            stdout.flush()
            self.assertIn(b"\\ufffd", buffer.getvalue())
        self.assertEqual(1, result)
        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual("failed", summary["status"])
        self.assertEqual(["Fast.One"], summary["groups"][0]["failed"])
        self.assertEqual("not-run", summary["groups"][1]["status"])
        self.assertIn(b"\xff", (output / (summary["first_failure"]["group"] + ".log")).read_bytes())

    def test_qualification_skip_fails(self):
        plan = self.plan([self.entry("Qual.One", "skip", labels=["harken-qualification"]),
                          self.entry("Fast.One")], mode="qualification")
        result = RUNNER.run_plan(plan, self.output)
        self.assertEqual("failed", result["status"])
        self.assertEqual(["Qual.One"], result["skipped"])

    def test_timeout_kills_process_tree(self):
        row = self.entry("Fast.One", "hang")
        marker = self.root / "child-survived"
        row["properties"][1]["value"].append("FAKE_CHILD_MARKER=" + str(marker))
        plan = self.plan([row, self.entry("Heavy.Smoke")])
        plan["groups"][0]["timeout_seconds"] = 0.3
        result = RUNNER.run_plan(plan, self.output)
        self.assertTrue(result["groups"][0]["timed_out"])
        self.assertEqual("failed", result["status"])
        time.sleep(0.8)
        self.assertFalse(marker.exists())

    def test_exceeding_budget_fails_and_persists_the_slowest_groups(self):
        plan = self.plan()
        plan["budget_seconds"] = 0.001
        result = RUNNER.run_plan(plan, self.output)
        self.assertTrue(all(group["status"] == "passed" for group in result["groups"]))
        self.assertTrue(result["over_budget"])
        self.assertEqual("failed", result["status"])
        self.assertEqual([group["id"] for group in plan["groups"] if
                          next(item for item in result["groups"] if item["id"] == group["id"]).get("seconds")],
                         sorted(item["group"] for item in result["slowest_groups"]))
        summary = json.loads((self.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(result["slowest_groups"], summary["slowest_groups"])
        report = (self.root / "job-summary.md").read_text(encoding="utf-8")
        self.assertIn("Slowest groups:", report)
        for entry in result["slowest_groups"]:
            self.assertIn(f"`{entry['group']}`: {entry['seconds']}s", report)


if __name__ == "__main__":
    unittest.main()
