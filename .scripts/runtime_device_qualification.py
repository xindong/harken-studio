#!/usr/bin/env python3
"""Run the bounded M12 Windows real-device qualification catalog."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import locale
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
FORMAL_SCENARIO_SECONDS = 1080
FORMAL_OUTER_SECONDS = 1170
CLEANUP_RESERVE_SECONDS = 20
CLOCK_ANCHOR_TOLERANCE_NS = 10_000_000
CLOCK_ANCHOR_MAX_SPAN_NS = 1_000_000
CLOCK_BOUNDARY_MIN_GUARD_NS = 10_000
RESTART_EVENT_CORRELATION_TAIL_NS = 1_000_000
EXPECTED_RESTART_GLITCH_EVENT = "OP_EVT_GLITCH_CP_SERVER_INPUT_STARVATION"


class QualificationError(RuntimeError):
    pass


@dataclass(frozen=True)
class Scenario:
    name: str
    sample_rate: int
    render_voices: int
    seconds: int
    pitch_cents: float = 0.0
    virtual_voices: int = 0
    restart_count: int = 0
    device_callback_only: bool = False


FORMAL_CATALOG = (
    Scenario("native-48k-r64", 48000, 64, 60),
    Scenario("native-44k1-r64", 44100, 64, 60),
    Scenario("native-48k-r128", 48000, 128, 60),
    Scenario("native-44k1-r128", 44100, 128, 60),
    Scenario("pitch-down-1200-r64", 48000, 64, 60, -1200.0),
    Scenario("pitch-up-1200-r64", 48000, 64, 60, 1200.0),
    Scenario("virtual-512-r64", 48000, 64, 60, 0.0, 512),
    Scenario("restart-10-r64", 48000, 64, 60, 0.0, 0, 10),
    Scenario("soak-48k-r64", 48000, 64, 600, 0.0, 0, 0, True),
)

SMOKE_CATALOG = (
    Scenario("smoke-native-48k-r8", 48000, 8, 2),
    Scenario("smoke-virtual-8-r8", 48000, 8, 2, 0.0, 8),
    Scenario("smoke-restart-2-r8", 48000, 8, 3, 0.0, 0, 2),
)


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise QualificationError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise QualificationError(f"JSON root is not an object: {path}")
    return value


def sha256_file(path: Path, deadline: float | None = None) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            if deadline is not None and time.monotonic() >= deadline:
                raise QualificationError("M12 hard time budget exhausted while hashing")
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_xperf_utc(value: str) -> datetime:
    main, dot, fraction = value.partition(".")
    parsed = datetime.strptime(main, "%Y/%m/%d:%H:%M:%S").replace(
        tzinfo=timezone.utc
    )
    if dot:
        parsed = parsed.replace(microsecond=int((fraction + "000000")[:6]))
    return parsed


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def inspect_jsonl_artifact(
    path: Path, output_dir: Path, deadline: float
) -> dict[str, Any]:
    resolved = path.resolve()
    if resolved.parent != output_dir.resolve():
        raise QualificationError(f"raw artifact escaped output directory: {resolved}")
    if not resolved.is_file():
        raise QualificationError(f"raw artifact is missing: {resolved}")
    digest = hashlib.sha256()
    line_count = 0
    byte_count = 0
    with resolved.open("rb") as handle:
        for line in handle:
            if time.monotonic() >= deadline:
                raise QualificationError(
                    "M12 hard time budget exhausted while validating raw evidence"
                )
            digest.update(line)
            byte_count += len(line)
            if line.strip():
                line_count += 1
    return {
        "path": str(resolved),
        "sha256": digest.hexdigest(),
        "bytes": byte_count,
        "lineCount": line_count,
    }


def validate_raw_artifacts(
    report: dict[str, Any], output_dir: Path, deadline: float
) -> list[dict[str, Any]]:
    raw = report.get("rawRecords")
    if not isinstance(raw, dict):
        raise QualificationError("qualification report rawRecords is missing")
    artifacts = []
    for stem in ("timeline", "worker"):
        path_value = raw.get(f"{stem}Path")
        expected_count = raw.get(f"{stem}Count")
        if not isinstance(path_value, str) or not isinstance(expected_count, int):
            raise QualificationError(f"raw {stem} evidence metadata is invalid")
        artifact = inspect_jsonl_artifact(Path(path_value), output_dir, deadline)
        require_equal(
            artifact["lineCount"], expected_count, f"raw {stem} line count"
        )
        artifact["kind"] = stem
        artifacts.append(artifact)
    return artifacts


def stop_process(process: subprocess.Popen[Any], deadline: float) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    remaining = deadline - time.monotonic()
    if remaining <= 0.0:
        process.kill()
        return
    try:
        process.wait(timeout=min(5.0, remaining))
        return
    except subprocess.TimeoutExpired:
        process.kill()
    remaining = deadline - time.monotonic()
    if remaining > 0.0:
        try:
            process.wait(timeout=min(5.0, remaining))
        except subprocess.TimeoutExpired:
            pass


def require_equal(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise QualificationError(
            f"{label}: expected {expected!r}, observed {actual!r}"
        )


def aggregate_facet_status(facets: dict[str, str]) -> str:
    if any(value == "failed" for value in facets.values()):
        return "failed"
    if all(value == "passed" for value in facets.values()):
        return "passed"
    return "incomplete_external_facets"


def validate_report(
    scenario: Scenario,
    report: dict[str, Any],
    expected_commit: str | None,
    formal: bool,
    expected_min_callback_frames: int = 64,
) -> dict[str, Any]:
    profile = report.get("profile")
    build = report.get("build")
    backend = report.get("backend")
    lifecycle = report.get("lifecycle")
    restart = report.get("restart")
    identity = report.get("identity")
    semantic = report.get("semantic")
    callback = report.get("callbackTiming")
    publication = report.get("mixPublicationTiming")
    storage = report.get("storage")
    measurement = report.get("measurement")
    controls = report.get("controls")
    coverage = report.get("coverage")
    continuity = report.get("callbackContinuity")
    for label, value in (
        ("profile", profile),
        ("build", build),
        ("backend", backend),
        ("lifecycle", lifecycle),
        ("restart", restart),
        ("identity", identity),
        ("semantic", semantic),
        ("callbackTiming", callback),
        ("mixPublicationTiming", publication),
        ("storage", storage),
        ("measurement", measurement),
        ("controls", controls),
        ("coverage", coverage),
        ("callbackContinuity", continuity),
    ):
        if not isinstance(value, dict):
            raise QualificationError(f"{scenario.name}.{label} is missing")

    require_equal(report.get("schemaVersion"), 1, f"{scenario.name}.schemaVersion")
    require_equal(profile.get("device"), "real", f"{scenario.name}.device")
    require_equal(profile.get("seconds"), float(scenario.seconds),
                  f"{scenario.name}.seconds")
    require_equal(
        profile.get("sampleRate"), scenario.sample_rate,
        f"{scenario.name}.sampleRate",
    )
    require_equal(
        profile.get("voices"), scenario.render_voices,
        f"{scenario.name}.renderVoices",
    )
    require_equal(
        profile.get("virtualVoices"), scenario.virtual_voices,
        f"{scenario.name}.virtualVoices",
    )
    require_equal(
        profile.get("pitchCents"), scenario.pitch_cents,
        f"{scenario.name}.pitchCents",
    )
    require_equal(
        profile.get("restartCount"), scenario.restart_count,
        f"{scenario.name}.restartCount",
    )
    require_equal(
        profile.get("deviceCallbackOnly"), scenario.device_callback_only,
        f"{scenario.name}.deviceCallbackOnly",
    )
    require_equal(
        profile.get("minCallbackFrames"), expected_min_callback_frames,
        f"{scenario.name}.minCallbackFrames",
    )
    require_equal(backend.get("valid"), 1, f"{scenario.name}.backend.valid")
    require_equal(backend.get("name"), "WASAPI", f"{scenario.name}.backend.name")
    if not backend.get("name") or not backend.get("deviceName"):
        raise QualificationError(f"{scenario.name} backend identity is empty")
    if not isinstance(backend.get("deviceIdHash"), int) or (
        backend["deviceIdHash"] <= 0
    ):
        raise QualificationError(f"{scenario.name} stable device identity is empty")
    require_equal(
        backend.get("clientSampleRate"), scenario.sample_rate,
        f"{scenario.name}.clientSampleRate",
    )
    require_equal(
        backend.get("internalSampleRate"), scenario.sample_rate,
        f"{scenario.name}.internalSampleRate",
    )
    require_equal(controls.get("deviceCallback"), True,
                  f"{scenario.name}.controls.deviceCallback")
    require_equal(coverage.get("deviceCallback"), "passed",
                  f"{scenario.name}.coverage.deviceCallback")
    if scenario.device_callback_only:
        require_equal(coverage.get("mixPublication"), "disabled-by-profile",
                      f"{scenario.name}.coverage.mixPublication")
    else:
        require_equal(controls.get("mixPublication"), True,
                      f"{scenario.name}.controls.mixPublication")
        require_equal(coverage.get("mixPublication"), "passed",
                      f"{scenario.name}.coverage.mixPublication")

    expected_lifecycle = {
        "init": 0,
        "setup": 0,
        "update": 0,
        "snapshot": 0,
        "term": 0,
        "detach": True,
    }
    require_equal(lifecycle, expected_lifecycle, f"{scenario.name}.lifecycle")
    require_equal(restart.get("status"), "passed", f"{scenario.name}.restart")
    require_equal(
        restart.get("completed"), scenario.restart_count,
        f"{scenario.name}.restart.completed",
    )
    observations = restart.get("observations")
    if not isinstance(observations, list) or len(observations) != scenario.restart_count:
        raise QualificationError(f"{scenario.name}.restart observations are incomplete")
    measurement_start_ticks = measurement.get("startTicks")
    measurement_end_ticks = measurement.get("endTicks")
    measurement_tick_rate = measurement.get("tickRate")
    measurement_start_unix_ns = measurement.get("startUnixNs")
    measurement_end_unix_ns = measurement.get("endUnixNs")
    start_anchor_before = measurement.get("startAnchorTicksBefore")
    start_anchor_after = measurement.get("startAnchorTicksAfter")
    end_anchor_before = measurement.get("endAnchorTicksBefore")
    end_anchor_after = measurement.get("endAnchorTicksAfter")
    for label, value in (
        ("startTicks", measurement_start_ticks),
        ("endTicks", measurement_end_ticks),
        ("tickRate", measurement_tick_rate),
        ("startUnixNs", measurement_start_unix_ns),
        ("endUnixNs", measurement_end_unix_ns),
        ("startAnchorTicksBefore", start_anchor_before),
        ("startAnchorTicksAfter", start_anchor_after),
        ("endAnchorTicksBefore", end_anchor_before),
        ("endAnchorTicksAfter", end_anchor_after),
    ):
        if not isinstance(value, int) or value <= 0:
            raise QualificationError(
                f"{scenario.name}.measurement.{label} is invalid"
            )
    if measurement_end_ticks <= measurement_start_ticks:
        raise QualificationError(f"{scenario.name}.measurement tick order is invalid")
    if measurement_end_unix_ns <= measurement_start_unix_ns:
        raise QualificationError(f"{scenario.name}.measurement UTC order is invalid")
    if not (
        start_anchor_before <= start_anchor_after <= measurement_start_ticks
        <= measurement_end_ticks <= end_anchor_before <= end_anchor_after
    ):
        raise QualificationError(
            f"{scenario.name}.measurement anchor order is invalid"
        )
    start_anchor_ticks = (start_anchor_before + start_anchor_after) // 2
    end_anchor_ticks = (end_anchor_before + end_anchor_after) // 2
    start_anchor_span_ns = round(
        (start_anchor_after - start_anchor_before) * 1_000_000_000
        / measurement_tick_rate
    )
    end_anchor_span_ns = round(
        (end_anchor_after - end_anchor_before) * 1_000_000_000
        / measurement_tick_rate
    )
    if max(start_anchor_span_ns, end_anchor_span_ns) > CLOCK_ANCHOR_MAX_SPAN_NS:
        raise QualificationError(
            f"{scenario.name}.measurement anchor capture was interrupted"
        )
    tick_duration_ns = round(
        (end_anchor_ticks - start_anchor_ticks) * 1_000_000_000
        / measurement_tick_rate
    )
    utc_duration_ns = measurement_end_unix_ns - measurement_start_unix_ns
    if abs(tick_duration_ns - utc_duration_ns) > CLOCK_ANCHOR_TOLERANCE_NS:
        raise QualificationError(
            f"{scenario.name}.measurement clock anchors diverged"
        )

    clock_residual_ns = abs(tick_duration_ns - utc_duration_ns)
    boundary_guard_ns = max(
        CLOCK_BOUNDARY_MIN_GUARD_NS,
        (start_anchor_span_ns + 1) // 2,
        (end_anchor_span_ns + 1) // 2,
        clock_residual_ns,
    )

    def interpolated_unix_ns(ticks: int) -> int:
        return measurement_start_unix_ns + round(
            (ticks - start_anchor_ticks) * utc_duration_ns
            / (end_anchor_ticks - start_anchor_ticks)
        )

    measurement_start_utc = datetime.fromtimestamp(
        interpolated_unix_ns(measurement_start_ticks) / 1_000_000_000,
        timezone.utc,
    )
    measurement_end_utc = datetime.fromtimestamp(
        interpolated_unix_ns(measurement_end_ticks) / 1_000_000_000,
        timezone.utc,
    )
    generation_before = restart.get("generationBefore")
    if not isinstance(generation_before, int) or generation_before < 0:
        raise QualificationError(
            f"{scenario.name}.restart.generationBefore is invalid"
        )
    restart_windows: list[dict[str, Any]] = []
    prior_recovered_ticks: int | None = None
    for index, observation in enumerate(observations):
        if not isinstance(observation, dict):
            raise QualificationError(f"{scenario.name}.restart[{index}] is invalid")
        require_equal(observation.get("callbackObserved"), True,
                      f"{scenario.name}.restart[{index}].callbackObserved")
        require_equal(observation.get("activeVoiceCount"), scenario.render_voices,
                      f"{scenario.name}.restart[{index}].activeVoiceCount")
        require_equal(observation.get("parkedVoiceCount"), scenario.virtual_voices,
                      f"{scenario.name}.restart[{index}].parkedVoiceCount")
        require_equal(
            observation.get("generation"), generation_before + index + 1,
            f"{scenario.name}.restart[{index}].generation",
        )
        suspend_ticks = observation.get("suspendBeginTicks")
        resume_ticks = observation.get("resumeEndTicks")
        callback_begin_ticks = observation.get("callbackBeginTicks")
        recovered_ticks = observation.get("callbackRecoveredTicks")
        if not all(
            isinstance(value, int)
            for value in (
                suspend_ticks, resume_ticks, callback_begin_ticks,
                recovered_ticks,
            )
        ) or not (
            measurement_start_ticks <= suspend_ticks <= resume_ticks
            <= measurement_end_ticks
            and suspend_ticks <= callback_begin_ticks <= recovered_ticks
            <= measurement_end_ticks
        ) or (
            prior_recovered_ticks is not None
            and suspend_ticks <= prior_recovered_ticks
        ):
            raise QualificationError(
                f"{scenario.name}.restart[{index}] tick window is invalid"
            )
        start_ns = interpolated_unix_ns(suspend_ticks)
        resume_end_ns = interpolated_unix_ns(resume_ticks)
        callback_begin_ns = interpolated_unix_ns(callback_begin_ticks)
        callback_recovered_ns = interpolated_unix_ns(recovered_ticks)
        end_ns = callback_recovered_ns + RESTART_EVENT_CORRELATION_TAIL_NS
        restart_windows.append(
            {
                "scenario": scenario.name,
                "observationIndex": index,
                "generation": observation["generation"],
                "startUtc": datetime.fromtimestamp(
                    start_ns / 1_000_000_000, timezone.utc
                ).isoformat(),
                "resumeEndUtc": datetime.fromtimestamp(
                    resume_end_ns / 1_000_000_000, timezone.utc
                ).isoformat(),
                "callbackBeginUtc": datetime.fromtimestamp(
                    callback_begin_ns / 1_000_000_000, timezone.utc
                ).isoformat(),
                "callbackRecoveredUtc": datetime.fromtimestamp(
                    callback_recovered_ns / 1_000_000_000, timezone.utc
                ).isoformat(),
                "endUtc": datetime.fromtimestamp(
                    end_ns / 1_000_000_000, timezone.utc
                ).isoformat(),
                "boundaryGuardNs": boundary_guard_ns,
                "eventCorrelationTailNs": RESTART_EVENT_CORRELATION_TAIL_NS,
            }
        )
        prior_recovered_ticks = recovered_ticks

    for key in (
        "mismatchCount",
        "generationOverflowCount",
        "workerCoordinatorMismatchCount",
        "concurrentCallbackCount",
    ):
        require_equal(identity.get(key), 0, f"{scenario.name}.identity.{key}")
    for section_name, section in (
        ("callbackTiming", callback),
        ("mixPublicationTiming", publication),
    ):
        require_equal(
            section.get("invalidCount"), 0,
            f"{scenario.name}.{section_name}.invalidCount",
        )
        require_equal(
            section.get("deadlineMisses"), 0,
            f"{scenario.name}.{section_name}.deadlineMisses",
        )
    if not isinstance(callback.get("count"), int) or callback["count"] <= 0:
        raise QualificationError(f"{scenario.name} has no device callbacks")
    require_equal(continuity.get("status"), "passed",
                  f"{scenario.name}.callbackContinuity")
    require_equal(
        continuity.get("measurementWindowEdgesIncluded"), True,
        f"{scenario.name}.callbackContinuity.measurementWindowEdgesIncluded",
    )
    for key in ("timelineOverflow", "workerBarrierOverflow"):
        require_equal(storage.get(key), 0, f"{scenario.name}.storage.{key}")
    for key in (
        "arenaOverflowCount",
        "meterRingOverflowCount",
        "noVoiceCount",
        "backpressureCount",
        "notificationDropCount",
        "logicNotificationDropCount",
        "voiceStarvedCount",
        "returnRefusedCount",
        "pendingStartCount",
    ):
        require_equal(semantic.get(key), 0, f"{scenario.name}.semantic.{key}")
    require_equal(
        semantic.get("activeVoiceCount"), scenario.render_voices,
        f"{scenario.name}.semantic.activeVoiceCount",
    )
    require_equal(
        semantic.get("parkedVoiceCount"), scenario.virtual_voices,
        f"{scenario.name}.semantic.parkedVoiceCount",
    )
    require_equal(report.get("captureIntegrityStatus"), "passed",
                  f"{scenario.name}.captureIntegrityStatus")
    require_equal(report.get("coverageStatus"), "passed",
                  f"{scenario.name}.coverageStatus")
    if formal:
        require_equal(report.get("contractDurationStatus"), "passed",
                      f"{scenario.name}.contractDurationStatus")
        require_equal(report.get("qualificationInputStatus"), "ready",
                      f"{scenario.name}.qualificationInputStatus")
        measured_seconds = measurement.get("wallSeconds")
        if (
            not isinstance(measured_seconds, (int, float))
            or not math.isfinite(float(measured_seconds))
            or measured_seconds < scenario.seconds
        ):
            raise QualificationError(
                f"{scenario.name} measured duration is short"
            )
        require_equal(build.get("configuration"), "Release",
                      f"{scenario.name}.build.configuration")
        require_equal(build.get("gitDirty"), False,
                      f"{scenario.name}.build.gitDirty")
        require_equal(build.get("buildInfoGitDirty"), False,
                      f"{scenario.name}.build.buildInfoGitDirty")
        require_equal(build.get("trackedWorktreeClean"), True,
                      f"{scenario.name}.build.trackedWorktreeClean")
    if expected_commit:
        require_equal(build.get("gitCommit"), expected_commit,
                      f"{scenario.name}.build.gitCommit")
        require_equal(build.get("buildInfoGitCommit"), expected_commit,
                      f"{scenario.name}.build.buildInfoGitCommit")

    return {
        "name": scenario.name,
        "status": "passed",
        "report": str(report.get("_reportPath", "")),
        "device": backend.get("deviceName"),
        "deviceIdHash": backend.get("deviceIdHash"),
        "backend": backend.get("name"),
        "sampleRate": scenario.sample_rate,
        "renderVoices": scenario.render_voices,
        "virtualVoices": scenario.virtual_voices,
        "pitchCents": scenario.pitch_cents,
        "restartCount": scenario.restart_count,
        "deviceCallbackOnly": scenario.device_callback_only,
        "seconds": scenario.seconds,
        "callbackCount": callback.get("count"),
        "callbackP99Us": callback.get("p99Us"),
        "callbackMaxUs": callback.get("maxUs"),
        "shortestObservedCallbackPeriodUs": report.get(
            "shortestObservedCallbackPeriodUs"
        ),
        "minimumCallbackFrames": callback.get("minFrames"),
        "expectedCallbackPeriodUs": continuity.get("expectedPeriodUs"),
        "measurementStartUtc": measurement_start_utc.isoformat(),
        "measurementEndUtc": measurement_end_utc.isoformat(),
        "measurementClockResidualNs": clock_residual_ns,
        "measurementClockBoundaryGuardNs": boundary_guard_ns,
        "plannedRestartWindowsUtc": restart_windows,
    }


def validate_process_time_binding(
    result: dict[str, Any], process_started_utc: str, process_ended_utc: str
) -> None:
    process_start = datetime.fromisoformat(process_started_utc)
    process_end = datetime.fromisoformat(process_ended_utc)
    measurement_start = datetime.fromisoformat(str(result["measurementStartUtc"]))
    measurement_end = datetime.fromisoformat(str(result["measurementEndUtc"]))
    if not (
        process_start <= measurement_start <= measurement_end <= process_end
    ):
        raise QualificationError(
            f"{result['name']} measurement is outside its process lifetime"
        )
    prior_end = measurement_start
    for index, window in enumerate(result["plannedRestartWindowsUtc"]):
        start = datetime.fromisoformat(str(window["startUtc"]))
        end = datetime.fromisoformat(str(window["endUtc"]))
        if not (prior_end < start <= end <= measurement_end):
            raise QualificationError(
                f"{result['name']} restart window {index} is not ordered"
            )
        prior_end = end


def external_status(
    path: Path | None,
    kind: str,
    required: dict[str, Any],
    binding: dict[str, Any],
    deadline: float | None = None,
) -> tuple[str, dict[str, Any] | None]:
    if path is None:
        return "not_observed", None
    evidence = read_json(path)
    require_equal(evidence.get("schemaVersion"), 1, f"{path.name}.schemaVersion")
    require_equal(evidence.get("kind"), kind, f"{path.name}.kind")
    for key, expected in required.items():
        require_equal(evidence.get(key), expected, f"{path.name}.{key}")
    evidence_binding = evidence.get("binding")
    if not isinstance(evidence_binding, dict):
        raise QualificationError(f"{path.name}.binding is missing")
    for key, expected in binding.items():
        require_equal(
            evidence_binding.get(key), expected, f"{path.name}.binding.{key}"
        )
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise QualificationError(f"{path.name}.artifacts is empty")
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            raise QualificationError(f"{path.name}.artifacts[{index}] is invalid")
        artifact_path = Path(str(artifact.get("path", ""))).resolve()
        if not artifact_path.is_file() or artifact_path.stat().st_size == 0:
            raise QualificationError(f"external artifact missing: {artifact_path}")
        require_equal(
            sha256_file(artifact_path, deadline),
            artifact.get("sha256"),
            f"{path.name}.artifacts[{index}].sha256",
        )
    if kind == "audible-listening-record":
        if not evidence.get("listener") or not evidence.get("route"):
            raise QualificationError(f"{path.name} listener/route is missing")
    if kind == "device-replacement-record":
        if (
            not isinstance(evidence.get("switchCount"), int)
            or evidence["switchCount"] < 1
            or not evidence.get("beforeDevice")
            or not evidence.get("afterDevice")
            or evidence.get("beforeDevice") == evidence.get("afterDevice")
        ):
            raise QualificationError(f"{path.name} replacement record is incomplete")
    return "passed", evidence


def start_dpc_counter(output: Path, sample_count: int) -> subprocess.Popen[str] | None:
    command = [
        "typeperf",
        r"\Processor Information(_Total)\% DPC Time",
        r"\Processor Information(_Total)\% Interrupt Time",
        r"\Processor Information(_Total)\DPC Rate",
        r"\Processor Information(_Total)\Interrupts/sec",
        "-si",
        "1",
        "-sc",
        str(sample_count),
        "-f",
        "CSV",
        "-o",
        str(output),
        "-y",
    ]
    try:
        return subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return None


def run_command(command: list[str], label: str, deadline: float) -> str:
    remaining = deadline - time.monotonic()
    if remaining <= 0.0:
        raise QualificationError(f"M12 hard time budget exhausted before {label}")
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding=locale.getpreferredencoding(False),
            errors="replace",
            check=False,
            timeout=remaining,
        )
    except subprocess.TimeoutExpired as error:
        raise QualificationError(f"{label} exceeded the M12 hard deadline") from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise QualificationError(
            f"{label} failed with {completed.returncode}: {detail}"
        )
    return completed.stdout


def xperf_path() -> str:
    value = shutil.which("xperf")
    if value is None:
        raise QualificationError("xperf is not available on PATH")
    return value


@dataclass
class TraceSessions:
    kernel_owned: bool = False
    audio_owned: bool = False
    kernel_path: Path | None = None
    audio_path: Path | None = None
    audio_name: str = ""


def active_xperf_loggers(deadline: float) -> str:
    return run_command([xperf_path(), "-loggers"], "xperf logger preflight", deadline)


def parse_xperf_loggers(text: str) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for line in text.splitlines():
        name = re.match(r"\s*Logger Name\s*:\s*(.*?)\s*$", line)
        if name is not None:
            if current is not None:
                entries.append(current)
            current = {"name": name.group(1)}
            continue
        filename = re.match(r"\s*Log Filename\s*:\s*(.*?)\s*$", line)
        if filename is not None and current is not None:
            current["path"] = filename.group(1)
    if current is not None:
        entries.append(current)
    return entries


def normalized_logger_path(value: str | Path) -> str:
    return os.path.normcase(os.path.abspath(os.fspath(value)))


def reconcile_trace_ownership(
    sessions: TraceSessions, deadline: float
) -> None:
    entries = parse_xperf_loggers(active_xperf_loggers(deadline))
    sessions.kernel_owned = sessions.kernel_path is not None and any(
        entry.get("name", "").casefold() == "nt kernel logger"
        and normalized_logger_path(entry.get("path", ""))
        == normalized_logger_path(sessions.kernel_path)
        for entry in entries
    )
    sessions.audio_owned = sessions.audio_path is not None and any(
        entry.get("name", "").casefold() == sessions.audio_name.casefold()
        and normalized_logger_path(entry.get("path", ""))
        == normalized_logger_path(sessions.audio_path)
        for entry in entries
    )


def start_windows_audio_trace(
    output_dir: Path, deadline: float, cleanup_deadline: float
) -> TraceSessions:
    xperf = xperf_path()
    kernel_path = output_dir / "windows-kernel.etl"
    audio_path = output_dir / "windows-audio-provider.etl"
    sessions = TraceSessions(
        kernel_path=kernel_path,
        audio_path=audio_path,
        audio_name=f"HarkenAudioM12-{os.getpid()}-{time.time_ns()}",
    )
    loggers = parse_xperf_loggers(active_xperf_loggers(deadline))
    if any(
        entry.get("name", "").casefold() == "nt kernel logger"
        for entry in loggers
    ):
        raise QualificationError("required xperf logger name is already active")
    sessions.kernel_owned = True
    try:
        run_command(
            [
                xperf,
                "-on", "DPC+INTERRUPT",
                "-f", str(kernel_path),
                "-BufferSize", "64",
                "-MinBuffers", "64",
                "-MaxBuffers", "256",
                "-MaxFile", "1024",
                "-FileMode", "Sequential",
            ],
            "xperf DPC/ISR start",
            deadline,
        )
        sessions.audio_owned = True
        run_command(
            [
                xperf,
                "-start", sessions.audio_name,
                "-on", "ae4bd3be-f36f-45b6-8d21-bdd6fb832853:0x0400000000000000:5",
                "-f", str(audio_path),
                "-BufferSize", "64",
                "-MinBuffers", "16",
                "-MaxBuffers", "64",
                "-MaxFile", "64",
                "-FileMode", "Sequential",
            ],
            "xperf Audio provider start",
            deadline,
        )
    except QualificationError as start_error:
        try:
            reconcile_trace_ownership(sessions, cleanup_deadline)
        except QualificationError as ownership_error:
            raise QualificationError(
                f"{start_error}; trace ownership reconciliation failed: "
                f"{ownership_error}"
            ) from start_error
        cancel_windows_audio_trace(sessions, cleanup_deadline)
        raise
    return sessions


def stop_windows_audio_trace(
    sessions: TraceSessions,
    path: Path,
    deadline: float,
    cleanup_deadline: float,
) -> None:
    try:
        if sessions.audio_owned:
            run_command(
                [xperf_path(), "-stop", sessions.audio_name],
                "xperf Audio provider stop",
                deadline,
            )
            sessions.audio_owned = False
        if sessions.kernel_owned:
            run_command(
                [xperf_path(), "-d", str(path)],
                "xperf kernel stop and save",
                deadline,
            )
            sessions.kernel_owned = False
    except QualificationError:
        cancel_windows_audio_trace(sessions, cleanup_deadline)
        raise


def cancel_windows_audio_trace(
    sessions: TraceSessions, deadline: float
) -> None:
    xperf = shutil.which("xperf")
    if xperf is None:
        return
    if sessions.audio_owned:
        remaining = deadline - time.monotonic()
        try:
            if remaining > 0.0:
                subprocess.run(
                    [xperf, "-stop", sessions.audio_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                    timeout=min(10.0, remaining),
                )
            else:
                subprocess.Popen(
                    [xperf, "-stop", sessions.audio_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
        except (OSError, subprocess.TimeoutExpired):
            pass
        sessions.audio_owned = False
    if sessions.kernel_owned:
        remaining = deadline - time.monotonic()
        try:
            if remaining > 0.0:
                subprocess.run(
                    [xperf, "-stop"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                    timeout=min(10.0, remaining),
                )
            else:
                subprocess.Popen(
                    [xperf, "-stop"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
        except (OSError, subprocess.TimeoutExpired):
            pass
        sessions.kernel_owned = False


def parse_trace_stats(text: str, label: str) -> dict[str, Any]:
    lost_buffers = re.search(r"Total # Lost Buffers\s*:\s*(\d+)", text)
    lost_events = re.search(r"Total # Lost Events\s*:\s*(\d+)", text)
    duration = re.search(
        r"\(\+\s*(\d+):(\d+):(\d+):(\d+(?:\.\d+)?)\)", text
    )
    start = re.search(
        r"Start time \(UTC\)\s*:\s*(\d{4}/\d{2}/\d{2}:\d{2}:\d{2}:\d{2}(?:\.\d+)?)",
        text,
    )
    end = re.search(
        r"End time \(UTC\)\s*:\s*(\d{4}/\d{2}/\d{2}:\d{2}:\d{2}:\d{2}(?:\.\d+)?)",
        text,
    )
    if (
        lost_buffers is None
        or lost_events is None
        or duration is None
        or start is None
        or end is None
    ):
        raise QualificationError(f"{label} trace statistics are incomplete")
    days, hours, minutes, seconds = duration.groups()
    duration_seconds = (
        int(days) * 86400 + int(hours) * 3600 + int(minutes) * 60
        + float(seconds)
    )
    return {
        "lostBuffers": int(lost_buffers.group(1)),
        "lostEvents": int(lost_events.group(1)),
        "durationSeconds": duration_seconds,
        "startUtc": parse_xperf_utc(start.group(1)).isoformat(),
        "endUtc": parse_xperf_utc(end.group(1)).isoformat(),
    }


def classify_audio_glitch_lines(
    glitch_lines: list[str],
    trace_start_utc: str,
    restart_windows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    trace_start = datetime.fromisoformat(trace_start_utc)
    windows = [
        (
            datetime.fromisoformat(str(window["startUtc"])),
            datetime.fromisoformat(str(window["endUtc"])),
            str(window["scenario"]),
            int(window["boundaryGuardNs"]),
        )
        for window in restart_windows
    ]
    for index, (start, end, _, guard_ns) in enumerate(windows):
        if start > end or (index != 0 and start <= windows[index - 1][1]):
            raise QualificationError(
                "planned restart windows overlap or are invalid"
            )
        if guard_ns < 0:
            raise QualificationError("restart window boundary guard is invalid")
    window_event_counts = [0] * len(windows)
    classified = []
    for line in glitch_lines:
        timestamp = re.search(
            r"(OP_EVT_GLITCH_[^,]+),\s*(\d+),", line, re.IGNORECASE
        )
        if timestamp is None:
            raise QualificationError("Windows Audio glitch timestamp is missing")
        event_name = timestamp.group(1).upper()
        timestamp_us = int(timestamp.group(2))
        event_unix_seconds = trace_start.timestamp() + timestamp_us / 1_000_000
        event_utc = datetime.fromtimestamp(
            event_unix_seconds, timezone.utc
        )
        matching_window = next(
            (
                index
                for index, (start, end, _, guard_ns) in enumerate(windows)
                if (
                    start.timestamp() + guard_ns / 1_000_000_000
                    < event_utc.timestamp()
                    < end.timestamp() - guard_ns / 1_000_000_000
                )
            ),
            None,
        )
        expected_type = event_name == EXPECTED_RESTART_GLITCH_EVENT
        restart_coincident = (
            expected_type
            and matching_window is not None
            and window_event_counts[matching_window] == 0
        )
        if restart_coincident:
            window_event_counts[matching_window] += 1
        matching_scenario = (
            windows[matching_window][2]
            if matching_window is not None
            else None
        )
        classified.append(
            {
                "eventName": event_name,
                "timestampUsFromTraceStart": timestamp_us,
                "eventUtc": event_utc.isoformat(),
                "classification": (
                    "restart-window-coincident"
                    if restart_coincident
                    else "unexpected"
                ),
                "classificationReason": (
                    "expected-event-in-unique-guarded-restart-window"
                    if restart_coincident
                    else (
                        "event-type-not-expected-during-restart"
                        if not expected_type
                        else (
                            "outside-restart-window"
                            if matching_window is None
                            else "duplicate-event-in-restart-window"
                        )
                    )
                ),
                "scenario": matching_scenario,
                "restartWindowIndex": matching_window,
                "rawLine": line,
            }
        )
    return classified


def analyze_windows_audio_trace(
    trace_path: Path,
    output_dir: Path,
    binding: dict[str, Any],
    planned_seconds: int,
    deadline: float,
) -> dict[str, Any]:
    xperf = xperf_path()
    audio_events = output_dir / "windows-audio-events.txt"
    dpc_summary = output_dir / "windows-dpc-isr-summary.txt"
    audio_provider_trace = output_dir / "windows-audio-provider.etl"
    audio_trace_stats = output_dir / "windows-audio-trace-stats.txt"
    kernel_trace_stats = output_dir / "windows-kernel-trace-stats.txt"
    run_command(
        [
            xperf,
            "-i", str(audio_provider_trace),
            "-o", str(audio_events),
            "-a", "dumper",
            "-provider", "{ae4bd3be-f36f-45b6-8d21-bdd6fb832853}",
            "-add_fieldnames",
        ],
        "Windows Audio event export",
        deadline,
    )
    run_command(
        [
            xperf,
            "-i", str(trace_path),
            "-o", str(dpc_summary),
            "-a", "dpcisr",
            "-summary",
        ],
        "DPC/ISR summary export",
        deadline,
    )
    audio_stats_text = run_command(
        [xperf, "-i", str(audio_provider_trace), "-a", "tracestats"],
        "Windows Audio trace statistics",
        deadline,
    )
    kernel_stats_text = run_command(
        [xperf, "-i", str(trace_path), "-a", "tracestats"],
        "kernel trace statistics",
        deadline,
    )
    audio_trace_stats.write_text(audio_stats_text, encoding="utf-8")
    kernel_trace_stats.write_text(kernel_stats_text, encoding="utf-8")
    audio_stats = parse_trace_stats(audio_stats_text, "Windows Audio")
    kernel_stats = parse_trace_stats(kernel_stats_text, "kernel")
    for label, stats in (("Windows Audio", audio_stats), ("kernel", kernel_stats)):
        if stats["lostBuffers"] != 0 or stats["lostEvents"] != 0:
            raise QualificationError(f"{label} trace lost events or buffers")
        if stats["durationSeconds"] + 0.5 < planned_seconds:
            raise QualificationError(f"{label} trace did not cover the catalog")
        required_start = datetime.fromisoformat(
            str(binding["scenarioProcessesStartedUtc"])
        )
        required_end = datetime.fromisoformat(
            str(binding["scenarioProcessesEndedUtc"])
        )
        if datetime.fromisoformat(stats["startUtc"]) > required_start:
            raise QualificationError(f"{label} trace started after scenario execution")
        if datetime.fromisoformat(stats["endUtc"]) < required_end:
            raise QualificationError(f"{label} trace ended before scenario execution")
    event_text = audio_events.read_text(encoding="utf-8", errors="replace")
    if "BeginHeader" not in event_text or "EndHeader" not in event_text:
        raise QualificationError("Windows Audio event export schema is missing")
    event_body = event_text.split("EndHeader", 1)[-1]
    glitch_lines = [
        line
        for line in event_body.splitlines()
        if "OP_EVT_GLITCH_" in line.upper()
    ]
    restart_windows = binding.get("plannedRestartWindowsUtc", [])
    if not isinstance(restart_windows, list):
        raise QualificationError("planned restart window binding is invalid")
    classified_glitches = classify_audio_glitch_lines(
        glitch_lines, audio_stats["startUtc"], restart_windows
    )
    restart_coincident_glitches = [
        item
        for item in classified_glitches
        if item["classification"] == "restart-window-coincident"
    ]
    unexpected_glitches = [
        item
        for item in classified_glitches
        if item["classification"] == "unexpected"
    ]
    if not dpc_summary.is_file() or dpc_summary.stat().st_size == 0:
        raise QualificationError("DPC/ISR summary is empty")
    dpc_text = dpc_summary.read_text(encoding="utf-8", errors="replace")
    duration_buckets = [
        (int(upper), int(count))
        for _, upper, count in re.findall(
            r"Elapsed Time, >\s*(\d+) usecs AND <=\s*(\d+) usecs,\s*(\d+)",
            dpc_text,
        )
    ]
    nonempty_upper_bounds = [
        upper for upper, count in duration_buckets if count > 0
    ]
    if not nonempty_upper_bounds:
        raise QualificationError("DPC/ISR duration histogram is missing")
    artifacts = []
    for artifact_path in (
        trace_path,
        audio_provider_trace,
        audio_events,
        dpc_summary,
        audio_trace_stats,
        kernel_trace_stats,
    ):
        if time.monotonic() >= deadline:
            raise QualificationError("M12 hard time budget exhausted during trace analysis")
        if not artifact_path.is_file() or artifact_path.stat().st_size == 0:
            raise QualificationError(f"trace artifact is empty: {artifact_path}")
        artifacts.append(
            {
                "path": str(artifact_path.resolve()),
                "sha256": sha256_file(artifact_path, deadline),
                "bytes": artifact_path.stat().st_size,
            }
        )
    return {
        "schemaVersion": 1,
        "kind": "windows-audio-etw",
        "xrunStatus": "verified",
        "xrunScope": (
            "active-service-windows-with-guarded-planned-restart-exclusions-"
            "plus-fixed-1ms-etw-event-correlation-tail"
        ),
        "audioGlitchCount": len(classified_glitches),
        "rawAudioGlitchCount": len(classified_glitches),
        "restartCoincidentAudioGlitchCount": len(restart_coincident_glitches),
        "unexpectedAudioGlitchCount": len(unexpected_glitches),
        "environmentIdentityStatus": "verified",
        "binding": binding,
        "artifacts": artifacts,
        "dpcIsrStatus": "captured-for-review",
        "maxDpcIsrDurationBucketUpperUs": max(nonempty_upper_bounds),
        "audioProviderGuid": "ae4bd3be-f36f-45b6-8d21-bdd6fb832853",
        "audioProviderSessionStatus": "owned-and-captured",
        "audioTraceStatistics": audio_stats,
        "kernelTraceStatistics": kernel_stats,
        "glitchEventLines": glitch_lines[:100],
        "glitchEventClassifications": classified_glitches[:100],
    }


def summarize_dpc_counter(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"status": "unavailable", "sampleCount": 0}
    raw = path.read_bytes()
    encoding = (
        "utf-16"
        if raw.startswith((b"\xff\xfe", b"\xfe\xff"))
        else "utf-8"
    )
    rows = list(csv.reader(raw.decode(encoding).splitlines()))
    if len(rows) < 2:
        return {"status": "unavailable", "sampleCount": 0}
    values: list[list[float]] = []
    for row in rows[1:]:
        try:
            values.append([float(value) for value in row[1:5]])
        except (ValueError, IndexError):
            continue
    if not values:
        return {"status": "unavailable", "sampleCount": 0}
    labels = ("dpcTimePercent", "interruptTimePercent", "dpcRate", "interruptsPerSecond")
    return {
        "status": "diagnostic_only_not_latency_authority",
        "sampleCount": len(values),
        "maximum": {
            label: max(row[index] for row in values)
            for index, label in enumerate(labels)
        },
        "mean": {
            label: sum(row[index] for row in values) / len(values)
            for index, label in enumerate(labels)
        },
    }


def run_catalog(args: argparse.Namespace) -> dict[str, Any]:
    formal = args.mode == "formal"
    catalog = FORMAL_CATALOG if formal else SMOKE_CATALOG
    planned_seconds = sum(item.seconds for item in catalog)
    outer_seconds = FORMAL_OUTER_SECONDS if formal else planned_seconds + 45
    started = time.monotonic()
    hard_deadline = started + outer_seconds
    deadline = hard_deadline - CLEANUP_RESERVE_SECONDS
    binary = args.binary.resolve()
    if not binary.is_file():
        raise QualificationError(f"qualification binary not found: {binary}")
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if formal and not (
        isinstance(args.expected_commit, str)
        and re.fullmatch(r"[0-9a-f]{40}", args.expected_commit)
    ):
        raise QualificationError(
            "formal mode requires --expected-commit with 40 lowercase hex characters"
        )
    if formal and not args.capture_windows_audio:
        raise QualificationError(
            "formal mode requires live --capture-windows-audio evidence"
        )
    if formal and args.system_audio_evidence is not None:
        raise QualificationError(
            "formal mode does not accept imported system Audio/DPC evidence"
        )
    binary_sha256 = sha256_file(binary, deadline)
    run_id = args.run_id or f"m12-{time.time_ns()}"
    host = {
        "node": platform.node(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "python": platform.python_version(),
    }
    run_started_utc = utc_now()
    print(
        f"M12 {args.mode}: {len(catalog)} scenarios, "
        f"{planned_seconds} measured seconds, {outer_seconds} second hard limit",
        flush=True,
    )
    print(f"M12 run id: {run_id}", flush=True)

    dpc_path = output_dir / "dpc-counters.csv"
    trace_path = output_dir / "windows-audio.etl"
    kernel_trace_path = output_dir / "windows-kernel.etl"
    audio_provider_path = output_dir / "windows-audio-provider.etl"
    manifest_path = output_dir / "m12-manifest.json"
    failure_path = output_dir / "m12-failure.json"
    derived_paths = (
        output_dir / "windows-audio-events.txt",
        output_dir / "windows-dpc-isr-summary.txt",
        output_dir / "windows-audio-trace-stats.txt",
        output_dir / "windows-kernel-trace-stats.txt",
        output_dir / "windows-audio-evidence.json",
    )
    for stale_path in (
        dpc_path,
        trace_path,
        kernel_trace_path,
        audio_provider_path,
        manifest_path,
        failure_path,
        *derived_paths,
        *(
            path
            for scenario in catalog
            for path in (
                output_dir / f"{scenario.name}.json",
                output_dir / f"{scenario.name}.json.timeline.jsonl",
                output_dir / f"{scenario.name}.json.workers.jsonl",
            )
        ),
    ):
        try:
            stale_path.unlink()
        except FileNotFoundError:
            pass
    trace_sessions: TraceSessions | None = None
    if args.capture_windows_audio:
        trace_sessions = start_windows_audio_trace(
            output_dir, deadline, hard_deadline
        )
    dpc_process = start_dpc_counter(dpc_path, outer_seconds)
    results: list[dict[str, Any]] = []
    observed_callback_frames: dict[int, int] = {}
    expected_device: tuple[str, str, int] | None = None
    catalog_complete = False
    try:
        for index, scenario in enumerate(catalog, start=1):
            remaining = deadline - time.monotonic()
            if remaining <= scenario.seconds:
                raise QualificationError("M12 hard time budget exhausted")
            if sha256_file(binary, deadline) != binary_sha256:
                raise QualificationError("qualification binary changed between scenarios")
            report_path = output_dir / f"{scenario.name}.json"
            try:
                report_path.unlink()
            except FileNotFoundError:
                pass
            min_callback_frames = (
                observed_callback_frames.get(scenario.sample_rate, 0)
                if scenario.device_callback_only
                else 64
            )
            if scenario.device_callback_only and min_callback_frames <= 0:
                raise QualificationError(
                    f"{scenario.name} has no same-rate callback observation"
                )
            command = [
                str(binary),
                "--output", str(report_path),
                "--device", "real",
                "--seconds", str(scenario.seconds),
                "--sample-rate", str(scenario.sample_rate),
                "--block-frames", "256",
                "--min-callback-frames", str(min_callback_frames),
                "--voices", str(scenario.render_voices),
                "--virtual-voices", str(scenario.virtual_voices),
                "--pitch-cents", str(scenario.pitch_cents),
                "--restart-count", str(scenario.restart_count),
            ]
            if scenario.device_callback_only:
                command.append("--device-callback-only")
            if not formal:
                command.append("--allow-short-diagnostic")
            print(
                f"[{index}/{len(catalog)}] {scenario.name}: "
                f"fixed {scenario.seconds} seconds",
                flush=True,
            )
            process_started_utc = utc_now()
            process = subprocess.Popen(command)
            try:
                return_code = process.wait(
                    timeout=min(remaining, scenario.seconds + 30)
                )
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise
            process_ended_utc = utc_now()
            if return_code != 0:
                raise QualificationError(
                    f"{scenario.name} exited with {return_code}"
                )
            if sha256_file(binary, deadline) != binary_sha256:
                raise QualificationError("qualification binary changed during a scenario")
            report = read_json(report_path)
            report["_reportPath"] = str(report_path)
            result = validate_report(
                scenario,
                report,
                args.expected_commit,
                formal,
                min_callback_frames,
            )
            validate_process_time_binding(
                result, process_started_utc, process_ended_utc
            )
            result["reportSha256"] = sha256_file(report_path, deadline)
            result["rawArtifacts"] = validate_raw_artifacts(
                report, output_dir, deadline
            )
            result["process"] = {
                "pid": process.pid,
                "startedUtc": process_started_utc,
                "endedUtc": process_ended_utc,
                "binarySha256BeforeAndAfter": "matched",
            }
            observed_frames = result.get("minimumCallbackFrames")
            if not isinstance(observed_frames, int) or observed_frames <= 0:
                raise QualificationError(
                    f"{scenario.name} did not report callback frame coverage"
                )
            prior_frames = observed_callback_frames.get(scenario.sample_rate)
            observed_callback_frames[scenario.sample_rate] = (
                observed_frames
                if prior_frames is None
                else min(prior_frames, observed_frames)
            )
            current_device = (
                str(result["backend"]),
                str(result["device"]),
                int(result["deviceIdHash"]),
            )
            if expected_device is None:
                expected_device = current_device
            elif current_device != expected_device:
                raise QualificationError(
                    f"device changed across scenarios: {expected_device!r} "
                    f"to {current_device!r}"
                )
            results.append(result)
        catalog_complete = True
    finally:
        if dpc_process is not None:
            stop_process(dpc_process, hard_deadline)
        if trace_sessions is not None:
            if catalog_complete:
                stop_windows_audio_trace(
                    trace_sessions, trace_path, deadline, hard_deadline
                )
            else:
                cancel_windows_audio_trace(trace_sessions, hard_deadline)

    if time.monotonic() > deadline:
        raise QualificationError("M12 exceeded its hard time budget")

    device_names = sorted({str(item["device"]) for item in results})
    run_ended_utc = utc_now()
    binding = {
        "runId": run_id,
        "gitCommit": args.expected_commit,
        "binarySha256": binary_sha256,
        "deviceNames": device_names,
        "host": host,
    }
    trace_binding = {
        **binding,
        "catalogStartedUtc": run_started_utc,
        "catalogEndedUtc": run_ended_utc,
        "scenarioProcessesStartedUtc": min(
            item["process"]["startedUtc"] for item in results
        ),
        "scenarioProcessesEndedUtc": max(
            item["process"]["endedUtc"] for item in results
        ),
        "plannedRestartWindowsUtc": [
            window
            for item in results
            for window in item["plannedRestartWindowsUtc"]
        ],
    }
    if args.capture_windows_audio:
        system_evidence = analyze_windows_audio_trace(
            trace_path, output_dir, trace_binding, planned_seconds, deadline
        )
        observed_periods = [
            float(item["expectedCallbackPeriodUs"])
            for item in results
            if isinstance(
                item.get("expectedCallbackPeriodUs"), (int, float)
            )
            and float(item["expectedCallbackPeriodUs"]) > 0.0
        ]
        if not observed_periods:
            raise QualificationError("device callback period evidence is missing")
        dpc_within_period = (
            system_evidence["maxDpcIsrDurationBucketUpperUs"]
            <= min(observed_periods)
        )
        system_evidence["shortestRuntimeExpectedCallbackPeriodUs"] = min(
            observed_periods
        )
        system_evidence["dpcIsrStatus"] = (
            "verified" if dpc_within_period else "failed"
        )
        system_status = (
            "passed"
            if system_evidence["unexpectedAudioGlitchCount"] == 0
            and dpc_within_period
            else "failed"
        )
        write_json_atomic(
            output_dir / "windows-audio-evidence.json", system_evidence
        )
    else:
        system_status, system_evidence = external_status(
            args.system_audio_evidence,
            "windows-audio-etw",
            {
                "xrunStatus": "verified",
                "unexpectedAudioGlitchCount": 0,
                "environmentIdentityStatus": "verified",
                "dpcIsrStatus": "verified",
            },
            binding,
            deadline,
        )
    listening_status, listening_evidence = external_status(
        args.listening_evidence,
        "audible-listening-record",
        {"audibleArtifactStatus": "verified", "audibleArtifactCount": 0},
        binding,
        deadline,
    )
    switch_status, switch_evidence = external_status(
        args.device_switch_evidence,
        "device-replacement-record",
        {"deviceReplacementStatus": "verified", "failureCount": 0},
        binding,
        deadline,
    )
    facets = {
        "runtimeRealDevice": "passed",
        "systemAudioAndDpcActiveService": system_status,
        "audibleArtifacts": listening_status,
        "manualDeviceReplacement": switch_status,
    }
    aggregate_status = aggregate_facet_status(facets)
    dpc_summary = summarize_dpc_counter(dpc_path)
    if time.monotonic() > deadline:
        raise QualificationError("M12 exceeded its hard time budget")
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "harken-runtime-m12-device-qualification",
        "mode": args.mode,
        "runId": run_id,
        "status": aggregate_status,
        "plannedMeasurementSeconds": planned_seconds,
        "hardLimitSeconds": outer_seconds,
        "actualWallSeconds": time.monotonic() - started,
        "binary": str(binary),
        "binarySha256": binary_sha256,
        "host": host,
        "catalogStartedUtc": run_started_utc,
        "catalogEndedUtc": run_ended_utc,
        "_hardDeadlineMonotonic": hard_deadline,
        "deviceNames": device_names,
        "facets": facets,
        "claimBoundary": {
            "runtimeRealDevice": (
                "direct Runtime callback/lifecycle evidence on the named device"
            ),
            "systemAudioAndDpcActiveService": (
                "Windows Audio and DPC evidence during Harken active-service "
                "intervals; guarded planned Suspend-to-first-callback windows "
                "plus a fixed 1 ms ETW event-correlation tail are excluded, "
                "and all raw events remain reported"
            ),
            "dpcCounters": (
                "diagnostic operating-system counters, not latency or xrun authority"
            ),
            "unobservedFacets": (
                "not passed and not inferred from Runtime callback timing"
            ),
        },
        "dpcCounters": dpc_summary,
        "scenarios": results,
        "externalEvidence": {
            "systemAudioAndDpcActiveService": system_evidence,
            "audibleArtifacts": listening_evidence,
            "manualDeviceReplacement": switch_evidence,
        },
    }


def lock_qualification_file(lock_fd: int, exclusive: bool) -> None:
    if sys.platform != "win32":
        return
    import msvcrt

    msvcrt.locking(
        lock_fd, msvcrt.LK_NBLCK if exclusive else msvcrt.LK_UNLCK, 1
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--mode", choices=("smoke", "formal"), default="smoke")
    parser.add_argument("--expected-commit")
    parser.add_argument("--run-id")
    parser.add_argument("--capture-windows-audio", action="store_true")
    parser.add_argument("--system-audio-evidence", type=Path)
    parser.add_argument("--listening-evidence", type=Path)
    parser.add_argument("--device-switch-evidence", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / "m12-manifest.json"
    failure_path = output_dir / "m12-failure.json"
    lock_path = output_dir / ".m12-qualification.lock"
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR)
    try:
        if os.path.getsize(lock_path) == 0:
            os.write(lock_fd, b" ")
        os.lseek(lock_fd, 0, os.SEEK_SET)
        lock_qualification_file(lock_fd, True)
    except OSError:
        os.close(lock_fd)
        print(f"error: another M12 run owns {lock_path}", file=sys.stderr)
        return 2
    try:
        try:
            manifest = run_catalog(args)
        except (OSError, QualificationError, subprocess.TimeoutExpired) as error:
            try:
                manifest_path.unlink()
            except FileNotFoundError:
                pass
            write_json_atomic(
                failure_path,
                {
                    "schemaVersion": SCHEMA_VERSION,
                    "kind": "harken-runtime-m12-device-qualification-failure",
                    "status": "failed",
                    "error": str(error),
                },
            )
            print(f"error: {error}", file=sys.stderr)
            return 2
        hard_deadline = float(manifest.pop("_hardDeadlineMonotonic"))
        if (
            manifest["actualWallSeconds"] > manifest["hardLimitSeconds"]
            or time.monotonic() >= hard_deadline
        ):
            write_json_atomic(
                failure_path,
                {
                    "schemaVersion": SCHEMA_VERSION,
                    "kind": "harken-runtime-m12-device-qualification-failure",
                    "status": "failed",
                    "error": "M12 exceeded its hard time budget before manifest write",
                },
            )
            return 2
        write_json_atomic(manifest_path, manifest)
        if time.monotonic() >= hard_deadline:
            manifest_path.unlink(missing_ok=True)
            write_json_atomic(
                failure_path,
                {
                    "schemaVersion": SCHEMA_VERSION,
                    "kind": "harken-runtime-m12-device-qualification-failure",
                    "status": "failed",
                    "error": "M12 exceeded its hard time budget during manifest write",
                },
            )
            return 2
        print(f"M12 manifest: {manifest_path}", flush=True)
        print(f"M12 status: {manifest['status']}", flush=True)
        return 2 if manifest["status"] == "failed" else 0
    finally:
        os.lseek(lock_fd, 0, os.SEEK_SET)
        lock_qualification_file(lock_fd, False)
        os.close(lock_fd)


if __name__ == "__main__":
    raise SystemExit(main())
