#!/usr/bin/env python3
"""Validate and merge private Runtime real-time qualification evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
CONTROL_MATRIX = {
    "off": {
        "deviceCallback": False,
        "mixPublication": False,
        "workerBarrier": False,
    },
    "device": {
        "deviceCallback": True,
        "mixPublication": False,
        "workerBarrier": False,
    },
    "publication": {
        "deviceCallback": False,
        "mixPublication": True,
        "workerBarrier": False,
    },
    "worker": {
        "deviceCallback": False,
        "mixPublication": False,
        "workerBarrier": True,
    },
    "all": {
        "deviceCallback": True,
        "mixPublication": True,
        "workerBarrier": True,
    },
}
BUILD_IDENTITY_KEYS = (
    "gitCommit",
    "gitDirty",
    "buildInfoGitCommit",
    "buildInfoGitDirty",
    "trackedWorktreeClean",
    "untrackedEntriesPresent",
    "configuration",
    "generator",
    "compilerId",
    "compilerVersion",
    "compilerPath",
    "generatorToolset",
    "vsToolsetVersion",
    "windowsSdk",
    "msvcFullVersion",
)
PROFILE_IDENTITY_KEYS = (
    "seconds",
    "sampleRate",
    "blockFrames",
    "minCallbackFrames",
    "channels",
    "voices",
    "workers",
    "meterVariables",
    "audibilityParking",
)
PROFILE_COMMON_KEYS = tuple(
    key for key in PROFILE_IDENTITY_KEYS if key != "workers"
)
CAPACITY_PROFILE_BASE_KEYS = tuple(
    key for key in PROFILE_IDENTITY_KEYS if key != "voices"
)
BACKEND_IDENTITY_KEYS = (
    "valid",
    "code",
    "name",
    "deviceName",
    "clientSampleRate",
    "clientChannels",
    "clientFormat",
    "requestedPeriodFrames",
    "requestedPeriods",
    "internalSampleRate",
    "internalChannels",
    "internalFormat",
    "internalPeriodFrames",
    "internalPeriods",
)
SEMANTIC_IDENTITY_KEYS = (
    "outputHashValid",
    "outputHash",
    "outputSignalMetricsValid",
    "outputPeakAbs",
    "outputRms",
    "outputNonzeroSampleCount",
    "outputSampleCount",
    "outputNonFiniteSampleCount",
    "pumpedBlocks",
    "snapshotBlockIndex",
    "activeVoiceCount",
    "voiceHighWater",
    "arenaOverflowCount",
    "renderWorkerCount",
    "meterRingOverflowCount",
)
TIMING_BOUNDARY = {
    "deviceCallback": (
        "Runtime DataCallback entry through post-render; excludes "
        "miniaudio/WASAPI pre/post and final probe leave"
    ),
    "mixPublication": (
        "before DynamicRoutes EndBlock through snapshot, meter, "
        "source-cursor publication, and block completion"
    ),
    "overBudgetAuthority": (
        "internal Runtime interval; not an xrun or audio-glitch authority"
    ),
}


class EvidenceError(RuntimeError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"JSON root is not an object: {path}")
    return value


def artifact_record(path: Path, role: str) -> dict[str, Any]:
    try:
        resolved = path.resolve(strict=True)
        digest = hashlib.sha256()
        size = 0
        with resolved.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
                size += len(chunk)
    except OSError as exc:
        raise EvidenceError(
            f"cannot bind artifact {role} at {path}: {exc}"
        ) from exc
    return {
        "role": role,
        "path": str(resolved),
        "bytes": size,
        "sha256": digest.hexdigest(),
    }


def raw_artifact_record(
    path: Path,
    role: str,
    expected_count: int,
) -> dict[str, Any]:
    count = 0
    try:
        with path.resolve(strict=True).open(
            "r", encoding="utf-8", newline=""
        ) as stream:
            for line_number, line in enumerate(stream, start=1):
                if not line.strip():
                    raise EvidenceError(
                        f"{role} contains a blank JSONL record "
                        f"at line {line_number}"
                    )
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise EvidenceError(
                        f"{role} contains invalid JSON at "
                        f"line {line_number}: {exc}"
                    ) from exc
                if not isinstance(record, dict):
                    raise EvidenceError(
                        f"{role} line {line_number} is not an object"
                    )
                if record.get("sequence") != count:
                    raise EvidenceError(
                        f"{role} sequence discontinuity at "
                        f"line {line_number}"
                    )
                count += 1
    except OSError as exc:
        raise EvidenceError(f"cannot validate raw artifact {role}: {exc}") from exc
    if count != expected_count:
        raise EvidenceError(
            f"{role} record count mismatch: "
            f"expected {expected_count}, got {count}"
        )
    return artifact_record(path, role)


def require_equal(
    reports: dict[str, dict[str, Any]],
    section: str,
    keys: tuple[str, ...],
) -> dict[str, Any]:
    reference_name = next(iter(reports))
    reference = reports[reference_name].get(section)
    if not isinstance(reference, dict):
        raise EvidenceError(f"{reference_name}.{section} is not an object")
    identity = {key: reference.get(key) for key in keys}
    for name, report in reports.items():
        candidate = report.get(section)
        if not isinstance(candidate, dict):
            raise EvidenceError(f"{name}.{section} is not an object")
        values = {key: candidate.get(key) for key in keys}
        if values != identity:
            raise EvidenceError(
                f"{section} identity drift in {name}: "
                f"expected {identity}, got {values}"
            )
    return identity


def require_controls(
    reports: dict[str, dict[str, Any]],
) -> None:
    for name, expected in CONTROL_MATRIX.items():
        actual = reports[name].get("controls")
        if actual != expected:
            raise EvidenceError(
                f"{name} controls mismatch: expected {expected}, got {actual}"
            )


def require_lifecycle(report_name: str, report: dict[str, Any]) -> None:
    build = report.get("build")
    boolean_fields = (
        "gitDirty",
        "buildInfoGitDirty",
        "trackedWorktreeClean",
        "untrackedEntriesPresent",
    )
    if (
        not isinstance(build, dict)
        or any(type(build.get(key)) is not bool for key in boolean_fields)
        or build.get("trackedWorktreeClean") is not True
    ):
        raise EvidenceError(
            f"{report_name} is not bound to clean tracked source"
        )
    commit = build.get("gitCommit")
    if (
        not isinstance(commit, str)
        or re.fullmatch(r"[0-9a-f]{40}", commit) is None
        or build.get("buildInfoGitCommit") != commit
    ):
        raise EvidenceError(f"{report_name} git commit identity is invalid")
    expected_dirty = (
        not build["trackedWorktreeClean"]
        or build["untrackedEntriesPresent"]
    )
    if build["gitDirty"] is not expected_dirty:
        raise EvidenceError(f"{report_name} git dirty identity is invalid")
    lifecycle = report.get("lifecycle")
    if not isinstance(lifecycle, dict):
        raise EvidenceError(f"{report_name}.lifecycle is not an object")
    expected = {
        "init": 0,
        "setup": 0,
        "update": 0,
        "snapshot": 0,
        "term": 0,
        "detach": True,
    }
    if lifecycle != expected:
        raise EvidenceError(
            f"{report_name} lifecycle failure: {lifecycle}"
        )
    storage = report.get("storage")
    identity = report.get("identity")
    if not isinstance(storage, dict) or not isinstance(identity, dict):
        raise EvidenceError(
            f"{report_name} storage/identity section is invalid"
        )
    if storage.get("timelineOverflow") != 0:
        raise EvidenceError(f"{report_name} timeline overflow")
    if storage.get("workerBarrierOverflow") != 0:
        raise EvidenceError(f"{report_name} worker barrier overflow")
    if identity.get("mismatchCount") != 0:
        raise EvidenceError(f"{report_name} coordinator identity drift")
    if identity.get("generationOverflowCount") != 0:
        raise EvidenceError(f"{report_name} generation overflow")
    if identity.get("workerCoordinatorMismatchCount") != 0:
        raise EvidenceError(
            f"{report_name} worker coordinator identity drift"
        )
    if identity.get("concurrentCallbackCount") != 0:
        raise EvidenceError(
            f"{report_name} concurrent device callbacks observed"
        )
    raw_records = report.get("rawRecords")
    if not isinstance(raw_records, dict):
        raise EvidenceError(f"{report_name} raw record export is missing")
    if raw_records.get("timelineCount") != storage.get("timelineCount"):
        raise EvidenceError(f"{report_name} raw timeline count mismatch")
    if raw_records.get("workerCount") != storage.get(
        "workerBarrierCount"
    ):
        raise EvidenceError(f"{report_name} raw worker count mismatch")


def require_timing_clean(
    report_name: str,
    report: dict[str, Any],
) -> None:
    if report.get("timingBoundary") != TIMING_BOUNDARY:
        raise EvidenceError(f"{report_name}.timingBoundary drift")
    for section in ("callbackTiming", "mixPublicationTiming"):
        timing = report.get(section)
        if not isinstance(timing, dict):
            raise EvidenceError(f"{report_name}.{section} is invalid")
        if timing.get("invalidCount") != 0:
            raise EvidenceError(f"{report_name}.{section} invalid records")
        if timing.get("deadlineMisses") != 0:
            raise EvidenceError(f"{report_name}.{section} deadline miss")
    worker = report.get("workerBarrier")
    if not isinstance(worker, dict):
        raise EvidenceError(f"{report_name}.workerBarrier is invalid")
    for key in (
        "invalidCount",
        "faultCount",
        "serialFallbackCount",
        "incompleteScratchCount",
        "identityMismatchMask",
    ):
        if worker.get(key) != 0:
            raise EvidenceError(
                f"{report_name}.workerBarrier {key}={worker.get(key)}"
            )


def require_semantic_health(
    report_name: str,
    report: dict[str, Any],
) -> None:
    profile = report.get("profile")
    semantic = report.get("semantic")
    controls = report.get("controls")
    publication = report.get("mixPublicationTiming")
    publication_shape = report.get("publicationShape")
    if (
        not isinstance(profile, dict)
        or not isinstance(semantic, dict)
        or not isinstance(controls, dict)
        or not isinstance(publication, dict)
        or not isinstance(publication_shape, dict)
    ):
        raise EvidenceError(f"{report_name} semantic/profile is invalid")
    voices = profile.get("voices")
    workers = profile.get("workers")
    if semantic.get("arenaOverflowCount") != 0:
        raise EvidenceError(f"{report_name} arena overflow")
    if semantic.get("meterRingOverflowCount") != 0:
        raise EvidenceError(f"{report_name} meter ring overflow")
    if semantic.get("activeVoiceCount") != voices:
        raise EvidenceError(f"{report_name} active voice count drift")
    high_water = semantic.get("voiceHighWater")
    if (
        not isinstance(high_water, int)
        or not isinstance(voices, int)
        or high_water < voices
    ):
        raise EvidenceError(f"{report_name} voice high-water is invalid")
    if semantic.get("renderWorkerCount") != workers:
        raise EvidenceError(f"{report_name} render worker count drift")
    if int(semantic.get("snapshotBlockIndex", 0)) <= 0:
        raise EvidenceError(f"{report_name} rendered no mix blocks")
    publication_count = int(publication.get("count", 0))
    expected_source_cursor_count = (
        publication_count
        if controls.get("mixPublication")
        and profile.get("audibilityParking") is True
        else 0
    )
    if publication_shape.get("count") != publication_count:
        raise EvidenceError(
            f"{report_name} publication shape count drift"
        )
    if publication_shape.get("meterArmedCount") != 0:
        raise EvidenceError(
            f"{report_name} unexpectedly armed meter publication"
        )
    if (
        publication_shape.get("sourceCursorPublishedCount")
        != expected_source_cursor_count
    ):
        raise EvidenceError(
            f"{report_name} source cursor publication drift"
        )

    if profile.get("device") == "null":
        if semantic.get("outputHashValid") is not True:
            raise EvidenceError(f"{report_name} output hash is invalid")
        if semantic.get("outputSignalMetricsValid") is not True:
            raise EvidenceError(
                f"{report_name} output signal metrics are invalid"
            )
        if int(semantic.get("pumpedBlocks", 0)) <= 0:
            raise EvidenceError(f"{report_name} pumped no null blocks")
        if int(semantic.get("outputSampleCount", 0)) <= 0:
            raise EvidenceError(f"{report_name} captured no output samples")
        if int(semantic.get("outputNonzeroSampleCount", 0)) <= 0:
            raise EvidenceError(f"{report_name} output is silent")
        if semantic.get("outputNonFiniteSampleCount") != 0:
            raise EvidenceError(f"{report_name} output is non-finite")
        if float(semantic.get("outputPeakAbs", 0.0)) <= 0.0:
            raise EvidenceError(f"{report_name} output peak is zero")
        if float(semantic.get("outputRms", 0.0)) <= 0.0:
            raise EvidenceError(f"{report_name} output RMS is zero")
    else:
        if semantic.get("outputHashValid") is not False:
            raise EvidenceError(
                f"{report_name} real-device hash claim is invalid"
            )
        if semantic.get("outputSignalMetricsValid") is not False:
            raise EvidenceError(
                f"{report_name} real-device signal claim is invalid"
            )


def validate_matrix_shape(
    null_reports: dict[str, dict[str, Any]],
    real_reports: dict[str, dict[str, Any]],
) -> None:
    require_controls(null_reports)
    require_controls(real_reports)
    for group_name, reports in (
        ("null", null_reports),
        ("real", real_reports),
    ):
        for name, report in reports.items():
            require_lifecycle(f"{group_name}.{name}", report)
            require_timing_clean(f"{group_name}.{name}", report)
            require_semantic_health(f"{group_name}.{name}", report)
            if report.get("contractDurationStatus") != "passed":
                raise EvidenceError(
                    f"{group_name}.{name} did not meet 60-second contract"
                )
            if report.get("captureIntegrityStatus") != "passed":
                raise EvidenceError(
                    f"{group_name}.{name} capture integrity failed"
                )
            if report.get("coverageStatus") != "passed":
                raise EvidenceError(
                    f"{group_name}.{name} probe coverage failed"
                )
            if report.get("qualificationInputStatus") != "ready":
                raise EvidenceError(
                    f"{group_name}.{name} is not qualification input"
                )
    for name, report in null_reports.items():
        if report.get("profile", {}).get("device") != "null":
            raise EvidenceError(f"null.{name} is not a null-device report")
    for name, report in real_reports.items():
        if report.get("profile", {}).get("device") != "real":
            raise EvidenceError(f"real.{name} is not a real-device report")

    if real_reports["off"]["callbackTiming"].get("count") != 0:
        raise EvidenceError("all-Off unexpectedly contains callback timing")
    for name, report in null_reports.items():
        if report["callbackTiming"].get("count") != 0:
            raise EvidenceError(
                f"null.{name} unexpectedly contains device callbacks"
            )
    if real_reports["device"]["callbackTiming"].get("count", 0) <= 0:
        raise EvidenceError("device-only has no callback timing")
    if real_reports["all"]["callbackTiming"].get("count", 0) <= 0:
        raise EvidenceError("all-On has no callback timing")
    if real_reports["publication"]["mixPublicationTiming"].get(
        "count", 0
    ) <= 0:
        raise EvidenceError("publication-only has no publication records")
    if real_reports["all"]["mixPublicationTiming"].get("count", 0) <= 0:
        raise EvidenceError("all-On has no publication records")
    if null_reports["worker"]["workerBarrier"].get("count", 0) <= 0:
        raise EvidenceError("worker-only null diagnostic has no records")
    if null_reports["all"]["workerBarrier"].get("count", 0) <= 0:
        raise EvidenceError("all-On null diagnostic has no worker records")


def validate_observer_cost(
    observer: dict[str, Any],
    callback_period_us: float,
    expected_build: dict[str, Any],
) -> dict[str, Any]:
    if observer.get("scope") != (
        "qualification-process-thread-observer-primitive"
    ):
        raise EvidenceError("observer cost scope mismatch")
    if observer.get("claimBoundary") != (
        "QPC-plus-callback-ownership-and-fixed-slot-cost-"
        "not-total-shipping-delta"
    ):
        raise EvidenceError("observer cost claim boundary mismatch")
    build = observer.get("build")
    if not isinstance(build, dict):
        raise EvidenceError("observer build identity is missing")
    observer_build = {key: build.get(key) for key in BUILD_IDENTITY_KEYS}
    if observer_build != expected_build:
        raise EvidenceError("observer build identity drift")
    if build.get("trackedWorktreeClean") is not True:
        raise EvidenceError("observer tracked source tree is not clean")
    iterations = observer.get("iterations")
    warmup = observer.get("warmupRecords")
    measured = observer.get("measuredRecords")
    if iterations != 200000 or warmup != 1000:
        raise EvidenceError("observer iteration contract mismatch")
    if measured != iterations - warmup:
        raise EvidenceError("observer measured-record count mismatch")
    if observer.get("timelineCount") != iterations:
        raise EvidenceError("observer timeline count mismatch")
    storage_bytes = observer.get("storageBytes")
    storage_limit = observer.get("storageLimitBytes")
    if (
        not isinstance(storage_bytes, int)
        or not isinstance(storage_limit, int)
        or storage_bytes <= 0
        or storage_bytes > storage_limit
        or storage_limit != 16 * 1024 * 1024
    ):
        raise EvidenceError("observer storage contract mismatch")
    if observer.get("timelineOverflow") != 0:
        raise EvidenceError("observer primitive overflow")
    if observer.get("identityMismatchCount") != 0:
        raise EvidenceError("observer primitive identity drift")
    if observer.get("concurrentCallbackCount") != 0:
        raise EvidenceError("observer primitive concurrent callback")
    if observer.get("detached") is not True:
        raise EvidenceError("observer primitive did not detach")
    p50 = float(observer.get("p50Us", float("inf")))
    p99 = float(observer.get("p99Us", float("inf")))
    p999 = float(observer.get("p99_9Us", float("inf")))
    maximum = float(observer.get("maxUs", float("inf")))
    if not (
        0.0 <= p50 <= p99 <= p999 <= maximum < float("inf")
    ):
        raise EvidenceError("observer percentile ordering is invalid")
    p99_limit = min(25.0, callback_period_us * 0.005)
    return {
        "p50Us": p50,
        "p99Us": p99,
        "p50LimitUs": 5.0,
        "p99LimitUs": p99_limit,
        "status": "passed"
        if p50 <= 5.0 and p99 <= p99_limit
        else "failed",
    }


def validate_overhead_trials(
    trials: dict[str, dict[str, Any]],
    *,
    front_controls: dict[str, bool],
    front_label: str,
    require_worker_records: bool,
) -> tuple[dict[str, Any], float]:
    expected_controls = {
        "baselineBefore": CONTROL_MATRIX["device"],
        "frontFirst": front_controls,
        "frontSecond": front_controls,
        "baselineAfter": CONTROL_MATRIX["device"],
    }
    for name, report in trials.items():
        if report.get("controls") != expected_controls[name]:
            raise EvidenceError(f"overhead.{name} controls mismatch")
        require_lifecycle(f"overhead.{name}", report)
        require_timing_clean(f"overhead.{name}", report)
        require_semantic_health(f"overhead.{name}", report)
        for field, expected in (
            ("contractDurationStatus", "passed"),
            ("captureIntegrityStatus", "passed"),
            ("coverageStatus", "passed"),
            ("qualificationInputStatus", "ready"),
        ):
            if report.get(field) != expected:
                raise EvidenceError(
                    f"overhead.{name} {field} failed"
                )
        if report.get("profile", {}).get("device") != "real":
            raise EvidenceError(
                f"overhead.{name} is not a real-device report"
            )
        if report.get("callbackTiming", {}).get("count", 0) <= 0:
            raise EvidenceError(
                f"overhead.{name} has no callback timing"
            )
        worker_count = report.get("workerBarrier", {}).get("count", 0)
        if require_worker_records:
            if name.startswith("front") and worker_count <= 0:
                raise EvidenceError(
                    f"overhead.{name} has no worker barrier records"
                )
            if name.startswith("baseline") and worker_count != 0:
                raise EvidenceError(
                    f"overhead.{name} unexpectedly has worker records"
                )
        frame_counts = report.get("callbackShape", {}).get("frameCounts")
        if not isinstance(frame_counts, dict) or not frame_counts:
            raise EvidenceError(
                f"overhead.{name} has no callback frame histogram"
            )

    require_equal(trials, "build", BUILD_IDENTITY_KEYS)
    require_equal(trials, "profile", PROFILE_IDENTITY_KEYS)
    backend_identity = require_equal(
        trials, "backend", BACKEND_IDENTITY_KEYS
    )
    if backend_identity.get("valid") != 1:
        raise EvidenceError("B-F-F-B backend identity is unavailable")
    histogram_names = (
        "frameCounts",
        "mixBlockCounts",
        "stagingReadBefore",
        "stagingAvailBefore",
        "stagingReadAfter",
        "stagingAvailAfter",
    )
    for histogram_name in histogram_names:
        histograms = []
        for name, report in trials.items():
            histogram = report.get("callbackShape", {}).get(
                histogram_name
            )
            if not isinstance(histogram, dict) or not histogram:
                raise EvidenceError(
                    f"overhead.{name} has no {histogram_name} histogram"
                )
            if any(
                not isinstance(value, int) or value < 0
                for value in histogram.values()
            ):
                raise EvidenceError(
                    f"overhead.{name} {histogram_name} is invalid"
                )
            histograms.append(histogram)
        key_sets = {tuple(sorted(item)) for item in histograms}
        if len(key_sets) != 1:
            raise EvidenceError(
                f"B-F-F-B {histogram_name} buckets drifted"
            )
        keys = next(iter(key_sets))
        for key in keys:
            counts = [item[key] for item in histograms]
            if max(counts) - min(counts) > 1:
                raise EvidenceError(
                    f"B-F-F-B {histogram_name} distribution drifted"
                )
        totals = [sum(item.values()) for item in histograms]
        if max(totals) - min(totals) > 1:
            raise EvidenceError(
                f"B-F-F-B {histogram_name} sample count drifted"
            )
        if histogram_name == "frameCounts":
            for (name, report), total in zip(trials.items(), totals):
                if report["callbackTiming"].get("count") != total:
                    raise EvidenceError(
                        f"overhead.{name} callback histogram count mismatch"
                    )
    shortest_period = min(
        float(report["shortestObservedCallbackPeriodUs"])
        for report in trials.values()
    )
    if shortest_period <= 0.0:
        raise EvidenceError("B-F-F-B callback period is unavailable")

    baselines = [
        trials["baselineBefore"]["callbackTiming"],
        trials["baselineAfter"]["callbackTiming"],
    ]
    fronts = [
        trials["frontFirst"]["callbackTiming"],
        trials["frontSecond"]["callbackTiming"],
    ]
    baseline_p50 = [float(item["p50Us"]) for item in baselines]
    baseline_p99 = [float(item["p99Us"]) for item in baselines]
    front_p50 = [float(item["p50Us"]) for item in fronts]
    front_p99 = [float(item["p99Us"]) for item in fronts]
    p50_delta = max(front_p50) - min(baseline_p50)
    p99_delta = max(front_p99) - min(baseline_p99)
    p99_limit = min(25.0, shortest_period * 0.005)
    return {
        "sequence": [
            "device-only",
            front_label,
            front_label,
            "device-only",
        ],
        "comparison": (
            "max(front)-min(device-only), conservative across B-F-F-B"
        ),
        "baselineP50Us": baseline_p50,
        "baselineP99Us": baseline_p99,
        "frontP50Us": front_p50,
        "frontP99Us": front_p99,
        "p50DeltaUs": p50_delta,
        "p99DeltaUs": p99_delta,
        "p50LimitUs": 5.0,
        "p99LimitUs": p99_limit,
        "numericThresholdStatus": "passed"
        if p50_delta <= 5.0 and p99_delta <= p99_limit
        else "failed",
    }, shortest_period


def validate_capacity_profiles(
    reports: dict[str, dict[str, Any]],
    reference: dict[str, Any],
) -> dict[str, Any]:
    expected_voices = {"light": 8, "medium": 32, "heavy": 64}
    if set(reports) != set(expected_voices):
        raise EvidenceError("capacity profile set mismatch")
    for name, report in reports.items():
        if report.get("controls") != CONTROL_MATRIX["device"]:
            raise EvidenceError(f"capacity.{name} controls mismatch")
        require_lifecycle(f"capacity.{name}", report)
        require_timing_clean(f"capacity.{name}", report)
        require_semantic_health(f"capacity.{name}", report)
        if report.get("contractDurationStatus") != "passed":
            raise EvidenceError(f"capacity.{name} duration failed")
        if report.get("captureIntegrityStatus") != "passed":
            raise EvidenceError(f"capacity.{name} capture failed")
        if report.get("coverageStatus") != "passed":
            raise EvidenceError(f"capacity.{name} coverage failed")
        if report.get("qualificationInputStatus") != "ready":
            raise EvidenceError(f"capacity.{name} is not qualification input")
        profile = report.get("profile", {})
        if (
            profile.get("device") != "real"
            or profile.get("voices") != expected_voices[name]
            or profile.get("workers") != 0
            or profile.get("meterVariables") != 0
            or profile.get("audibilityParking") is not False
        ):
            raise EvidenceError(f"capacity.{name} profile mismatch")
        if report.get("callbackTiming", {}).get("count", 0) <= 0:
            raise EvidenceError(f"capacity.{name} has no callback timing")
    require_equal(reports, "build", BUILD_IDENTITY_KEYS)
    require_equal(reports, "backend", BACKEND_IDENTITY_KEYS)
    require_equal(
        {"reference": reference, **reports},
        "profile",
        CAPACITY_PROFILE_BASE_KEYS,
    )
    return {
        "claimBoundary": (
            "48kHz-stereo-256-frame-PCM-pitch1-no-FX-send-meter-"
            "workers0; 64 is a sampled load tier, not a maximum or "
            "certified shipping envelope"
        ),
        "tiers": {
            name: {
                "voices": expected_voices[name],
                "callbackTiming": report["callbackTiming"],
                "captureStatus": "passed",
                "deadlineStatus": "passed",
            }
            for name, report in reports.items()
        }
    }


def validate_parking_diagnostic(
    report: dict[str, Any],
    reference: dict[str, Any],
) -> dict[str, Any]:
    if report.get("controls") != CONTROL_MATRIX["all"]:
        raise EvidenceError("parking diagnostic controls mismatch")
    require_lifecycle("parking", report)
    require_timing_clean("parking", report)
    require_semantic_health("parking", report)
    for field, expected in (
        ("contractDurationStatus", "passed"),
        ("captureIntegrityStatus", "passed"),
        ("coverageStatus", "passed"),
        ("qualificationInputStatus", "ready"),
    ):
        if report.get(field) != expected:
            raise EvidenceError(f"parking diagnostic {field} failed")
    profile = report.get("profile", {})
    if (
        profile.get("device") != "real"
        or profile.get("voices") != 8
        or profile.get("workers") != 0
        or profile.get("meterVariables") != 0
        or profile.get("audibilityParking") is not True
    ):
        raise EvidenceError("parking diagnostic profile mismatch")
    if report.get("callbackTiming", {}).get("count", 0) <= 0:
        raise EvidenceError("parking diagnostic has no callback timing")
    if report.get("mixPublicationTiming", {}).get("count", 0) <= 0:
        raise EvidenceError("parking diagnostic has no publication timing")
    require_equal(
        {"reference": reference, "parking": report},
        "build",
        BUILD_IDENTITY_KEYS,
    )
    require_equal(
        {"reference": reference, "parking": report},
        "backend",
        BACKEND_IDENTITY_KEYS,
    )
    require_equal(
        {"reference": reference, "parking": report},
        "profile",
        tuple(
            key
            for key in PROFILE_IDENTITY_KEYS
            if key != "audibilityParking"
        ),
    )
    return {
        "captureStatus": "passed",
        "deadlineStatus": "passed",
        "callbackTiming": report["callbackTiming"],
        "mixPublicationTiming": report["mixPublicationTiming"],
    }


def validate_callback_closure(
    report: dict[str, Any],
    expected_commit: str,
) -> dict[str, Any]:
    if report.get("kind") != "harken-runtime-source-callback-closure":
        raise EvidenceError("callback closure kind mismatch")
    if report.get("verdict") != (
        "SOURCE_CLOSURE_CONSTRUCTED_WITH_OPEN_BLOCKERS"
    ):
        raise EvidenceError("callback closure did not pass")
    if report.get("analysisLevel") != "source-ast":
        raise EvidenceError("callback closure analysis level mismatch")
    if report.get("safetyVerdict") != "UNRESOLVED":
        raise EvidenceError("callback closure safety boundary drift")
    git = report.get("git")
    if not isinstance(git, dict):
        raise EvidenceError("callback closure git identity is missing")
    if git.get("commit") != expected_commit:
        raise EvidenceError("callback closure commit drift")
    if git.get("trackedWorktreeClean") is not True:
        raise EvidenceError("callback closure source tree is not clean")
    dynamic = report.get("dynamicEdges")
    if not isinstance(dynamic, dict) or dynamic.get("status") != "PASS":
        raise EvidenceError("callback closure dynamic edge gate failed")
    if dynamic.get("unknownCalls") != []:
        raise EvidenceError("callback closure has unknown calls")
    closure = report.get("closure")
    if not isinstance(closure, dict):
        raise EvidenceError("callback closure payload is missing")
    if not isinstance(closure.get("digest"), str):
        raise EvidenceError("callback closure digest is missing")
    if int(closure.get("nodeCount", 0)) <= 0:
        raise EvidenceError("callback closure contains no nodes")
    blockers = report.get("knownBlockers")
    if not isinstance(blockers, list) or not blockers:
        raise EvidenceError("callback closure blockers are missing")
    blocker_status = {
        item.get("id"): item.get("status")
        for item in blockers
        if isinstance(item, dict)
    }
    required_open = {
        "RT-B001",
        "RT-B002",
        "RT-B003",
        "RT-B004",
        "RT-B006",
        "RT-B008",
    }
    if any(blocker_status.get(item) != "OPEN" for item in required_open):
        raise EvidenceError("callback closure blocker status drift")
    if blocker_status.get("RT-B007") != "NOT_DETECTED":
        raise EvidenceError("callback closure external-leaf status drift")
    if any(
        status not in {"OPEN", "NOT_DETECTED"}
        for status in blocker_status.values()
    ):
        raise EvidenceError("callback closure has an invalid blocker status")
    return {
        "status": "source-closure-with-open-blockers",
        "safetyVerdict": "UNRESOLVED",
        "digest": closure["digest"],
        "nodeCount": closure["nodeCount"],
        "edgeCount": closure.get("edgeCount"),
        "knownBlockers": blockers,
    }


def build_manifest(
    null_reports: dict[str, dict[str, Any]],
    real_reports: dict[str, dict[str, Any]],
    observer: dict[str, Any],
    compileout: dict[str, Any] | None,
    overhead_trials: dict[str, dict[str, Any]],
    worker_overhead_trials: dict[str, dict[str, Any]],
    capacity_profiles: dict[str, dict[str, Any]],
    parking_diagnostic: dict[str, Any],
    callback_closure: dict[str, Any],
    etw: dict[str, Any] | None,
) -> dict[str, Any]:
    validate_matrix_shape(null_reports, real_reports)
    build_identity = require_equal(
        {**{f"null.{k}": v for k, v in null_reports.items()},
         **{f"real.{k}": v for k, v in real_reports.items()}},
        "build",
        BUILD_IDENTITY_KEYS,
    )
    null_profile = require_equal(
        null_reports, "profile", PROFILE_IDENTITY_KEYS
    )
    real_profile = require_equal(
        real_reports, "profile", PROFILE_IDENTITY_KEYS
    )
    semantic_identity = require_equal(
        null_reports, "semantic", SEMANTIC_IDENTITY_KEYS
    )
    closure_gate = validate_callback_closure(
        callback_closure, str(build_identity["gitCommit"])
    )

    publication_incremental, publication_shortest_period = (
        validate_overhead_trials(
            overhead_trials,
            front_controls=CONTROL_MATRIX["all"],
            front_label="all-on",
            require_worker_records=False,
        )
    )
    worker_front_controls = {
        "deviceCallback": True,
        "mixPublication": False,
        "workerBarrier": True,
    }
    worker_incremental, worker_shortest_period = (
        validate_overhead_trials(
            worker_overhead_trials,
            front_controls=worker_front_controls,
            front_label="device-plus-worker",
            require_worker_records=True,
        )
    )
    shortest_period = min(
        publication_shortest_period, worker_shortest_period
    )
    capacity_gate = validate_capacity_profiles(
        capacity_profiles, real_reports["device"]
    )
    parking_gate = validate_parking_diagnostic(
        parking_diagnostic, real_reports["all"]
    )
    combined_build_reports = {
        **{
            f"matrix.null.{key}": value
            for key, value in null_reports.items()
        },
        **{
            f"matrix.real.{key}": value
            for key, value in real_reports.items()
        },
        **{
            f"overhead.{key}": value
            for key, value in overhead_trials.items()
        },
        **{
            f"workerOverhead.{key}": value
            for key, value in worker_overhead_trials.items()
        },
        **{
            f"capacity.{key}": value
            for key, value in capacity_profiles.items()
        },
        "parking": parking_diagnostic,
    }
    require_equal(
        combined_build_reports, "build", BUILD_IDENTITY_KEYS
    )
    require_equal(
        {
            "matrix.real": real_reports["device"],
            **{
                f"overhead.{key}": value
                for key, value in overhead_trials.items()
            },
        },
        "profile",
        PROFILE_IDENTITY_KEYS,
    )
    require_equal(
        worker_overhead_trials, "profile", PROFILE_IDENTITY_KEYS
    )
    require_equal(
        {
            "matrix.real": real_reports["device"],
            "workerOverhead": worker_overhead_trials["baselineBefore"],
        },
        "profile",
        PROFILE_COMMON_KEYS,
    )
    observer_gate = validate_observer_cost(
        observer, shortest_period, build_identity
    )

    compileout_status = "unverified"
    if compileout is not None:
        compileout_status = str(compileout.get("status", "unverified"))
        compileout_git = compileout.get("git")
        if (
            not isinstance(compileout_git, dict)
            or compileout_git.get("commit")
            != build_identity["gitCommit"]
            or compileout_git.get("trackedWorktreeClean") is not True
        ):
            raise EvidenceError("compile-out evidence commit drift")
    xrun_status = "unverified"
    environment_status = "unverified"
    etw_glitches: int | None = None
    if etw is not None:
        xrun_status = str(etw.get("xrunStatus", "unverified"))
        environment_status = str(
            etw.get("environmentIdentityStatus", "unverified")
        )
        value = etw.get("audioGlitchCount")
        etw_glitches = value if isinstance(value, int) else None
        if xrun_status == "verified" and etw_glitches is None:
            raise EvidenceError(
                "verified ETW xrun status lacks audioGlitchCount"
            )

    mechanics_passed = (
        compileout_status == "passed"
        and closure_gate["status"] == "source-closure-with-open-blockers"
        and observer_gate["status"] == "passed"
        and publication_incremental["numericThresholdStatus"] == "passed"
        and worker_incremental["numericThresholdStatus"] == "passed"
    )
    external_facets_passed = (
        xrun_status == "verified"
        and etw_glitches == 0
        and environment_status == "verified"
    )
    def attributable_status(numeric_status: str) -> str:
        if environment_status == "drifted":
            return "failed"
        if environment_status != "verified":
            return "unverified"
        return "passed" if numeric_status == "passed" else "failed"

    publication_incremental["attributableOverheadStatus"] = (
        attributable_status(
            publication_incremental["numericThresholdStatus"]
        )
    )
    worker_incremental["attributableOverheadStatus"] = (
        attributable_status(
            worker_incremental["numericThresholdStatus"]
        )
    )
    attributable_overhead_passed = (
        publication_incremental["attributableOverheadStatus"] == "passed"
        and worker_incremental["attributableOverheadStatus"] == "passed"
    )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "m2InfrastructureStatus": (
            "passed"
            if mechanics_passed and attributable_overhead_passed
            else "blocked"
        ),
        "m2ProbeMechanicsStatus": (
            "passed" if mechanics_passed else "blocked"
        ),
        "productionQualificationStatus": "blocked",
        "claimBoundary": {
            "allOff": (
                "compile-out/ABI/semantic/external-glitch baseline"
            ),
            "totalProbeVsShippingOffDelta": "unverified",
            "incrementalTiming": (
                "device-only common observer shell to all-on"
            ),
            "xrunAuthority": "ETW/WPA only",
            "realDeviceOutputSignal": (
                "unverified-without-loopback-capture"
            ),
            "voiceTiers": (
                "8/32/64 are narrow sampled load tiers, not a maximum "
                "or certified shipping capacity envelope"
            ),
        },
        "buildIdentity": build_identity,
        "nullProfile": null_profile,
        "realProfile": real_profile,
        "nullSemanticIdentity": semantic_identity,
        "compileoutStatus": compileout_status,
        "callbackClosure": closure_gate,
        "observerPrimitive": observer_gate,
        "publicationIncrementalCallback": publication_incremental,
        "workerIncrementalCallback": worker_incremental,
        "serialCapacityCharacterization": capacity_gate,
        "parkingPublicationDiagnostic": parking_gate,
        "shortestObservedCallbackPeriodUs": shortest_period,
        "xrunStatus": xrun_status,
        "environmentIdentityStatus": environment_status,
        "audioGlitchCount": etw_glitches,
        "externalRealtimeFacetsStatus": (
            "passed" if external_facets_passed else "blocked"
        ),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    for device in ("null", "real"):
        for cell in CONTROL_MATRIX:
            parser.add_argument(
                f"--{device}-{cell}",
                type=Path,
                required=True,
            )
    parser.add_argument("--observer-cost", type=Path, required=True)
    parser.add_argument("--compileout", type=Path)
    parser.add_argument("--callback-closure", type=Path, required=True)
    parser.add_argument("--etw-evidence", type=Path)
    parser.add_argument("--overhead-device-before", type=Path, required=True)
    parser.add_argument("--overhead-all-first", type=Path, required=True)
    parser.add_argument("--overhead-all-second", type=Path, required=True)
    parser.add_argument("--overhead-device-after", type=Path, required=True)
    parser.add_argument(
        "--worker-overhead-device-before", type=Path, required=True
    )
    parser.add_argument(
        "--worker-overhead-front-first", type=Path, required=True
    )
    parser.add_argument(
        "--worker-overhead-front-second", type=Path, required=True
    )
    parser.add_argument(
        "--worker-overhead-device-after", type=Path, required=True
    )
    for profile in ("light", "medium", "heavy"):
        parser.add_argument(
            f"--capacity-{profile}", type=Path, required=True
        )
    parser.add_argument(
        "--parking-diagnostic", type=Path, required=True
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        null_reports = {
            cell: load_json(getattr(args, f"null_{cell}"))
            for cell in CONTROL_MATRIX
        }
        real_reports = {
            cell: load_json(getattr(args, f"real_{cell}"))
            for cell in CONTROL_MATRIX
        }
        observer = load_json(args.observer_cost)
        compileout = (
            load_json(args.compileout) if args.compileout else None
        )
        overhead_trials = {
            "baselineBefore": load_json(args.overhead_device_before),
            "frontFirst": load_json(args.overhead_all_first),
            "frontSecond": load_json(args.overhead_all_second),
            "baselineAfter": load_json(args.overhead_device_after),
        }
        worker_overhead_trials = {
            "baselineBefore": load_json(
                args.worker_overhead_device_before
            ),
            "frontFirst": load_json(
                args.worker_overhead_front_first
            ),
            "frontSecond": load_json(
                args.worker_overhead_front_second
            ),
            "baselineAfter": load_json(
                args.worker_overhead_device_after
            ),
        }
        capacity_profiles = {
            name: load_json(getattr(args, f"capacity_{name}"))
            for name in ("light", "medium", "heavy")
        }
        parking_diagnostic = load_json(args.parking_diagnostic)
        callback_closure = load_json(args.callback_closure)
        etw = (
            load_json(args.etw_evidence)
            if args.etw_evidence
            else None
        )
        manifest = build_manifest(
            null_reports,
            real_reports,
            observer,
            compileout,
            overhead_trials,
            worker_overhead_trials,
            capacity_profiles,
            parking_diagnostic,
            callback_closure,
            etw,
        )
        artifacts: list[dict[str, Any]] = []
        for device, reports in (
            ("null", null_reports),
            ("real", real_reports),
        ):
            for cell, report in reports.items():
                report_path = getattr(args, f"{device}_{cell}")
                role = f"matrix.{device}.{cell}"
                artifacts.append(artifact_record(report_path, role))
                raw = report["rawRecords"]
                artifacts.append(
                    raw_artifact_record(
                        Path(str(raw["timelinePath"])),
                        role + ".timeline",
                        int(raw["timelineCount"]),
                    )
                )
                artifacts.append(
                    raw_artifact_record(
                        Path(str(raw["workerPath"])),
                        role + ".workers",
                        int(raw["workerCount"]),
                    )
                )
        overhead_paths = {
            "baselineBefore": args.overhead_device_before,
            "frontFirst": args.overhead_all_first,
            "frontSecond": args.overhead_all_second,
            "baselineAfter": args.overhead_device_after,
        }
        for name, report in overhead_trials.items():
            role = f"overhead.{name}"
            artifacts.append(
                artifact_record(overhead_paths[name], role)
            )
            raw = report["rawRecords"]
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["timelinePath"])),
                    role + ".timeline",
                    int(raw["timelineCount"]),
                )
            )
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["workerPath"])),
                    role + ".workers",
                    int(raw["workerCount"]),
                )
            )
        worker_overhead_paths = {
            "baselineBefore": args.worker_overhead_device_before,
            "frontFirst": args.worker_overhead_front_first,
            "frontSecond": args.worker_overhead_front_second,
            "baselineAfter": args.worker_overhead_device_after,
        }
        for name, report in worker_overhead_trials.items():
            role = f"worker-overhead.{name}"
            artifacts.append(
                artifact_record(worker_overhead_paths[name], role)
            )
            raw = report["rawRecords"]
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["timelinePath"])),
                    role + ".timeline",
                    int(raw["timelineCount"]),
                )
            )
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["workerPath"])),
                    role + ".workers",
                    int(raw["workerCount"]),
                )
            )
        capacity_paths = {
            name: getattr(args, f"capacity_{name}")
            for name in ("light", "medium", "heavy")
        }
        for name, report in capacity_profiles.items():
            role = f"capacity.{name}"
            artifacts.append(
                artifact_record(capacity_paths[name], role)
            )
            raw = report["rawRecords"]
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["timelinePath"])),
                    role + ".timeline",
                    int(raw["timelineCount"]),
                )
            )
            artifacts.append(
                raw_artifact_record(
                    Path(str(raw["workerPath"])),
                    role + ".workers",
                    int(raw["workerCount"]),
                )
            )
        artifacts.append(
            artifact_record(args.parking_diagnostic, "parking")
        )
        parking_raw = parking_diagnostic["rawRecords"]
        artifacts.append(
            raw_artifact_record(
                Path(str(parking_raw["timelinePath"])),
                "parking.timeline",
                int(parking_raw["timelineCount"]),
            )
        )
        artifacts.append(
            raw_artifact_record(
                Path(str(parking_raw["workerPath"])),
                "parking.workers",
                int(parking_raw["workerCount"]),
            )
        )
        artifacts.append(
            artifact_record(args.observer_cost, "observer-cost")
        )
        artifacts.append(
            artifact_record(args.callback_closure, "callback-closure")
        )
        if args.compileout:
            artifacts.append(
                artifact_record(args.compileout, "compileout")
            )
        if args.etw_evidence:
            artifacts.append(
                artifact_record(args.etw_evidence, "etw-evidence")
            )
        manifest["artifacts"] = artifacts
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except EvidenceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return (
        0
        if manifest.get("m2InfrastructureStatus") == "passed"
        else 3
    )


if __name__ == "__main__":
    raise SystemExit(main())
