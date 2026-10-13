#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


CASES = ((1000, 64), (10000, 256), (50000, 1024))
RUNTIME_TIMING_FIELDS = (
    "export_us",
    "inspect_us",
    "load_init_us",
    "content_cold_us",
    "content_warm_us",
    "replace_us",
    "unload_us",
    "lookup_loaded_us",
    "lookup_unloaded_us",
    "post_loaded_us",
    "post_unloaded_us",
)
STUDIO_TIMING_FIELDS = (
    "rebuildMs",
    "wordSearchMs",
    "phraseSearchMs",
    "trigramSearchMs",
    "incrementalIndexMs",
    "boundedBatchMs",
)
FIXTURE_FILES = ("Init.pack", "Content.pack", "ContentNext.pack")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_hash(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def git_output(source_root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=source_root, text=True, encoding="utf-8"
    ).strip()


def source_dirty_identity(source_root: Path) -> tuple[str, str]:
    status = git_output(
        source_root, "status", "--porcelain", "--untracked-files=all"
    )
    digest = hashlib.sha256()
    digest.update(
        subprocess.check_output(
            ["git", "diff", "--binary", "HEAD", "--"], cwd=source_root
        )
    )
    untracked = git_output(
        source_root, "ls-files", "--others", "--exclude-standard"
    )
    for relative in sorted(filter(None, untracked.splitlines())):
        digest.update(relative.encode("utf-8"))
        path = source_root / relative
        if path.is_file():
            digest.update(sha256_file(path).encode("ascii"))
    return status, digest.hexdigest()


def parse_record(stdout: str) -> dict[str, Any]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ValueError("qualification executable must emit one JSON record")
    value = json.loads(lines[0])
    if not isinstance(value, dict):
        raise ValueError("qualification executable emitted a non-object")
    return value


def runtime_build_identity_matches(
    record: dict[str, Any], commit: str, configuration: str
) -> bool:
    return (
        record.get("build_commit") == commit
        and record.get("build_dirty") is False
        and record.get("build_configuration") == configuration
    )


def studio_build_identity_matches(
    record: dict[str, Any], commit: str, configuration: str
) -> bool:
    return (
        record.get("buildCommit") == commit
        and record.get("buildDirty") is False
        and record.get("buildConfiguration") == configuration
    )


def fixture_identity(root: Path) -> dict[str, Any]:
    files = []
    for name in FIXTURE_FILES:
        path = root / name
        if not path.is_file():
            raise FileNotFoundError(path)
        files.append(
            {"name": name, "bytes": path.stat().st_size,
             "sha256": sha256_file(path)}
        )
    return {"files": files, "sha256": stable_hash(files)}


def run_observation(
    command: list[str], raw_path: Path, fixture_root: Path | None = None
) -> dict[str, Any]:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=180.0,
        )
        elapsed = time.perf_counter() - started
        record = parse_record(completed.stdout)
        raw: dict[str, Any] = {
            "command": command,
            "elapsed_seconds": elapsed,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "record": record,
        }
        if fixture_root is not None:
            raw["fixture"] = fixture_identity(fixture_root)
        atomic_json(raw_path, raw)
    except Exception as error:
        atomic_json(
            raw_path,
            {
                "command": command,
                "elapsed_seconds": time.perf_counter() - started,
                "error": str(error),
            },
        )
        raise
    if completed.returncode != 0:
        raise RuntimeError(
            f"qualification child failed ({completed.returncode}): "
            + " ".join(command)
        )
    return raw


def metric_summary(
    records: list[dict[str, Any]], fields: tuple[str, ...]
) -> tuple[dict[str, Any], list[str]]:
    result: dict[str, Any] = {}
    unstable: list[str] = []
    for field in fields:
        values = [float(record[field]) for record in records]
        median = statistics.median(values)
        stable = math.isfinite(median) and all(
            math.isfinite(value) and value >= 0.0 and value <= median * 3.0
            for value in values
        )
        result[field] = {
            "median": median,
            "minimum": min(values),
            "maximum": max(values),
            "values": values,
            "stable": stable,
        }
        if not stable:
            unstable.append(field)
    return result, unstable


def gate(name: str, actual: float, limit: float, comparator: str) -> dict[str, Any]:
    if comparator == "lt":
        passed = actual < limit
    elif comparator == "le":
        passed = actual <= limit
    else:
        raise ValueError(comparator)
    return {
        "name": name,
        "actual": actual,
        "limit": limit,
        "comparator": comparator,
        "passed": passed,
    }


