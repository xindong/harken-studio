#!/usr/bin/env python3
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any


HOST = "127.0.0.1"
PORT = 9527
PROJECT_NAME = "SixContainerPackQualification"
SAMPLE_RATE = 48_000
TONE_SECONDS = 0.6
TONE_AMPLITUDE = 0.28
FRAME_SAMPLES = 256
ACTIVE_FLOOR = 0.005
MIN_TONE_RUN_MS = 100.0
# This seed makes Random Queue reverse the authored order, distinguishing it
# from Sequence Queue while keeping every selection signature deterministic.
CALLER_SEED = 5
WASAPI_CAPTURE_READY_TIMEOUT_MS = 20000
WASAPI_HANDSHAKE_S = WASAPI_CAPTURE_READY_TIMEOUT_MS / 1000

SELECTION_MODE = {
    "Random": 0,
    "RandomQueue": 1,
    "Sequence": 2,
    "SequenceQueue": 3,
    "VariableControl": 4,
}


@dataclass(frozen=True)
class Case:
    name: str
    mode: str
    repeats: int
    seconds_per_trigger: float
    expected_tones: tuple[int, ...] | None
    require_both: bool = False


CASES = (
    Case(
        "Random_Draw",
        "Random",
        8,
        0.85,
        None,
        True,
    ),
    Case("Shuffle", "Random", 2, 0.85, None, True),
    Case("Random_Queue", "RandomQueue", 1, 1.6, (660, 440), True),
    Case("Sequence", "Sequence", 2, 0.85, (440, 660)),
    Case("Sequence_Queue", "SequenceQueue", 1, 1.6, (440, 660)),
    Case("Variable_Control", "VariableControl", 2, 0.85, (440, 660)),
)


class QualificationError(RuntimeError):
    pass


_HOST_IO_TAIL = 2000


def _tail_text(text: str, limit: int = _HOST_IO_TAIL) -> str:
    if len(text) <= limit:
        return text
    return text[-limit:]


class HandshakeFailure(QualificationError):
    """verify_ready failed after host stdout and stderr were already drained."""

    def __init__(
        self,
        message: str,
        returncode: int | None,
        stdout: str,
        stderr: str,
    ) -> None:
        self.host_io = {
            "returncode": returncode,
            "stdout": stdout,
            "stderr": stderr,
        }
        super().__init__(
            "{0}\n{1}{2}\n{3}{4}\n{5}{6}".format(
                message,
                "host_returncode=",
                returncode,
                "host_stdout_tail=",
                _tail_text(stdout),
                "host_stderr_tail=",
                _tail_text(stderr),
            )
        )


def host_result_from_exception(error: BaseException) -> dict[str, Any] | None:
    if not isinstance(error, HandshakeFailure):
        return None
    payload = error.host_io
    stdout = payload.get("stdout")
    stderr = payload.get("stderr")
    if "returncode" not in payload or type(stdout) is not str or type(stderr) is not str:
        return None
    return {
        "returncode": payload["returncode"],
        "stdout": stdout,
        "stderr": stderr,
    }


class Ipc:
    def __init__(self, timeout: float = 30.0) -> None:
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.sequence = 0
        self.unusable = False

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            # Teardown is best-effort after the qualification result is known.
            pass

    def receive(self) -> dict[str, Any]:
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise QualificationError("Studio IPC socket closed")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def _require_correlated(self, response: Any, request_id: str, command: str) -> None:
        if type(response) is not dict:
            self._kill_stream()
            raise StreamUnusable(f"IPC response was not an object for {command}")
        request = response.get("request_id")
        if response.get("type") == "event" or type(request) is not str or request != request_id:
            self._kill_stream()
            raise StreamUnusable(f"IPC request id mismatch for {command}")

    def call(self, command: str, args: dict[str, Any] | None = None, deadline: float | None = None) -> dict[str, Any]:
        if self.unusable:
            raise StreamUnusable(f"IPC stream is closed for {command}")
        if deadline is not None:
            return self._call_bounded(command, args, deadline)
        self.sequence += 1
        request_id = f"six-pack-{self.sequence}"
        payload = json.dumps(
            {"cmd": command, "args": args or {}, "request_id": request_id},
            separators=(",", ":"),
        ) + "\n"
        self.sock.sendall(payload.encode("utf-8"))
        response = self.receive()
        self._require_correlated(response, request_id, command)
        return response

    @classmethod

    def from_connected(cls, sock: Any, timeout: float = 30.0) -> "Ipc":
        ipc = cls.__new__(cls)
        ipc.sock = sock
        ipc.buffer = b""
        ipc.sequence = 0
        ipc.unusable = False
        try:
            sock.settimeout(timeout)
        except OSError:
            pass
        return ipc

    def _kill_stream(self) -> None:
        self.buffer = b""
        self.unusable = True
        try:
            self.sock.close()
        except OSError:
            pass

    def _call_bounded(self, command: str, args: dict[str, Any] | None, deadline: float) -> dict[str, Any]:
        if self.unusable:
            raise StreamUnusable(f"IPC stream is closed for {command}")
        if deadline - monotonic() <= 0:
            raise LocalDeadline(f"IPC deadline already passed for {command}")
        self.sequence += 1
        request_id = f"six-pack-{self.sequence}"
        payload = (json.dumps({"cmd": command, "args": args or {}, "request_id": request_id}, separators=(",", ":")) + "\n").encode("utf-8")
        sent = 0
        try:
            previous_timeout = self.sock.gettimeout()
            while sent < len(payload):
                remaining = deadline - monotonic()
                if remaining <= 0:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC send crossed the deadline for {command}")
                self.sock.settimeout(remaining)
                wrote = self.sock.send(payload[sent:])
                if not wrote:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC send failed for {command}")
                sent += wrote
                if monotonic() > deadline:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC send finished past the deadline for {command}")
            while b"\n" not in self.buffer:
                remaining = deadline - monotonic()
                if remaining <= 0:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC recv crossed the deadline for {command}")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(1)
                if monotonic() > deadline:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC byte arrived past the deadline for {command}")
                if not chunk:
                    self._kill_stream()
                    raise StreamUnusable(f"IPC socket closed for {command}")
                self.buffer += chunk
            if monotonic() > deadline:
                self._kill_stream()
                raise StreamUnusable(f"IPC line completed past the deadline for {command}")
            line, _, rest = self.buffer.partition(b"\n")
            try:
                response = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                self._kill_stream()
                raise StreamUnusable(f"IPC response was not JSON for {command}") from error
            if type(response) is not dict:
                self._kill_stream()
                raise StreamUnusable(f"IPC response was not an object for {command}")
            self._require_correlated(response, request_id, command)
            if monotonic() > deadline:
                self._kill_stream()
                raise StreamUnusable(f"IPC response correlated past the deadline for {command}")
        except (LocalDeadline, StreamUnusable):
            raise
        except (TimeoutError, OSError) as error:
            self._kill_stream()
            raise StreamUnusable(f"IPC transport failed for {command}: {error}") from error
        try:
            self.sock.settimeout(previous_timeout)
        except OSError as error:
            self._kill_stream()
            raise StreamUnusable(f"IPC timeout restore failed for {command}: {error}") from error
        accepted_at = monotonic()
        if accepted_at > deadline:
            self._kill_stream()
            raise StreamUnusable(f"IPC acceptance crossed the deadline for {command}")
        self.buffer = rest
        response["_accepted_at"] = accepted_at
        return response

    def require(self, command: str, args: dict[str, Any] | None = None) -> dict[str, Any]:
        response = self.call(command, args)
        if response.get("status") != "ok":
            raise QualificationError(
                f"{command} failed: {json.dumps(response, ensure_ascii=True)}"
            )
        result = response.get("result")
        return result if isinstance(result, dict) else {}


def monotonic() -> float:
    return time.monotonic()

def pause(seconds: float) -> None:
    time.sleep(seconds)

class LocalDeadline(QualificationError):
    pass

class StreamUnusable(QualificationError):
    pass


class CommandRoleError(QualificationError):
    pass

