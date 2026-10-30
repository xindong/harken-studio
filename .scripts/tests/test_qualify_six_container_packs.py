from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock as mock
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "qualify_six_container_packs.py"
SPEC = importlib.util.spec_from_file_location(
    "qualify_six_container_packs", SCRIPT_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class FakeHost:
    def __init__(self, pid: int) -> None:
        self.pid = pid
        self.stdout = io.StringIO(
            f"pack-play capture-ready armed pid={pid}\n"
        )
        self.stderr = io.StringIO("")
        self.returncode: int | None = None
        self.terminated = False
        self.waited = False

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True
        self.returncode = -15

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float | None = None) -> int:
        self.wait_timeout = timeout
        self.waited = True
        assert self.returncode is not None
        return self.returncode



class _RecordingCapture:
    def __init__(self) -> None:
        self.pid = 99
        self.returncode = 0
        self.communicate_timeout: float | None = None

    def communicate(self, timeout: float | None = None) -> tuple[str, str]:
        self.communicate_timeout = timeout
        return "", ""

    def poll(self) -> int:
        return self.returncode

    def terminate(self) -> None:
        self.returncode = -15

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float | None = None) -> int:
        del timeout
        return self.returncode


class _PublishingCapture:
    def __init__(self, wav: Path, gate: Path, host_pid: int) -> None:
        self.pid = 99
        self.returncode = 0
        self.communicate_timeout: float | None = None
        self._wav = wav
        self._gate = gate
        self._host_pid = host_pid

    def communicate(self, timeout: float | None = None) -> tuple[str, str]:
        self.communicate_timeout = timeout
        marker = "process_id=%s\n" % self._host_pid
        self._gate.write_text(marker, encoding="ascii")
        self._wav.write_bytes(b"R" * 45)
        stamp = time.time_ns() + 1_000_000
        for path in (self._gate, self._wav):
            os.utime(path, ns=(stamp, stamp))
        return marker, ""

    def poll(self) -> int:
        return self.returncode

    def terminate(self) -> None:
        self.returncode = -15

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float | None = None) -> int:
        del timeout
        return self.returncode



def _report_from_main_popen_failure(error: BaseException) -> dict:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        studio = bin_dir / "HarkenStudio.exe"
        host = bin_dir / "harken_host.exe"
        capture = bin_dir / "harken_process_loopback_capture.exe"
        (bin_dir / "Harkenizer.exe").write_bytes(b"harkenizer")
        studio.write_bytes(b"studio")
        host.write_bytes(b"host")
        capture.write_bytes(b"capture")
        output = root / "fresh-out"
        argv = [
            "qualify_six_container_packs.py",
            "--studio-exe",
            str(studio),
            "--host-exe",
            str(host),
            "--capture-exe",
            str(capture),
            "--output-root",
            str(output),
        ]
        with mock.patch.object(MODULE, "port_is_open", return_value=False), mock.patch.object(
            sys, "argv", argv
        ), mock.patch.object(MODULE.subprocess, "Popen", side_effect=error):
            try:
                MODULE.main()
            except MODULE.QualificationError as caught:
                if caught is not error:
                    raise
            else:
                raise AssertionError("main returned without writing a failure report")
        return MODULE.json.loads((output / "report.json").read_text(encoding="utf-8"))

