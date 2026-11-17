#!/usr/bin/env python3
import argparse
import datetime
import importlib
import json
import platform
import shutil
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from drivers import engine  # noqa: E402
from drivers.studio_cli import StudioCli  # noqa: E402

SCENARIOS = ("exported_packs_sound", "value_domain_limits", "preview_matches_export", "remote_connection",
             "integration_consumer", "integration_unity")
PASSED, FAILED, SKIPPED = "passed", "failed", "skipped"


class Skip(Exception):
    pass


class Context:
    def __init__(self, args, capabilities: dict, work: Path):
        self.args = args
        self.capabilities = capabilities
        self.work = work
        self.python = sys.executable
        self.engine_install = args.engine_install
        self.details = {}

    def studio(self) -> StudioCli:
        return StudioCli(self.args.studio_cli, self.work)

    def require(self, *names: str) -> None:
        missing = [f"{name}: {self.capabilities[name]['reason']}" for name in names
                   if not self.capabilities[name]["available"]]
        if missing:
            raise Skip("; ".join(missing))

    def engine(self, plan: dict) -> dict:
        plan = {"install": str(self.engine_install), "sample_rate": 48000, "block_frames": 256, **plan}
        return engine.run_session(self.python, plan, self.work, self.args.timeout)


def probe_capabilities(args) -> dict:
    found = {}

    def mark(name, available, reason="", **facts):
        found[name] = {"available": available, "reason": reason, **facts}

    if args.studio_cli is None:
        mark("studio-cli", False, "no --studio-cli given")
    elif not args.studio_cli.is_file():
        mark("studio-cli", False, f"{args.studio_cli} does not exist")
    else:
        try:
            mark("studio-cli", True, version=StudioCli(args.studio_cli, args.work).version())
        except Exception as error:
            mark("studio-cli", False, f"version query failed: {error}")
    for edition in engine.EDITIONS:
        name = f"engine-{edition}"
        if args.engine_install is None:
            mark(name, False, "no --engine-install given")
            continue
        library = args.engine_install / engine.library_name(edition)
        manifest = engine.manifest_path(args.engine_install)
        if not library.is_file() or not manifest.is_file():
            mark(name, False, f"{library.name} or ABI manifest missing under {args.engine_install}")
        else:
            mark(name, True, library=str(library))
    if args.integration_package is None:
        mark("integration-package", False, "no --integration-package given")
    elif not (args.integration_package / "package.json").is_file():
        mark("integration-package", False, f"{args.integration_package} has no package.json")
    else:
        mark("integration-package", True, package=str(args.integration_package))
    for name, executable in (("unity", args.unity), ("dotnet", args.dotnet)):
        if executable is None:
            mark(name, False, f"no --{name} given and none found")
        elif not executable.is_file():
            mark(name, False, f"{executable} does not exist")
        else:
            mark(name, True, executable=str(executable))
    return found


def run_scenario(name: str, args, capabilities: dict, root: Path) -> dict:
    module = importlib.import_module(f"scenarios.{name}")
    work = root / name
    work.mkdir(parents=True)
    context = Context(args, capabilities, work)
    started = time.monotonic()
    try:
        module.run(context)
        status, reason = PASSED, ""
    except Skip as skip:
        status, reason = SKIPPED, str(skip)
    except Exception as error:
        status, reason = FAILED, f"{type(error).__name__}: {error}"
        (work / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
    return {"scenario": name, "status": status, "reason": reason, "seconds": round(time.monotonic() - started, 1),
            "work": str(work), "details": context.details}


def summarize(report: dict) -> str:
    lines = [f"crossproduct-test {report['started']}  {report['host']['platform']}", ""]
    for entry in report["scenarios"]:
        line = f"  {entry['status'].upper():8} {entry['scenario']}  ({entry['seconds']}s)"
        if entry["reason"]:
            line += f"\n           {entry['reason']}"
        lines.append(line)
    counts = {status: sum(1 for e in report["scenarios"] if e["status"] == status) for status in (PASSED, FAILED, SKIPPED)}
    lines += ["", f"  passed {counts[PASSED]}  failed {counts[FAILED]}  skipped {counts[SKIPPED]}"]
    if report["required_but_skipped"]:
        lines.append(f"  REQUIRED CAPABILITY MISSING: {', '.join(report['required_but_skipped'])}")
    if counts[SKIPPED]:
        lines.append("  Skipped scenarios did not run and are not evidence of correctness.")
    lines.append(f"  verdict: {report['verdict']}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Cross-product verification driven only by built products.")
    parser.add_argument("--engine-install", type=Path, help="cmake --install prefix of HarkenEngine")
    parser.add_argument("--studio-cli", type=Path, help="HarkenStudioCLI executable")
    parser.add_argument("--work-dir", type=Path, default=HERE.parents[1] / ".temp" / "crossproduct-test")
    parser.add_argument("--scenario", action="append", choices=SCENARIOS)
    parser.add_argument("--require", action="append", default=[],
                        help="capability this environment must provide; missing it fails instead of skipping")
    parser.add_argument("--integration-package", type=Path,
                        help="Unity Integration Package root (package.json); Unity scenarios need its native plugins")
    parser.add_argument("--unity", type=Path, help="Unity Editor executable")
    parser.add_argument("--dotnet", type=Path, help="dotnet executable (defaults to the one on PATH)")
    parser.add_argument("--timeout", type=float, default=300, help="per external process, seconds")
    parser.add_argument("--unity-timeout", type=float, default=1800, help="per Unity test run, seconds")
    args = parser.parse_args()
    if args.dotnet is None and shutil.which("dotnet"):
        args.dotnet = Path(shutil.which("dotnet"))
    for name in ("engine_install", "studio_cli", "integration_package", "unity", "dotnet"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())

    started = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    root = args.work_dir.resolve() / started
    root.mkdir(parents=True)
    args.work = root
    capabilities = probe_capabilities(args)
    unknown = [name for name in args.require if name not in capabilities]
    if unknown:
        parser.error(f"unknown capability {', '.join(unknown)}; known: {', '.join(capabilities)}")
    required_missing = [name for name in args.require if not capabilities[name]["available"]]

    scenarios = [run_scenario(name, args, capabilities, root) for name in (args.scenario or SCENARIOS)]
    if any(entry["status"] == FAILED for entry in scenarios) or required_missing:
        verdict = "fail"
    elif any(entry["status"] == SKIPPED for entry in scenarios):
        verdict = "incomplete"
    else:
        verdict = "pass"
    report = {
        "started": started,
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "inputs": {"engine_install": str(args.engine_install), "studio_cli": str(args.studio_cli),
                   "integration_package": str(args.integration_package), "unity": str(args.unity)},
        "capabilities": capabilities,
        "required": args.require,
        "required_but_skipped": required_missing,
        "scenarios": scenarios,
        "verdict": verdict,
    }
    (root / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    summary = summarize(report)
    (root / "summary.txt").write_text(summary + "\n", encoding="utf-8")
    print(summary)
    print(f"\n  report: {root / 'report.json'}")
    return 1 if verdict == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())