FIXTURE_BUDGET_S = 1.5
POLL_GAP_S = 0.1
PREVIEW_ID_MAX = 2**31 - 1
TECHNICAL_LAYERS = ('null_smoke', 'pack', 'preview', 'capture_tech', 'cleanup')
ATT_LAYERS = ('hardware', 'ear', 'near_far', 'first_block_rms', 'vc_preview')
TECHNICAL_VALUES = {'PASS', 'FAIL', 'NOT_RUN'}
ATT_VALUES = {'PASS', 'FAIL', 'NOT_RUN', 'NOT_COVERED', 'UNVERIFIED'}
CASE_NAMES = ('Random_Draw', 'Shuffle', 'Random_Queue', 'Sequence', 'Sequence_Queue', 'Variable_Control')
HARD_BOOLS = ('expected_wav_absent', 'fmt_invalid', 'non_frame_aligned', 'capture_dead', 'wav_shorter')

def gate_marker_for_pack_play(pack_pid: int) -> str:
    return f'process_id={pack_pid}\n'

def gate_identifies_pack_play_not_recorder(marker: str, pack_pid: int, recorder_pid: int) -> bool:
    return marker == gate_marker_for_pack_play(pack_pid) and marker != (
        '' if recorder_pid == pack_pid else gate_marker_for_pack_play(recorder_pid)
    ) or (recorder_pid == pack_pid and marker == gate_marker_for_pack_play(pack_pid))

def _strict_bool(value: Any) -> bool:
    return type(value) is bool


def _legal_id(value: Any) -> bool:
    return type(value) is int and 1 <= value <= PREVIEW_ID_MAX


def _exact_case_list(value: Any) -> bool:
    return type(value) is list and value == list(CASE_NAMES)


def _legal_handle(value: Any, *, zero_ok: bool) -> bool:
    if type(value) is not int:
        return False
    if value == 0:
        return zero_ok
    return -(2**63) < value <= -1


def _hard_capture(raw: dict[str, Any]) -> bool:
    if raw.get("capture_complete") == "FAIL" or raw.get("wav_tail") == "FRAGMENT":
        return True
    exit_code = raw.get("capture_exit", None)
    if type(exit_code) is int and exit_code != 0:
        return True
    return any(raw.get(key) is True for key in HARD_BOOLS)


def _bad_present_types(raw: dict[str, Any]) -> bool:
    for key in TECHNICAL_LAYERS:
        if key in raw and (type(raw[key]) is not str or raw[key] not in TECHNICAL_VALUES):
            return True
    for key in ATT_LAYERS:
        if key in raw and (type(raw[key]) is not str or raw[key] not in ATT_VALUES):
            return True
    if "capture_exit" in raw and type(raw["capture_exit"]) is not int:
        return True
    for key in HARD_BOOLS:
        if key in raw and type(raw[key]) is not bool:
            return True
    if "wav_tail" in raw and type(raw["wav_tail"]) is not str:
        return True
    if "capture_complete" in raw and type(raw["capture_complete"]) is not str:
        return True
    return False


def overall(raw: Any) -> tuple[str, str | None]:
    if type(raw) is not dict:
        return "FAIL", "INPUT"
    if raw.get("u3_12b_complete") is True:
        return "FAIL", "INPUT"
    if type(raw.get("summary_is_completion")) is not bool or raw.get("summary_is_completion") is not False:
        return "FAIL", "INPUT"
    attempted = raw.get("run_attempted", None)
    errored = raw.get("input_error", None)
    if not _strict_bool(attempted) or not _strict_bool(errored):
        return "FAIL", "INPUT"
    if errored is True:
        return "FAIL", "INPUT"
    facets = raw.get("facets", {})
    if "facets" in raw and type(facets) is not dict:
        return "FAIL", "INPUT"
    if type(facets) is dict and "capture_complete" in facets and "capture_complete" in raw:
        return "FAIL", "INPUT"
    if "cases" in raw and not _exact_case_list(raw.get("cases")):
        return "FAIL", "INPUT"
    if attempted is True and not _exact_case_list(raw.get("cases")):
        return "FAIL", "INPUT"
    if _bad_present_types(raw):
        return "FAIL", "INPUT"
    if attempted is False:
        if _hard_capture(raw) or any(raw.get(key) == "FAIL" for key in TECHNICAL_LAYERS + ATT_LAYERS):
            return "FAIL", "STALE_EVIDENCE"
        if any(raw.get(key) == "PASS" for key in TECHNICAL_LAYERS):
            return "FAIL", "INVARIANT"
        return "NOT_RUN", None
    for key in TECHNICAL_LAYERS:
        if key not in raw or raw[key] == "NOT_RUN":
            return "FAIL", "INVARIANT"
    if raw["null_smoke"] == "FAIL":
        return "FAIL", "NULL_SMOKE"
    if raw["pack"] == "FAIL":
        return "FAIL", "PACK"
    if raw["preview"] == "FAIL":
        return "FAIL", "PREVIEW"
    if raw["capture_tech"] == "FAIL" or _hard_capture(raw):
        return "FAIL", "CAPTURE"
    if raw["cleanup"] == "FAIL":
        return "FAIL", "CLEANUP"
    if any(raw.get(key) == "FAIL" for key in ATT_LAYERS):
        return "FAIL", "ATT_FACET"
    covered = all(raw.get(key) == "PASS" for key in ATT_LAYERS)
    if all(raw[key] == "PASS" for key in TECHNICAL_LAYERS) and not covered:
        return "BLOCKED", "COVERAGE"
    if all(raw[key] == "PASS" for key in TECHNICAL_LAYERS) and covered:
        return "BLOCKED", "SCRIPT_CANNOT_CLOSE_ATT07"
    return "FAIL", "UNCLASSIFIED"


def confirm_preview_path(created: dict[str, Any], listed: dict[str, Any]) -> str | None:
    guid = created.get("guid")
    stable = listed.get("runtime_stable_id")
    created_path = created.get("path")
    listed_path = listed.get("path")
    full_path = listed.get("full_path")
    if type(guid) is not str or guid == "" or guid != stable:
        return None
    paths = (created_path, listed_path, full_path)
    if not all(type(item) is str and item != "" for item in paths):
        return None
    if created_path != listed_path or listed_path != full_path:
        return None
    return created_path


def _result_dict(response: dict[str, Any]) -> dict[str, Any]:
    result = response.get("result")
    return result if type(result) is dict else {}


def parse_play(response: dict[str, Any], sent_path: str) -> dict[str, Any]:
    body = _result_dict(response)
    audition_id = body.get("auditionID")
    owned = audition_id if _legal_id(audition_id) else None
    pending = body.get("pending")
    state = body.get("state")
    handle = body.get("playbackHandle")
    path = body.get("object_path")
    ok = response.get("status") == "ok" and path == sent_path
    playing = (
        ok
        and state == "playing"
        and pending is False
        and owned is not None
        and _legal_handle(handle, zero_ok=False)
    )
    queued = (
        ok
        and state == "queued"
        and pending is True
        and owned is not None
        and handle == 0
        and _legal_handle(handle, zero_ok=True)
    )
    kind = "playing" if playing else "queued" if queued else "other"
    return {"owned": owned, "kind": kind, "handle": handle if playing else None}


def judge_state(response: dict[str, Any], owned: int | None) -> dict[str, Any]:
    body = _result_dict(response)
    rows = body.get("auditions")
    if response.get("status") != "ok" or type(rows) is not list:
        return {"accepted": True, "known": False, "present": False, "row": None, "mismatch": False}
    for row in rows:
        if type(row) is not dict or type(row.get("auditionID")) is not int:
            return {"accepted": True, "known": False, "present": False, "row": None, "mismatch": False}
        if type(row.get("auditionID")) is not int:
            return {"accepted": True, "known": False, "present": False, "row": None, "mismatch": False}
    if owned is None:
        return {"accepted": True, "known": True, "present": False, "row": None, "mismatch": False}
    found = [row for row in rows if row.get("auditionID") == owned]
    if not found:
        return {"accepted": True, "known": True, "present": False, "row": None, "mismatch": False}
    row = found[0]
    return {"accepted": True, "known": True, "present": True, "row": row, "mismatch": False}


def _row_playing(row: dict[str, Any], bound: int | None) -> bool:
    handle = row.get("playbackHandle")
    if not (type(row.get("state")) is int and row.get("state") == 1 and row.get("pending") is False and _legal_handle(handle, zero_ok=False)):
        return False
    return bound is None or handle == bound