class QualifySixContainerPacksTest(unittest.TestCase):
    def test_export_polling_uses_structured_outcome(self) -> None:
        status = {
            "in_progress": False,
            "message": "Completed",
            "result": {"status": "ok", "result": {"success": True}},
        }
        records = [{"platform_id": "instance-id", "platform_name": "Windows_Desktop"}]
        ipc = mock.Mock()
        ipc.require.side_effect = [status, {"records": records}]
        self.assertEqual({"status": status, "records": records}, MODULE.wait_for_export(ipc))

    def test_export_polling_rejects_missing_failed_or_partial_outcomes(self) -> None:
        for outcome in ({}, {"status": "error"}, {"status": "ok", "result": {"success": False}}):
            with self.subTest(outcome=outcome):
                ipc = mock.Mock()
                ipc.require.side_effect = [
                    {"in_progress": False, "message": "Export complete", "result": outcome},
                    {"records": []},
                ]
                with self.assertRaises(MODULE.QualificationError):
                    MODULE.wait_for_export(ipc)

    def test_capture_spawn_failure_reaps_armed_host(self) -> None:
        host = FakeHost(4321)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(
                MODULE.subprocess,
                "Popen",
                side_effect=[host, OSError("capture spawn failed")],
            ):
                with self.assertRaisesRegex(OSError, "capture spawn failed"):
                    MODULE.run_capture(
                        Path("capture.exe"),
                        root / "capture.wav",
                        1.0,
                        ["host.exe"],
                        1,
                        root / "capture.ready",
                    )

        self.assertTrue(host.terminated)
        self.assertTrue(host.waited)

    def test_wasapi_handshake_timeout_is_one_constant(self) -> None:
        case = next(item for item in MODULE.CASES if item.name == "Shuffle")
        gate = Path("gate.ready")
        wasapi = MODULE.host_command(
            Path("host.exe"), Path("export"), case, "variable", False, gate
        )
        flag = wasapi.index("--capture-ready-timeout-ms")
        self.assertEqual(str(MODULE.WASAPI_CAPTURE_READY_TIMEOUT_MS), wasapi[flag + 1])
        self.assertEqual(20000, MODULE.WASAPI_CAPTURE_READY_TIMEOUT_MS)
        self.assertEqual(
            MODULE.WASAPI_CAPTURE_READY_TIMEOUT_MS / 1000,
            MODULE.WASAPI_HANDSHAKE_S,
        )
        for null_gate in (gate, None):
            null_command = MODULE.host_command(
                Path("host.exe"),
                Path("export"),
                case,
                "variable",
                True,
                null_gate,
            )
            self.assertIn("--null", null_command)
            self.assertNotIn("--capture-ready-timeout-ms", null_command)
        without_gate = MODULE.host_command(
            Path("host.exe"), Path("export"), case, "variable", False, None
        )
        self.assertNotIn("--capture-ready-timeout-ms", without_gate)

        host = FakeHost(4321)
        host.returncode = 0
        capture = _RecordingCapture()
        capture_seconds = 1.6
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                with self.assertRaises(MODULE.QualificationError):
                    MODULE.run_capture(
                        Path("capture.exe"),
                        root / "capture.wav",
                        capture_seconds,
                        ["host.exe"],
                        1,
                        root / "capture.ready",
                    )
        self.assertEqual(MODULE.WASAPI_HANDSHAKE_S + capture_seconds + 10, host.wait_timeout)
        self.assertEqual(10 + capture_seconds + 10, capture.communicate_timeout)

    def test_wasapi_handshake_publishes_gate_and_returns(self) -> None:
        host_pid = 4321
        host = FakeHost(host_pid)
        host.returncode = 0
        host.stdout = io.StringIO(
            ("pack-play capture-ready armed pid=%s\n"
             "pack-play capture-ready pid=%s\n"
             "pack-play trigger 1/1 peak=0.5\n") % (host_pid, host_pid)
        )
        capture_seconds = 1.6
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wav = root / "capture.wav"
            gate = root / "capture.ready"
            capture = _PublishingCapture(wav, gate, host_pid)
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                result = MODULE.run_capture(
                    Path("capture.exe"),
                    wav,
                    capture_seconds,
                    ["host.exe"],
                    1,
                    gate,
                )
            self.assertEqual(
                MODULE.WASAPI_HANDSHAKE_S + capture_seconds + 10,
                host.wait_timeout,
            )
            self.assertEqual(10 + capture_seconds + 10, capture.communicate_timeout)
            self.assertGreater(wav.stat().st_size, 44)
            self.assertEqual("process_id=%s\n" % host_pid, gate.read_text(encoding="ascii"))
        self.assertEqual(0, result["host"]["returncode"])
        self.assertEqual(0, result["capture_returncode"])
        self.assertIn("process_id=%s" % host_pid, result["capture_stdout"])
        self.assertTrue(result["ready_gate"]["ordering_verified"])
        self.assertLess(
            result["ready_gate"]["ready_line_index"],
            result["ready_gate"]["first_trigger_line_index"],
        )
        self.assertEqual(
            ["pack-play trigger 1/1 peak=0.5"],
            result["host"]["trigger_lines"],
        )

    def test_handshake_failure_keeps_host_io(self) -> None:
        host = FakeHost(3152)
        host.returncode = 0
        host.stdout = io.StringIO(
            "pack-play capture-ready armed pid=3152\n"
            "SKIPPED: no real WASAPI device\n"
        )
        host.stderr = io.StringIO("")
        capture = _RecordingCapture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                with self.assertRaises(MODULE.QualificationError) as caught:
                    MODULE.run_capture(
                        Path("capture.exe"),
                        root / "capture.wav",
                        1.6,
                        ["host.exe"],
                        8,
                        root / "capture.ready",
                    )
        text = str(caught.exception)
        self.assertIn("found", text)
        self.assertIn("host_returncode=0", text)
        self.assertIn("host_stdout_tail=", text)
        self.assertIn("host_stderr_tail=", text)
        self.assertIn("SKIPPED: no real WASAPI device", text)
        self.assertTrue(text.endswith("host_stderr_tail="))
        host_result = MODULE.host_result_from_exception(caught.exception)
        self.assertIsNotNone(host_result)
        report = {"error": text, "host_result": host_result}
        self.assertEqual(
            {"returncode", "stdout", "stderr"},
            set(report["host_result"]),
        )
        self.assertEqual(0, report["host_result"]["returncode"])
        self.assertEqual("", report["host_result"]["stderr"])
        self.assertIn("armed pid=3152", report["host_result"]["stdout"])
        self.assertNotIn(
            "pack-play capture-ready pid=",
            report["host_result"]["stdout"],
        )
        self.assertIsNone(
            MODULE.host_result_from_exception(MODULE.QualificationError("other failure"))
        )

    def test_handshake_failure_tails_are_capped_but_report_is_full(self) -> None:
        host = FakeHost(3152)
        host.returncode = 3
        marker = "TAIL-MARKER"
        host.stdout = io.StringIO(
            "pack-play capture-ready armed pid=3152\n" + ("x" * 4000) + marker + "\n"
        )
        host.stderr = io.StringIO("e" * 2500)
        capture = _RecordingCapture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                with self.assertRaises(MODULE.QualificationError) as caught:
                    MODULE.run_capture(
                        Path("capture.exe"),
                        root / "capture.wav",
                        1.0,
                        ["host.exe"],
                        1,
                        root / "capture.ready",
                    )
        text = str(caught.exception)
        self.assertIn("found", text)
        stdout_tail = text.split("host_stdout_tail=", 1)[1].split(
            "\nhost_stderr_tail=", 1
        )[0]
        stderr_tail = text.split("host_stderr_tail=", 1)[1]
        self.assertLessEqual(len(stdout_tail), 2000)
        self.assertLessEqual(len(stderr_tail), 2000)
        self.assertIn(marker, stdout_tail)
        saved = MODULE.host_result_from_exception(caught.exception)
        self.assertIsNotNone(saved)
        self.assertGreater(len(saved["stdout"]), 2000)
        self.assertGreater(len(saved["stderr"]), 2000)
        self.assertEqual(3, saved["returncode"])
        self.assertTrue(saved["stdout"].endswith(stdout_tail))
        self.assertTrue(saved["stderr"].endswith(stderr_tail))

    def test_ready_after_trigger_keeps_did_not_precede_and_host_io(self) -> None:
        host = FakeHost(4321)
        host.returncode = 0
        host.stdout = io.StringIO(
            "pack-play capture-ready armed pid=4321\n"
            "pack-play trigger 1/1 peak=0.5\n"
            "pack-play capture-ready pid=4321\n"
        )
        capture = _RecordingCapture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                with self.assertRaises(MODULE.QualificationError) as caught:
                    MODULE.run_capture(
                        Path("capture.exe"),
                        root / "capture.wav",
                        1.0,
                        ["host.exe"],
                        1,
                        root / "capture.ready",
                    )
        text = str(caught.exception)
        self.assertIn("did not precede", text)
        self.assertIn("host_returncode=0", text)
        saved = MODULE.host_result_from_exception(caught.exception)
        self.assertEqual(0, saved["returncode"])
        self.assertIn("pack-play trigger 1/1", saved["stdout"])

    def test_capture_failure_after_ready_does_not_forge_host_result(self) -> None:
        host_pid = 4321
        host = FakeHost(host_pid)
        host.returncode = 0
        host.stdout = io.StringIO(
            (
                "pack-play capture-ready armed pid=%s\n"
                "pack-play capture-ready pid=%s\n"
                "pack-play trigger 1/1 peak=0.5\n"
            )
            % (host_pid, host_pid)
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wav = root / "capture.wav"
            gate = root / "capture.ready"
            capture = _PublishingCapture(wav, gate, host_pid)
            capture.returncode = 3
            with mock.patch.object(MODULE.subprocess, "Popen", side_effect=[host, capture]):
                with self.assertRaises(MODULE.QualificationError) as caught:
                    MODULE.run_capture(
                        Path("capture.exe"), wav, 1.0, ["host.exe"], 1, gate
                    )
        text = str(caught.exception)
        self.assertIn("Loopback capture failed", text)
        self.assertNotIn("host_returncode=", text)
        self.assertIsNone(MODULE.host_result_from_exception(caught.exception))

    def test_main_writes_handshake_failure_report(self) -> None:
        failure = MODULE.HandshakeFailure(
            "Expected exactly one capture-ready line "
            "'pack-play capture-ready pid=3152', found 0",
            0,
            "pack-play capture-ready armed pid=3152\n",
            "",
        )
        written = _report_from_main_popen_failure(failure)
        self.assertEqual(Path(written["binaries"]["harkenizer"]["path"]).name, "Harkenizer.exe")
        self.assertEqual(written["binaries"]["harkenizer"]["sha256"],
                         MODULE.hashlib.sha256(b"harkenizer").hexdigest().upper())
        self.assertIn("found", written["error"])
        self.assertEqual(
            {"returncode", "stdout", "stderr"},
            set(written["host_result"]),
        )
        self.assertEqual(0, written["host_result"]["returncode"])
        self.assertEqual("", written["host_result"]["stderr"])
        self.assertIn("armed pid=3152", written["host_result"]["stdout"])
        self.assertNotIn(
            "pack-play capture-ready pid=",
            written["host_result"]["stdout"],
        )
        self.assertIs(False, written["summary_is_completion"])

    def test_main_other_failure_omits_host_result(self) -> None:
        written = _report_from_main_popen_failure(
            MODULE.QualificationError("Null smoke failed for Random_Draw")
        )
        self.assertNotIn("host_result", written)
        self.assertIn("Null smoke failed", written["error"])
        self.assertIs(False, written["summary_is_completion"])

    def test_ready_ordering_rejects_missing_ready_line(self) -> None:
        with self.assertRaises(MODULE.QualificationError):
            MODULE.verify_ready_before_first_trigger(
                "pack-play trigger 1/1 peak=0.5\n", 4321
            )

    def test_ready_ordering_rejects_ready_after_trigger(self) -> None:
        stdout = (
            "pack-play trigger 1/1 peak=0.5\n"
            "pack-play capture-ready pid=4321\n"
        )
        with self.assertRaises(MODULE.QualificationError):
            MODULE.verify_ready_before_first_trigger(stdout, 4321)

    def test_ipc_runtime_stable_id_accepts_object_id_string(self) -> None:
        item = {
            "runtime_stable_id": "3da91a64-b947-4560-0000-000000000007",
            "object_id": 123,
            "authoring_handle": 7,
        }
        self.assertEqual(
            "3da91a64-b947-4560-0000-000000000007",
            MODULE.runtime_stable_id_from_ipc(item, "Play_Random_Draw"),
        )

    def test_ipc_runtime_stable_id_rejects_zero_number_and_missing(self) -> None:
        for value, name in (
            ({}, "missing"),
            ({"runtime_stable_id": "00000000-0000-0000-0000-000000000000"}, "null"),
            ({"runtime_stable_id": "0"}, "decimal_zero"),
            ({"runtime_stable_id": 7}, "number"),
            ({"runtime_stable_id": "07"}, "leading_zero"),
            ({"runtime_stable_id": "7"}, "decimal_string"),
            (
                {
                    "runtime_stable_id": "3da91a64-b947-4560-0000-000000000007",
                    "object_id": "7",
                },
                "object_id_string",
            ),
            ({"object_id": 123}, "object_id_only"),
            ({"authoring_handle": 7}, "authoring_handle_only"),
            (
                {
                    "runtime_stable_id": "3da91a64-b947-4560-0000-000000000007",
                    "object_id": True,
                },
                "object_id_bool",
            ),
            (
                {
                    "runtime_stable_id": "3DA91A64-B947-4560-0000-000000000007",
                },
                "uppercase",
            ),
            (
                {
                    "runtime_stable_id": "{3da91a64-b947-4560-0000-000000000007}",
                },
                "braces",
            ),
        ):
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    MODULE.QualificationError, "runtime_stable_id|object_id"
                ):
                    MODULE.runtime_stable_id_from_ipc(value, "Play_Random_Draw")

    def _sample_report(self, **overrides: object) -> dict:
        uuid_a = "3da91a64-b947-4560-0000-000000000007"
        uuid_var = "3da91a64-b947-4560-0000-000000000003"
        containers = {
            case.name: {"action_stable_id": uuid_a} for case in MODULE.CASES
        }
        report: dict = {
            "schema_version": MODULE.REPORT_SCHEMA_VERSION,
            "authoring": {
                "variable_stable_id": uuid_var,
                "containers": containers,
            },
        }
        report.update(overrides)
        return report

    def test_report_identity_fields_accepts_schema_v2_uuid_strings(self) -> None:
        identities = MODULE.report_identity_fields(self._sample_report())
        self.assertEqual(
            "3da91a64-b947-4560-0000-000000000003",
            identities["variable_stable_id"],
        )
        self.assertEqual(
            "3da91a64-b947-4560-0000-000000000007",
            identities["Random_Draw"],
        )

    def test_report_identity_fields_rejects_schema_v1_or_numeric_ids(self) -> None:
        uuid_a = "3da91a64-b947-4560-0000-000000000007"
        cases = (
            ({"schema_version": 1}, "schema_version"),
            (
                {
                    "authoring": {
                        "variable_stable_id": 3,
                        "containers": {
                            case.name: {"action_stable_id": uuid_a}
                            for case in MODULE.CASES
                        },
                    }
                },
                "variable_stable_id",
            ),
            (
                {
                    "authoring": {
                        "variable_stable_id": uuid_a,
                        "containers": {
                            case.name: {"action_stable_id": 7}
                            for case in MODULE.CASES
                        },
                    }
                },
                "action_stable_id",
            ),
            (
                {
                    "authoring": {
                        "variable_stable_id": "00000000-0000-0000-0000-000000000000",
                        "containers": {
                            case.name: {"action_stable_id": uuid_a}
                            for case in MODULE.CASES
                        },
                    }
                },
                "variable_stable_id",
            ),
            (
                {
                    "authoring": {
                        "variable_stable_id": uuid_a,
                        "containers": {
                            case.name: {
                                "action_stable_id": "00000000-0000-0000-0000-000000000000"
                            }
                            for case in MODULE.CASES
                        },
                    }
                },
                "action_stable_id",
            ),
        )
        for overrides, needle in cases:
            with self.subTest(needle=needle):
                report = self._sample_report(**overrides)
                with self.assertRaisesRegex(MODULE.QualificationError, needle):
                    MODULE.report_identity_fields(report)

    def test_ready_ordering_reports_exact_line_indices(self) -> None:
        stdout = (
            "pack-play capture-ready waiting pid=4321\n"
            "pack-play capture-ready pid=4321\n"
            "pack-play trigger 1/1 peak=0.5\n"
        )
        self.assertEqual(
            {
                "ready_line": "pack-play capture-ready pid=4321",
                "ready_line_index": 1,
                "first_trigger_line_index": 2,
                "ordering_verified": True,
            },
            MODULE.verify_ready_before_first_trigger(stdout, 4321),
        )



