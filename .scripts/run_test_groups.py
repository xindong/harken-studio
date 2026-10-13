from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]


class TestRunError(Exception):
    pass


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def run_process(command, *, cwd, env, timeout, log, stderr=subprocess.STDOUT):
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=stream,
                                   stderr=stderr, start_new_session=os.name != "nt")
        timed_out = False
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               stdout=stream, stderr=subprocess.STDOUT, timeout=20, check=False)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            code = process.wait(timeout=20)
    return {"exit_code": code, "timed_out": timed_out,
            "seconds": round(time.monotonic() - started, 3)}


def discover(build_dir, config, output_dir, ctest=None):
    log = output_dir / "discovery.log"
    with (output_dir / "discovery.stderr.log").open("w", encoding="utf-8") as stderr:
        result = run_process([*(ctest or ["ctest"]), "--test-dir", str(build_dir), "-C", config,
                              "--show-only=json-v1"], cwd=ROOT, env=os.environ.copy(),
                             timeout=120, log=log, stderr=stderr)
    if result["exit_code"] or result["timed_out"]:
        raise TestRunError("CTest discovery failed; see discovery.log and discovery.stderr.log")
    try:
        tests = json.loads(log.read_text(encoding="utf-8"))["tests"]
    except (ValueError, KeyError, TypeError) as error:
        raise TestRunError("CTest discovery did not return a test inventory") from error
    if not isinstance(tests, list) or not tests:
        raise TestRunError("CTest discovered no tests")
    entries = []
    names = set()
    for test in tests:
        name = test.get("name", "")
        command = test.get("command", [])
        if not name or name in names or "NOT_BUILT" in name or not command:
            raise TestRunError(f"Invalid, duplicate, or unbuilt CTest entry: {name!r}")
        names.add(name)
        if not isinstance(command, list) or not all(isinstance(arg, str) for arg in command):
            raise TestRunError(f"Invalid command: {name}")
        if (len(command) > 2 and command[-2] == "-P"
                and Path(command[-1]).name == "LaunchTest.cmake"
                and Path(command[-1]).parent.name == "GoogleTest"):
            options = command[1:-2]
            if len(options) != 10 or options[::2] != ["-D"] * 5:
                raise TestRunError(f"Unsupported CMake GoogleTest launcher arguments: {name}")
            definitions = dict(value.split("=", 1) for value in options[1::2] if "=" in value)
            required = {"TEST_EXECUTABLE", "TEST_EXECUTOR", "TEST_FILTER", "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"}
            if (set(definitions) != required
                    or any(definitions[key] for key in ("TEST_EXECUTOR", "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"))):
                raise TestRunError(f"Unsupported CMake GoogleTest launcher options: {name}")
            command = [definitions["TEST_EXECUTABLE"], "--gtest_filter=" + definitions["TEST_FILTER"]]
        props = {item["name"]: item["value"] for item in test.get("properties", [])}
        cwd = Path(props.get("WORKING_DIRECTORY", build_dir)).resolve()
        executable = Path(command[0])
        if not executable.is_absolute():
            local = cwd / executable
            executable = local if local.is_file() else Path(shutil.which(command[0]) or local)
        if not executable.is_file():
            raise TestRunError(f"Missing executable for {name}: {executable}")
        command = [str(executable.resolve()), *command[1:]]
        filters = [arg.split("=", 1)[1] for arg in command if arg.startswith("--gtest_filter=")]
        if len(filters) > 1:
            raise TestRunError(f"Multiple GTest filters: {name}")
        case = filters[0] if filters else name
        if filters and ("." not in case or any(char in case for char in "*?:")):
            raise TestRunError(f"CTest entry must identify one exact GTest case: {name}")
        if not filters:
            unsupported = set(props) - {"WORKING_DIRECTORY", "ENVIRONMENT", "ENVIRONMENT_MODIFICATION",
                                        "DISABLED", "TIMEOUT", "LABELS", "SKIP_RETURN_CODE",
                                        "PROCESSORS", "RESOURCE_LOCK", "RUN_SERIAL", "COST"}
            if unsupported:
                raise TestRunError(f"Unsupported native CTest properties for {name}: {', '.join(sorted(unsupported))}")
        disabled = bool(props.get("DISABLED", False))
        if filters and any(part.startswith("DISABLED_") for part in re.split(r"[./]", case)):
            disabled = True
        entries.append({"name": name, "test": case, "kind": "gtest" if filters else "native",
                        "command": command, "cwd": str(ROOT if filters else cwd),
                        "properties": props, "disabled": disabled})
    if not any(not entry["disabled"] for entry in entries):
        raise TestRunError("CTest discovered no enabled tests")
    return entries