def _row_queued(row: dict[str, Any]) -> bool:
    return type(row.get("state")) is int and row.get("state") == 0 and row.get("pending") is True


def preview_iterations(case: Case) -> int:
    if case.name == "Variable_Control":
        return 0
    if case.mode.endswith("Queue"):
        return 1
    return case.repeats


def _throttle(deadline: float) -> bool:
    remaining = deadline - monotonic()
    if remaining <= 0:
        return False
    pause(min(POLL_GAP_S, remaining))
    return monotonic() < deadline


def _remember(result: dict[str, Any], reason: str) -> None:
    result["status"] = "FAIL"
    if result["reason"] is None:
        result["reason"] = reason


def _stop_owned(ipc: Ipc, owned: int, d_cleanup: float, result: dict[str, Any]) -> None:
    if result.get("transport") or ipc.unusable or monotonic() >= d_cleanup:
        result["cleanup"] = "unproven"
        return
    try:
        stopped = ipc.call("audition_stop", {"auditionID": owned, "fade_ms": 0}, d_cleanup)
    except LocalDeadline:
        result["cleanup"] = "unproven"
        _remember(result, "cleanup-budget")
        return
    except StreamUnusable:
        result["transport"] = True
        result["cleanup"] = "unproven"
        _remember(result, "transport")
        return
    result["stops"] += 1
    result["stop_ids"].append(owned)
    body = _result_dict(stopped)
    if stopped.get("status") != "ok" or body.get("action") != "stopped" or body.get("auditionID") != owned:
        _remember(result, "stop-not-accepted")
    while True:
        if monotonic() >= d_cleanup:
            result["cleanup"] = "unproven"
            _remember(result, "cleanup-budget")
            return
        try:
            nxt = ipc.call("audition_get_state", {}, d_cleanup)
        except LocalDeadline:
            result["cleanup"] = "unproven"
            _remember(result, "cleanup-budget")
            return
        except StreamUnusable:
            result["transport"] = True
            result["cleanup"] = "unproven"
            _remember(result, "transport")
            return
        view = judge_state(nxt, owned)
        if not view["known"]:
            result["cleanup"] = "unproven"
            _remember(result, "state-untyped")
            return
        if not view["present"]:
            result["cleanup"] = "proven"
            return
        if not _throttle(d_cleanup):
            result["cleanup"] = "unproven"
            _remember(result, "cleanup-budget")
            return


def run_preview_iteration(ipc: Ipc, object_path: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "status": "PASS",
        "reason": None,
        "stops": 0,
        "stop_ids": [],
        "transport": False,
        "cleanup": "unproven",
        "owned": None,
    }
    if ipc.unusable:
        result["transport"] = True
        _remember(result, "transport")
        return result
    command_deadline = monotonic() + FIXTURE_BUDGET_S
    try:
        play = ipc.call(
            "audition_play",
            {"object_path": object_path, "game_object_id": 0},
            command_deadline,
        )
    except LocalDeadline:
        _remember(result, "play-budget")
        return result
    except StreamUnusable:
        result["transport"] = True
        _remember(result, "transport")
        return result
    accepted_at = play.pop("_accepted_at")
    parsed = parse_play(play, object_path)
    owned = parsed["owned"]
    result["owned"] = owned
    start = parsed["kind"] == "playing"
    bound = parsed["handle"] if start else None
    d_observe = accepted_at + FIXTURE_BUDGET_S
    try:
        state = ipc.call("audition_get_state", {}, d_observe)
    except LocalDeadline:
        _remember(result, "observe-budget")
        return result
    except StreamUnusable:
        result["transport"] = True
        _remember(result, "transport")
        return result
    t_last = state.pop("_accepted_at")
    view = judge_state(state, owned)
    if not view["known"]:
        _remember(result, "state-untyped")
        result["cleanup"] = "unproven"
        return result
    if not view["present"]:
        if start:
            result["cleanup"] = "proven"
            return result
        _remember(result, "absent-before-start")
        return result
    row = view["row"]
    assert row is not None
    if not start:
        while _row_queued(row) and monotonic() < d_observe:
            if not _throttle(d_observe):
                break
            try:
                state = ipc.call("audition_get_state", {}, d_observe)
            except LocalDeadline:
                break
            except StreamUnusable:
                result["transport"] = True
                _remember(result, "transport")
                return result
            observed_at = state.pop("_accepted_at", None)
            if type(observed_at) is float:
                t_last = observed_at
            view = judge_state(state, owned)
            if not view["known"]:
                _remember(result, "state-untyped")
                result["cleanup"] = "unproven"
                return result
            if not view["present"]:
                _remember(result, "absent-before-start")
                return result
            row = view["row"]
            assert row is not None
            if _row_playing(row, None):
                start = True
                bound = row.get("playbackHandle")
                break
        if not start:
            if _row_queued(row):
                _remember(result, "queued-budget")
            elif type(row.get("state")) is int and row.get("state") == 0 and row.get("pending") is False:
                _remember(result, "state0-pending-false")
            else:
                _remember(result, "no-start")
            if owned is not None and view["present"] and not ipc.unusable:
                _stop_owned(ipc, owned, t_last + FIXTURE_BUDGET_S, result)
            return result
    if bound is not None and row.get("playbackHandle") != bound:
        _remember(result, "handle-mismatch")
    elif not _row_playing(row, bound) and not (type(row.get("state")) is int and row.get("state") == 0):
        _remember(result, "unexpected-row")
    if owned is None:
        _remember(result, "missing-id")
        return result
    if not view["present"]:
        if start:
            result["cleanup"] = "proven"
        return result
    _stop_owned(ipc, owned, t_last + FIXTURE_BUDGET_S, result)
    if result["cleanup"] != "proven":
        _remember(result, "cleanup-unproven")
    return result


def run_preview_case(ipc: Ipc, case: Case, object_path: str | None, blocked_paths: set[str]) -> dict[str, Any]:
    case_result: dict[str, Any] = {
        "name": case.name,
        "status": "PASS",
        "reason": None,
        "plays": 0,
        "stops": 0,
        "stop_ids": [],
        "transport": False,
        "blocked_by_transport": False,
        "iterations": [],
    }
    if case.name == "Variable_Control":
        case_result["status"] = "NOT_RUN"
        return case_result
    if object_path is None:
        case_result["status"] = "FAIL"
        case_result["reason"] = "object_path"
        return case_result
    if object_path in blocked_paths:
        case_result["status"] = "FAIL"
        case_result["reason"] = "path-blocked"
        return case_result
    for _ in range(preview_iterations(case)):
        if ipc.unusable:
            case_result["transport"] = True
            case_result["status"] = "FAIL"
            if case_result["reason"] is None:
                case_result["reason"] = "transport"
            break
        cycle = run_preview_iteration(ipc, object_path)
        case_result["iterations"].append(cycle)
        case_result["plays"] += 1
        case_result["stops"] += cycle["stops"]
        case_result["stop_ids"].extend(cycle["stop_ids"])
        if cycle["status"] != "PASS" and case_result["reason"] is None:
            case_result["reason"] = cycle["reason"]
            case_result["status"] = "FAIL"
        if cycle.get("transport") or cycle["cleanup"] != "proven":
            blocked_paths.add(object_path)
        if cycle.get("transport"):
            case_result["transport"] = True
            case_result["status"] = "FAIL"
            break
        if case_result["status"] == "FAIL":
            break
    return case_result


def run_preview_session(ipc: Ipc, paths: dict[str, str | None]) -> dict[str, Any]:
    blocked: set[str] = set()
    rows = []
    transport = False
    first = None
    for case in CASES:
        if transport and case.name != "Variable_Control":
            rows.append({
                "name": case.name,
                "status": "NOT_RUN",
                "reason": None,
                "plays": 0,
                "stops": 0,
                "stop_ids": [],
                "transport": True,
                "blocked_by_transport": True,
                "iterations": [],
            })
            continue
        row = run_preview_case(ipc, case, paths.get(case.name), blocked)
        rows.append(row)
        if row.get("transport"):
            transport = True
        if row["status"] == "FAIL" and first is None:
            first = row["reason"] or "preview"
    non_vc = [row for row in rows if row["name"] != "Variable_Control"]
    if any(row["status"] == "FAIL" for row in non_vc):
        preview = "FAIL"
    elif non_vc and all(row["status"] == "PASS" for row in non_vc):
        preview = "PASS"
    else:
        preview = "NOT_RUN"
    return {"preview": preview, "cases": rows, "first_failure": first, "transport": transport}