class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t


class DeadlineIpc:
    def __init__(self, replies: list) -> None:
        self.replies = list(replies)
        self.calls = []
        self.unusable = False
        self.sequence = 0
        self.after = []

    def call(self, command, args=None, deadline=None):
        if self.unusable:
            raise MODULE.StreamUnusable("closed")
        now = MODULE.monotonic()
        if deadline is not None and deadline - now <= 0:
            raise MODULE.LocalDeadline("before io")
        self.sequence += 1
        self.calls.append({"cmd": command, "args": args or {}, "deadline": deadline, "at": now})
        if not self.replies:
            self.unusable = True
            raise MODULE.StreamUnusable("eof")
        item = self.replies.pop(0)
        if isinstance(item, BaseException):
            if isinstance(item, MODULE.StreamUnusable):
                self.unusable = True
            raise item
        if callable(item):
            item = item(self)
        item = dict(item)
        item.setdefault("request_id", f"six-pack-{self.sequence}")
        item["_accepted_at"] = MODULE.monotonic()
        if self.after:
            self.after.pop(0)()
        return item


def _tag(internal: int = 9) -> int:
    return ((1 << 63) | internal) - (1 << 64)


def _playing(path="obj", audition_id=4, handle=_tag(9)):
    return {"status": "ok", "result": {"state": "playing", "pending": False, "auditionID": audition_id, "playbackHandle": handle, "object_path": path}}


def _queued(path="obj", audition_id=4):
    return {"status": "ok", "result": {"state": "queued", "pending": True, "auditionID": audition_id, "playbackHandle": 0, "object_path": path}}


def _states(rows):
    return {"status": "ok", "result": {"auditions": rows}}


def _row(audition_id, state, pending, handle):
    return {"auditionID": audition_id, "state": state, "pending": pending, "playbackHandle": handle}


