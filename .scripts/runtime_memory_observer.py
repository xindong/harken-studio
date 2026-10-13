#!/usr/bin/env python3
"""Observe a qualified Runtime benchmark process without changing Runtime."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


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


def memory_values(process_handle: int) -> dict[str, int]:
    if os.name != "nt":
        raise OSError("Runtime memory qualification requires Windows")
    from ctypes import wintypes

    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("page_fault_count", wintypes.DWORD),
            ("peak_working_set_size", ctypes.c_size_t),
            ("working_set_size", ctypes.c_size_t),
            ("quota_peak_paged_pool_usage", ctypes.c_size_t),
            ("quota_paged_pool_usage", ctypes.c_size_t),
            ("quota_peak_non_paged_pool_usage", ctypes.c_size_t),
            ("quota_non_paged_pool_usage", ctypes.c_size_t),
            ("pagefile_usage", ctypes.c_size_t),
            ("peak_pagefile_usage", ctypes.c_size_t),
            ("private_usage", ctypes.c_size_t),
        ]

    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(ProcessMemoryCountersEx),
        wintypes.DWORD,
    ]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    counters = ProcessMemoryCountersEx()
    counters.cb = ctypes.sizeof(counters)
    if not psapi.GetProcessMemoryInfo(
        wintypes.HANDLE(process_handle),
        ctypes.byref(counters),
        counters.cb,
    ):
        raise OSError(
            ctypes.get_last_error(),
            "GetProcessMemoryInfo failed",
        )
    return {
        "private_bytes": int(counters.private_usage),
        "working_set_bytes": int(counters.working_set_size),
    }


def parse_runtime_output(path: Path) -> tuple[dict[str, int], dict[str, Any]]:
    arena_high_water = 0
    arena_overflow = 0
    capacities: dict[str, Any] = {}
    layouts: set[str] = set()
    measured_rows = 0
    declared_measured_rows: int | None = None
    complete_rows = 0
    for line in path.read_text(
        encoding="utf-8",
        errors="replace",
    ).splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(row, dict):
            continue
        if row.get("benchmark") != "phase1":
            continue
        status = row.get("status")
        if status == "complete":
            complete_rows += 1
            value = row.get("measured_rows")
            if not isinstance(value, int) or isinstance(value, bool):
                raise ValueError(
                    "phase1 completion row has invalid measured_rows"
                )
            declared_measured_rows = value
            continue
        if status != "measured":
            continue
        measured_rows += 1
        if isinstance(row.get("layout"), str):
            layouts.add(row["layout"])
        arena_high_water = max(
            arena_high_water,
            int(row.get("arena_high_water_bytes", 0)),
        )
        arena_overflow = max(
            arena_overflow,
            int(row.get("arena_overflow_count", 0)),
        )
        for key in (
            "sample_rate",
            "block_frames",
            "voices",
            "buses",
            "workers",
            "channels",
            "arena_bytes",
        ):
            if key in row:
                value = row[key]
                if (
                    not isinstance(value, (int, float))
                    or isinstance(value, bool)
                ):
                    raise ValueError(
                        f"phase1 measured row has invalid {key}"
                    )
                capacities[key] = max(
                    int(value),
                    int(capacities.get(key, 0)),
                )
        for key in (
            "voice_high_water",
            "gain_pool_high_water",
            "limiter_pool_high_water",
        ):
            if key in row:
                if (
                    not isinstance(row[key], (int, float))
                    or isinstance(row[key], bool)
                ):
                    raise ValueError(
                        f"phase1 measured row has invalid {key}"
                    )
                capacities[key] = max(
                    int(row[key]),
                    int(capacities.get(key, 0)),
                )
    if complete_rows != 1:
        raise ValueError(
            "phase1 output must contain exactly one completion row"
        )
    if measured_rows == 0 or declared_measured_rows != measured_rows:
        raise ValueError(
            "phase1 measured row count does not match completion row"
        )
    if layouts:
        capacities["layouts"] = sorted(layouts)
    capacities["measured_rows"] = measured_rows
    return (
        {
            "high_water_bytes": arena_high_water,
            "overflow_count": arena_overflow,
        },
        capacities,
    )


def static_allocation_inventory(
    capacities: dict[str, Any],
) -> list[dict[str, Any]]:
    return [
        {
            "name": "BlockArena",
            "status": "configured",
            "bytes": 1 << 24,
            "source": "phase1 kPhase1ArenaBytes",
        },
        {
            "name": "VoicePool",
            "status": "capacity_known_bytes_unresolved",
            "bytes": None,
            "capacity": capacities.get("voices"),
            "source": "phase1 JSON rows",
        },
        {
            "name": "VoiceLedger arrays",
            "status": "unresolved_in_m1",
            "bytes": None,
            "source": "M2 internal allocation ledger",
        },
        {
            "name": "Effect pools and delay lines",
            "status": "capacity_partial_bytes_unresolved",
            "bytes": None,
            "capacity": {
                "gain_high_water": capacities.get(
                    "gain_pool_high_water"
                ),
                "limiter_high_water": capacities.get(
                    "limiter_pool_high_water"
                ),
            },
            "source": "phase1 JSON rows and M2 ledger",
        },
        {
            "name": "Streaming rings and staging",
            "status": "unresolved_in_m1",
            "bytes": None,
            "source": "M2 internal allocation ledger",
        },
        {
            "name": "Bus storage",
            "status": "unresolved_in_m1",
            "bytes": None,
            "source": "M2 internal allocation ledger",
        },
        {
            "name": "Command and notification queues",
            "status": "capacity_partial_bytes_unresolved",
            "bytes": None,
            "capacity": {"command_queue_depth": 512},
            "source": "phase1 RunPhase1Case and M2 ledger",
        },
        {
            "name": "Topology plans",
            "status": "unresolved_in_m1",
            "bytes": None,
            "source": "M2 internal allocation ledger",
        },
        {
            "name": "Bindings, meter ring, and dynamic routes",
            "status": "unresolved_in_m1",
            "bytes": None,
            "source": "M2 internal allocation ledger",
        },
    ]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--run-root", required=True)
    result.add_argument("--commit", required=True)
    result.add_argument("--binary", required=True)
    result.add_argument("--report", required=True)
    result.add_argument("--stdout", required=True)
    result.add_argument("--stderr", required=True)
    result.add_argument("--sample-ms", type=float, default=1.0)
    result.add_argument("command", nargs=argparse.REMAINDER)
    return result


def main() -> int:
    args = parser().parse_args()
    run_root = Path(args.run_root).resolve()
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
            f"memory observer requires qualified binary: {expected_binary}"
        )
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if args.sample_ms <= 0.0 or args.sample_ms > 100.0:
        raise ValueError("sample-ms must be in (0, 100]")
    output_paths = (report_path, stdout_path, stderr_path)
    if len(set(output_paths)) != len(output_paths):
        raise ValueError("memory evidence paths must be distinct")
    if any(path.exists() for path in output_paths):
        raise ValueError("memory evidence output already exists")

    child_args = list(args.command)
    if child_args and child_args[0] == "--":
        child_args = child_args[1:]
    if child_args != ["--filter", "phase1"]:
        raise ValueError(
            "memory observer requires child args: --filter phase1"
        )
    argv = [str(binary), *child_args]
    binary_sha256 = sha256_file(binary)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)

    samples = 0
    initial_private = 0
    initial_working_set = 0
    peak_private = 0
    peak_working_set = 0
    started = time.perf_counter()
    with stdout_path.open("wb") as stdout_stream, stderr_path.open(
        "wb"
    ) as stderr_stream:
        child = subprocess.Popen(
            argv,
            cwd=binary.parent,
            stdout=stdout_stream,
            stderr=stderr_stream,
        )
        while True:
            try:
                values = memory_values(int(child._handle))
                if samples == 0:
                    initial_private = values["private_bytes"]
                    initial_working_set = values["working_set_bytes"]
                peak_private = max(
                    peak_private,
                    values["private_bytes"],
                )
                peak_working_set = max(
                    peak_working_set,
                    values["working_set_bytes"],
                )
                samples += 1
            except OSError:
                # Sampling may race process exit; poll below owns termination.
                pass
            if child.poll() is not None:
                break
            time.sleep(args.sample_ms / 1000.0)
        exit_code = int(child.wait())
    duration = time.perf_counter() - started

    arena, capacities = parse_runtime_output(stdout_path)
    post_binary_sha256 = sha256_file(binary)
    report = {
        "schema_version": 1,
        "claim_id": "M1-memory",
        "commit_sha": args.commit,
        "binary": {
            "path": str(binary),
            "sha256": binary_sha256,
            "post_sha256": post_binary_sha256,
            "unchanged": post_binary_sha256 == binary_sha256,
            "argv": argv,
            "cwd": str(binary.parent),
            "exit_code": exit_code,
        },
        "observer": {
            "path": str(Path(__file__).resolve()),
            "sha256": sha256_file(Path(__file__).resolve()),
            "memory_api": "GetProcessMemoryInfo",
            "sample_interval_ms": args.sample_ms,
            "sample_count": samples,
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
        "config": {
            "backend": "NullBackend",
            "sample_rate": capacities.get("sample_rate"),
            "block_frames": capacities.get("block_frames"),
            "layouts": capacities.get("layouts"),
            "scenario": "phase1",
        },
        "process_bytes": {
            "initial_observation": initial_private,
            "peak": peak_private,
            "after_exit": 0,
        },
        "working_set_bytes": {
            "initial_observation": initial_working_set,
            "peak": peak_working_set,
            "after_exit": 0,
        },
        "runtime_phase_bytes": {
            "after_init": None,
            "after_warmup": None,
            "high_water": peak_private,
            "after_term": None,
        },
        "arena": arena,
        "capacities": capacities,
        "static_allocations": static_allocation_inventory(capacities),
        "limitations": [
            (
                "External process sampling cannot separate after-Init, "
                "after-warm-up, and after-Term private bytes."
            ),
            (
                "This M1 report is a partial process baseline; M2 "
                "instrumentation owns phase-resolved allocation accounting."
            ),
        ],
    }
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(report_path)
    if post_binary_sha256 != binary_sha256:
        return 2
    return exit_code


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, OSError, ValueError) as error:
        print(f"runtime memory observer error: {error}", file=sys.stderr)
        raise SystemExit(2)