def _case_spec(name: Any) -> Case | None:
    if type(name) is not str:
        return None
    for case in CASES:
        if case.name == name:
            return case
    return None


def _legal_owned(value: Any) -> bool:
    return type(value) is int and 1 <= value <= PREVIEW_ID_MAX


def _proven_iteration(item: dict[str, Any]) -> bool:
    if item.get("reason") == "absent-before-start":
        return False
    cleanup = item.get("cleanup")
    return _legal_owned(item.get("owned")) and type(cleanup) is str and cleanup == "proven"


def _nonpass_iteration_ok(item: dict[str, Any]) -> bool:
    if not _legal_owned(item.get("owned")):
        return True
    cleanup = item.get("cleanup")
    if type(cleanup) is str and cleanup == "proven":
        return True
    stops = item.get("stops")
    return item.get("reason") == "absent-before-start" and type(stops) is int and stops == 0


def aggregate_cleanup(cases: Any) -> str:
    if type(cases) is not list:
        return "FAIL"
    for case in cases:
        if type(case) is not dict:
            return "FAIL"
        status = case.get("status")
        plays = case.get("plays")
        iterations = case.get("iterations")
        if type(status) is not str or status not in {"PASS", "FAIL", "NOT_RUN"}:
            return "FAIL"
        if type(plays) is not int or type(iterations) is not list or plays != len(iterations):
            return "FAIL"
        if any(type(item) is not dict for item in iterations):
            return "FAIL"
        if case.get("name") == "Variable_Control":
            continue
        if status == "PASS":
            spec = _case_spec(case.get("name"))
            if spec is None or len(iterations) != preview_iterations(spec):
                return "FAIL"
            if any(not _proven_iteration(item) for item in iterations):
                return "FAIL"
            continue
        if any(not _nonpass_iteration_ok(item) for item in iterations):
            return "FAIL"
    return "PASS"


def technical_card(preview: str, *, cleanup: str | None = None, attempted: bool = True) -> dict:
    card = {
        "run_attempted": attempted,
        "input_error": False,
        "summary_is_completion": False,
        "cases": list(CASE_NAMES),
        "hardware": "NOT_COVERED",
        "ear": "NOT_COVERED",
        "near_far": "NOT_COVERED",
        "first_block_rms": "NOT_COVERED",
        "vc_preview": "NOT_COVERED",
    }
    if attempted:
        if cleanup not in {"PASS", "FAIL"}:
            cleanup = "FAIL"
        card.update({
            "null_smoke": "PASS",
            "pack": "PASS",
            "preview": preview,
            "capture_tech": "PASS",
            "cleanup": cleanup,
        })
    return card


def attach_preview_contract(report: dict, ipc: Ipc, null_only: bool) -> None:
    report["summary_is_completion"] = False
    report.pop("u3_12b_complete", None)
    if null_only:
        card = technical_card("NOT_RUN", attempted=False)
        report["u3_12b"] = card
        report["u3_12b_overall"] = list(overall(card))
        return
    containers = report["authoring"]["containers"]
    paths: dict[str, str | None] = {}
    for case in CASES:
        if case.name == "Variable_Control":
            paths[case.name] = None
            continue
        created = containers[case.name]["container"]
        listed = find_item(ipc, case.name)
        paths[case.name] = confirm_preview_path(created, listed)
    session = run_preview_session(ipc, paths)
    card = technical_card(
        session["preview"],
        cleanup=aggregate_cleanup(session["cases"]),
        attempted=True,
    )
    report["u3_12b"] = card
    report["preview_cases"] = session["cases"]
    report["u3_12b_overall"] = list(overall(card))
    report["preview_first_failure"] = session["first_failure"]
    if session["preview"] == "FAIL":
        raise QualificationError(session["first_failure"] or "preview contract failed")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def write_tone(path: Path, frequency: float) -> None:
    frame_count = int(SAMPLE_RATE * TONE_SECONDS)
    pcm = array.array(
        "h",
        (
            int(
                32767.0
                * TONE_AMPLITUDE
                * math.sin(2.0 * math.pi * frequency * frame / SAMPLE_RATE)
            )
            for frame in range(frame_count)
        ),
    )
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(pcm.tobytes())


def port_is_open() -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=0.3):
            return True
    except OSError:
        return False


def _close_unusable(ipc: Ipc, message: str) -> None:
    ipc.unusable = True
    kill = getattr(ipc, "_kill_stream", None)
    if callable(kill):
        kill()
    raise StreamUnusable(message)


def establish_command_role(ipc: Ipc) -> None:
    deadline = monotonic() + FIXTURE_BUDGET_S
    try:
        response = ipc.call("set_channel_role", {"role": "command"}, deadline)
    except (LocalDeadline, StreamUnusable):
        ipc.unusable = True
        raise
    result = response.get("result")
    role = result.get("role") if type(result) is dict else None
    if response.get("status") != "ok" or type(role) is not str or role != "command":
        _close_unusable(ipc, "command role was not accepted")


def connect_qualified_ipc(ipc: Ipc) -> Ipc:
    if ipc.call("ping").get("status") != "ok":
        raise QualificationError("Studio IPC ping failed")
    establish_command_role(ipc)
    return ipc


def wait_for_ipc(timeout: float = 90.0) -> Ipc:
    deadline = time.monotonic() + timeout
    last_error = "not attempted"
    while time.monotonic() < deadline:
        try:
            ipc = Ipc(timeout=10.0)
            if ipc.call("ping").get("status") == "ok":
                try:
                    establish_command_role(ipc)
                except QualificationError as error:
                    raise CommandRoleError(str(error)) from error
                return ipc
            ipc.close()
        except CommandRoleError:
            raise
        except Exception as error:  # noqa: BLE001
            last_error = str(error)
        time.sleep(0.4)
    raise QualificationError(f"Studio IPC did not become ready: {last_error}")