class PreviewContractTest(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self._monotonic = MODULE.monotonic
        self._pause = MODULE.pause
        MODULE.monotonic = self.clock
        MODULE.pause = lambda seconds: self.clock.__setattr__("t", self.clock.t + seconds) or None

    def tearDown(self):
        MODULE.monotonic = self._monotonic
        MODULE.pause = self._pause

    def _case(self, name):
        return next(case for case in MODULE.CASES if case.name == name)

    def test_immediate_playing_absent_is_not_false_red_or_tail_pass(self):
        ipc = DeadlineIpc([_playing(), _states([])])
        result = MODULE.run_preview_case(ipc, next(case for case in MODULE.CASES if case.name == "Sequence_Queue"), "obj", set())
        self.assertEqual("PASS", result["status"])
        self.assertEqual(0, result["stops"])
        self.assertEqual(["audition_play", "audition_get_state"], [item["cmd"] for item in ipc.calls])
        card = MODULE.technical_card("PASS", cleanup="PASS")
        self.assertEqual(("BLOCKED", "COVERAGE"), MODULE.overall(card))

    def test_queued_state0_then_stop_state0_then_absent(self):
        ipc = DeadlineIpc([
            _queued(),
            _states([_row(4, 0, True, 0)]),
            _states([_row(4, 1, False, _tag(9))]),
            {"status": "ok", "result": {"action": "stopped", "auditionID": 4}},
            _states([_row(4, 0, False, _tag(9))]),
            _states([]),
        ])
        result = MODULE.run_preview_case(ipc, self._case("Random_Queue"), "obj", set())
        self.assertEqual("PASS", result["status"])
        self.assertEqual([4], result["stop_ids"])
        self.assertNotIn(0, result["stop_ids"])

    def test_queued_budget_does_not_refresh_cleanup_deadline(self):
        def first_state(_ipc):
            self.clock.t = 0.2
            return _states([_row(4, 0, True, 0)])
        ipc = DeadlineIpc([
            _queued(),
            first_state,
            {"status": "ok", "result": {"action": "stopped", "auditionID": 4}},
            _states([]),
        ])
        def pause(seconds):
            self.clock.t = 1.5
        MODULE.pause = pause
        result = MODULE.run_preview_case(ipc, next(case for case in MODULE.CASES if case.name == "Sequence_Queue"), "obj", set())
        self.assertEqual("FAIL", result["status"])
        self.assertEqual("queued-budget", result["reason"])
        stop = next(item for item in ipc.calls if item["cmd"] == "audition_stop")
        self.assertEqual(1.7, stop["deadline"])
        self.assertEqual(4, stop["args"]["auditionID"])

    def test_no_successful_observation_does_not_invent_cleanup_clock(self):
        def play(_ipc):
            return _playing()
        ipc = DeadlineIpc([play])
        ipc.after.append(lambda: setattr(self.clock, "t", 10.0))
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertEqual("observe-budget", result["reason"])
        self.assertEqual(0, result["stops"])
        self.assertEqual(["audition_play"], [item["cmd"] for item in ipc.calls])

    def test_absent_before_start_does_not_stop(self):
        ipc = DeadlineIpc([_queued(), _states([])])
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertEqual("absent-before-start", result["reason"])
        self.assertEqual(0, result["stops"])

    def test_handle_mismatch_stops_original_id_only(self):
        ipc = DeadlineIpc([
            _playing(handle=_tag(9)),
            _states([_row(4, 1, False, _tag(8))]),
            {"status": "ok", "result": {"action": "stopped", "auditionID": 4}},
            _states([]),
        ])
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertEqual("handle-mismatch", result["reason"])
        self.assertEqual([4], result["stop_ids"])
        self.assertNotIn("stop_all", [item["args"].get("action") for item in ipc.calls])

    def test_nan_infinity_bool_and_float_are_not_ids(self):
        for bad in (float("nan"), float("inf"), 1.0, True, False, "4"):
            parsed = MODULE.parse_play({"status": "ok", "result": {"state": "playing", "pending": False, "auditionID": bad, "playbackHandle": _tag(1), "object_path": "obj"}}, "obj")
            self.assertIsNone(parsed["owned"])

    def test_path_mismatch_is_preview_fail_not_blocked(self):
        self.assertIsNone(MODULE.confirm_preview_path({"guid": "abc", "path": "a"}, {"runtime_stable_id": "zzz", "path": "a", "full_path": "a"}))
        self.assertEqual("obj", MODULE.confirm_preview_path({"guid": "abc", "path": "obj"}, {"runtime_stable_id": "abc", "path": "obj", "full_path": "obj"}))
        card = MODULE.technical_card("FAIL")
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))

    def test_transport_leaves_later_cases_not_run_and_preview_fail(self):
        ipc = DeadlineIpc([MODULE.StreamUnusable("down")])
        paths = {name: name for name in MODULE.CASE_NAMES}
        session = MODULE.run_preview_session(ipc, paths)
        self.assertEqual("FAIL", session["preview"])
        blocked = [row for row in session["cases"] if row.get("blocked_by_transport")]
        self.assertTrue(blocked)
        self.assertTrue(all(row["status"] == "NOT_RUN" for row in blocked))
        vc = next(row for row in session["cases"] if row["name"] == "Variable_Control")
        self.assertEqual(0, vc["plays"])
        self.assertEqual(0, vc["stops"])
        card = MODULE.technical_card("FAIL")
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))
        self.assertNotEqual("INVARIANT", MODULE.overall(card)[1])

    def test_gate_marker_uses_pack_pid_not_recorder_pid(self):
        self.assertTrue(MODULE.gate_identifies_pack_play_not_recorder("process_id=99\n", 99, 100))
        self.assertFalse(MODULE.gate_identifies_pack_play_not_recorder("process_id=100\n", 99, 100))

    def test_overall_rejects_completion_flag_and_keeps_coverage_block(self):
        card = MODULE.technical_card("PASS", cleanup="PASS")
        card["u3_12b_complete"] = True
        self.assertEqual(("FAIL", "INPUT"), MODULE.overall(card))
        card = MODULE.technical_card("PASS", cleanup="PASS")
        for key in MODULE.ATT_LAYERS:
            card[key] = "PASS"
        self.assertEqual(("BLOCKED", "SCRIPT_CANNOT_CLOSE_ATT07"), MODULE.overall(card))
        stale = {"run_attempted": False, "input_error": False, "summary_is_completion": False, "preview": "FAIL"}
        self.assertEqual(("FAIL", "STALE_EVIDENCE"), MODULE.overall(stale))

    def test_same_path_not_replayed_when_cleanup_unproven(self):
        ipc = DeadlineIpc([_playing()])
        ipc.after.append(lambda: setattr(self.clock, "t", 10.0))
        blocked = set()
        first = MODULE.run_preview_case(ipc, next(case for case in MODULE.CASES if case.name == "Sequence_Queue"), "same", blocked)
        second = MODULE.run_preview_case(ipc, self._case("Sequence"), "same", blocked)
        self.assertEqual("FAIL", first["status"])
        self.assertEqual("path-blocked", second["reason"])
        self.assertEqual(0, second["plays"])



    def test_later_queued_observation_freezes_cleanup_from_last_t_last(self):
        def at(when, payload):
            def reply(_ipc):
                self.clock.t = when
                return payload
            return reply
        ipc = DeadlineIpc([
            _queued(),
            at(0.2, _states([_row(4, 0, True, 0)])),
            at(1.2, _states([_row(4, 0, True, 0)])),
            {"status": "ok", "result": {"action": "stopped", "auditionID": 4}},
            at(2.0, _states([_row(4, 0, False, _tag(9))])),
            at(2.4, _states([])),
        ])
        pauses = {"n": 0}
        def pause(seconds):
            pauses["n"] += 1
            if pauses["n"] == 1:
                self.clock.t = 1.2
            elif pauses["n"] == 2:
                self.clock.t = 1.5
            else:
                self.clock.t += seconds
        MODULE.pause = pause
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertEqual("queued-budget", result["reason"])
        frozen = 1.2 + MODULE.FIXTURE_BUDGET_S
        stop_at = next(i for i, item in enumerate(ipc.calls) if item["cmd"] == "audition_stop")
        self.assertEqual(frozen, ipc.calls[stop_at]["deadline"])
        observes = [item for item in ipc.calls[:stop_at] if item["cmd"] == "audition_get_state"]
        self.assertEqual(2, len(observes))
        self.assertTrue(all(item["deadline"] == 1.5 for item in observes))
        later = [item for item in ipc.calls[stop_at + 1:] if item["cmd"] == "audition_get_state"]
        self.assertGreaterEqual(len(later), 1)
        self.assertTrue(all(item["deadline"] == frozen for item in later))

    def test_later_playing_observation_freezes_cleanup_from_last_t_last(self):
        def at(when, payload):
            def reply(_ipc):
                self.clock.t = when
                return payload
            return reply
        ipc = DeadlineIpc([
            _queued(),
            at(0.2, _states([_row(4, 0, True, 0)])),
            at(1.2, _states([_row(4, 1, False, _tag(9))])),
            {"status": "ok", "result": {"action": "stopped", "auditionID": 4}},
            at(2.0, _states([])),
        ])
        MODULE.pause = lambda seconds: setattr(self.clock, "t", 1.2)
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertEqual("PASS", result["status"])
        frozen = 1.2 + MODULE.FIXTURE_BUDGET_S
        stop_at = next(i for i, item in enumerate(ipc.calls) if item["cmd"] == "audition_stop")
        self.assertEqual(frozen, ipc.calls[stop_at]["deadline"])
        self.assertNotEqual(0.2 + MODULE.FIXTURE_BUDGET_S, ipc.calls[stop_at]["deadline"])
        observes = [item for item in ipc.calls[:stop_at] if item["cmd"] == "audition_get_state"]
        self.assertEqual([1.5, 1.5], [item["deadline"] for item in observes])
        later = [item for item in ipc.calls[stop_at + 1:] if item["cmd"] == "audition_get_state"]
        self.assertTrue(later)
        self.assertTrue(all(item["deadline"] == frozen for item in later))

