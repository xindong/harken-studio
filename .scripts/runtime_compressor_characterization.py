#!/usr/bin/env python3
"""Capture and validate the complete M1 Compressor characterization."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


EXPECTED_CHILD_ARGS = ["--filter", "compressor-matrix"]
EXPECTED_SCENARIO_WORKERS = {
    "voice-self-64": (0,),
    "bus-routed-16x4": (0,),
    "mixed-worst": (0, 1, 2, 4),
    "inactive-route-platform": (0,),
}
EXPECTED_MISSING_EVIDENCE = [
    "isolated",
    "bus-worker-0-paired",
    "external-phase1-b1-f1-f2-b2",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_under(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    resolved.relative_to(parent.resolve())
    return resolved


def json_rows(path: Path) -> list[dict[str, Any]]:
    rows = []
    for number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(),
        start=1,
    ):
        if not line.strip():
            continue
        value = json.loads(line)
        if not isinstance(value, dict):
            raise ValueError(f"line {number} is not a JSON object")
        rows.append(value)
    return rows


def finite_numbers(value: Any) -> bool:
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(finite_numbers(item) for item in value.values())
    if isinstance(value, list):
        return all(finite_numbers(item) for item in value)
    return True


def non_bool_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def finite_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def validate_process_contract(
    exit_code: int,
    stderr_size: int,
) -> list[str]:
    issues = []
    if exit_code != 1:
        issues.append(
            f"child exit code is {exit_code}, expected design value 1"
        )
    if stderr_size != 0:
        issues.append("child stderr is not empty")
    return issues


def validate_rows(
    rows: list[dict[str, Any]],
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            issues.append(message)

    def require_contract(
        row: dict[str, Any],
        scoped_applicable: bool,
        scoped_passed: bool,
        miss_count: int,
        context: str,
    ) -> None:
        expected = {
            "performance_contract_vintage": 2,
            "performance_gate_scope": "finite-cost-regression-only",
            "scoped_cost_gate_applicable": scoped_applicable,
            "scoped_cost_gate_passed": scoped_passed,
            "deadline_misses_diagnostic_only": True,
            "deadline_miss_hard_fail_applicable": False,
            "deadline_reliability_claimed": False,
            "deadline_observation_status": (
                "clear" if miss_count == 0 else "unclassified"
            ),
            "scoped_gate_meaning": (
                "finite-cost-regression-only-not-hard-realtime-"
                "or-glitch-free"
            ),
        }
        for field, value in expected.items():
            require(row.get(field) == value, f"{context}: {field}")

    require(bool(rows), "stdout contains no JSON evidence")
    require(
        all(row.get("benchmark") == "compressor-matrix" for row in rows),
        "unexpected benchmark row",
    )
    require(
        all(finite_numbers(row) for row in rows),
        "JSON evidence contains a non-finite number",
    )
    matrix_rows = [row for row in rows if row.get("status") == "matrix"]
    measured_rows = [
        row for row in rows if row.get("status") == "measured"
    ]
    complete_rows = [
        row for row in rows if row.get("status") == "complete"
    ]
    require(
        len(rows)
        == len(matrix_rows) + len(measured_rows) + len(complete_rows),
        "unexpected status row",
    )
    require(len(matrix_rows) == 1, "expected exactly one matrix row")
    require(len(complete_rows) == 1, "expected exactly one complete row")
    require(len(measured_rows) == 35, "expected exactly 35 measured rows")

    if matrix_rows:
        matrix = matrix_rows[0]
        require_contract(matrix, False, False, 0, "matrix")
        require(matrix.get("sample_rate") == 48000, "matrix sample rate")
        require(matrix.get("block_frames") == 256, "matrix block frames")
        require(matrix.get("channels") == 8, "matrix channel count")
        require(matrix.get("warmup_blocks") == 2000, "matrix warmup")
        require(
            matrix.get("measured_blocks") == 10000,
            "matrix measured blocks",
        )
        require(matrix.get("runs") == 5, "matrix run count")
        require(
            matrix.get("workers") == [0, 1, 2, 4],
            "matrix worker set",
        )
        require(matrix.get("canonical_counts") is False, "matrix scope")
        require(matrix.get("process_pinned") is True, "matrix pinning")
        require(matrix.get("priority_set") is True, "matrix priority")
        require(
            matrix.get("expected_scoped_scenario_worker_mask")
            == "00001f11",
            "matrix expected coverage mask",
        )
        require(
            matrix.get("expected_scoped_rows") == 7,
            "matrix expected scoped rows",
        )

    expected_keys = {
        (scenario, worker, run)
        for scenario, workers in EXPECTED_SCENARIO_WORKERS.items()
        for worker in workers
        for run in range(1, 6)
    }
    observed_keys: list[tuple[Any, Any, Any]] = []
    deadline_misses = 0
    absolute_budget_failures = []
    max_callback_fraction = 0.0
    for row in measured_rows:
        key = (row.get("scenario"), row.get("workers"), row.get("run"))
        observed_keys.append(key)
        require(isinstance(key[0], str), f"{key}: scenario type")
        require(non_bool_int(key[1]), f"{key}: worker type")
        require(non_bool_int(key[2]), f"{key}: run type")
        require(row.get("build") == "release", f"{key}: release build")
        require(row.get("sample_rate") == 48000, f"{key}: sample rate")
        require(row.get("block_frames") == 256, f"{key}: block frames")
        require(row.get("channels") == 8, f"{key}: channel count")
        require(row.get("warmup_blocks") == 2000, f"{key}: warmup")
        require(
            row.get("measured_blocks") == 10000,
            f"{key}: measured blocks",
        )
        require(row.get("process_pinned") is True, f"{key}: pinning")
        require(row.get("priority_set") is True, f"{key}: priority")
        require(row.get("setup_valid") is True, f"{key}: setup")
        require(row.get("setup_error") == "none", f"{key}: setup error")
        require(row.get("output_finite") is True, f"{key}: finite output")
        require(
            row.get("nonfinite_output_sample_count") == 0,
            f"{key}: non-finite sample count",
        )
        for field in (
            "arena_overflow_count",
            "dynamic_route_refusal_count",
            "dynamic_plan_retire_overflow_count",
        ):
            require(row.get(field) == 0, f"{key}: {field}")
        expected_identity = (
            "serial-base" if row.get("workers") == 0 else "identical"
        )
        require(
            row.get("worker_identity") == expected_identity,
            f"{key}: worker identity",
        )
        timing_fields = (
            "median_us",
            "p99_us",
            "max_us",
            "deadline_us",
            "p99_budget_us",
        )
        for field in timing_fields:
            require(
                finite_number(row.get(field))
                and float(row[field]) >= 0.0,
                f"{key}: {field}",
            )
        if all(finite_number(row.get(field)) for field in timing_fields):
            require(
                float(row["median_us"])
                <= float(row["p99_us"])
                <= float(row["max_us"]),
                f"{key}: percentile order",
            )
            require(
                float(row["deadline_us"]) > 0.0,
                f"{key}: positive deadline",
            )
        miss_count = row.get("deadline_misses")
        require(
            non_bool_int(miss_count) and miss_count >= 0,
            f"{key}: deadline miss type",
        )
        if non_bool_int(miss_count) and miss_count >= 0:
            deadline_misses += miss_count
            require_contract(
                row,
                False,
                False,
                miss_count,
                str(key),
            )
        deadline = row.get("deadline_us")
        maximum = row.get("max_us")
        if isinstance(deadline, (int, float)) and deadline > 0 and isinstance(
            maximum, (int, float)
        ):
            max_callback_fraction = max(
                max_callback_fraction,
                float(maximum) / float(deadline),
            )
        if (
            row.get("absolute_callback_budget_diagnostic_applicable")
            is True
            and row.get("absolute_callback_budget_diagnostic_passed")
            is False
        ):
            absolute_budget_failures.append(
                {
                    "scenario": row.get("scenario"),
                    "workers": row.get("workers"),
                    "run": row.get("run"),
                    "p99_us": row.get("p99_us"),
                    "p99_budget_us": row.get("p99_budget_us"),
                }
            )
    require(
        len(observed_keys) == len(set(observed_keys)),
        "duplicate measured scenario/worker/run row",
    )
    require(set(observed_keys) == expected_keys, "measured coverage mismatch")

    if complete_rows:
        complete = complete_rows[0]
        require_contract(
            complete,
            False,
            False,
            deadline_misses,
            "complete",
        )
        expected_values = {
            "setup_valid": True,
            "worker_identity": True,
            "process_pinned": True,
            "priority_set": True,
            "canonical_counts": False,
            "mixed_selected": True,
            "paired_aggregate_selected": True,
            "expected_scoped_scenario_worker_mask": "00001f11",
            "observed_scoped_scenario_worker_mask": "00001f11",
            "expected_scoped_rows": 7,
            "observed_scoped_rows": 7,
            "scoped_coverage_complete": True,
            "output_finite": True,
            "nonfinite_output_sample_count": 0,
            "required_mixed_coverage_complete": True,
            "required_inactive_coverage_complete": True,
            "required_full_scenario_worker_coverage_complete": True,
            "scoped_gate_applicable": False,
            "scoped_gate_passed": False,
            "scoped_gate_status": "not-applicable-smoke",
            "inactive_external_gate_selected": True,
            "inactive_external_gate_verified": False,
            "inactive_external_gate_status": "unverified",
            "overall_runtime_gate_passed": False,
            "overall_runtime_gate_status": (
                "unverified-full-matrix-and-external-evidence-required"
            ),
            "missing_runtime_evidence": EXPECTED_MISSING_EVIDENCE,
        }
        for field, expected in expected_values.items():
            require(
                complete.get(field) == expected,
                f"complete row {field}",
            )

    return (
        issues,
        {
            "measured_rows": len(measured_rows),
            "deadline_misses": deadline_misses,
            "absolute_budget_failures": absolute_budget_failures,
            "max_callback_deadline_fraction": max_callback_fraction,
            "full_runtime_gate_status": (
                complete_rows[0].get("overall_runtime_gate_status")
                if complete_rows
                else None
            ),
        },
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--run-root", required=True)
    result.add_argument("--commit", required=True)
    result.add_argument("--binary", required=True)
    result.add_argument("--report", required=True)
    result.add_argument("--stdout", required=True)
    result.add_argument("--stderr", required=True)
    result.add_argument("command", nargs=argparse.REMAINDER)
    return result


def main() -> int:
    args = parser().parse_args()
    run_root = Path(args.run_root).resolve()
    source_root = require_under(run_root / "source", run_root)
    binary = require_under(Path(args.binary), run_root)
    report_path = require_under(Path(args.report), run_root)
    stdout_path = require_under(Path(args.stdout), run_root)
    stderr_path = require_under(Path(args.stderr), run_root)
    expected_binary = (
        run_root
        / "build"
        / "vs"
        / "bin"
        / "Release"
        / "harken_benchmarks.exe"
    ).resolve()
    if binary != expected_binary:
        raise ValueError(
            "Compressor characterization requires the qualified binary"
        )
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if not source_root.is_dir():
        raise FileNotFoundError(source_root)
    child_args = list(args.command)
    if child_args and child_args[0] == "--":
        child_args = child_args[1:]
    if child_args != EXPECTED_CHILD_ARGS:
        raise ValueError(
            "Compressor characterization requires "
            "--filter compressor-matrix"
        )
    output_paths = (report_path, stdout_path, stderr_path)
    if len(set(output_paths)) != len(output_paths):
        raise ValueError("Compressor evidence paths must be distinct")
    if any(path.exists() for path in output_paths):
        raise ValueError("Compressor evidence output already exists")
    for path in output_paths:
        path.parent.mkdir(parents=True, exist_ok=True)

    argv = [str(binary), *child_args]
    binary_sha256 = sha256_file(binary)
    started = time.perf_counter()
    with stdout_path.open("wb") as stdout_stream, stderr_path.open(
        "wb"
    ) as stderr_stream:
        child = subprocess.run(
            argv,
            cwd=source_root,
            check=False,
            stdout=stdout_stream,
            stderr=stderr_stream,
        )
    duration = time.perf_counter() - started
    issues = validate_process_contract(
        int(child.returncode),
        stderr_path.stat().st_size,
    )
    summary: dict[str, Any] = {}
    try:
        rows = json_rows(stdout_path)
        row_issues, summary = validate_rows(rows)
        issues.extend(row_issues)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        issues.append(f"invalid child stdout: {error}")
    post_binary_sha256 = sha256_file(binary)
    if post_binary_sha256 != binary_sha256:
        issues.append("qualified binary changed during characterization")

    report = {
        "schema_version": 1,
        "claim_id": "M1-compressor-characterization",
        "commit_sha": args.commit,
        "validation_passed": not issues,
        "validation_issues": issues,
        "binary": {
            "path": str(binary),
            "sha256": binary_sha256,
            "post_sha256": post_binary_sha256,
            "unchanged": post_binary_sha256 == binary_sha256,
            "argv": argv,
            "cwd": str(source_root),
            "exit_code": child.returncode,
        },
        "observer": {
            "path": str(Path(__file__).resolve()),
            "sha256": sha256_file(Path(__file__).resolve()),
            "duration_seconds": duration,
        },
        "raw_evidence": {
            "stdout": {
                "path": str(stdout_path),
                "sha256": sha256_file(stdout_path),
                "size": stdout_path.stat().st_size,
            },
            "stderr": {
                "path": str(stderr_path),
                "sha256": sha256_file(stderr_path),
                "size": stderr_path.stat().st_size,
            },
        },
        "result": summary,
        "claim_boundary": {
            "characterization_complete": not issues,
            "finite_cost_regression_certified": False,
            "hard_realtime_certified": False,
            "glitch_free_certified": False,
            "full_runtime_gate_closed": False,
        },
    }
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(report_path)
    return 0 if not issues else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, OSError, ValueError) as error:
        print(
            f"runtime compressor characterization error: {error}",
            file=sys.stderr,
        )
        raise SystemExit(2)