def wait_for_project(ipc: Ipc, timeout: float = 90.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = ipc.require("get_app_state")
        if last.get("state") == "project_loaded":
            return last
        time.sleep(0.3)
    raise QualificationError(f"Studio did not load the project: {last}")


def wait_for_export(ipc: Ipc, timeout: float = 180.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = ipc.require("get_pack_build_status")
        if not last.get("in_progress"):
            records = ipc.require("get_last_pack_build_records")
            result = last.get("result", {})
            if result.get("status") != "ok" or not result.get("result", {}).get("success"):
                raise QualificationError(f"Pack export failed: {last}")
            return {"status": last, "records": records.get("records", [])}
        time.sleep(0.5)
    raise QualificationError(f"Pack export timed out: {last}")


def find_item(ipc: Ipc, name: str) -> dict[str, Any]:
    queue = list(ipc.require("list_items").get("items", []))
    seen: set[str] = set()
    while queue:
        item = queue.pop(0)
        path = str(item.get("path") or item.get("full_path") or "")
        if item.get("name") == name:
            return item
        key = path or str(item.get("guid") or item.get("authoring_handle") or "")
        if key in seen:
            continue
        seen.add(key)
        if item.get("has_children") and path:
            queue.extend(
                ipc.require("list_items", {"parent": path}).get("items", [])
            )
    raise QualificationError(f"Project item was not found: {name}")


_OBJECT_ID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)
_NULL_OBJECT_ID = "00000000-0000-0000-0000-000000000000"
REPORT_SCHEMA_VERSION = 2


def runtime_stable_id_from_ipc(item: dict[str, Any], name: str) -> str:
    value = item.get("runtime_stable_id")
    if (
        not isinstance(value, str)
        or not _OBJECT_ID_RE.fullmatch(value)
        or value == _NULL_OBJECT_ID
    ):
        raise QualificationError(
            f"IPC runtime_stable_id missing or unusable for {name}: {value!r}"
        )
    if "object_id" in item and type(item.get("object_id")) is not int:
        raise QualificationError(
            f"IPC object_id must stay a number for {name}: {item.get('object_id')!r}"
        )
    return value


def report_identity_fields(report: dict[str, Any]) -> dict[str, str]:
    if report.get("schema_version") != REPORT_SCHEMA_VERSION:
        raise QualificationError(
            f"report schema_version must be {REPORT_SCHEMA_VERSION} when identity fields are UUID strings"
        )
    authoring = report.get("authoring")
    if not isinstance(authoring, dict):
        raise QualificationError("report authoring is missing")
    variable_id = authoring.get("variable_stable_id")
    if (
        not isinstance(variable_id, str)
        or not _OBJECT_ID_RE.fullmatch(variable_id)
        or variable_id == _NULL_OBJECT_ID
    ):
        raise QualificationError(
            f"report variable_stable_id must be a lowercase UUID string: {variable_id!r}"
        )
    containers = authoring.get("containers")
    if not isinstance(containers, dict):
        raise QualificationError("report authoring.containers is missing")
    identities = {"variable_stable_id": variable_id}
    for case in CASES:
        row = containers.get(case.name)
        if not isinstance(row, dict):
            raise QualificationError(f"report container {case.name} is missing")
        action_id = row.get("action_stable_id")
        if (
            not isinstance(action_id, str)
            or not _OBJECT_ID_RE.fullmatch(action_id)
            or action_id == _NULL_OBJECT_ID
        ):
            raise QualificationError(
                f"report action_stable_id for {case.name} must be a lowercase UUID string: {action_id!r}"
            )
        identities[case.name] = action_id
    return identities



def _saved_shuffle_discipline(project_dir: Path, guid: Any) -> str:
    if type(guid) is not str or guid == "":
        raise QualificationError("Shuffle create_item guid must be a non-empty string")
    root = project_dir.resolve()
    suffix = f"__{guid}.vp"
    matches: list[Path] = []
    for current, dirnames, filenames in os.walk(root, followlinks=False):
        current_path = Path(current)
        try:
            current_path.resolve().relative_to(root)
        except ValueError:
            for filename in filenames:
                if filename.endswith(suffix):
                    raise QualificationError("Shuffle candidate escaped the project directory")
            dirnames[:] = []
            continue
        dirnames[:] = [
            name for name in dirnames if not (current_path / name).is_symlink()
        ]
        for filename in filenames:
            if not filename.endswith(suffix):
                continue
            candidate = current_path / filename
            if candidate.is_symlink():
                raise QualificationError("Shuffle candidate escaped the project directory")
            try:
                candidate.resolve().relative_to(root)
            except ValueError:
                raise QualificationError("Shuffle candidate escaped the project directory") from None
            if not candidate.is_file():
                continue
            matches.append(candidate)
    if len(matches) != 1:
        raise QualificationError(
            f"Expected exactly one Shuffle document ending with {suffix}, found {len(matches)}"
        )
    saved = matches[0]
    try:
        raw = saved.read_text(encoding="utf-8")
    except OSError as error:
        raise QualificationError(f"Shuffle document could not be read: {saved}") from error
    try:
        document = json.loads(raw)
    except json.JSONDecodeError as error:
        raise QualificationError(f"Shuffle document was not JSON: {saved}") from error
    if type(document) is not dict:
        raise QualificationError("Shuffle document must be an object")
    harken_document = document.get("harkenDocument")
    content = document.get("content")
    if type(harken_document) is not dict or type(content) is not dict:
        raise QualificationError("Shuffle harkenDocument and content must be objects")
    if content.get("guid") != guid or harken_document.get("id") != guid:
        raise QualificationError("Shuffle document guid does not match create_item")
    components = content.get("components")
    selection = components.get("selection") if type(components) is dict else None
    behavior = selection.get("behavior") if type(selection) is dict else None
    discipline = behavior.get("discipline") if type(behavior) is dict else None
    if type(discipline) is not str or discipline != "shuffle":
        raise QualificationError("Shuffle discipline did not persist structurally")
    return discipline


def author_project(
    ipc: Ipc, project_dir: Path, tone_a: Path, tone_b: Path
) -> dict[str, Any]:
    try:
        layer = find_item(ipc, "Layer")
    except QualificationError:
        layer = ipc.require("create_item", {"name": "Layer", "type": "Layer"})
    variable = ipc.require("create_item", {"name": "StepVar", "type": "Variable"})
    ipc.require(
        "set_item_property",
        {"path": variable["path"], "property": "kind", "value": 0},
    )
    ipc.require(
        "set_item_property",
        {"path": variable["path"], "property": "range", "value": [0, 1]},
    )
    ipc.require(
        "set_item_property",
        {
            "path": variable["path"],
            "property": "labels",
            "value": [
                {"anchor": 0, "name": "A"},
                {"anchor": 1, "name": "B"},
            ],
        },
    )

    authored: dict[str, Any] = {}
    for case in CASES:
        container = ipc.require(
            "create_item",
            {"parent": layer["path"], "name": case.name, "type": "Container"},
        )
        ipc.require(
            "set_item_property",
            {
                "path": container["path"],
                "property": "mode",
                "value": SELECTION_MODE[case.mode],
            },
        )
        if case.name == "Shuffle":
            ipc.require(
                "set_item_property",
                {
                    "path": container["path"],
                    "property": "behavior",
                    "value": {"discipline": "shuffle", "avoidRepeat": 1},
                },
            )
        sound_a = ipc.require(
            "create_item",
            {
                "parent": container["path"],
                "name": f"{case.name}_A",
                "type": "Sound",
            },
        )
        sound_b = ipc.require(
            "create_item",
            {
                "parent": container["path"],
                "name": f"{case.name}_B",
                "type": "Sound",
            },
        )
        for sound, source in ((sound_a, tone_a), (sound_b, tone_b)):
            ipc.require(
                "import_audio",
                {
                    "path": str(source),
                    "pack": layer["name"],
                    "target_parent_path": sound["path"],
                },
            )
        if case.mode == "VariableControl":
            ipc.require(
                "set_item_property",
                {
                    "path": container["path"],
                    "property": "grid",
                    "value": {
                        "discrete": {
                            "axes": [variable["guid"]],
                            "rows": [
                                {"target": sound_a["guid"], "cells": [0]},
                                {"target": sound_b["guid"], "cells": [1]},
                            ],
                        }
                    },
                },
            )
        action = ipc.require(
            "create_item", {"name": f"Play_{case.name}", "type": "Action"}
        )
        ipc.require(
            "set_item_property",
            {
                "path": action["path"],
                "property": "steps",
                "value": [
                    {
                        "type": "Play",
                        "targetObjectID": container["authoring_handle"],
                        "targetName": case.name,
                        "delay": 0.0,
                    }
                ],
            },
        )
        authored[case.name] = {
            "container": container,
            "sounds": {"A": sound_a, "B": sound_b},
            "action": action,
        }

    ipc.require("save_project")
    for case in CASES:
        row = authored[case.name]
        listed = find_item(ipc, row["action"]["name"])
        row["action_stable_id"] = runtime_stable_id_from_ipc(
            listed, row["action"]["name"]
        )
    variable_id = runtime_stable_id_from_ipc(find_item(ipc, "StepVar"), "StepVar")

    discipline = _saved_shuffle_discipline(
        project_dir, authored["Shuffle"]["container"].get("guid")
    )
    return {
        "layer": layer,
        "variable": variable,
        "variable_stable_id": variable_id,
        "shuffle_discipline": discipline,
        "containers": authored,
    }


def host_command(
    host_exe: Path,
    export_dir: Path,
    case: Case,
    variable_id: str,
    null_mode: bool,
    capture_ready_gate: Path | None = None,
) -> list[str]:
    command = [
        str(host_exe),
        "pack-play",
        "--root",
        str(export_dir),
        "--action-name",
        f"Play_{case.name}",
        "--seconds",
        str(case.seconds_per_trigger),
        "--repeats",
        str(case.repeats),
        "--no-recovery",
        "--caller-seed",
        str(CALLER_SEED),
    ]
    if case.mode == "VariableControl":
        command.extend(
            ["--variable-id", str(variable_id), "--variable-values", "0,1"]
        )
    if null_mode:
        command.append("--null")
    elif capture_ready_gate is not None:
        command.extend(
            [
                "--capture-ready-gate",
                str(capture_ready_gate),
                "--capture-ready-timeout-ms",
                str(WASAPI_CAPTURE_READY_TIMEOUT_MS),
            ]
        )
    return command


def run_host(command: list[str], timeout: float) -> dict[str, Any]:
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    trigger_lines = null_trigger_lines(completed.stdout)
    return {
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "trigger_lines": trigger_lines,
    }


def read_wav(path: Path) -> tuple[int, int, list[float]]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise QualificationError(f"Capture is not a RIFF WAVE file: {path}")
    cursor = 12
    fmt = b""
    payload = b""
    while cursor + 8 <= len(data):
        chunk_id = data[cursor : cursor + 4]
        chunk_size = struct.unpack_from("<I", data, cursor + 4)[0]
        chunk = data[cursor + 8 : cursor + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt = chunk
        elif chunk_id == b"data":
            payload = chunk
        cursor += 8 + chunk_size + (chunk_size & 1)
    if len(fmt) < 16 or not payload:
        raise QualificationError(f"Capture is missing fmt or data: {path}")
    format_tag, channels, sample_rate, _, _, bits = struct.unpack_from(
        "<HHIIHH", fmt, 0
    )
    if format_tag == 0xFFFE and len(fmt) >= 40:
        format_tag = struct.unpack_from("<I", fmt, 24)[0]
    if channels == 0:
        raise QualificationError(f"Capture has no channels: {path}")
    if format_tag == 3 and bits == 32:
        values = array.array("f")
        values.frombytes(payload)
        if sys.byteorder != "little":
            values.byteswap()
        scale = 1.0
    elif format_tag == 1 and bits == 16:
        values = array.array("h")
        values.frombytes(payload)
        if sys.byteorder != "little":
            values.byteswap()
        scale = 1.0 / 32768.0
    else:
        raise QualificationError(
            f"Unsupported capture format tag={format_tag} bits={bits}"
        )
    mono = [
        sum(float(values[index + channel]) * scale for channel in range(channels))
        / channels
        for index in range(0, len(values) - channels + 1, channels)
    ]
    return sample_rate, channels, mono


def tone_score(samples: list[float], sample_rate: int, frequency: float) -> float:
    real = 0.0
    imaginary = 0.0
    step = 2.0 * math.pi * frequency / sample_rate
    for index, sample in enumerate(samples):
        angle = step * index
        real += sample * math.cos(angle)
        imaginary -= sample * math.sin(angle)
    return real * real + imaginary * imaginary


def analyze_capture(path: Path) -> dict[str, Any]:
    sample_rate, channels, mono = read_wav(path)
    frame_rows: list[dict[str, Any]] = []
    for start in range(0, len(mono) - FRAME_SAMPLES + 1, FRAME_SAMPLES):
        frame = mono[start : start + FRAME_SAMPLES]
        peak = max(abs(sample) for sample in frame)
        if peak <= ACTIVE_FLOOR:
            frame_rows.append({"start": start, "tone": 0, "peak": peak})
            continue
        score_440 = tone_score(frame, sample_rate, 440.0)
        score_660 = tone_score(frame, sample_rate, 660.0)
        frame_rows.append(
            {
                "start": start,
                "tone": 440 if score_440 >= score_660 else 660,
                "peak": peak,
            }
        )

    runs: list[dict[str, Any]] = []
    active_tone = 0
    active_start = 0
    active_end = 0
    active_peak = 0.0
    silent_frames = 0
    for row in frame_rows:
        tone = int(row["tone"])
        if tone == 0:
            if active_tone:
                silent_frames += 1
                if silent_frames >= 3:
                    runs.append(
                        {
                            "tone_hz": active_tone,
                            "start_ms": round(1000.0 * active_start / sample_rate, 1),
                            "end_ms": round(1000.0 * active_end / sample_rate, 1),
                            "peak": round(active_peak, 6),
                        }
                    )
                    active_tone = 0
                    silent_frames = 0
            continue
        if active_tone and tone != active_tone:
            runs.append(
                {
                    "tone_hz": active_tone,
                    "start_ms": round(1000.0 * active_start / sample_rate, 1),
                    "end_ms": round(1000.0 * active_end / sample_rate, 1),
                    "peak": round(active_peak, 6),
                }
            )
            active_tone = 0
        if not active_tone:
            active_tone = tone
            active_start = int(row["start"])
            active_peak = 0.0
        active_end = int(row["start"]) + FRAME_SAMPLES
        active_peak = max(active_peak, float(row["peak"]))
        silent_frames = 0
    if active_tone:
        runs.append(
            {
                "tone_hz": active_tone,
                "start_ms": round(1000.0 * active_start / sample_rate, 1),
                "end_ms": round(1000.0 * active_end / sample_rate, 1),
                "peak": round(active_peak, 6),
            }
        )
    runs = [
        row
        for row in runs
        if float(row["end_ms"]) - float(row["start_ms"]) >= MIN_TONE_RUN_MS
    ]
    return {
        "sample_rate": sample_rate,
        "channels": channels,
        "frames": len(mono),
        "duration_seconds": round(len(mono) / sample_rate, 4),
        "peak": round(max((abs(sample) for sample in mono), default=0.0), 6),
        "runs": runs,
        "tones": [row["tone_hz"] for row in runs],
    }


def evaluate_case(case: Case, analysis: dict[str, Any]) -> tuple[bool, str]:
    tones = list(analysis.get("tones", []))
    if case.expected_tones is not None:
        expected = list(case.expected_tones)
        if tones[: len(expected)] != expected:
            return False, f"expected leading tones {expected}, got {tones}"
    expected_runs = 2 if case.mode.endswith("Queue") else case.repeats
    if len(tones) != expected_runs:
        return False, f"expected exactly {expected_runs} tone runs, got {tones}"
    if any(tone not in (440, 660) for tone in tones):
        return False, f"unexpected tone classification: {tones}"
    if case.require_both and set(tones) != {440, 660}:
        return False, f"both 440 and 660 Hz are required, got {tones}"
    return True, "semantic tone contract satisfied"


_SELECTION_CHILD_RE = re.compile(
    r"^pack-play selection-child trigger=(\d+)/(\d+) ordinal=(-|\d+) "
    r"guid=([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}) "
    r"firing=(\d+)$"
)


def null_trigger_lines(stdout: str) -> list[str]:
    return [line for line in stdout.splitlines() if "pack-play trigger " in line]


def wasapi_trigger_lines(stdout: str) -> list[str]:
    return [line for line in stdout.splitlines() if line.startswith("pack-play trigger ")]


def parse_selection_child_lines(stdout: str) -> list[dict[str, Any]]:
    parsed: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if not line.startswith("pack-play selection-child"):
            continue
        match = _SELECTION_CHILD_RE.fullmatch(line)
        if match is None:
            raise QualificationError(f"Unreadable selection-child line: {line}")
        ordinal = match.group(3)
        parsed.append(
            {
                "trigger": int(match.group(1)),
                "repeats": int(match.group(2)),
                "ordinal": None if ordinal == "-" else int(ordinal),
                "guid": match.group(4),
                "firing": int(match.group(5)),
                "line": line,
            }
        )
    return parsed


_TRIGGER_FIRING_RE = re.compile(
    r"^pack-play trigger (\d+)/(\d+) peak=\S+ firing=(\d+)$"
)


def parse_trigger_firing_lines(stdout: str) -> list[dict[str, Any]]:
    parsed: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if not line.startswith("pack-play trigger "):
            continue
        match = _TRIGGER_FIRING_RE.fullmatch(line)
        if match is None:
            raise QualificationError(f"Unreadable pack-play trigger line: {line}")
        parsed.append(
            {
                "trigger": int(match.group(1)),
                "repeats": int(match.group(2)),
                "firing": int(match.group(3)),
                "line": line,
            }
        )
    return parsed


def require_selection_child_run(stdout: str, repeats: int) -> list[dict[str, Any]]:
    rows = parse_selection_child_lines(stdout)
    triggers = parse_trigger_firing_lines(stdout)
    if len(rows) != repeats:
        raise QualificationError(
            f"expected exactly {repeats} selection-child lines, got {len(rows)}"
        )
    if len(triggers) != repeats:
        raise QualificationError(
            f"expected exactly {repeats} trigger lines with firing, got {len(triggers)}"
        )
    for index, (row, trigger) in enumerate(zip(rows, triggers), start=1):
        if row["trigger"] != index or row["repeats"] != repeats:
            raise QualificationError(
                f"selection-child trigger order mismatch at {row['line']}"
            )
        if trigger["trigger"] != index or trigger["repeats"] != repeats:
            raise QualificationError(
                f"pack-play trigger order mismatch at {trigger['line']}"
            )
        if row["guid"] == _NULL_OBJECT_ID:
            raise QualificationError("selection-child guid is null")
        if row["firing"] != trigger["firing"]:
            raise QualificationError(
                "selection-child firing "
                f"{row['firing']} does not match trigger firing {trigger['firing']} "
                f"at {row['line']}"
            )
    return rows


def _authored_sound_hz(sounds: dict[str, Any]) -> dict[str, int]:
    if not isinstance(sounds, dict):
        raise QualificationError("authoring sounds are missing")
    mapping: dict[str, int] = {}
    for name, hz in (("A", 440), ("B", 660)):
        sound = sounds.get(name)
        if not isinstance(sound, dict):
            raise QualificationError(f"authoring sound {name} is missing")
        guid = sound.get("guid")
        if (
            type(guid) is not str
            or _OBJECT_ID_RE.fullmatch(guid) is None
            or guid == _NULL_OBJECT_ID
        ):
            raise QualificationError(
                f"authoring sound {name} guid is unusable: {guid!r}"
            )
        if guid in mapping:
            raise QualificationError("authoring sounds A and B share a guid")
        mapping[guid] = hz
    return mapping


def selection_child_tone_contract(
    case: Case,
    sounds: dict[str, Any],
    stdout: str,
    wav_tones: list[Any],
) -> tuple[bool, str]:
    try:
        rows = require_selection_child_run(stdout, case.repeats)
        hz_by_guid = _authored_sound_hz(sounds)
    except QualificationError as error:
        return False, str(error)
    mapped: list[int] = []
    for row in rows:
        guid = row["guid"]
        if guid not in hz_by_guid:
            return False, f"selection-child guid is not in this authoring run: {guid}"
        mapped.append(hz_by_guid[guid])
    tones = [int(tone) for tone in wav_tones]
    if mapped != tones:
        return False, f"selection-child tones {mapped} != wav tones {tones}"
    return True, "selection-child matches authored sounds and wav"


def verify_ready_before_first_trigger(stdout: str, host_pid: int) -> dict[str, Any]:
    lines = stdout.splitlines()
    ready_line = f"pack-play capture-ready pid={host_pid}"
    ready_indices = [
        index for index, line in enumerate(lines) if line == ready_line
    ]
    if len(ready_indices) != 1:
        raise QualificationError(
            f"Expected exactly one capture-ready line {ready_line!r}, "
            f"found {len(ready_indices)}"
        )
    trigger_indices = [
        index for index, line in enumerate(lines)
        if line.startswith("pack-play trigger ")
    ]
    if not trigger_indices:
        raise QualificationError("Pack playback emitted no trigger line")
    ready_index = ready_indices[0]
    first_trigger_index = trigger_indices[0]
    if ready_index >= first_trigger_index:
        raise QualificationError(
            "Capture-ready line did not precede the first trigger line"
        )
    return {
        "ready_line": ready_line,
        "ready_line_index": ready_index,
        "first_trigger_line_index": first_trigger_index,
        "ordering_verified": True,
    }


def run_capture(
    capture_exe: Path,
    capture_path: Path,
    capture_seconds: float,
    host_cmd: list[str],
    expected_triggers: int,
    ready_gate_path: Path,
) -> dict[str, Any]:
    if capture_path.exists():
        raise QualificationError(f"Fresh capture path already exists: {capture_path}")
    if ready_gate_path.exists():
        raise QualificationError(
            f"Fresh capture-ready gate already exists: {ready_gate_path}"
        )
    started_ns = time.time_ns()
    host = subprocess.Popen(
        host_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    host_stdout_lines: list[str] = []
    host_stderr_lines: list[str] = []
    armed = threading.Event()
    armed_line = f"pack-play capture-ready armed pid={host.pid}"

    def drain_stdout() -> None:
        assert host.stdout is not None
        for line in host.stdout:
            host_stdout_lines.append(line)
            if line.strip() == armed_line:
                armed.set()

    def drain_stderr() -> None:
        assert host.stderr is not None
        host_stderr_lines.extend(host.stderr)

    stdout_thread = threading.Thread(target=drain_stdout, daemon=True)
    stderr_thread = threading.Thread(target=drain_stderr, daemon=True)
    stdout_thread.start()
    stderr_thread.start()
    capture: subprocess.Popen[Any] | None = None
    capture_stdout = ""
    capture_stderr = ""
    try:
        if not armed.wait(timeout=10.0):
            raise QualificationError(
                "Pack playback did not arm a fresh capture-ready gate"
            )
        capture = subprocess.Popen(
            [
                str(capture_exe),
                "--pid",
                str(host.pid),
                "--out",
                str(capture_path),
                "--seconds",
                str(capture_seconds),
                "--capture-ready-gate",
                str(ready_gate_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        host.wait(timeout=WASAPI_HANDSHAKE_S + capture_seconds + 10)
        capture_stdout, capture_stderr = capture.communicate(
            timeout=10 + capture_seconds + 10
        )
    except BaseException:
        if capture is not None:
            terminate_owned_process(capture)
        terminate_owned_process(host)
        raise
    finally:
        stdout_thread.join(timeout=5.0)
        stderr_thread.join(timeout=5.0)
        if stdout_thread.is_alive() or stderr_thread.is_alive():
            raise QualificationError("Host output drain threads did not stop")
    assert capture is not None
    host_stdout = "".join(host_stdout_lines)
    host_stderr = "".join(host_stderr_lines)
    trigger_lines = wasapi_trigger_lines(host_stdout)
    try:
        ordering = verify_ready_before_first_trigger(host_stdout, host.pid)
    except QualificationError as error:
        raise HandshakeFailure(
            str(error), host.returncode, host_stdout, host_stderr
        ) from error
    host_result = {
        "command": host_cmd,
        "pid": host.pid,
        "returncode": host.returncode,
        "stdout": host_stdout,
        "stderr": host_stderr,
        "trigger_lines": trigger_lines,
    }
    if capture.returncode != 0:
        raise QualificationError(
            f"Loopback capture failed ({capture.returncode}): {capture_stderr}"
        )
    if not ready_gate_path.is_file():
        raise QualificationError(
            f"Capture-ready gate was not published: {ready_gate_path}"
        )
    ready_stat = ready_gate_path.stat()
    if ready_stat.st_mtime_ns <= started_ns:
        raise QualificationError(
            f"Capture-ready gate is not newer than its action: {ready_gate_path}"
        )
    expected_marker = gate_marker_for_pack_play(host.pid)
    marker = ready_gate_path.read_text(encoding="ascii")
    if not gate_identifies_pack_play_not_recorder(marker, host.pid, capture.pid):
        raise QualificationError(
            f"Capture-ready gate PID mismatch for host {host.pid}: {marker!r}"
        )
    if not capture_path.exists() or capture_path.stat().st_size <= 44:
        raise QualificationError(f"Loopback capture was not created: {capture_path}")
    if capture_path.stat().st_mtime_ns < started_ns:
        raise QualificationError(f"Loopback capture is older than its action: {capture_path}")
    if f"process_id={host.pid}" not in capture_stdout:
        raise QualificationError(
            f"Loopback capture did not identify host PID {host.pid}: {capture_stdout}"
        )
    if host_result["returncode"] != 0:
        raise QualificationError(
            f"Pack playback failed: {host_result['stderr']}\n{host_result['stdout']}"
        )
    if len(host_result["trigger_lines"]) != expected_triggers:
        raise QualificationError(
            f"Expected {expected_triggers} fresh trigger lines, got "
            f"{host_result['trigger_lines']}"
        )
    return {
        "baseline": {"path_existed": False, "captured_at_ns": started_ns},
        "path": str(capture_path),
        "bytes": capture_path.stat().st_size,
        "sha256": sha256(capture_path),
        "capture_returncode": capture.returncode,
        "capture_stdout": capture_stdout,
        "capture_stderr": capture_stderr,
        "ready_gate": {
            "baseline": {"path_existed": False, "started_at_ns": started_ns},
            "path": str(ready_gate_path),
            "bytes": ready_stat.st_size,
            "mtime_ns": ready_stat.st_mtime_ns,
            "sha256": sha256(ready_gate_path),
            "marker": marker.rstrip("\n"),
            "expected_process_id": host.pid,
            "armed_line": armed_line,
            **ordering,
        },
        "host": host_result,
    }


def terminate_owned_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Qualify Studio-generated Packs for all six Container variants."
    )
    parser.add_argument("--studio-exe", required=True, type=Path)
    parser.add_argument("--host-exe", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--capture-exe", type=Path)
    parser.add_argument("--null-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    studio_exe = args.studio_exe.resolve()
    host_exe = args.host_exe.resolve()
    output_root = args.output_root.resolve()
    capture_exe = args.capture_exe.resolve() if args.capture_exe else None
    if output_root.exists():
        raise QualificationError(
            f"--output-root must be a fresh, nonexistent path: {output_root}"
        )
    for executable in (studio_exe, host_exe):
        if not executable.is_file():
            raise QualificationError(f"Executable was not found: {executable}")
    if not args.null_only and (capture_exe is None or not capture_exe.is_file()):
        raise QualificationError("--capture-exe is required for WASAPI qualification")
    packc_candidates = (studio_exe.parent / "packc.exe", studio_exe.parent / "harken_packc.exe")
    if not any(candidate.is_file() for candidate in packc_candidates):
        raise QualificationError(f"packc is not bundled beside Studio: {studio_exe.parent}")
    if port_is_open():
        raise QualificationError(
            f"Studio IPC port {PORT} is already in use; do not share a qualification session"
        )

    output_root.mkdir(parents=True)
    wav_dir = output_root / "wav"
    project_location = output_root / "project"
    export_dir = output_root / "export"
    capture_dir = output_root / "wasapi"
    wav_dir.mkdir()
    project_location.mkdir()
    export_dir.mkdir()
    if not args.null_only:
        capture_dir.mkdir()
    tone_a = wav_dir / "tone_a_440.wav"
    tone_b = wav_dir / "tone_b_660.wav"
    write_tone(tone_a, 440.0)
    write_tone(tone_b, 660.0)

    launch = [
        str(studio_exe),
        "--create-project",
        "--project-name",
        PROJECT_NAME,
        "--project-location",
        str(project_location),
    ]
    report: dict[str, Any] = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "started_at_ns": time.time_ns(),
        "binaries": {
            "studio": {"path": str(studio_exe), "sha256": sha256(studio_exe)},
            "host": {"path": str(host_exe), "sha256": sha256(host_exe)},
            "capture": (
                {"path": str(capture_exe), "sha256": sha256(capture_exe)}
                if capture_exe
                else None
            ),
        },
        "studio_launch": launch,
        "source_wavs": {
            "A": {"path": str(tone_a), "hz": 440, "sha256": sha256(tone_a)},
            "B": {"path": str(tone_b), "hz": 660, "sha256": sha256(tone_b)},
        },
    }
    report_path = output_root / "report.json"
    studio_process: subprocess.Popen[Any] | None = None
    ipc: Ipc | None = None
    try:
        studio_process = subprocess.Popen(launch)
        ipc = wait_for_ipc()
        state = wait_for_project(ipc)
        project_path = Path(str(state["project_path"]))
        project_dir = project_path.parent
        report["project"] = {"state": state, "path": str(project_path)}
        report["authoring"] = author_project(ipc, project_dir, tone_a, tone_b)
        identities = report_identity_fields(report)
        variable_id = identities["variable_stable_id"]
        platforms = ipc.require("list_project_platforms", {})["platforms"]
        candidates = [entry for entry in platforms if entry["basePlatform"] == "Windows"]
        if len(candidates) != 1:
            raise QualificationError("Qualification requires exactly one Windows-based project platform")
        platform = candidates[0]
        ipc.require(
            "export_pack", {"output_path": str(export_dir), "platform_id": platform["id"]}
        )
        export_dir = export_dir / platform["name"]
        report["export"] = wait_for_export(ipc)
        companions = {}
        for name in ["Init", *(f"Play_{case.name}" for case in CASES)]:
            binary_path = export_dir / f"{name}.bytes"
            json_path = export_dir / f"{name}.json"
            if not binary_path.is_file() or not json_path.is_file():
                raise QualificationError(f"Pack companion pair was not generated: {name}")
            companions[name] = {
                "bytes_path": str(binary_path),
                "bytes_sha256": sha256(binary_path),
                "json_path": str(json_path),
                "json_sha256": sha256(json_path),
                "data": json.loads(json_path.read_text(encoding="utf-8")),
            }
        report["companions"] = companions

        null_results: dict[str, Any] = {}
        report["null_smoke"] = null_results
        for case in CASES:
            command = host_command(
                host_exe, export_dir, case, variable_id, True
            )
            result = run_host(
                command,
                timeout=case.repeats * case.seconds_per_trigger + 20.0,
            )
            child_rows = require_selection_child_run(result["stdout"], case.repeats)
            result["selection_child_lines"] = [row["line"] for row in child_rows]
            result["ok"] = (
                result["returncode"] == 0
                and len(result["trigger_lines"]) == case.repeats
                and len(child_rows) == case.repeats
            )
            if not result["ok"]:
                raise QualificationError(f"Null smoke failed for {case.name}: {result}")
            null_results[case.name] = result
        if not args.null_only:
            assert capture_exe is not None
            wasapi_results: dict[str, Any] = {}
            report["wasapi"] = wasapi_results
            for case in CASES:
                capture_seconds = (
                    case.repeats * case.seconds_per_trigger + 1.8
                )
                capture_path = capture_dir / f"{case.name}.wav"
                ready_gate_path = capture_dir / (
                    f"{case.name}.{uuid.uuid4().hex}.capture-ready"
                )
                command = host_command(
                    host_exe,
                    export_dir,
                    case,
                    variable_id,
                    False,
                    ready_gate_path,
                )
                result = run_capture(
                    capture_exe,
                    capture_path,
                    capture_seconds,
                    command,
                    case.repeats,
                    ready_gate_path,
                )
                analysis = analyze_capture(capture_path)
                ok, verdict = evaluate_case(case, analysis)
                if ok and case.expected_tones is None:
                    sounds = report["authoring"]["containers"][case.name]["sounds"]
                    child_ok, child_verdict = selection_child_tone_contract(
                        case,
                        sounds,
                        result["host"]["stdout"],
                        analysis.get("tones", []),
                    )
                    ok = child_ok
                    verdict = (
                        f"{verdict}; {child_verdict}" if child_ok else child_verdict
                    )
                result["analysis"] = analysis
                result["ok"] = ok
                result["verdict"] = verdict
                wasapi_results[case.name] = result
                if not ok:
                    raise QualificationError(
                        f"WASAPI semantic failure for {case.name}: {verdict}"
                    )
        attach_preview_contract(report, ipc, args.null_only)
        summary_value = "null-smoke-pass" if args.null_only else "qualified-pass"
        report["run_status"] = (
            "null-smoke-only" if args.null_only else "wasapi-qualified"
        )
        report["summary"] = {case.name: summary_value for case in CASES}
        report["completed_at_ns"] = time.time_ns()
        report_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=True), encoding="utf-8"
        )
        print(json.dumps(report["summary"], indent=2))
        print(f"report={report_path}")
        return 0
    except Exception as error:  # noqa: BLE001
        report["summary_is_completion"] = False
        report.pop("u3_12b_complete", None)
        report["error"] = str(error)
        host_result = host_result_from_exception(error)
        if host_result is not None:
            report["host_result"] = host_result
        report["failed_at_ns"] = time.time_ns()
        report_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=True), encoding="utf-8"
        )
        raise
    finally:
        if ipc is not None:
            ipc.close()
        if studio_process is not None:
            terminate_owned_process(studio_process)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except QualificationError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