class BoundedSocketTest(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self._monotonic = MODULE.monotonic
        MODULE.monotonic = self.clock

    def tearDown(self):
        MODULE.monotonic = self._monotonic

    def test_partial_send_and_byte_recv_honor_one_deadline(self):
        line = b"{\"status\":\"ok\",\"request_id\":\"six-pack-1\"}\n"
        sent_box = {"n": 0}

        class Sock:
            def __init__(self):
                self.sent = bytearray()
                self.closed = False
                self.timeouts = []
                self.reads = 0
            def send(self, data):
                self.sent += data[:1]
                self.clock_advance()
                return 1
            def clock_advance(self):
                sent_box["n"] += 1
                if sent_box["n"] > 3:
                    MODULE.monotonic.t = 2.0
            def recv(self, n):
                self.reads += 1
                return line[self.reads - 1:self.reads]
            def gettimeout(self):
                return None

            def settimeout(self, value):
                self.timeouts.append(value)
            def close(self):
                self.closed = True

        sock = Sock()
        ipc = MODULE.Ipc.from_connected(sock)
        sock.timeouts.clear()
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_play", {"object_path": "obj"}, 1.5)
        self.assertTrue(sock.closed)
        self.assertTrue(sock.timeouts)
        self.assertLessEqual(max(sock.timeouts), 1.5)

    def test_late_line_with_id_is_not_accepted(self):
        payload = b"{\"status\":\"ok\",\"request_id\":\"six-pack-1\",\"result\":{\"auditionID\":4}}\n"

        class Sock:
            def __init__(self):
                self.sent = bytearray()
                self.closed = False
                self.i = 0
            def send(self, data):
                self.sent += data
                return len(data)
            def recv(self, n):
                if self.i == 0:
                    MODULE.monotonic.t = 2.0
                char = payload[self.i:self.i + 1]
                self.i += 1
                return char
            def gettimeout(self):
                return None

            def settimeout(self, value):
                return None
            def close(self):
                self.closed = True

        ipc = MODULE.Ipc.from_connected(Sock())
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_play", {}, 1.5)
        self.assertTrue(ipc.unusable)
        self.assertEqual(b"", ipc.buffer)

    def test_request_mismatch_closes_without_reading_late_none(self):
        lines = [
            b"{\"status\":\"ok\",\"request_id\":\"nope\"}\n",
            b"{\"status\":\"ok\",\"request_id\":null}\n",
        ]

        class Sock:
            def __init__(self):
                self.sent = bytearray()
                self.closed = False
                self.reads = 0
                self.data = b"".join(lines)
                self.i = 0
            def send(self, data):
                self.sent += data
                return len(data)
            def recv(self, n):
                self.reads += 1
                char = self.data[self.i:self.i + 1]
                self.i += 1
                return char
            def gettimeout(self):
                return None

            def settimeout(self, value):
                return None
            def close(self):
                self.closed = True

        sock = Sock()
        ipc = MODULE.Ipc.from_connected(sock)
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_get_state", {}, 1.5)
        reads_after = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_get_state", {}, 1.5)
        self.assertEqual(reads_after, sock.reads)
        self.assertTrue(sock.closed)


    def test_tagged_handles_accept_minus_one_and_min_internal_on_play_and_poll(self):
        for good in (-1, _tag(1)):
            parsed = MODULE.parse_play(
                {"status": "ok", "result": {"state": "playing", "pending": False, "auditionID": 4, "playbackHandle": good, "object_path": "obj"}},
                "obj",
            )
            self.assertEqual("playing", parsed["kind"])
            self.assertEqual(4, parsed["owned"])
            self.assertTrue(MODULE._row_playing({"state": 1, "pending": False, "playbackHandle": good}, good))
            queued = MODULE.parse_play(
                {"status": "ok", "result": {"state": "queued", "pending": True, "auditionID": 4, "playbackHandle": 0, "object_path": "obj"}},
                "obj",
            )
            self.assertEqual("queued", queued["kind"])

    def test_tagged_handles_reject_small_positive_and_boundaries(self):
        for bad in (9, -(2**63), 2**63, True, False, 1.0, "9", None):
            parsed = MODULE.parse_play(
                {"status": "ok", "result": {"state": "playing", "pending": False, "auditionID": 4, "playbackHandle": bad, "object_path": "obj"}},
                "obj",
            )
            self.assertNotEqual("playing", parsed["kind"])
            self.assertFalse(MODULE._row_playing({"state": 1, "pending": False, "playbackHandle": bad}, None))

    def test_owned_unproven_cleanup_fails_but_overall_stays_preview(self):
        item = {"owned": 4, "cleanup": "unproven", "reason": "transport", "stops": 0}
        row = {"name": "Sequence_Queue", "status": "FAIL", "plays": 1, "iterations": [item]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([row]))
        card = MODULE.technical_card("FAIL", cleanup=MODULE.aggregate_cleanup([row]))
        self.assertEqual("FAIL", card["cleanup"])
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))

    def test_object_path_with_zero_iterations_cleanup_pass_preview_fail(self):
        row = {"name": "Sequence_Queue", "status": "FAIL", "plays": 0, "iterations": [], "reason": "object_path"}
        self.assertEqual("PASS", MODULE.aggregate_cleanup([row]))
        card = MODULE.technical_card("FAIL", cleanup="PASS")
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))

    def test_play_timeout_without_owned_cleanup_pass_preview_fail(self):
        ipc = DeadlineIpc([MODULE.LocalDeadline("late")])
        result = MODULE.run_preview_case(ipc, next(case for case in MODULE.CASES if case.name == "Sequence_Queue"), "obj", set())
        self.assertEqual("FAIL", result["status"])
        self.assertEqual("play-budget", result["reason"])
        self.assertEqual(1, result["plays"])
        self.assertEqual(1, len(result["iterations"]))
        self.assertIsNone(result["iterations"][0]["owned"])
        self.assertEqual("PASS", MODULE.aggregate_cleanup([result]))
        card = MODULE.technical_card("FAIL", cleanup="PASS")
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))

    def test_absent_before_start_zero_stops_is_cleanup_complete_until_unproven(self):
        gone = {"owned": 4, "cleanup": "unproven", "reason": "absent-before-start", "stops": 0}
        row = {"name": "Sequence_Queue", "status": "FAIL", "plays": 1, "iterations": [gone]}
        self.assertEqual("PASS", MODULE.aggregate_cleanup([row]))
        card = MODULE.technical_card("FAIL", cleanup="PASS")
        self.assertEqual(("FAIL", "PREVIEW"), MODULE.overall(card))
        bad = {"name": "Sequence", "status": "FAIL", "plays": 1, "iterations": [{"owned": 4, "cleanup": "unproven", "reason": "cleanup-unproven", "stops": 1}]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([row, bad]))

    def test_start_absent_proven_zero_stops_passes_cleanup(self):
        ipc = DeadlineIpc([_playing(), _states([])])
        result = MODULE.run_preview_case(ipc, next(case for case in MODULE.CASES if case.name == "Sequence_Queue"), "obj", set())
        self.assertEqual("PASS", result["status"])
        self.assertEqual(0, result["stops"])
        self.assertEqual("proven", result["iterations"][0]["cleanup"])
        self.assertEqual("PASS", MODULE.aggregate_cleanup([result]))

    def test_blocked_empty_case_does_not_clear_cleanup_fail(self):
        bad = {"name": "Random_Queue", "status": "FAIL", "plays": 1, "iterations": [{"owned": 4, "cleanup": "unproven", "reason": "transport", "stops": 0}]}
        empty = {"name": "Sequence", "status": "NOT_RUN", "plays": 0, "iterations": [], "blocked_by_transport": True}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([bad, empty]))
        self.assertEqual("PASS", MODULE.aggregate_cleanup([empty]))
        self.assertNotEqual("NOT_RUN", MODULE.aggregate_cleanup([bad, empty]))

    def test_pass_case_requires_full_proven_iterations(self):
        short = {"name": "Random_Draw", "status": "PASS", "plays": 1, "iterations": [{"owned": 4, "cleanup": "proven", "stops": 0}]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([short]))
        missing = {"name": "Sequence_Queue", "status": "PASS", "plays": 1, "iterations": [{"owned": None, "cleanup": "unproven", "reason": "absent-before-start", "stops": 0}]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([missing]))
        gone = {"name": "Sequence_Queue", "status": "PASS", "plays": 1, "iterations": [{"owned": 4, "cleanup": "proven", "reason": "absent-before-start", "stops": 0}]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([gone]))

    def test_fail_case_rejects_bad_plays_and_containers_without_throwing(self):
        bad_plays = {"name": "Sequence_Queue", "status": "FAIL", "plays": 2, "iterations": [{"owned": None, "cleanup": "unproven"}]}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([bad_plays]))
        played_true = {"name": "Sequence_Queue", "status": "FAIL", "plays": True, "iterations": []}
        self.assertEqual("FAIL", MODULE.aggregate_cleanup([played_true]))
        for bad in (None, ["case"], [{"status": "FAIL"}], [{"name": "Sequence", "status": "FAIL", "plays": 0, "iterations": "no"}]):
            self.assertEqual("FAIL", MODULE.aggregate_cleanup(bad))

    def test_attempted_false_omits_cleanup(self):
        card = MODULE.technical_card("NOT_RUN", attempted=False)
        self.assertNotIn("cleanup", card)
        self.assertEqual(("NOT_RUN", None), MODULE.overall(card))

    def test_hardcoded_cleanup_pass_is_rejected_for_fail_card(self):
        self.assertNotEqual("PASS", MODULE.technical_card("FAIL")["cleanup"])

    def test_seek_event_is_not_a_play_reply_and_closes(self):
        payload = b'{"type":"event","name":"audition_seek_finished","data":{"auditionID":4}}\n'
        sock = _ByteSock(payload)
        ipc = MODULE.Ipc.from_connected(sock)
        before = self.clock.t
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_play", {"object_path": "obj"}, 1.5)
        self.assertEqual(before, self.clock.t)
        self.assertTrue(ipc.unusable)
        self.assertEqual(b"", ipc.buffer)
        reads = sock.reads
        sent = bytes(sock.sent)
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_get_state", {}, 1.5)
        self.assertEqual(reads, sock.reads)
        self.assertEqual(sent, bytes(sock.sent))

    def test_wrong_role_blocks_preview_and_later_call_does_no_io(self):
        ipc = DeadlineIpc([
            {"status": "ok", "result": {}},
            {"status": "ok", "result": {"role": "observe"}},
        ])
        with self.assertRaises(MODULE.StreamUnusable):
            MODULE.connect_qualified_ipc(ipc)
        self.assertEqual(["ping", "set_channel_role"], [item["cmd"] for item in ipc.calls])
        self.assertEqual({"role": "command"}, ipc.calls[1]["args"])
        self.assertTrue(ipc.unusable)
        calls = len(ipc.calls)
        paths = {name: name for name in MODULE.CASE_NAMES}
        session = MODULE.run_preview_session(ipc, paths)
        self.assertEqual(calls, len(ipc.calls))
        self.assertEqual("FAIL", session["preview"])
        blocked = [row for row in session["cases"] if row.get("blocked_by_transport")]
        self.assertTrue(blocked)
        self.assertTrue(all(row["status"] == "NOT_RUN" for row in blocked))

    def test_role_command_is_the_call_after_ping(self):
        ipc = DeadlineIpc([
            {"status": "ok", "result": {}},
            {"status": "ok", "result": {"role": "command"}},
        ])
        MODULE.connect_qualified_ipc(ipc)
        self.assertEqual(["ping", "set_channel_role"], [item["cmd"] for item in ipc.calls])
        self.assertEqual("command", ipc.calls[1]["args"]["role"])
        self.assertLessEqual(ipc.calls[1]["deadline"] - ipc.calls[1]["at"], MODULE.FIXTURE_BUDGET_S)

    def test_overall_corrupt_cases_are_input_fail(self):
        base = {"run_attempted": False, "input_error": False, "summary_is_completion": False}
        for bad in (None, 1, True, "abc", {"name": "Sequence"}):
            raw = dict(base)
            raw["cases"] = bad
            self.assertEqual(("FAIL", "INPUT"), MODULE.overall(raw))
        missing = dict(base)
        missing["run_attempted"] = True
        self.assertEqual(("FAIL", "INPUT"), MODULE.overall(missing))
        card = MODULE.technical_card("PASS", cleanup="PASS")
        self.assertEqual(("BLOCKED", "COVERAGE"), MODULE.overall(card))


    def test_nondict_json_closes_without_reading_the_next_object(self):
        good = b'{"status":"ok","request_id":"six-pack-1","result":{"auditionID":4}}\n'
        samples = (b"null\n", b"true\n", b"false\n", b"1\n", b'"x"\n', b"[]\n")
        for first in samples:
            for deadline in (None, 1.5):
                sock = _ByteSock(first + good)
                ipc = MODULE.Ipc.from_connected(sock)
                with self.assertRaises(MODULE.StreamUnusable):
                    if deadline is None:
                        ipc.call("ping")
                    else:
                        ipc.call("ping", {}, deadline)
                self.assertTrue(sock.closed)
                self.assertTrue(ipc.unusable)
                self.assertEqual(b"", ipc.buffer)
                self.assertEqual(len(first), sock.i)
                sent = bytes(sock.sent)
                reads = sock.reads
                with self.assertRaises(MODULE.StreamUnusable):
                    ipc.call("audition_get_state", {}, 1.5)
                self.assertEqual(sent, bytes(sock.sent))
                self.assertEqual(reads, sock.reads)

    def test_dict_reply_still_returns_on_both_paths(self):
        line = b'{"status":"ok","request_id":"six-pack-1","result":{"auditionID":4}}\n'
        for deadline in (None, 1.5):
            sock = _ByteSock(line)
            ipc = MODULE.Ipc.from_connected(sock)
            if deadline is None:
                response = ipc.call("ping")
            else:
                response = ipc.call("ping", {}, deadline)
            self.assertEqual("ok", response["status"])
            self.assertEqual(4, response["result"]["auditionID"])
            self.assertFalse(ipc.unusable)
            self.assertFalse(sock.closed)


    def test_bounded_success_restores_original_timeout_for_unbounded(self):
        for initial in (10.0, None, 7.5):
            self.clock.t = 0.0
            payload = _json_line("six-pack-1", {"role": "command"}) + _json_line(
                "six-pack-2", {"state": "project_loaded"}
            )
            sock = TimeoutSock(initial, payload, jump_to=1.1, clock=self.clock)
            ipc = MODULE.Ipc.from_connected(sock, timeout=initial)
            MODULE.establish_command_role(ipc)
            self.assertEqual(initial, sock.gettimeout())
            self.assertNotIn(0.4, (sock.gettimeout(),))
            marked = len(sock.timeouts)
            ipc.call("get_app_state")
            self.assertEqual(initial, sock.gettimeout())
            self.assertEqual([], sock.timeouts[marked:])

    def test_wrong_role_restores_then_still_kills(self):
        self.clock.t = 0.0
        sock = TimeoutSock(10.0, _json_line("six-pack-1", {"role": "observe"}))
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.StreamUnusable):
            MODULE.establish_command_role(ipc)
        self.assertEqual(10.0, sock.timeouts[-1])
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_wrong_correlation_stays_unusable_without_more_io(self):
        self.clock.t = 0.0
        payload = b'{"type":"event","name":"audition_seek_finished","data":{"auditionID":4}}\n'
        sock = TimeoutSock(10.0, payload)
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("audition_play", {"object_path": "obj"}, 1.5)
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        self.assertNotEqual(10.0, sock.gettimeout())
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_recv_deadline_exhaustion_does_not_restore_usable(self):
        self.clock.t = 0.0
        sock = TimeoutSock(10.0, _json_line("six-pack-1", {"role": "command"}), jump_to=2.0, clock=self.clock)
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.StreamUnusable):
            MODULE.establish_command_role(ipc)
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_early_local_deadline_keeps_socket_healthy(self):
        self.clock.t = 0.0
        sock = TimeoutSock(10.0, b"")
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.LocalDeadline):
            ipc.call("ping", {}, 0.0)
        self.assertFalse(ipc.unusable)
        self.assertFalse(sock.closed)
        self.assertEqual(10.0, sock.gettimeout())
        self.assertEqual(0, len(sock.sent))
        self.assertEqual(0, sock.reads)

    def test_gettimeout_oserror_fail_closed(self):
        self.clock.t = 0.0
        sock = TimeoutSock(10.0, _json_line("six-pack-1", {"role": "command"}), fail_get=True)
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("set_channel_role", {"role": "command"}, 1.5)
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state", {}, 1.5)
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_restore_timeout_oserror_fail_closed(self):
        self.clock.t = 0.0
        sock = TimeoutSock(10.0, _json_line("six-pack-1", {"role": "command"}))
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        sock.fail_restore = 10.0
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("set_channel_role", {"role": "command"}, 1.5)
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_role_local_deadline_does_not_retry_readiness(self):
        constructed = {"n": 0}
        sleeps = []

        class OneIpc:
            def __init__(self, timeout=30.0):
                constructed["n"] += 1

            def call(self, command, args=None, deadline=None):
                if command == "ping":
                    return {"status": "ok"}
                raise MODULE.LocalDeadline("role late")

            def close(self):
                return None

        original_ipc = MODULE.Ipc
        original_sleep = MODULE.time.sleep
        MODULE.Ipc = OneIpc
        MODULE.time.sleep = lambda seconds: sleeps.append(seconds)
        try:
            with self.assertRaises(MODULE.CommandRoleError):
                MODULE.wait_for_ipc(timeout=90.0)
        finally:
            MODULE.Ipc = original_ipc
            MODULE.time.sleep = original_sleep
        self.assertEqual(1, constructed["n"])
        self.assertEqual([], sleeps)



    def test_restore_past_deadline_rejects_acceptance(self):
        self.clock.t = 0.0
        sock = _RestoreJumpSock(10.0, _json_line("six-pack-1", {"role": "command"}), self.clock, jump_to=2.0)
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("set_channel_role", {"role": "command"}, 1.5)
        self.assertEqual(2.0, self.clock.t)
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        self.assertEqual(b"", ipc.buffer)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_restore_past_deadline_preview_owns_nothing(self):
        self.clock.t = 0.0
        played = {
            "auditionID": 4,
            "state": "playing",
            "pending": False,
            "playbackHandle": -1,
            "object_path": "obj",
        }
        sock = _RestoreJumpSock(10.0, _json_line("six-pack-1", played), self.clock, jump_to=2.0)
        ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
        result = MODULE.run_preview_iteration(ipc, "obj")
        self.assertIsNone(result["owned"])
        self.assertEqual(0, result["stops"])
        self.assertEqual([], result["stop_ids"])
        self.assertNotIn("t_start", result)
        self.assertNotIn("t_last", result)
        self.assertTrue(result["transport"])
        self.assertEqual("transport", result["reason"])
        self.assertTrue(ipc.unusable)
        self.assertTrue(sock.closed)
        self.assertEqual(b"", ipc.buffer)
        sent = bytes(sock.sent)
        reads = sock.reads
        with self.assertRaises(MODULE.StreamUnusable):
            ipc.call("get_app_state")
        self.assertEqual(sent, bytes(sock.sent))
        self.assertEqual(reads, sock.reads)

    def test_restore_at_or_under_deadline_keeps_one_accept_sample(self):
        cases = ((1.5, 1.5), (1.0, 1.5))
        for first, deadline in cases:
            with self.subTest(first=first, deadline=deadline):
                clock = _AfterStepClock(first, 0.2)
                MODULE.monotonic = clock
                sock = _RestoreJumpSock(
                    10.0,
                    _json_line("six-pack-1", {"role": "command"}),
                    clock,
                    on_restore=clock.start,
                )
                ipc = MODULE.Ipc.from_connected(sock, timeout=10.0)
                response = ipc.call("set_channel_role", {"role": "command"}, deadline)
                self.assertEqual([first], clock.samples)
                self.assertEqual(first, response["_accepted_at"])
                self.assertLessEqual(response["_accepted_at"], deadline)
                self.assertFalse(ipc.unusable)
                self.assertFalse(sock.closed)
                self.assertEqual(10.0, sock.gettimeout())



