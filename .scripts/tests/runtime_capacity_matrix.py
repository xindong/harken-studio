#!/usr/bin/env python3
"""Run the bounded M11 Runtime capacity characterization."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import platform
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


PROGRAM_DEADLINE_SECONDS = 480.0
SUPERVISOR_DEADLINE_SECONDS = 540.0
CALIBRATION_LIMIT_SECONDS = 20.0
CALIBRATED_START_LIMIT_SECONDS = 360.0
CONTRACT_LIMIT_SECONDS = 60.0
RESOURCE_CHILD_LIMIT_SECONDS = 1.5
END_CONTROL_LIMIT_SECONDS = 6.0
REPORT_RESERVE_SECONDS = 30.0
PILOT_CHILD_LIMIT_SECONDS = 1.5

START_CONTROL = "render.native.48000.stereo.p0.r64"
MONO_PITCH_CONTROL = "pitch_stress.native.48000.mono.p48.r64"
CALIBRATION_GROUPS = (
    (
        "cal.render.r64.rate_paths",
        (
            "render.native.48000.stereo.p0.r64",
            "render.cross.44100_to_48000.stereo.p0.r64",
            "render.cross.48000_to_44100.stereo.p0.r64",
        ),
    ),
    (
        "cal.render.native.48000.stereo.p0.r512",
        ("render.native.48000.stereo.p0.r512",),
    ),
    (
        "cal.pitch.native.48000.stereo.p48.r16",
        ("pitch.native.48000.stereo.p48.r16",),
    ),
    ("cal.pitch_stress.native.48000.mono.p48.r64", (MONO_PITCH_CONTROL,)),
    (
        "cal.scale.r512.v512.resource_update",
        ("virtual.48000.r64.v512",),
    ),
    (
        "cal.stream.mixed.48000.r64.s16",
        ("stream.mixed.48000.r64.v64.s16.pcm48",),
    ),
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_hash(value: Any) -> str:
    data = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def parse_ndjson(text: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        value = json.loads(line)
        if not isinstance(value, dict):
            raise ValueError("worker emitted a non-object record")
        rows.append(value)
    return rows


def run_child(
    argv: list[str],
    timeout_seconds: float,
    program_started: float | None = None,
    environment: dict[str, str] | None = None,
) -> tuple[list[dict[str, Any]], float]:
    if program_started is not None:
        remaining = PROGRAM_DEADLINE_SECONDS - (
            time.perf_counter() - program_started
        )
        if remaining <= 0.0:
            raise TimeoutError("runner exceeded 480 second deadline")
        timeout_seconds = min(timeout_seconds, remaining)
    started = time.perf_counter()
    completed = subprocess.run(
        argv,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds,
        env=environment,
    )
    elapsed = time.perf_counter() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"child failed ({completed.returncode}): {' '.join(argv)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return parse_ndjson(completed.stdout), elapsed


def git_output(source_root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=source_root, text=True, encoding="utf-8"
    ).strip()


def git_bytes(source_root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=source_root)


def source_dirty_identity(source_root: Path) -> tuple[str, str]:
    status = git_output(
        source_root, "status", "--porcelain", "--untracked-files=all"
    )
    digest = hashlib.sha256()
    digest.update(git_bytes(source_root, "diff", "--binary", "HEAD", "--"))
    untracked = git_output(
        source_root, "ls-files", "--others", "--exclude-standard"
    )
    for relative in sorted(filter(None, untracked.splitlines())):
        digest.update(relative.encode("utf-8"))
        path = source_root / relative
        if path.is_file():
            digest.update(sha256_file(path).encode("ascii"))
    return status, digest.hexdigest()


def process_is_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        synchronize = 0x00100000
        query_limited_information = 0x00001000
        still_active = 259
        kernel32 = ctypes.windll.kernel32
        from ctypes import wintypes
        kernel32.OpenProcess.argtypes = [
            wintypes.DWORD, wintypes.BOOL, wintypes.DWORD
        ]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.GetExitCodeProcess.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)
        ]
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(
            synchronize | query_limited_information, False, pid
        )
        if not handle:
            return False
        try:
            exit_code = wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(
                handle, ctypes.byref(exit_code)
            ):
                return False
            return exit_code.value == still_active
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def acquire_lock(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "pid": os.getpid(),
        "created_unix": time.time(),
    }
    for _ in range(2):
        try:
            descriptor = os.open(
                path, os.O_CREAT | os.O_EXCL | os.O_WRONLY
            )
        except FileExistsError:
            owner = None
            for _ in range(5):
                try:
                    owner = load_json(path)
                    break
                except (json.JSONDecodeError, OSError, ValueError):
                    time.sleep(0.02)
            if owner is None:
                try:
                    age = time.time() - path.stat().st_mtime
                except FileNotFoundError:
                    continue
                if age < 1.0:
                    raise RuntimeError(
                        f"capacity runner lock is initializing: {path}"
                    )
                owner_pid = 0
                owner_age = SUPERVISOR_DEADLINE_SECONDS + 61.0
            else:
                owner_pid = int(owner.get("pid", 0))
                owner_age = time.time() - float(
                    owner.get("created_unix", 0.0)
                )
            if (
                process_is_alive(owner_pid)
                and owner_age <= SUPERVISOR_DEADLINE_SECONDS + 60.0
            ):
                raise RuntimeError(
                    f"capacity runner lock is held by pid {owner_pid}: {path}"
                )
            path.unlink(missing_ok=True)
            continue
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(payload, stream, sort_keys=True)
            stream.write("\n")
        return
    raise RuntimeError(f"could not acquire capacity runner lock: {path}")


def release_lock(path: Path) -> None:
    try:
        owner = load_json(path)
        if int(owner.get("pid", 0)) == os.getpid():
            path.unlink(missing_ok=True)
    except FileNotFoundError:
        pass


def supervise_formal_runner(argv: list[str]) -> int:
    if os.name == "nt":
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = [
                ("ReadOperationCount", ctypes.c_ulonglong),
                ("WriteOperationCount", ctypes.c_ulonglong),
                ("OtherOperationCount", ctypes.c_ulonglong),
                ("ReadTransferCount", ctypes.c_ulonglong),
                ("WriteTransferCount", ctypes.c_ulonglong),
                ("OtherTransferCount", ctypes.c_ulonglong),
            ]

        class BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimitInformation),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.argtypes = [
            ctypes.c_void_p, wintypes.LPCWSTR
        ]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD
        ]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = [
            wintypes.HANDLE, wintypes.HANDLE
        ]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        ntdll = ctypes.windll.ntdll
        ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
        ntdll.NtResumeProcess.restype = ctypes.c_long
        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            raise OSError("CreateJobObjectW failed")
        job_open = True
        process: Any = None
        try:
            information = ExtendedLimitInformation()
            information.BasicLimitInformation.LimitFlags = 0x00002000
            if not kernel32.SetInformationJobObject(
                job, 9, ctypes.byref(information), ctypes.sizeof(information)
            ):
                raise OSError("SetInformationJobObject failed")
            process = subprocess.Popen(
                argv,
                creationflags=(
                    subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000004
                ),
            )
            if not kernel32.AssignProcessToJobObject(
                job, wintypes.HANDLE(int(process._handle))
            ):
                process.terminate()
                process.wait(timeout=10.0)
                raise OSError("AssignProcessToJobObject failed")
            if ntdll.NtResumeProcess(
                wintypes.HANDLE(int(process._handle))
            ) != 0:
                process.terminate()
                process.wait(timeout=10.0)
                raise OSError("NtResumeProcess failed")
            try:
                return process.wait(timeout=PROGRAM_DEADLINE_SECONDS)
            except subprocess.TimeoutExpired as error:
                kernel32.CloseHandle(job)
                job_open = False
                process.wait(
                    timeout=SUPERVISOR_DEADLINE_SECONDS
                    - PROGRAM_DEADLINE_SECONDS
                )
                raise TimeoutError(
                    "formal runner process tree exceeded 480 seconds"
                ) from error
        finally:
            if job_open:
                kernel32.CloseHandle(job)
    process = subprocess.Popen(argv, start_new_session=True)
    try:
        return process.wait(timeout=PROGRAM_DEADLINE_SECONDS)
    except subprocess.TimeoutExpired as error:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(
            timeout=SUPERVISOR_DEADLINE_SECONDS - PROGRAM_DEADLINE_SECONDS
        )
        raise TimeoutError(
            "formal runner process group exceeded 480 seconds"
        ) from error


def report_progress(
    started: float,
    stage: str,
    current: int,
    total: int,
    estimated_total: float | None = None,
) -> None:
    elapsed = time.perf_counter() - started
    eta_text = (
        f"{max(0.0, estimated_total - elapsed):.1f}s"
        if estimated_total is not None
        else "pending_calibration"
    )
    remaining = max(0.0, PROGRAM_DEADLINE_SECONDS - elapsed)
    print(
        f"stage={stage} current={current}/{total} elapsed={elapsed:.1f}s "
        f"eta={eta_text} deadline_remaining={remaining:.1f}s",
        file=sys.stderr,
        flush=True,
    )


def enforce_program_deadline(started: float) -> float:
    elapsed = time.perf_counter() - started
    if elapsed > PROGRAM_DEADLINE_SECONDS:
        raise TimeoutError("runner exceeded 480 second deadline")
    return elapsed


def scenario_filename(identifier: str, suffix: str = "") -> str:
    safe = "".join(
        character if character.isalnum() else "_" for character in identifier
    )
    return f"{safe}{suffix}.json"


def timing_cap(row: dict[str, Any]) -> float:
    identifier = str(row["scenario_id"])
    if identifier == MONO_PITCH_CONTROL:
        return 15.0
    if identifier == "pitch_stress.native.48000.stereo.p48.r512":
        return 8.0
    if identifier.startswith("render.native.") and ".stereo.p0.r" in identifier:
        render = int(row["render_capacity"])
        return {64: 6.0, 128: 6.0, 256: 8.0, 512: 15.0}[render]
    if identifier.startswith("pitch.native."):
        return 8.0
    return 6.0


def screen_eligible(catalog: dict[str, Any]) -> bool:
    identifier = str(catalog["scenario_id"])
    return (
        identifier.startswith("pitch.native.")
        or identifier.startswith("pitch_stress.")
    )


def pump(row: dict[str, Any]) -> dict[str, Any]:
    measurements = row.get("measurements")
    if not isinstance(measurements, dict):
        raise ValueError("scenario has no measurements object")
    value = measurements.get("pump")
    if not isinstance(value, dict):
        raise ValueError("scenario has no pump measurement")
    return value


def strong_overload(row: dict[str, Any], catalog: dict[str, Any]) -> bool:
    value = pump(row)
    nominal = 1_000_000.0 * 256.0 / float(catalog["mix_rate"])
    return screen_eligible(catalog) and (
        int(value["nominal_period_exceed_count"]) >= 28
        and float(value["p50_us"]) > nominal * 1.25
    )


def median_repetition_p50(row: dict[str, Any]) -> float:
    repetitions = pump(row).get("repetitions")
    if not isinstance(repetitions, list) or len(repetitions) != 3:
        raise ValueError("control row does not contain three repetitions")
    return statistics.median(float(item["p50_us"]) for item in repetitions)


def measured_operation(row: dict[str, Any], name: str) -> dict[str, Any]:
    measurements = row["measurements"]
    value = measurements.get(name)
    if not isinstance(value, dict):
        return {}
    return value


RESOURCE_MEMORY_FIELDS = (
    "baseline_private_bytes",
    "baseline_working_set_bytes",
    "after_init_private_bytes",
    "after_init_working_set_bytes",
    "init_private_delta_bytes",
    "populated_private_bytes",
    "populated_working_set_bytes",
    "populated_private_delta_bytes",
    "init_working_set_delta_bytes",
    "populated_working_set_delta_bytes",
)


def summarize_resource_observations(
    catalog: dict[str, Any], observations: list[dict[str, Any]]
) -> dict[str, Any]:
    identifier = str(catalog["scenario_id"])
    if not observations:
        raise RuntimeError(f"resource observations missing for {identifier}")
    result: dict[str, Any] = {
        "record": "resource_summary",
        "scenario_id": identifier,
        "ladder": catalog["ladder"],
        "render_capacity": catalog["render_capacity"],
        "virtual_capacity": catalog["virtual_capacity"],
        "logical_capacity": catalog["logical_capacity"],
        "repetitions": len(observations),
        "measurement_status": (
            "measured_complete"
            if all(
                row.get("status") == "complete"
                and row.get("memory_available") is True
                for row in observations
            )
            else "unavailable"
        ),
        "product_memory_status": "memory_review_required",
        "support_claim": False,
    }
    for field in RESOURCE_MEMORY_FIELDS:
        values = [int(row[field]) for row in observations]
        result[field] = {
            "median": statistics.median(values),
            "minimum": min(values),
            "maximum": max(values),
        }
    return result


def workload_family(identifier: str) -> str:
    if identifier.startswith("render.native.") and ".mono." in identifier:
        return "mono_sentinels"
    if identifier.startswith("pitch_stress.") and ".mono." in identifier:
        return "mono_sentinels"
    if identifier.startswith("render.native."):
        return "native_render_ladders"
    if identifier.startswith("render.cross."):
        return "cross_rate_r64"
    if identifier.startswith("pitch.native."):
        return "pitch_curve_r16"
    if identifier.startswith("pitch_stress."):
        return "pitch_stress_render_ladder"
    if identifier.startswith("virtual."):
        return "virtual_population"
    if identifier.startswith("stream."):
        return "warm_streaming"
    raise RuntimeError(f"unclassified timing scenario: {identifier}")


def summarize_workloads(
    timing_records: dict[str, dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    grouped: dict[str, list[dict[str, Any]]] = {}
    candidates: list[dict[str, Any]] = []
    for identifier, record in timing_records.items():
        grouped.setdefault(workload_family(identifier), []).append(record)
        operation = pump(record)
        if operation["m12_screening_status"] == "candidate":
            contract = record["scenario_contract"]
            candidates.append({
                "scenario_id": identifier,
                "render_capacity": contract["render_capacity"],
                "virtual_capacity": contract["virtual_capacity"],
                "logical_capacity": contract["logical_capacity"],
                "product_memory_status": "memory_review_required",
                "support_claim": False,
            })
    summaries: list[dict[str, Any]] = []
    for family in sorted(grouped):
        rows = grouped[family]
        status_counts: dict[str, int] = {}
        nominal_counts: dict[str, int] = {}
        for row in rows:
            operation = pump(row)
            status = str(operation["measurement_status"])
            nominal = str(operation["nominal_period_observation"])
            status_counts[status] = status_counts.get(status, 0) + 1
            nominal_counts[nominal] = nominal_counts.get(nominal, 0) + 1
        summaries.append({
            "record": "workload_summary",
            "family": family,
            "scenario_count": len(rows),
            "measurement_status_counts": status_counts,
            "nominal_period_observation_counts": nominal_counts,
            "m12_compute_candidate_scenarios": sorted(
                row["scenario_id"] for row in rows
                if pump(row)["m12_screening_status"] == "candidate"
            ),
            "support_claim": False,
            "real_device_qualification": False,
        })
    return summaries, sorted(candidates, key=lambda row: row["scenario_id"])


def require_qualified(row: dict[str, Any]) -> None:
    if row.get("environment_status") != "qualified":
        raise RuntimeError(
            f"unqualified benchmark environment for {row.get('scenario_id')}"
        )


def catalog_contract(catalog: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value
        for key, value in catalog.items()
        if key not in {"record", "benchmark", "schema"}
    }


def bind_record_identity(
    record: dict[str, Any],
    catalog: dict[str, Any],
    identity: dict[str, Any],
) -> None:
    record["scenario_contract"] = catalog_contract(catalog)
    record["catalog_entry_hash"] = stable_hash(catalog)
    record["evidence_identity_hash"] = stable_hash(identity)


def validate_operation(
    operation: dict[str, Any],
    expected_samples: int | None,
    allow_dominated: bool,
    operation_name: str,
    nominal_period_us: float,
    formal: bool,
) -> None:
    required = {
        "measurement_status",
        "measurement_integrity_status",
        "determinism_status",
        "nominal_period_observation",
        "m12_screening_status",
    }
    if not required.issubset(operation):
        raise RuntimeError("operation result schema is incomplete")
    status = operation["measurement_status"]
    if status == "not_measured_dominated_by_lower_tier":
        if not allow_dominated or operation_name != "pump":
            raise RuntimeError("unexpected dominated timing result")
        if (
            operation["measurement_integrity_status"] != "not_evaluated"
            or operation["determinism_status"] != "not_evaluated"
            or operation["nominal_period_observation"] != "not_evaluated"
            or operation["m12_screening_status"] != "not_candidate"
        ):
            raise RuntimeError("dominated timing result has false evidence")
        return
    if status == "screened_strong_overload":
        if operation_name != "pump" or int(
            operation.get("sample_count", 0)
        ) != 32:
            raise RuntimeError("strong-overload screen has wrong sample count")
        if (
            operation["measurement_integrity_status"] != "passed"
            or operation["determinism_status"] != "not_evaluated"
            or operation["nominal_period_observation"] != "strong_overload"
            or operation["m12_screening_status"] != "not_candidate"
            or int(operation.get("nominal_period_exceed_count", 0)) < 28
            or float(operation.get("p50_us", 0.0))
                <= nominal_period_us * 1.25
        ):
            raise RuntimeError("strong-overload screen failed integrity")
        return
    if status != "measured_complete":
        raise RuntimeError(f"timing result is not complete: {status}")
    if expected_samples is not None and int(
        operation.get("sample_count", 0)
    ) != expected_samples:
        raise RuntimeError("timing result has wrong sample count")
    if operation["measurement_integrity_status"] != "passed":
        raise RuntimeError("timing result failed integrity")
    if expected_samples == 1200 and (
        operation["determinism_status"] != "passed"
        or len(operation.get("repetitions", [])) != 3
    ):
        raise RuntimeError("formal timing result failed determinism audit")
    repetitions = operation.get("repetitions", [])
    if formal and repetitions:
        if (
            abs(float(operation["p99_us"]) - max(
                float(item["p99_us"]) for item in repetitions
            )) > 0.001
            or abs(float(operation["max_us"]) - max(
                float(item["max_us"]) for item in repetitions
            )) > 0.001
            or int(operation["nominal_period_exceed_count"])
                != sum(
                    int(item["nominal_period_exceed_count"])
                    for item in repetitions
                )
        ):
            raise RuntimeError("formal repetition summary is inconsistent")
    if operation_name == "pump":
        exceed_count = int(operation["nominal_period_exceed_count"])
        expected_nominal = (
            "zero_exceeds" if exceed_count == 0 else "one_or_more_exceeds"
        )
        if operation["nominal_period_observation"] != expected_nominal:
            raise RuntimeError("nominal-period observation is inconsistent")
        candidate = (
            formal
            and len(repetitions) == 3
            and all(
                float(item["p99_us"]) <= nominal_period_us * 0.70
                and float(item["max_us"]) < nominal_period_us
                and int(item["nominal_period_exceed_count"]) == 0
                for item in repetitions
            )
        )
        expected_candidate = "candidate" if candidate else "not_candidate"
        if operation["m12_screening_status"] != expected_candidate:
            raise RuntimeError("M12 screening status is inconsistent")
    elif (
        operation["nominal_period_observation"] != "not_applicable"
        or operation["m12_screening_status"] != "not_applicable"
    ):
        raise RuntimeError("logic operation carries an audio deadline claim")


def validate_timing_record(
    record: dict[str, Any],
    catalog: dict[str, Any],
    identity: dict[str, Any],
    formal: bool,
) -> None:
    identifier = str(catalog["scenario_id"])
    if (
        record.get("record") != "scenario"
        or record.get("scenario_id") != identifier
        or record.get("schema") != "m11-null-v2"
        or record.get("catalog_entry_hash") != stable_hash(catalog)
        or record.get("evidence_identity_hash") != stable_hash(identity)
        or record.get("scenario_contract") != catalog_contract(catalog)
        or record.get("status") != "complete"
        or record.get("support_claim") is not False
        or record.get("real_device_qualification") is not False
    ):
        raise RuntimeError(f"timing record identity mismatch: {identifier}")
    operation = pump(record)
    dominated = operation.get("measurement_status") == (
        "not_measured_dominated_by_lower_tier"
    )
    if (
        operation.get("measurement_status") == "screened_strong_overload"
        and not screen_eligible(catalog)
    ):
        raise RuntimeError(f"invalid screen workload: {identifier}")
    if formal and not dominated:
        require_qualified(record)
        if (
            record.get("source_commit") != identity["commit"]
            or record.get("source_dirty") != "0"
            or int(record.get("pinned_cpu_index", -1)) < 0
            or int(record.get("hardware_threads", 0)) < 1
            or record.get("processor_identifier") in {None, "", "unknown"}
        ):
            raise RuntimeError(
                f"timing host/source identity mismatch: {identifier}"
            )
    dominated_allowed = identifier.startswith(
        "pitch_stress.native.48000.stereo.p48.r"
    )
    if dominated and not dominated_allowed:
        raise RuntimeError(f"invalid dominance family: {identifier}")
    expected_operations = {
        "render": {"pump"},
        "virtual": {"pump", "update"},
        "streaming": {"pump", "sync_serve", "update"},
    }[str(catalog["kind"])]
    measurements = record.get("measurements")
    if not isinstance(measurements, dict) or set(measurements) != (
        {"pump"} if dominated else expected_operations
    ):
        raise RuntimeError(f"operation coverage mismatch: {identifier}")
    expected_samples = 1200 if formal else 16
    nominal_period_us = (
        1_000_000.0 * 256.0 / float(catalog["mix_rate"])
    )
    validate_operation(
        operation, expected_samples, dominated_allowed, "pump",
        nominal_period_us, formal,
    )
    for name in expected_operations - {"pump"}:
        validate_operation(
            measurements[name], expected_samples, False, name,
            nominal_period_us, formal,
        )
    if formal and operation["measurement_status"] == "measured_complete":
        pump_warmups = tuple(
            int(item.get("warmup_blocks", 0))
            for item in operation["repetitions"]
        )
        if (
            len(pump_warmups) != 3
            or len(set(pump_warmups)) != 1
            or pump_warmups[0] != 128
        ):
            raise RuntimeError(
                f"formal warmup lifecycles are inconsistent: {identifier}"
            )
        for name in expected_operations - {"pump"}:
            operation_warmups = tuple(
                int(item.get("warmup_blocks", 0))
                for item in measurements[name]["repetitions"]
            )
            if operation_warmups != pump_warmups:
                raise RuntimeError(
                    f"operation warmup vectors differ: {identifier}"
                )


def validate_dominance_policy(
    timing_records: dict[str, dict[str, Any]]
) -> None:
    stress = sorted(
        (
            record for identifier, record in timing_records.items()
            if identifier.startswith(
                "pitch_stress.native.48000.stereo.p48.r"
            )
        ),
        key=lambda record: int(
            record["scenario_contract"]["render_capacity"]
        ),
    )
    lower_screened = False
    for record in stress:
        status = pump(record)["measurement_status"]
        if status == "not_measured_dominated_by_lower_tier" and not (
            lower_screened
        ):
            raise RuntimeError(
                "dominated Pitch stress row has no screened lower tier"
            )
        if status == "screened_strong_overload":
            lower_screened = True


def validate_resource_record(
    record: dict[str, Any],
    catalog: dict[str, Any],
    identity: dict[str, Any],
    repetition: int,
    formal: bool,
) -> None:
    identifier = str(catalog["scenario_id"])
    if (
        record.get("record") != "resource"
        or record.get("scenario_id") != identifier
        or record.get("schema") != "m11-null-v2"
        or record.get("catalog_entry_hash") != stable_hash(catalog)
        or record.get("evidence_identity_hash") != stable_hash(identity)
        or record.get("scenario_contract") != catalog_contract(catalog)
        or int(record.get("repetition", 0)) != repetition
        or record.get("status") != "complete"
        or record.get("support_claim") is not False
    ):
        raise RuntimeError(f"resource record identity mismatch: {identifier}")
    if formal and record.get("memory_available") is not True:
        raise RuntimeError(f"resource memory unavailable: {identifier}")
    if (
        int(record.get("resolved_render_pool_capacity", -1))
            != int(catalog["render_capacity"])
        or int(record.get("resolved_ledger_capacity", -1))
            != int(catalog["logical_capacity"])
        or int(record.get("resolved_park_record_capacity", -1))
            != int(catalog["logical_capacity"])
    ):
        raise RuntimeError(f"resource resolved shape mismatch: {identifier}")


def estimate_calibrated_seconds(
    catalogs: list[dict[str, Any]],
    pilots: dict[str, dict[str, Any]],
    child_elapsed: dict[str, float],
    resource_elapsed: float,
    calibration_elapsed: float,
) -> tuple[float, dict[str, dict[str, Any]]]:
    native64 = pump(pilots[START_CONTROL])
    native512 = pump(pilots["render.native.48000.stereo.p0.r512"])
    cross = max(
        float(pump(pilots[name])["mean_us"])
        for name in (
            "render.cross.44100_to_48000.stereo.p0.r64",
            "render.cross.48000_to_44100.stereo.p0.r64",
        )
    )
    pitch = pump(pilots["pitch.native.48000.stereo.p48.r16"])
    virtual = pilots["virtual.48000.r64.v512"]
    streaming = pilots["stream.mixed.48000.r64.v64.s16.pcm48"]
    maximum_overhead = 0.0
    for identifier, elapsed in child_elapsed.items():
        per_block_us = float(pump(pilots[identifier])["mean_us"])
        per_block_us += float(
            measured_operation(pilots[identifier], "update").get(
                "mean_us", 0.0
            )
        )
        per_block_us += float(
            measured_operation(pilots[identifier], "sync_serve").get(
                "mean_us", 0.0
            )
        )
        measured = per_block_us * 48.0 / 1e6
        maximum_overhead = max(maximum_overhead, max(0.0, elapsed - measured))

    native_slope = max(
        0.0,
        (float(native512["mean_us"]) - float(native64["mean_us"]))
        / (512.0 - 64.0),
    )
    native_intercept = max(
        0.0, float(native64["mean_us"]) - native_slope * 64.0
    )
    pitch_per_voice = float(pitch["mean_us"]) / 16.0
    update_upper = float(measured_operation(virtual, "update")["mean_us"])
    stream_pump_upper = float(pump(streaming)["mean_us"])
    stream_service_upper = float(
        measured_operation(streaming, "sync_serve")["mean_us"]
    )
    stream_update_upper = float(
        measured_operation(streaming, "update")["mean_us"]
    )

    decisions: dict[str, dict[str, Any]] = {}
    timing_total = 0.0
    predicted_screened_pitch_stress = False
    for catalog in catalogs:
        identifier = str(catalog["scenario_id"])
        is_pitch_stress = identifier.startswith(
            "pitch_stress.native.48000.stereo.p48.r"
        )
        if is_pitch_stress and predicted_screened_pitch_stress:
            fallback_budget = (
                PILOT_CHILD_LIMIT_SECONDS + timing_cap(catalog)
            )
            decisions[identifier] = {
                "calibration_expected_dominance": True,
                "fallback_pilot_budget_seconds": PILOT_CHILD_LIMIT_SECONDS,
                "fallback_formal_budget_seconds": timing_cap(catalog),
                "total_predicted_seconds": fallback_budget,
                "runtime_condition": (
                    "lower identical-family tier must actually satisfy the "
                    "strong-overload rule; the estimate still reserves both "
                    "the higher-tier pilot and formal hard caps"
                ),
            }
            timing_total += fallback_budget
            continue
        render = int(catalog["render_capacity"])
        native_model = native_intercept + native_slope * render
        if identifier == MONO_PITCH_CONTROL:
            distribution = pump(pilots[MONO_PITCH_CONTROL])
            pump_model = float(distribution["mean_us"])
            scale = 1.0
        elif identifier.startswith("render.cross."):
            pump_model = cross
            distribution = max(
                (
                    pump(pilots["render.cross.44100_to_48000.stereo.p0.r64"]),
                    pump(pilots["render.cross.48000_to_44100.stereo.p0.r64"]),
                ),
                key=lambda value: float(value["mean_us"]),
            )
            scale = 1.0
        elif int(catalog.get("pitch_semitones", 0)) != 0:
            pump_model = max(native_model, pitch_per_voice * render)
            distribution = pitch
            scale = render / 16.0
        elif catalog["kind"] == "streaming":
            pump_model = max(float(native64["mean_us"]), stream_pump_upper)
            distribution = pump(streaming)
            scale = 1.0
        else:
            pump_model = native_model
            distribution = native64 if render <= 64 else native512
            scale = render / (64.0 if render <= 64 else 512.0)

        extra_per_block_us = 0.0
        if catalog["kind"] == "virtual":
            extra_per_block_us = update_upper
        elif catalog["kind"] == "streaming":
            extra_per_block_us = stream_service_upper + stream_update_upper
        pilot_prediction = maximum_overhead + 1.25 * (
            pump_model + extra_per_block_us
        ) * 48.0 / 1e6
        formal_prediction = 3.0 * maximum_overhead + 1.25 * (
            pump_model + extra_per_block_us
        ) * 1584.0 / 1e6
        if pilot_prediction > PILOT_CHILD_LIMIT_SECONDS:
            raise RuntimeError(
                f"calibration predicts {identifier} pilot needs "
                f"{pilot_prediction:.3f}s but its cap is "
                f"{PILOT_CHILD_LIMIT_SECONDS:.3f}s"
            )
        cap = timing_cap(catalog)
        predicted_p15_625 = (
            float(distribution["p15_625_us"]) * scale
        )
        predicted_p50 = float(distribution["p50_us"]) * scale
        nominal = 1_000_000.0 * 256.0 / float(catalog["mix_rate"])
        expected_screen = (
            screen_eligible(catalog)
            and formal_prediction > cap
            and predicted_p15_625 > nominal
            and predicted_p50 > nominal * 1.25
        )
        if formal_prediction > cap and not expected_screen:
            raise RuntimeError(
                f"calibration predicts {identifier} formal worker needs "
                f"{formal_prediction:.3f}s "
                f"but its cap is {cap:.3f}s"
            )
        predicted = (
            pilot_prediction + cap
            if expected_screen
            else pilot_prediction + formal_prediction
        )
        if not expected_screen and formal_prediction > cap:
            raise RuntimeError(
                f"calibration predicts {identifier} formal path exceeds "
                f"its {cap:.3f}s cap"
            )
        decisions[identifier] = {
            "pilot_predicted_seconds": pilot_prediction,
            "pilot_hard_cap_seconds": PILOT_CHILD_LIMIT_SECONDS,
            "formal_predicted_seconds": formal_prediction,
            "formal_hard_cap_seconds": cap,
            "calibration_expected_screen": expected_screen,
            "total_predicted_seconds": predicted,
        }
        if is_pitch_stress and expected_screen:
            predicted_screened_pitch_stress = True
        timing_total += predicted

    resource_prediction = resource_elapsed * 1.25 + maximum_overhead
    if resource_prediction > RESOURCE_CHILD_LIMIT_SECONDS:
        raise RuntimeError(
            "calibration predicts resource child exceeds its 1.5 second cap"
        )
    total = (
        calibration_elapsed
        + CONTRACT_LIMIT_SECONDS
        + 36.0 * resource_prediction
        + timing_total
        + END_CONTROL_LIMIT_SECONDS
        + REPORT_RESERVE_SECONDS
    )
    return total, decisions


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--source-root", required=True)
    result.add_argument("--binary", required=True)
    result.add_argument("--output", required=True)
    result.add_argument("--mode", choices=("smoke", "formal"), required=True)
    result.add_argument("--contract-exe")
    result.add_argument("--build-evidence")
    result.add_argument("--resume", action="store_true")
    result.add_argument("--calibrate-only", action="store_true")
    result.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    return result


def main() -> int:
    args = parser().parse_args()
    if args.mode == "formal" and not args.worker:
        supervisor_argv = [
            sys.executable,
            str(Path(__file__).resolve()),
            *sys.argv[1:],
            "--worker",
        ]
        print(
            "stage=supervisor current=0/1 elapsed=0.0s "
            "eta=pending_calibration deadline_remaining=540.0s",
            file=sys.stderr,
            flush=True,
        )
        return supervise_formal_runner(supervisor_argv)
    source_root = Path(args.source_root).resolve()
    binary = Path(args.binary).resolve()
    output = Path(args.output).resolve()
    output.relative_to(source_root)
    if not binary.is_file():
        raise FileNotFoundError(binary)
    contract_exe = (
        Path(args.contract_exe).resolve() if args.contract_exe else None
    )
    if contract_exe is not None and not contract_exe.is_file():
        raise FileNotFoundError(contract_exe)
    output.mkdir(parents=True, exist_ok=True)
    lock_path = output / "runner.lock"
    global_lock_path = source_root / ".temp" / "m11-capacity-runner.lock"
    global_lock_path.parent.mkdir(parents=True, exist_ok=True)
    acquire_lock(lock_path)
    try:
        acquire_lock(global_lock_path)
    except Exception:
        release_lock(lock_path)
        raise
    started = time.perf_counter()
    try:
        catalog_rows, _ = run_child(
            [str(binary), "--list"], 5.0, started
        )
        resource_rows, _ = run_child(
            [str(binary), "--list-resources"], 5.0, started
        )
        catalogs = [row for row in catalog_rows if row["record"] == "catalog"]
        resources = [
            row for row in resource_rows if row["record"] == "resource_catalog"
        ]
        if len(catalogs) != 35 or len(resources) != 12:
            raise RuntimeError("compiled scenario catalog is incomplete")
        by_id = {str(row["scenario_id"]): row for row in catalogs}
        resource_by_id = {
            str(row["scenario_id"]): row for row in resources
        }
        commit = git_output(source_root, "rev-parse", "HEAD")
        dirty_text, dirty_diff_hash = source_dirty_identity(source_root)
        build_evidence_path = (
            Path(args.build_evidence).resolve() if args.build_evidence else None
        )
        build_evidence_hash = (
            sha256_file(build_evidence_path)
            if build_evidence_path is not None
            else None
        )
        identity = {
            "record": "manifest",
            "schema": "m11-runner-v1",
            "commit": commit,
            "source_dirty": bool(dirty_text),
            "dirty_status_hash": hashlib.sha256(
                dirty_text.encode("utf-8")
            ).hexdigest(),
            "dirty_diff_hash": dirty_diff_hash,
            "binary_sha256": sha256_file(binary),
            "runner_sha256": sha256_file(Path(__file__).resolve()),
            "catalog_hash": stable_hash(catalog_rows + resource_rows),
            "fixture_contract_hash": stable_hash({
                "catalog": catalog_rows,
                "resource_catalog": resource_rows,
                "block_frames": 256,
                "render_worker_count": 0,
                "arena_bytes": 16 << 20,
                "command_queue_depth": 2048,
                "notification_queue_depth": 2048,
                "max_streams": 16,
                "stream_ring_frames": 16384,
                "source_profile": "deterministic_generated_v1",
            }),
            "host": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "processor": platform.processor()
                    or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
                "logical_cpu_count": os.cpu_count(),
            },
            "binary": str(binary),
            "contract_binary_sha256": (
                sha256_file(contract_exe)
                if contract_exe is not None
                else None
            ),
            "mode": args.mode,
            "build_evidence_sha256": build_evidence_hash,
        }
        worker_environment = os.environ.copy()
        worker_environment["HARKEN_M11_SOURCE_COMMIT"] = commit
        worker_environment["HARKEN_M11_SOURCE_DIRTY"] = (
            "1" if dirty_text else "0"
        )
        manifest_path = output / "manifest.json"
        if manifest_path.exists():
            if not args.resume:
                raise FileExistsError(
                    "output already has a manifest; pass --resume"
                )
            if load_json(manifest_path) != identity:
                raise RuntimeError("resume identity does not match")
        else:
            atomic_json(manifest_path, identity)
        if args.mode == "formal" and dirty_text:
            raise RuntimeError("formal mode requires a clean commit")
        if args.mode == "formal":
            if contract_exe is None:
                raise RuntimeError("formal mode requires --contract-exe")
            if build_evidence_path is None or not build_evidence_path.is_file():
                raise RuntimeError("formal mode requires --build-evidence")
            build_evidence = load_json(build_evidence_path)
            if (
                build_evidence.get("commit") != commit
                or build_evidence.get("binary_sha256")
                != identity["binary_sha256"]
                or build_evidence.get("configuration") != "Release"
                or build_evidence.get("project")
                != "harken_runtime_capacity_benchmark"
                or build_evidence.get("contract_binary_sha256")
                != identity["contract_binary_sha256"]
                or build_evidence.get("contract_configuration") != "Debug"
                or build_evidence.get("contract_project")
                != "harken_runtime_tests"
                or build_evidence.get("status") != "success"
            ):
                raise RuntimeError("formal build evidence does not match")

        authoritative = output / "authoritative"
        authoritative.mkdir(exist_ok=True)
        calibration: dict[str, Any] | None = None
        decisions: dict[str, dict[str, Any]] = {}
        if args.mode == "formal" or args.calibrate_only:
            calibration_started = time.perf_counter()
            pilots: dict[str, dict[str, Any]] = {}
            child_elapsed: dict[str, float] = {}
            calibration_child = 0
            calibration_child_total = sum(
                len(identifiers) for _, identifiers in CALIBRATION_GROUPS
            ) + 1
            for group_name, identifiers in CALIBRATION_GROUPS:
                for identifier in identifiers:
                    calibration_child += 1
                    report_progress(
                        started,
                        f"calibration:{group_name}",
                        calibration_child,
                        calibration_child_total,
                    )
                    rows, elapsed = run_child(
                        [str(binary), "--scenario", identifier, "--pilot"],
                        PILOT_CHILD_LIMIT_SECONDS,
                        started,
                        worker_environment,
                    )
                    pilots[identifier] = rows[-1]
                    atomic_json(
                        authoritative / "calibration-pilots" / scenario_filename(identifier),
                        {"identity": identity, "worker_record": pilots[identifier],
                         "wall_seconds": elapsed},
                    )
                    if args.mode == "formal":
                        require_qualified(pilots[identifier])
                    child_elapsed[identifier] = elapsed
                    report_progress(
                        started, "calibration_complete",
                        calibration_child, calibration_child_total,
                    )
            resource_id = "resource.scale.r512.v512"
            report_progress(
                started, "calibration:resource", calibration_child_total,
                calibration_child_total,
            )
            resource_probe_rows, resource_elapsed = run_child(
                [str(binary), "--resource", resource_id],
                RESOURCE_CHILD_LIMIT_SECONDS,
                started,
                worker_environment,
            )
            report_progress(
                started, "calibration_complete", calibration_child_total,
                calibration_child_total,
            )
            calibration_elapsed = time.perf_counter() - calibration_started
            if calibration_elapsed > CALIBRATION_LIMIT_SECONDS:
                raise RuntimeError("calibration exceeded 20 seconds")
            predicted, decisions = estimate_calibrated_seconds(
                catalogs,
                pilots,
                child_elapsed,
                resource_elapsed,
                calibration_elapsed,
            )
            if predicted > CALIBRATED_START_LIMIT_SECONDS:
                raise RuntimeError(
                    f"calibrated full run estimate {predicted:.3f}s "
                    "exceeds 360 second start gate"
                )
            calibration = {
                "record": "calibration",
                "groups": [name for name, _ in CALIBRATION_GROUPS],
                "logical_probe_group_count": len(CALIBRATION_GROUPS),
                "timing_child_measurement_count": calibration_child_total - 1,
                "resource_child_measurement_count": 1,
                "elapsed_seconds": calibration_elapsed,
                "predicted_total_seconds": predicted,
                "program_deadline_seconds": PROGRAM_DEADLINE_SECONDS,
                "external_deadline_seconds": SUPERVISOR_DEADLINE_SECONDS,
                "decisions": decisions,
            }
            atomic_json(authoritative / "calibration.json", calibration)
            print(
                f"calibrated expected={predicted:.1f}s "
                "program<=480s supervisor<=540s",
                flush=True,
            )
            if args.calibrate_only:
                summary = {
                    "record": "summary",
                    "status": "calibration_complete",
                    "predicted_total_seconds": predicted,
                    "elapsed_seconds": time.perf_counter() - started,
                }
                atomic_json(output / "summary.json", summary)
                return 0

        contract_record: dict[str, Any] | None = None
        if contract_exe is not None:
            report_progress(
                started, "capacity_contract", 1, 1,
                calibration["predicted_total_seconds"]
                if calibration is not None else None,
            )
            contract_started = time.perf_counter()
            contract_timeout = min(
                CONTRACT_LIMIT_SECONDS,
                PROGRAM_DEADLINE_SECONDS - (
                    time.perf_counter() - started
                ),
            )
            if contract_timeout <= 0.0:
                raise TimeoutError("runner exceeded 480 second deadline")
            completed = subprocess.run(
                [
                    str(contract_exe),
                    "--gtest_filter=VoiceCapacity.ScaleTiersKeepIndependentBoundariesAtomic",
                    "--gtest_color=no",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=contract_timeout,
            )
            contract_record = {
                "record": "capacity_contract",
                "test": (
                    "VoiceCapacity."
                    "ScaleTiersKeepIndependentBoundariesAtomic"
                ),
                "exit_code": completed.returncode,
                "elapsed_seconds": time.perf_counter() - contract_started,
                "status": "passed" if completed.returncode == 0 else "failed",
                "stdout_sha256": hashlib.sha256(
                    completed.stdout.encode("utf-8")
                ).hexdigest(),
                "contract_binary_sha256": identity[
                    "contract_binary_sha256"
                ],
                "evidence_identity_hash": stable_hash(identity),
            }
            atomic_json(authoritative / "capacity_contract.json", contract_record)
            if completed.returncode != 0:
                raise RuntimeError("capacity contract failed")
            report_progress(
                started, "capacity_contract_complete", 1, 1,
                calibration["predicted_total_seconds"]
                if calibration is not None else None,
            )

        resource_plan = resources if args.mode == "formal" else [
            resource_by_id["resource.scale.r512.v512"]
        ]
        repetitions = 3 if args.mode == "formal" else 1
        resource_total = len(resource_plan) * repetitions
        resource_current = 0
        for resource in resource_plan:
            identifier = str(resource["scenario_id"])
            for repetition in range(repetitions):
                resource_current += 1
                path = authoritative / scenario_filename(
                    identifier, f".rep{repetition + 1}"
                )
                if path.exists() and args.resume:
                    resumed_resource = load_json(path)
                    validate_resource_record(
                        resumed_resource, resource, identity,
                        repetition + 1, args.mode == "formal",
                    )
                    continue
                report_progress(
                    started, f"resource:{identifier}", resource_current,
                    resource_total,
                    calibration["predicted_total_seconds"]
                    if calibration is not None else None,
                )
                rows, elapsed = run_child(
                    [str(binary), "--resource", identifier],
                    RESOURCE_CHILD_LIMIT_SECONDS,
                    started,
                    worker_environment,
                )
                record = rows[-1]
                record["repetition"] = repetition + 1
                record["wall_seconds"] = elapsed
                bind_record_identity(record, resource, identity)
                validate_resource_record(
                    record, resource, identity, repetition + 1,
                    args.mode == "formal",
                )
                atomic_json(path, record)
                report_progress(
                    started, "resource_complete", resource_current,
                    resource_total,
                    calibration["predicted_total_seconds"]
                    if calibration is not None else None,
                )

        timing_plan = catalogs if args.mode == "formal" else [
            by_id[START_CONTROL],
            by_id["pitch.native.48000.stereo.p48.r16"],
            by_id["virtual.48000.r64.v512"],
            by_id["stream.mixed.48000.r64.v64.s16.pcm48"],
        ]
        if args.mode == "formal":
            timing_plan = [by_id[START_CONTROL]] + [
                row for row in timing_plan
                if row["scenario_id"] != START_CONTROL
            ]
        timing_records: dict[str, dict[str, Any]] = {}
        screened_family = False
        for index, catalog in enumerate(timing_plan, 1):
            identifier = str(catalog["scenario_id"])
            path = authoritative / scenario_filename(identifier)
            if path.exists() and args.resume:
                resumed = load_json(path)
                validate_timing_record(
                    resumed, catalog, identity, args.mode == "formal"
                )
                timing_records[identifier] = resumed
                if identifier.startswith(
                    "pitch_stress.native.48000.stereo.p48.r"
                ) and pump(resumed).get("measurement_status") == (
                    "screened_strong_overload"
                ):
                    screened_family = True
                continue
            if (
                args.mode == "formal"
                and screened_family
                and identifier.startswith("pitch_stress.native.48000.stereo.p48.r")
            ):
                record = {
                    "benchmark": "runtime-capacity-matrix",
                    "schema": "m11-null-v2",
                    "record": "scenario",
                    "scenario_id": identifier,
                    "mode": "formal_inference",
                    "kind": catalog["kind"],
                    "status": "complete",
                    "environment_status": "not_applicable",
                    "functional_contract_status": "separate_suite",
                    "support_claim": False,
                    "real_device_qualification": False,
                    "measurements": {
                        "pump": {
                            "measurement_status":
                                "not_measured_dominated_by_lower_tier",
                            "measurement_integrity_status": "not_evaluated",
                            "determinism_status": "not_evaluated",
                            "nominal_period_observation": "not_evaluated",
                            "m12_screening_status": "not_candidate",
                        }
                    },
                    "inference": True,
                }
            else:
                report_progress(
                    started, f"timing:{identifier}", index,
                    len(timing_plan),
                    calibration["predicted_total_seconds"]
                    if calibration is not None else None,
                )
                if args.mode == "formal":
                    pilot_rows, _ = run_child(
                        [str(binary), "--scenario", identifier, "--pilot"],
                        PILOT_CHILD_LIMIT_SECONDS,
                        started,
                        worker_environment,
                    )
                    pilot = pilot_rows[-1]
                    require_qualified(pilot)
                    bind_record_identity(pilot, catalog, identity)
                    atomic_json(
                        authoritative / scenario_filename(identifier, ".pilot"), pilot
                    )
                    if screen_eligible(catalog) and strong_overload(
                        pilot, catalog
                    ):
                        require_qualified(pilot)
                        record = pilot
                        operation = pump(record)
                        operation["measurement_status"] = (
                            "screened_strong_overload"
                        )
                        operation["nominal_period_observation"] = (
                            "strong_overload"
                        )
                        operation["m12_screening_status"] = "not_candidate"
                        record["mode"] = "formal_screen"
                        if identifier.startswith(
                            "pitch_stress.native.48000.stereo.p48.r"
                        ):
                            screened_family = True
                    else:
                        full_rows, _ = run_child(
                            [str(binary), "--scenario", identifier],
                            timing_cap(catalog),
                            started,
                            worker_environment,
                        )
                        record = full_rows[-1]
                        require_qualified(record)
                else:
                    smoke_rows, _ = run_child(
                        [str(binary), "--scenario", identifier, "--smoke"],
                        timing_cap(catalog),
                        started,
                        worker_environment,
                    )
                    record = smoke_rows[-1]
            bind_record_identity(record, catalog, identity)
            validate_timing_record(
                record, catalog, identity, args.mode == "formal"
            )
            atomic_json(path, record)
            timing_records[identifier] = record
            report_progress(
                started, "timing_complete", index, len(timing_plan),
                calibration["predicted_total_seconds"]
                if calibration is not None else None,
            )

        validate_dominance_policy(timing_records)

        end_control: dict[str, Any] | None = None
        drift = None
        if args.mode == "formal":
            report_progress(
                started, "end_control", 1, 1,
                calibration["predicted_total_seconds"]
                if calibration is not None else None,
            )
            end_rows, _ = run_child(
                [str(binary), "--scenario", START_CONTROL],
                END_CONTROL_LIMIT_SECONDS,
                started,
                worker_environment,
            )
            end_control = end_rows[-1]
            require_qualified(end_control)
            bind_record_identity(end_control, by_id[START_CONTROL], identity)
            validate_timing_record(
                end_control, by_id[START_CONTROL], identity, True
            )
            end_control["record"] = "end_control"
            start_value = median_repetition_p50(timing_records[START_CONTROL])
            end_value = median_repetition_p50(end_control)
            drift = abs(end_value - start_value) / start_value
            end_control["relative_p50_drift"] = drift
            end_control["drift_status"] = "passed" if drift <= 0.10 else "failed"
            atomic_json(authoritative / "end_control.json", end_control)
            if drift > 0.10:
                raise RuntimeError("start/end control drift exceeds 10 percent")

        enforce_program_deadline(started)

        report_progress(
            started, "report_audit", 1, 1,
            calibration["predicted_total_seconds"]
            if calibration is not None else None,
        )
        records: list[dict[str, Any]] = [identity]
        records.extend(catalog_rows)
        records.extend(resource_rows)
        if calibration is not None:
            records.append(calibration)
        if contract_record is not None:
            records.append(contract_record)
        resource_files = sorted(authoritative.glob("resource_*.json"))
        expected_timing_ids = {
            str(catalog["scenario_id"]) for catalog in timing_plan
        }
        if set(timing_records) != expected_timing_ids:
            raise RuntimeError("timing result coverage is incomplete")
        expected_resource_count = len(resource_plan) * repetitions
        if len(resource_files) != expected_resource_count:
            raise RuntimeError("resource result coverage is incomplete")
        resource_observations = [load_json(path) for path in resource_files]
        resource_summaries = []
        for catalog in resource_plan:
            identifier = str(catalog["scenario_id"])
            observations = [
                row for row in resource_observations
                if row.get("scenario_id") == identifier
            ]
            if len(observations) != repetitions:
                raise RuntimeError(
                    f"resource repetition coverage is incomplete: {identifier}"
                )
            for observation in observations:
                validate_resource_record(
                    observation, catalog, identity,
                    int(observation["repetition"]),
                    args.mode == "formal",
                )
            resource_summaries.append(
                summarize_resource_observations(catalog, observations)
            )
        records.extend(resource_observations)
        records.extend(resource_summaries)
        for catalog in catalogs:
            identifier = str(catalog["scenario_id"])
            path = authoritative / scenario_filename(identifier)
            if path.exists():
                timing_record = load_json(path)
                validate_timing_record(
                    timing_record, catalog, identity, args.mode == "formal"
                )
                records.append(timing_record)
        workload_summaries, candidates = summarize_workloads(timing_records)
        records.extend(workload_summaries)
        if end_control is not None:
            if (
                end_control.get("catalog_entry_hash")
                    != stable_hash(by_id[START_CONTROL])
                or end_control.get("evidence_identity_hash")
                    != stable_hash(identity)
                or end_control.get("drift_status") != "passed"
            ):
                raise RuntimeError("end control audit failed")
            records.append(end_control)
        summary = {
            "record": "summary",
            "status": "complete",
            "capacity_contract_status": (
                contract_record["status"]
                if contract_record is not None else "not_run"
            ),
            "timing_scenarios": len(timing_records),
            "resource_scenarios": len(resource_summaries),
            "resource_observations": len(resource_files),
            "workload_families": workload_summaries,
            "m12_compute_candidates": candidates,
            "candidate_product_memory_status": (
                "memory_review_required" if candidates else "not_applicable"
            ),
            "relative_p50_drift": drift,
            "support_claim": False,
            "real_device_qualification": False,
            "elapsed_seconds": enforce_program_deadline(started),
        }
        records.append(summary)
        aggregate = output / "report.ndjson"
        temporary = aggregate.with_suffix(".ndjson.tmp")
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            for record in records:
                stream.write(
                    json.dumps(record, sort_keys=True, ensure_ascii=False)
                    + "\n"
                )
        try:
            enforce_program_deadline(started)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        os.replace(temporary, aggregate)
        atomic_json(output / "summary.json", summary)
        try:
            enforce_program_deadline(started)
        except Exception:
            aggregate.unlink(missing_ok=True)
            (output / "summary.json").unlink(missing_ok=True)
            raise
        print(
            f"complete scenarios={len(timing_records)} "
            f"resources={len(resource_files)} "
            f"elapsed={summary['elapsed_seconds']:.1f}s",
            flush=True,
        )
        return 0
    finally:
        release_lock(lock_path)
        release_lock(global_lock_path)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"runtime capacity runner failed: {error}", file=sys.stderr)
        raise SystemExit(1)