def execution_context(build_dir, config):
    context = {"started_at_utc": datetime.now(timezone.utc).isoformat(),
               "build_dir": str(build_dir.resolve()), "config": config}
    for field, command in (("source_head_sha", ["git", "rev-parse", "HEAD"]),
                           ("source_dirty", ["git", "status", "--porcelain=v1", "-z", "--untracked-files=normal"])):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=30, check=False)
        if result.returncode:
            raise TestRunError("Cannot record source identity for test evidence")
        context[field] = bool(result.stdout) if field == "source_dirty" else result.stdout.decode("ascii").strip()
    for variable in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
        if os.environ.get(variable):
            context[variable.lower()] = os.environ[variable]
    return context


def positive_finite(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise TestRunError(f"{name} must be a positive finite number")
    return value


def _labels(entry):
    labels = entry["properties"].get("LABELS", [])
    if not isinstance(labels, list) or not all(isinstance(label, str) and label for label in labels):
        raise TestRunError(f"Invalid CTest LABELS for {entry['name']}")
    return set(labels)


def make_plan(entries, product, mode, output_dir, *, group_timeout_seconds=300, budget_seconds,
              include_labels=(), exclude_labels=()):
    group_timeout_seconds = positive_finite(group_timeout_seconds, "group_timeout_seconds")
    budget_seconds = positive_finite(budget_seconds, "budget_seconds")
    include_labels = tuple(include_labels)
    exclude_labels = tuple(exclude_labels)
    if not all(isinstance(label, str) and label for label in (*include_labels, *exclude_labels)):
        raise TestRunError("Label filters must contain non-empty strings")
    inventory = []
    grouped = {}
    all_cases = {}
    disabled_cases = {}
    for entry in entries:
        labels = _labels(entry)
        qualification = "harken-qualification" in labels
        source_contract = "harken-source-contract" in labels
        include_match = not include_labels or bool(labels.intersection(include_labels))
        exclude_match = bool(labels.intersection(exclude_labels))
        status = "selected"
        reason = "selected"
        if entry["disabled"]:
            status = "disabled"
            reason = "disabled"
        elif mode == "qualification" and not qualification:
            status = "not-selected"
            reason = "qualification-only"
        elif exclude_match:
            status = "not-selected"
            reason = "excluded-label"
        elif mode != "qualification" and qualification:
            status = "qualification"
            reason = "qualification-label"
        elif not include_match:
            status = "not-selected"
            reason = "not-included-label"
        elif source_contract:
            status = "prebuild"
            reason = "source-contract"
        inventory.append({"name": entry["name"], "test": entry["test"], "kind": entry["kind"],
                          "labels": sorted(labels), "status": status,
                          "selection_reason": reason})
        native = entry["kind"] == "native"
        command = entry["command"] if native else [arg for arg in entry["command"]
                   if not arg.startswith(("--gtest_filter=", "--gtest_output=", "--gtest_catch_exceptions=", "--gtest_fail_fast="))
                   and arg != "--gtest_also_run_disabled_tests"]
        suite = entry["name"] if native else entry["test"].split(".", 1)[0]
        phase = 0 if native or "harken-artifact-contract" in labels else 1
        props = entry["properties"]
        key = (entry["kind"], tuple(command), suite, entry["cwd"],
               json.dumps(props.get("ENVIRONMENT", []), sort_keys=True),
               json.dumps(props.get("ENVIRONMENT_MODIFICATION", []), sort_keys=True))
        if not native and not entry["disabled"]:
            all_cases.setdefault(key, set()).add(entry["test"])
        if not native and entry["disabled"]:
            disabled_cases.setdefault(key, []).append(entry["test"])
        if status != "selected":
            continue
        group = grouped.setdefault(key, {"kind": entry["kind"], "suite": suite, "command": command,
                                          "cwd": entry["cwd"], "properties": props, "tests": [],
                                          "ctest_names": [], "phase": phase,
                                          "timeout_seconds": group_timeout_seconds})
        group["phase"] = min(group["phase"], phase)
        group["tests"].append(entry["test"])
        group["ctest_names"].append(entry["name"])
        if native and props.get("TIMEOUT", 0) > 0:
            group["timeout_seconds"] = min(group["timeout_seconds"], props["TIMEOUT"])
    groups = []
    for index, (key, group) in enumerate(sorted(grouped.items(), key=lambda item: (item[1]["phase"], item[0])), 1):
        group["id"] = f"{index:04d}-{re.sub(r'[^A-Za-z0-9_.-]', '_', group['suite'])[:80]}"
        group["tests"].sort()
        if group["kind"] == "gtest":
            if set(group["tests"]) == all_cases[key]:
                test_filter = group["suite"] + ".*"
                if disabled_cases.get(key):
                    test_filter += "-" + ":".join(sorted(disabled_cases[key]))
            else:
                test_filter = ":".join(group["tests"])
            group["command"] = [*group["command"], "--gtest_filter=" + test_filter,
                                "--gtest_catch_exceptions=0",
                                "--gtest_output=json:" + str(output_dir / (group["id"] + ".gtest.json"))]
            if mode == "pr":
                group["command"].append("--gtest_fail_fast=1")
            if len(subprocess.list2cmdline(group["command"])) > 30000:
                raise TestRunError(f"Partial GTest filter for {group['suite']} exceeds the Windows command limit; select its full suite")
        groups.append(group)
    if not groups:
        raise TestRunError("Test selection contains no executable groups")
    return {"version": 1, "product": product, "mode": mode,
            "budget_seconds": budget_seconds, "group_timeout_seconds": group_timeout_seconds,
            "include_labels": list(include_labels), "exclude_labels": list(exclude_labels),
            "qualification": [row["name"] for row in inventory if row["status"] == "qualification"],
            "inventory": inventory, "groups": groups}


def group_environment(properties):
    env = os.environ.copy()
    for value in properties.get("ENVIRONMENT", []):
        name, separator, content = value.partition("=")
        if not separator:
            raise TestRunError(f"Invalid CTest environment: {value}")
        env[name] = content
    baseline = env.copy()
    for value in properties.get("ENVIRONMENT_MODIFICATION", []):
        name, _, specification = value.partition("=")
        operation, separator, content = specification.partition(":")
        if not separator:
            raise TestRunError(f"Invalid CTest environment modification: {value}")
        if operation == "reset":
            if name in baseline:
                env[name] = baseline[name]
            else:
                env.pop(name, None)
        elif operation == "set":
            env[name] = content
        elif operation == "unset":
            env.pop(name, None)
        elif operation in ("string_append", "string_prepend", "path_list_append", "path_list_prepend",
                           "cmake_list_append", "cmake_list_prepend"):
            delimiter = os.pathsep if operation.startswith("path_") else ";" if operation.startswith("cmake_") else ""
            old = env.get(name, "")
            pieces = [old, content] if operation.endswith("append") else [content, old]
            env[name] = delimiter.join(piece for piece in pieces if piece)
        else:
            raise TestRunError(f"Unsupported CTest environment operation: {operation}")
    return env


def inspect_gtest_report(path, expected):
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
        cases = [(suite["name"] + "." + case["name"], case)
                 for suite in report["testsuites"] for case in suite["testsuite"]]
        names = [name for name, _ in cases]
        if len(names) != len(expected) or set(names) != set(expected) or report["tests"] != len(expected):
            raise TestRunError("GTest report does not match selected tests")
        failed = [name for name, case in cases if case.get("failures")]
        skipped = [name for name, case in cases if case.get("result") == "SKIPPED"]
        not_run = [name for name, case in cases if case.get("status") != "RUN"
                   or case.get("result") not in ("COMPLETED", "SKIPPED")]
        if (report.get("errors", 0) or report.get("failures") != len(failed)
                or any(case.get("errors") for _, case in cases)):
            raise TestRunError("GTest report has inconsistent failure or error counts")
        if not_run and not failed:
            raise TestRunError("GTest report contains incomplete results without a failure")
        return {"failed": failed, "skipped": skipped, "not_run": not_run}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise TestRunError(f"Missing or invalid GTest report: {path.name}") from error


def run_plan(plan, output_dir):
    started = time.monotonic()
    results = []
    first_failure = None
    for group in plan["groups"]:
        result = {"id": group["id"], "tests": group["tests"]}
        if first_failure and plan["mode"] == "pr":
            result.update(status="not-run", seconds=0)
        else:
            try:
                result.update(run_process(group["command"], cwd=group["cwd"],
                                          env=group_environment(group["properties"]),
                                          timeout=group["timeout_seconds"],
                                          log=output_dir / (group["id"] + ".log")))
                result["status"] = "failed" if result["exit_code"] or result["timed_out"] else "passed"
                if group["kind"] == "gtest":
                    result.update(inspect_gtest_report(output_dir / (group["id"] + ".gtest.json"), group["tests"]))
                    if result["failed"] or result["not_run"]:
                        result["status"] = "failed"
                elif not result["timed_out"] and result["exit_code"] == group["properties"].get("SKIP_RETURN_CODE"):
                    result.update(status="skipped", skipped=group["tests"])
                if plan["mode"] == "qualification" and result.get("skipped"):
                    result.update(status="failed", error="Qualification skipped required tests: " + ", ".join(result["skipped"]))
            except (OSError, TestRunError, subprocess.SubprocessError) as error:
                result.update(status="failed", error=str(error))
            if result["status"] == "failed" and first_failure is None:
                first_failure = {"group": group["id"], "seconds": round(time.monotonic() - started, 3)}
            if result["status"] == "failed":
                print(f"::error::Test group {group['id']} failed: {result.get('error', 'see group log')}", flush=True)
                log = output_dir / (group["id"] + ".log")
                if log.is_file():
                    with log.open("rb") as stream:
                        stream.seek(max(0, log.stat().st_size - 8000))
                        print("\n".join(stream.read().decode("utf-8", errors="replace").splitlines()[-40:]), flush=True)
        write_json(output_dir / (group["id"] + ".result.json"), result)
        results.append(result)
        print(f"{result['status']}: {group['id']} ({result.get('seconds', 0)}s)", flush=True)
    seconds = round(time.monotonic() - started, 3)
    over_budget = seconds > plan["budget_seconds"]
    summary = {"version": 1, "product": plan["product"], "mode": plan["mode"],
               "execution": plan.get("execution"),
               "status": "failed" if first_failure or over_budget else "passed", "seconds": seconds,
               "budget_seconds": plan["budget_seconds"], "over_budget": over_budget,
               "first_failure": first_failure, "groups": results,
               "skipped": [name for result in results for name in result.get("skipped", [])],
               "not_run": [name for result in results for name in
                           (result["tests"] if result["status"] == "not-run" else result.get("not_run", []))],
               "disabled": [row["name"] for row in plan["inventory"] if row["status"] == "disabled"],
               "qualification": [row["name"] for row in plan["inventory"] if row["status"] == "qualification"],
               "not_selected": [row["name"] for row in plan["inventory"] if row["status"] == "not-selected"],
               "prebuild": [row["name"] for row in plan["inventory"] if row["status"] == "prebuild"]}
    if summary["over_budget"]:
        slowest = sorted((result for result in results if result.get("seconds")),
                         key=lambda result: result["seconds"], reverse=True)[:5]
        summary["slowest_groups"] = [{"group": result["id"], "seconds": result["seconds"]}
                                      for result in slowest]
        print(f"::error::Test duration {seconds}s exceeds budget {plan['budget_seconds']}s. Slowest: "
              + ", ".join(f"{result['id']} {result['seconds']}s" for result in slowest), flush=True)
    else:
        summary["slowest_groups"] = []
    write_json(output_dir / "summary.json", summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
            stream.write(f"\n### {plan['product']} tests ({plan['mode']})\n\n"
                         f"Result: {summary['status']}; {seconds}s; budget {plan['budget_seconds']}s.\n\n"
                         f"Groups: {len(results)}; not selected: {len(summary['not_selected'])}; "
                         f"separate qualification: {len(summary['qualification'])}; "
                         f"disabled: {len(summary['disabled'])}; skipped: {len(summary['skipped'])}; "
                         f"not run: {len(summary['not_run'])}.\n\n")
            if first_failure:
                stream.write(f"First failure: `{first_failure['group']}` after {first_failure['seconds']}s.\n")
            if summary["over_budget"]:
                stream.write("Slowest groups:\n" + "".join(
                    f"- `{entry['group']}`: {entry['seconds']}s\n"
                    for entry in summary["slowest_groups"]) + "\n")
            if summary["execution"]:
                context = summary["execution"]
                source_state = "dirty working tree; evidence applies to local changes" if context["source_dirty"] else "clean working tree"
                stream.write(f"\nSource: `{context['source_head_sha']}` ({source_state}).\n"
                             f"Build: `{context['build_dir']}` / `{context['config']}`.\n")
    return summary


def _positive_finite_argument(value):
    try:
        return positive_finite(float(value), "argument")
    except (TypeError, ValueError, TestRunError) as error:
        raise argparse.ArgumentTypeError("must be a positive finite number") from error


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--product", choices=("runtime", "editor", "hub"), required=True)
    parser.add_argument("--mode", choices=("pr", "full", "qualification"), required=True)
    parser.add_argument("--include-label", action="append", default=[])
    parser.add_argument("--exclude-label", action="append", default=[])
    parser.add_argument("--group-timeout-seconds", type=_positive_finite_argument, default=300)
    parser.add_argument("--budget-seconds", type=_positive_finite_argument, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--plan-only", action="store_true")
    args = parser.parse_args(argv)
    output = args.output_dir.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error("Output directory must be empty to prevent stale evidence reuse")
    output.mkdir(parents=True, exist_ok=True)
    context = None
    try:
        context = execution_context(args.build_dir, args.config)
        entries = discover(args.build_dir.resolve(), args.config, output)
        plan = make_plan(entries, args.product, args.mode, output,
                         group_timeout_seconds=args.group_timeout_seconds,
                         budget_seconds=args.budget_seconds,
                         include_labels=args.include_label,
                         exclude_labels=args.exclude_label)
        plan["execution"] = context
        write_json(output / "plan.json", plan)
        if args.plan_only:
            print(f"Planned {len(plan['groups'])} groups; see {output / 'plan.json'}")
            return 0
        return int(run_plan(plan, output)["status"] != "passed")
    except (OSError, ValueError, TestRunError, subprocess.SubprocessError) as error:
        write_json(output / "summary.json", {"status": "failed", "error": str(error), "execution": context})
        print(f"Test runner failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