class _ByteSock:
    def __init__(self, payload: bytes):
        self.sent = bytearray()
        self.closed = False
        self.reads = 0
        self.data = payload
        self.i = 0
        self._timeout = None

    def send(self, data):
        self.sent += data
        return len(data)

    def sendall(self, data):
        self.send(data)

    def recv(self, n):
        self.reads += 1
        char = self.data[self.i:self.i + 1]
        self.i += 1
        return char

    def gettimeout(self):
        return self._timeout

    def settimeout(self, value):
        self._timeout = value

    def close(self):
        self.closed = True



class TimeoutSock:
    def __init__(self, timeout, payload, jump_to=None, clock=None, fail_get=False):
        self._timeout = timeout
        self.timeouts = []
        self.sent = bytearray()
        self.reads = 0
        self.closed = False
        self.data = payload
        self.i = 0
        self.jump_to = jump_to
        self.clock = clock
        self.fail_get = fail_get
        self.fail_restore = None

    def gettimeout(self):
        if self.fail_get:
            raise OSError("gettimeout")
        return self._timeout

    def settimeout(self, value):
        self.timeouts.append(value)
        if self.fail_restore is not None and value == self.fail_restore:
            raise OSError("restore")
        self._timeout = value

    def send(self, data):
        self.sent += data
        return len(data)

    def sendall(self, data):
        self.send(data)

    def recv(self, n):
        self.reads += 1
        if self.jump_to is not None and self.clock is not None and self.reads == 1:
            self.clock.t = self.jump_to
        if self.i >= len(self.data):
            return b""
        char = self.data[self.i:self.i + 1]
        self.i += 1
        return char

    def close(self):
        self.closed = True