def runtime_gates(cases: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    small = cases["1000-64"]
    large = cases["50000-1024"]
    small_metrics = small["metrics"]
    large_metrics = large["metrics"]
    gates = [
        gate("inspect_50k_us", large_metrics["inspect_us"]["median"],
             50_000.0, "lt"),
        gate("load_init_50k_us", large_metrics["load_init_us"]["median"],
             100_000.0, "lt"),
        gate("content_cold_64_us",
             small_metrics["content_cold_us"]["median"], 50_000.0, "lt"),
        gate("content_warm_64_us",
             small_metrics["content_warm_us"]["median"],
             small_metrics["content_cold_us"]["median"] + 5_000.0, "le"),
        gate("replace_64_us", small_metrics["replace_us"]["median"],
             80_000.0, "lt"),
        gate("unload_64_us", small_metrics["unload_us"]["median"],
             20_000.0, "lt"),
        gate("resident_1024_body_plus_index_bytes",
             large["resident_footprint_bytes"], float(8 << 20), "le"),
    ]
    for field, label in (
        ("post_loaded_us", "post_loaded_50k_to_1k_ratio"),
        ("post_unloaded_us", "post_unloaded_50k_to_1k_ratio"),
    ):
        denominator = small_metrics[field]["median"]
        ratio = (
            large_metrics[field]["median"] / denominator
            if denominator > 0.0 else float("inf")
        )
        gates.append(gate(label, ratio, 3.0, "lt"))
    return gates


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--source-root", required=True)
    result.add_argument("--runtime-binary", required=True)
    result.add_argument("--studio-binary", required=True)
    result.add_argument("--output", required=True)
    result.add_argument("--mode", choices=("smoke", "formal"), required=True)
    result.add_argument("--configuration", default="Debug")
    return result


def main() -> int:
    args = parser().parse_args()
    source_root = Path(args.source_root).resolve()
    runtime_binary = Path(args.runtime_binary).resolve()
    studio_binary = Path(args.studio_binary).resolve()
    output = Path(args.output).resolve()
    output.relative_to(source_root)
    if output == source_root:
        raise ValueError("output must be below the source root")
    for binary in (runtime_binary, studio_binary):
        if not binary.is_file():
            raise FileNotFoundError(binary)
    if (output / "manifest.json").exists():
        raise FileExistsError("output already contains a manifest")

    commit = git_output(source_root, "rev-parse", "HEAD")
    dirty_status, dirty_diff_hash = source_dirty_identity(source_root)
    fixture_source = (
        source_root / "HarkenEngine" / "Test" / "Common" / "PackFoundationFixture.h"
    )
    manifest = {
        "record": "manifest",
        "schema": "plan05-f3-qualification-v1",
        "mode": args.mode,
        "configuration": args.configuration,
        "commit": commit,
        "source_dirty": bool(dirty_status),
        "dirty_status_sha256": hashlib.sha256(
            dirty_status.encode("utf-8")
        ).hexdigest(),
        "dirty_diff_sha256": dirty_diff_hash,
        "runtime_binary": str(runtime_binary),
        "runtime_binary_sha256": sha256_file(runtime_binary),
        "studio_binary": str(studio_binary),
        "studio_binary_sha256": sha256_file(studio_binary),
        "runner_sha256": sha256_file(Path(__file__).resolve()),
        "fixture_source_sha256": sha256_file(fixture_source),
        "cases": [
            {"catalog": catalog, "resident": resident}
            for catalog, resident in CASES
        ],
        "warmup_iterations": 3 if args.mode == "formal" else 0,
        "measured_iterations": 7 if args.mode == "formal" else 1,
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor()
                or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
            "logical_cpu_count": os.cpu_count(),
        },
    }
    atomic_json(output / "manifest.json", manifest)
    if args.mode == "formal" and dirty_status:
        raise RuntimeError("formal mode requires a clean commit")
    if args.mode == "formal" and args.configuration != "RelWithDebInfo":
        raise RuntimeError("formal mode requires RelWithDebInfo binaries")
    if args.mode == "formal" and (
        os.name != "nt" or platform.machine().lower() not in ("amd64", "x86_64")
    ):
        raise RuntimeError("formal mode requires Windows x64")

    warmups = int(manifest["warmup_iterations"])
    measured = int(manifest["measured_iterations"])
    case_summaries: dict[str, dict[str, Any]] = {}
    all_runtime_complete = True
    all_runtime_identity = True
    all_runtime_release = True
    all_runtime_stable = True
    for catalog, resident in CASES:
        case_id = f"{catalog}-{resident}"
        fixture_root = output / "fixtures" / case_id
        observed: list[dict[str, Any]] = []
        fixture_hashes: list[str] = []
        for phase, count in (("warmup", warmups), ("measured", measured)):
            for iteration in range(1, count + 1):
                command = [
                    str(runtime_binary),
                    "--catalog", str(catalog),
                    "--resident", str(resident),
                    "--fixture-root", str(fixture_root),
                ]
                raw = run_observation(
                    command,
                    output / "raw" / "runtime" / case_id
                    / f"{phase}-{iteration}.json",
                    fixture_root,
                )
                fixture_hashes.append(str(raw["fixture"]["sha256"]))
                if phase == "measured":
                    observed.append(raw["record"])
        metrics, unstable = metric_summary(observed, RUNTIME_TIMING_FIELDS)
        complete = all(
            record.get("benchmark") == "pack-foundation-runtime"
            and record.get("schema") == "plan05-f3-runtime-v1"
            and record.get("status") == "complete"
            and int(record.get("catalog_count", -1)) == catalog
            and int(record.get("resident_count", -1)) == resident
            and int(record.get("retiring_count", -1)) == 0
            for record in observed
        )
        build_identity = all(
            runtime_build_identity_matches(
                record, commit, args.configuration
            )
            for record in observed
        )
        release = all(record.get("release_build") is True for record in observed)
        fixture_stable = len(set(fixture_hashes)) == 1
        case_summary = {
            "catalog_count": catalog,
            "resident_count": resident,
            "behavior_complete": complete,
            "build_identity": build_identity,
            "release_build": release,
            "fixture_sha256": fixture_hashes[-1],
            "fixture_stable": fixture_stable,
            "metrics": metrics,
            "unstable_metrics": unstable,
            "resident_body_bytes": int(observed[-1]["resident_body_bytes"]),
            "composite_index_bytes": int(
                observed[-1]["composite_index_bytes"]
            ),
            "resident_footprint_bytes": int(
                observed[-1]["resident_footprint_bytes"]
            ),
            "replacement_formula_bytes": int(
                observed[-1]["replacement_formula_bytes"]
            ),
        }
        case_summaries[case_id] = case_summary
        all_runtime_complete = all_runtime_complete and complete and fixture_stable
        all_runtime_identity = all_runtime_identity and build_identity
        all_runtime_release = all_runtime_release and release
        all_runtime_stable = all_runtime_stable and not unstable

    studio_observed: list[dict[str, Any]] = []
    for phase, count in (("warmup", warmups), ("measured", measured)):
        for iteration in range(1, count + 1):
            raw = run_observation(
                [str(studio_binary), "--objects", "50000"],
                output / "raw" / "studio" / f"{phase}-{iteration}.json",
            )
            if phase == "measured":
                studio_observed.append(raw["record"])
    studio_metrics, studio_unstable = metric_summary(
        studio_observed, STUDIO_TIMING_FIELDS
    )
    studio_complete = all(
        record.get("benchmark") == "pack-foundation-studio"
        and record.get("schema") == "plan05-f3-studio-v1"
        and record.get("status") == "complete"
        and int(record.get("objectCount", -1)) == 50000
        and int(record.get("boundedBatchCount", -1)) == 256
        for record in studio_observed
    )
    studio_release = all(
        record.get("releaseBuild") is True for record in studio_observed
    )
    studio_identity = all(
        studio_build_identity_matches(record, commit, args.configuration)
        for record in studio_observed
    )

    gates = runtime_gates(case_summaries) if args.mode == "formal" else []
    formal_passed = (
        all_runtime_complete
        and all_runtime_identity
        and all_runtime_release
        and all_runtime_stable
        and studio_complete
        and studio_identity
        and studio_release
        and not studio_unstable
        and all(item["passed"] for item in gates)
    )
    smoke_passed = (
        all_runtime_complete
        and all_runtime_identity
        and studio_complete
        and studio_identity
    )
    summary = {
        "record": "summary",
        "schema": "plan05-f3-qualification-v1",
        "status": "complete"
            if (formal_passed if args.mode == "formal" else smoke_passed)
            else "failed",
        "mode": args.mode,
        "commit": commit,
        "runtime": {
            "behavior_complete": all_runtime_complete,
            "build_identity": all_runtime_identity,
            "release_build": all_runtime_release,
            "stable": all_runtime_stable,
            "cases": case_summaries,
            "gates": gates,
            "threshold_status": "evaluated"
                if args.mode == "formal" else "not_evaluated_debug_smoke",
        },
        "studio": {
            "behavior_complete": studio_complete,
            "build_identity": studio_identity,
            "release_build": studio_release,
            "stable": not studio_unstable,
            "unstable_metrics": studio_unstable,
            "metrics": studio_metrics,
            "runtime_gate": False,
            "timing_status": "observational",
        },
        "support_claim": False,
        "real_device_qualification": False,
    }
    atomic_json(output / "summary.json", summary)
    print(json.dumps(summary, sort_keys=True, ensure_ascii=False))
    return 0 if summary["status"] == "complete" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"pack foundation qualification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