class _AfterStepClock:
    def __init__(self, first, step):
        self.first = first
        self.step = step
        self.samples = []
        self.live = False
        self.t = 0.0

    def start(self):
        self.live = True

    def __call__(self):
        if not self.live:
            return 0.0
        value = self.first + self.step * len(self.samples)
        self.samples.append(value)
        self.t = value
        return value


class _RestoreJumpSock:
    def __init__(self, timeout, payload, clock, jump_to=None, on_restore=None):
        self._timeout = timeout
        self.original = timeout
        self.data = payload
        self.i = 0
        self.clock = clock
        self.jump_to = jump_to
        self.on_restore = on_restore
        self.sent = bytearray()
        self.reads = 0
        self.closed = False
        self.timeouts = []
        self.restored = False

    def gettimeout(self):
        return self._timeout

    def settimeout(self, value):
        self.timeouts.append(value)
        restoring = (not self.restored) and self.reads > 0 and value == self.original
        self._timeout = value
        if not restoring:
            return
        self.restored = True
        if self.jump_to is not None and self.clock is not None:
            self.clock.t = self.jump_to
        if self.on_restore is not None:
            self.on_restore()

    def send(self, data):
        self.sent += data
        return len(data)

    def recv(self, n):
        self.reads += 1
        if self.i >= len(self.data):
            return b""
        char = self.data[self.i:self.i + 1]
        self.i += 1
        return char

    def close(self):
        self.closed = True



def _json_line(request_id, result):
    payload = {"status": "ok", "request_id": request_id, "result": result}
    return (MODULE.json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")



class SelectionChildContractTest(unittest.TestCase):
    def test_random_draw_has_no_fixed_tone_table(self):
        case = MODULE.CASES[0]
        self.assertEqual(case.name, "Random_Draw")
        self.assertIsNone(case.expected_tones)
        self.assertTrue(case.require_both)
        self.assertEqual(case.repeats, 8)
        shuffle = MODULE.CASES[1]
        self.assertEqual(shuffle.name, "Shuffle")
        self.assertIsNone(shuffle.expected_tones)
        self.assertTrue(shuffle.require_both)
        self.assertEqual(shuffle.repeats, 2)
        self.assertEqual(
            [item.expected_tones for item in MODULE.CASES[2:]],
            [(660, 440), (440, 660), (440, 660), (440, 660)],
        )

    def test_evaluate_case_keeps_shape_without_the_old_sequence(self):
        case = MODULE.CASES[0]
        observed = [660, 440, 440, 440, 660, 660, 440, 440]
        ok, _message = MODULE.evaluate_case(case, {"tones": observed})
        self.assertTrue(ok)
        self.assertNotEqual(tuple(observed), (440, 660, 440, 660, 660, 660, 660, 440))
        short_ok, short_message = MODULE.evaluate_case(case, {"tones": observed[:-1]})
        self.assertFalse(short_ok)
        self.assertIn("exactly 8", short_message)
        only_one_ok, only_one_message = MODULE.evaluate_case(case, {"tones": [440] * 8})
        self.assertFalse(only_one_ok)
        self.assertIn("both 440 and 660", only_one_message)

    def test_shuffle_shape_accepts_either_order_but_not_one_tone(self):
        case = MODULE.CASES[1]
        self.assertEqual(case.name, "Shuffle")
        self.assertIsNone(case.expected_tones)
        for tones in ([660, 440], [440, 660]):
            ok, message = MODULE.evaluate_case(case, {"tones": tones})
            self.assertTrue(ok, message)
        rejected, rejected_message = MODULE.evaluate_case(case, {"tones": [660, 660]})
        self.assertFalse(rejected)
        self.assertIn("both 440 and 660", rejected_message)

    def test_selection_child_lines_do_not_count_as_triggers(self):
        guid_a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"
        guid_b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"
        stdout = "\n".join(
            [
                "pack-play capture-ready armed pid=4",
                "pack-play capture-ready pid=4",
                _selection_child(1, 2, guid_a, firing=10),
                "pack-play trigger 1/2 peak=0.1000 firing=10",
                _selection_child(2, 2, guid_b, ordinal="1", firing=11),
                "pack-play trigger 2/2 peak=0.1000 firing=11",
            ]
        )
        self.assertEqual(MODULE.null_trigger_lines(stdout), [
            "pack-play trigger 1/2 peak=0.1000 firing=10",
            "pack-play trigger 2/2 peak=0.1000 firing=11",
        ])
        self.assertEqual(MODULE.wasapi_trigger_lines(stdout), MODULE.null_trigger_lines(stdout))
        ordering = MODULE.verify_ready_before_first_trigger(stdout, 4)
        self.assertLess(ordering["ready_line_index"], ordering["first_trigger_line_index"])
        self.assertTrue(ordering["ordering_verified"])

    def test_wasapi_child_contract_uses_this_authoring_and_wav_order(self):
        case = MODULE.CASES[0]
        guid_a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"
        guid_b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"
        sounds = {"A": {"guid": guid_a}, "B": {"guid": guid_b}}
        order = [guid_b, guid_a, guid_a, guid_a, guid_b, guid_b, guid_a, guid_a]
        tones = [660, 440, 440, 440, 660, 660, 440, 440]
        stdout = "\n".join(
            _trigger_and_child(index, 8, guid, firing=100 + index, ordinal="1" if guid == guid_b else "0")
            for index, guid in enumerate(order, start=1)
        )
        ok, message = MODULE.selection_child_tone_contract(case, sounds, stdout, tones)
        self.assertTrue(ok, message)
        historical = "519dd49c-982e-4986-a814-3643256e430e"
        self.assertNotIn(historical, sounds["A"]["guid"] + sounds["B"]["guid"])
        rejected, rejected_message = MODULE.selection_child_tone_contract(
            case, sounds, stdout.replace(guid_a, historical, 1), tones
        )
        self.assertFalse(rejected)
        self.assertIn("not in this authoring run", rejected_message)
        missing, missing_message = MODULE.selection_child_tone_contract(
            case, sounds, "\n".join(stdout.splitlines()[:-1]), tones
        )
        self.assertFalse(missing)
        self.assertIn("exactly 8", missing_message)
        extra, extra_message = MODULE.selection_child_tone_contract(
            case, sounds, stdout + "\n" + _selection_child(9, 8, guid_a, firing=109), tones
        )
        self.assertFalse(extra)
        self.assertIn("exactly 8", extra_message)
        flipped = [guid_a if guid == guid_b else guid_b for guid in order]
        flipped_stdout = "\n".join(
            _trigger_and_child(index, 8, guid, firing=200 + index)
            for index, guid in enumerate(flipped, start=1)
        )
        reordered, reordered_message = MODULE.selection_child_tone_contract(
            case, sounds, flipped_stdout, tones
        )
        self.assertFalse(reordered)
        self.assertIn("!=", reordered_message)

    def test_null_child_lines_are_required_without_a_wav_comparison(self):
        historical = "519dd49c-982e-4986-a814-3643256e430e"
        stdout = "\n".join(
            [
                "pack-play trigger 1/1 peak=0.2000 firing=3",
                _selection_child(1, 1, historical, firing=3),
            ]
        )
        rows = MODULE.require_selection_child_run(stdout, 1)
        self.assertEqual(rows[0]["guid"], historical)
        self.assertEqual(len(MODULE.null_trigger_lines(stdout)), 1)
        with self.assertRaises(MODULE.QualificationError):
            MODULE.require_selection_child_run("pack-play trigger 1/1 peak=0.2000 firing=3", 1)
        with self.assertRaises(MODULE.QualificationError):
            MODULE.parse_selection_child_lines(
                "pack-play selection-child trigger=1/1 ordinal=0 guid=not-a-guid firing=1"
            )

    def test_selection_child_firing_must_match_its_trigger(self):
        guid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"
        stdout = "\n".join(
            [
                "pack-play trigger 1/2 peak=0.1000 firing=10",
                _selection_child(1, 2, guid, firing=11),
                "pack-play trigger 2/2 peak=0.1000 firing=11",
                _selection_child(2, 2, guid, firing=10),
            ]
        )
        with self.assertRaises(MODULE.QualificationError) as caught:
            MODULE.require_selection_child_run(stdout, 2)
        self.assertIn("does not match trigger firing", str(caught.exception))

    def test_host_selection_copy_survives_service_retirement(self):
        exe = _selection_copy_test_exe()
        completed = subprocess.run(
            [str(exe)],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("before-retirement captured ordinal=1 guid=3dfbe430-399d-4e1c-88ff-5af4a56d6e96", completed.stdout)
        self.assertIn("after-retirement captured=0", completed.stdout)
        self.assertIn(
            "pack-play selection-child trigger=1/1 ordinal=1 guid=3dfbe430-399d-4e1c-88ff-5af4a56d6e96 firing=42",
            completed.stdout,
        )

    def test_missing_selection_copy_executable_fails(self):
        missing = Path(__file__).resolve().parent / "missing-harken_host_selection_copy_test.exe"
        with mock.patch.dict(os.environ, {"HARKEN_HOST_SELECTION_TEST": str(missing)}):
            with self.assertRaises(AssertionError) as caught:
                _selection_copy_test_exe()
        self.assertIn("HARKEN_HOST_SELECTION_TEST is not a file", str(caught.exception))

    def test_multiple_selection_copy_executables_fail_without_running_the_first(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "build" / "bin" / "Debug" / "harken_host_selection_copy_test.exe"
            second = root / "build-core" / "bin" / "Release" / "harken_host_selection_copy_test.exe"
            first.parent.mkdir(parents=True)
            second.parent.mkdir(parents=True)
            first.write_bytes(b"old")
            second.write_bytes(b"new")
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("HARKEN_HOST_SELECTION_TEST", None)
                with self.assertRaises(AssertionError) as caught:
                    _selection_copy_test_exe([first, second])
        message = str(caught.exception)
        self.assertIn(str(first), message)
        self.assertIn(str(second), message)
        self.assertIn("HARKEN_HOST_SELECTION_TEST", message)
        self.assertIn("first match was not run", message)
        self.assertNotEqual(message, str(first))




def _trigger_and_child(trigger, repeats, guid, firing, ordinal="0"):
    return "\n".join(
        [
            f"pack-play trigger {trigger}/{repeats} peak=0.1000 firing={firing}",
            _selection_child(trigger, repeats, guid, ordinal=ordinal, firing=firing),
        ]
    )


def _selection_copy_candidates() -> list[Path]:
    root = Path(__file__).resolve().parents[2]
    names = ("harken_host_selection_copy_test.exe", "harken_host_selection_copy_test")
    configs = ("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel")
    build_dirs = ("build", "build-core", "build-nc", "build-vs")
    candidates: list[Path] = []
    for build_dir in build_dirs:
        for config in configs:
            for name in names:
                parts = [root, build_dir, "bin"]
                if config:
                    parts.append(config)
                parts.append(name)
                candidates.append(Path(*parts))
    return candidates


def _selection_copy_test_exe(candidates: list[Path] | None = None) -> Path:
    override = os.environ.get("HARKEN_HOST_SELECTION_TEST")
    if override:
        path = Path(override)
        if not path.is_file():
            raise AssertionError(f"HARKEN_HOST_SELECTION_TEST is not a file: {path}")
        return path
    searched = _selection_copy_candidates() if candidates is None else list(candidates)
    found = [candidate for candidate in searched if candidate.is_file()]
    if len(found) == 1:
        return found[0]
    if not found:
        looked_at = ", ".join(str(candidate) for candidate in searched)
        raise AssertionError(
            "harken_host_selection_copy_test was not built. Set HARKEN_HOST_SELECTION_TEST "
            "or build the CTest target into <build>/bin. Looked at: " + looked_at
        )
    listed = ", ".join(str(candidate) for candidate in found)
    raise AssertionError(
        "harken_host_selection_copy_test matched more than one build, so the first "
        "match was not run. Set HARKEN_HOST_SELECTION_TEST to the executable from this "
        "build. Found: " + listed
    )


def _selection_child(trigger, repeats, guid, ordinal="0", firing=10):
    return (
        f"pack-play selection-child trigger={trigger}/{repeats} "
        f"ordinal={ordinal} guid={guid} firing={firing}"
    )




if __name__ == "__main__":
    unittest.main()
