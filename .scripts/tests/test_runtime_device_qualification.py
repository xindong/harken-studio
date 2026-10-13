from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "runtime_device_qualification.py"
SPEC = importlib.util.spec_from_file_location(
    "runtime_device_qualification", SCRIPT
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_report(scenario):
    measurement_start_ticks = 1_000_000_000
    tick_rate = 10_000_000
    measurement_end_ticks = (
        measurement_start_ticks + scenario.seconds * tick_rate
    )
    return {
        "schemaVersion": 1,
        "profile": {
            "device": "real",
            "seconds": float(scenario.seconds),
            "sampleRate": scenario.sample_rate,
            "minCallbackFrames": 64,
            "voices": scenario.render_voices,
            "virtualVoices": scenario.virtual_voices,
            "pitchCents": scenario.pitch_cents,
            "restartCount": scenario.restart_count,
            "deviceCallbackOnly": scenario.device_callback_only,
        },
        "build": {
            "gitCommit": "a" * 40,
            "buildInfoGitCommit": "a" * 40,
            "gitDirty": False,
            "buildInfoGitDirty": False,
            "trackedWorktreeClean": True,
            "configuration": "Release",
        },
        "backend": {
            "valid": 1,
            "name": "WASAPI",
            "deviceName": "Test Device",
            "deviceIdHash": 123456789,
            "clientSampleRate": scenario.sample_rate,
            "internalSampleRate": scenario.sample_rate,
        },
        "lifecycle": {
            "init": 0,
            "setup": 0,
            "update": 0,
            "snapshot": 0,
            "term": 0,
            "detach": True,
        },
        "restart": {
            "status": "passed",
            "completed": scenario.restart_count,
            "generationBefore": 1,
            "observations": [
                {
                    "generation": index + 2,
                    "callbackObserved": True,
                    "suspendBeginTicks": (
                        measurement_start_ticks + (index + 1) * tick_rate
                    ),
                    "resumeEndTicks": (
                        measurement_start_ticks + (index + 1) * tick_rate
                        + 200_000
                    ),
                    "callbackBeginTicks": (
                        measurement_start_ticks + (index + 1) * tick_rate
                        + 300_000
                    ),
                    "callbackRecoveredTicks": (
                        measurement_start_ticks + (index + 1) * tick_rate
                        + 400_000
                    ),
                    "activeVoiceCount": scenario.render_voices,
                    "parkedVoiceCount": scenario.virtual_voices,
                }
                for index in range(scenario.restart_count)
            ],
        },
        "identity": {
            "mismatchCount": 0,
            "generationOverflowCount": 0,
            "workerCoordinatorMismatchCount": 0,
            "concurrentCallbackCount": 0,
        },
        "semantic": {
            "activeVoiceCount": scenario.render_voices,
            "parkedVoiceCount": scenario.virtual_voices,
            "arenaOverflowCount": 0,
            "meterRingOverflowCount": 0,
            "noVoiceCount": 0,
            "backpressureCount": 0,
            "notificationDropCount": 0,
            "logicNotificationDropCount": 0,
            "voiceStarvedCount": 0,
            "returnRefusedCount": 0,
            "pendingStartCount": 0,
        },
        "callbackTiming": {
            "count": 100,
            "minFrames": 960,
            "invalidCount": 0,
            "deadlineMisses": 0,
            "p99Us": 1000.0,
            "maxUs": 1200.0,
        },
        "mixPublicationTiming": {
            "count": 100,
            "invalidCount": 0,
            "deadlineMisses": 0,
        },
        "storage": {
            "timelineOverflow": 0,
            "workerBarrierOverflow": 0,
        },
        "measurement": {
            "startTicks": measurement_start_ticks,
            "endTicks": measurement_end_ticks,
            "tickRate": tick_rate,
            "startAnchorTicksBefore": measurement_start_ticks - 10,
            "startAnchorTicksAfter": measurement_start_ticks,
            "startUnixNs": 1_785_456_000_000_000_000,
            "endAnchorTicksBefore": measurement_end_ticks,
            "endAnchorTicksAfter": measurement_end_ticks + 10,
            "endUnixNs": (
                1_785_456_000_000_000_000
                + scenario.seconds * 1_000_000_000
            ),
            "wallSeconds": float(scenario.seconds),
        },
        "controls": {
            "deviceCallback": True,
            "mixPublication": True,
            "workerBarrier": True,
        },
        "coverage": {
            "deviceCallback": "passed",
            "mixPublication": (
                "disabled-by-profile"
                if scenario.device_callback_only
                else "passed"
            ),
            "workerBarrier": "not-applicable-zero-workers",
        },
        "callbackContinuity": {
            "status": "passed",
            "expectedPeriodUs": 20000.0,
            "measurementWindowEdgesIncluded": True,
        },
        "captureIntegrityStatus": "passed",
        "coverageStatus": "passed",
        "contractDurationStatus": "passed",
        "qualificationInputStatus": "ready",
        "shortestObservedCallbackPeriodUs": 20000.0,
    }


class RuntimeDeviceQualificationTest(unittest.TestCase):
    def test_valid_formal_report_passes(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        result = MODULE.validate_report(
            scenario, make_report(scenario), "a" * 40, True
        )
        self.assertEqual("passed", result["status"])

    def test_occupancy_drift_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[6]
        report = make_report(scenario)
        report["semantic"]["parkedVoiceCount"] = 511
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_dirty_formal_binary_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        report = make_report(scenario)
        report["build"]["gitDirty"] = True
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_debug_formal_binary_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        report = make_report(scenario)
        report["build"]["configuration"] = "Debug"
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_short_formal_measurement_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[-1]
        report = make_report(scenario)
        report["measurement"]["wallSeconds"] = 60.0
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True, 64)

    def test_non_finite_formal_measurement_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        report = make_report(scenario)
        report["measurement"]["wallSeconds"] = float("nan")
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_divergent_clock_anchors_are_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        report = make_report(scenario)
        report["measurement"]["endUnixNs"] += (
            MODULE.CLOCK_ANCHOR_TOLERANCE_NS * 2
        )
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_trace_stats_require_loss_free_duration(self):
        stats = MODULE.parse_trace_stats(
            "Start time (UTC) : 2026/07/31:15:00:00.0000000\n"
            "Total # Lost Buffers : 0\n"
            "Total # Lost Events  : 0\n"
            "End time (UTC) : 2026/07/31:15:18:00.5000000  "
            "(+ 0:00:18:00.5000000)\n",
            "fixture",
        )
        self.assertEqual(0, stats["lostEvents"])
        self.assertAlmostEqual(1080.5, stats["durationSeconds"])

    def test_audio_glitches_are_classified_by_exact_restart_window(self):
        trace_start = "2026-07-31T15:00:00+00:00"
        lines = [
            "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION,  1000000, data",
            "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION,  3000000, data",
        ]
        classified = MODULE.classify_audio_glitch_lines(
            lines,
            trace_start,
            [
                {
                    "scenario": "restart-10-r64",
                    "startUtc": "2026-07-31T15:00:00.900000+00:00",
                    "endUtc": "2026-07-31T15:00:01.100000+00:00",
                    "boundaryGuardNs": 10_000_000,
                }
            ],
        )
        self.assertEqual(
            ["restart-window-coincident", "unexpected"],
            [item["classification"] for item in classified],
        )

    def test_wrong_or_duplicate_restart_glitches_stay_unexpected(self):
        window = {
            "scenario": "restart-10-r64",
            "startUtc": "2026-07-31T15:00:00.900000+00:00",
            "endUtc": "2026-07-31T15:00:01.100000+00:00",
            "boundaryGuardNs": 10_000_000,
        }
        classified = MODULE.classify_audio_glitch_lines(
            [
                "Provider/op_EVT_GLITCH_ARBITRARY_FATAL, 1000000, data",
                "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION, 1000000, data",
                "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION, 1001000, data",
            ],
            "2026-07-31T15:00:00+00:00",
            [window],
        )
        self.assertEqual(
            ["unexpected", "restart-window-coincident", "unexpected"],
            [item["classification"] for item in classified],
        )

    def test_measurement_utc_must_fit_process_lifetime(self):
        scenario = MODULE.FORMAL_CATALOG[0]
        result = MODULE.validate_report(
            scenario, make_report(scenario), "a" * 40, True
        )
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_process_time_binding(
                result,
                "2026-07-31T15:00:00+00:00",
                "2026-07-31T15:01:00+00:00",
            )

    def test_restart_window_outside_measurement_is_rejected(self):
        scenario = MODULE.FORMAL_CATALOG[7]
        report = make_report(scenario)
        report["restart"]["observations"][0]["suspendBeginTicks"] = (
            report["measurement"]["startTicks"] - 1
        )
        with self.assertRaises(MODULE.QualificationError):
            MODULE.validate_report(scenario, report, "a" * 40, True)

    def test_restart_window_has_bounded_etw_correlation_tail(self):
        scenario = MODULE.FORMAL_CATALOG[7]
        result = MODULE.validate_report(
            scenario, make_report(scenario), "a" * 40, True
        )
        window = result["plannedRestartWindowsUtc"][0]
        recovered = MODULE.datetime.fromisoformat(window["callbackRecoveredUtc"])
        end = MODULE.datetime.fromisoformat(window["endUtc"])
        self.assertEqual(
            MODULE.RESTART_EVENT_CORRELATION_TAIL_NS // 1000,
            round((end - recovered).total_seconds() * 1_000_000),
        )

    def test_etw_tail_inside_is_coincident_and_outer_edge_is_unexpected(self):
        window = {
            "scenario": "restart-10-r64",
            "startUtc": "2026-07-31T15:00:00.900000+00:00",
            "endUtc": "2026-07-31T15:00:01.101000+00:00",
            "boundaryGuardNs": 10_000,
        }
        inside = MODULE.classify_audio_glitch_lines(
            [
                "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION, "
                "1000168, data"
            ],
            "2026-07-31T15:00:00.100000+00:00",
            [window],
        )
        edge = MODULE.classify_audio_glitch_lines(
            [
                "Provider/op_EVT_GLITCH_CP_SERVER_INPUT_STARVATION, "
                "1001000, data"
            ],
            "2026-07-31T15:00:00.100000+00:00",
            [window],
        )
        self.assertEqual("restart-window-coincident", inside[0]["classification"])
        self.assertEqual("unexpected", edge[0]["classification"])

    def test_raw_artifacts_are_counted_and_hashed(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            timeline = output / "scenario.json.timeline.jsonl"
            workers = output / "scenario.json.workers.jsonl"
            timeline.write_text('{"a":1}\n{"a":2}\n', encoding="utf-8")
            workers.write_bytes(b"")
            artifacts = MODULE.validate_raw_artifacts(
                {
                    "rawRecords": {
                        "timelinePath": str(timeline),
                        "timelineCount": 2,
                        "workerPath": str(workers),
                        "workerCount": 0,
                    }
                },
                output,
                MODULE.time.monotonic() + 10.0,
            )
            self.assertEqual([2, 0], [item["lineCount"] for item in artifacts])
            self.assertTrue(all(len(item["sha256"]) == 64 for item in artifacts))

    def test_observed_facet_failure_fails_aggregate(self):
        self.assertEqual(
            "failed",
            MODULE.aggregate_facet_status(
                {
                    "runtimeRealDevice": "passed",
                    "systemAudioAndDpcActiveService": "failed",
                    "audibleArtifacts": "not_observed",
                }
            ),
        )

    def test_partial_kernel_trace_start_is_cancelled_as_owned(self):
        with tempfile.TemporaryDirectory() as directory:
            kernel_path = Path(directory) / "windows-kernel.etl"
            with (
                mock.patch.object(MODULE, "xperf_path", return_value="xperf"),
                mock.patch.object(
                    MODULE,
                    "active_xperf_loggers",
                    side_effect=[
                        "",
                        "Logger Name : NT Kernel Logger\n"
                        f"Log Filename : {kernel_path}",
                    ],
                ),
                mock.patch.object(
                    MODULE,
                    "run_command",
                    side_effect=MODULE.QualificationError("partial start"),
                ),
                mock.patch.object(MODULE, "cancel_windows_audio_trace") as cancel,
            ):
                with self.assertRaises(MODULE.QualificationError):
                    MODULE.start_windows_audio_trace(
                        Path(directory),
                        MODULE.time.monotonic() + 10.0,
                        MODULE.time.monotonic() + 20.0,
                    )
            sessions = cancel.call_args.args[0]
            self.assertTrue(sessions.kernel_owned)
            self.assertFalse(sessions.audio_owned)

    def test_racing_foreign_kernel_trace_is_not_cancelled(self):
        with tempfile.TemporaryDirectory() as directory:
            expected_path = Path(directory) / "windows-kernel.etl"
            with (
                mock.patch.object(MODULE, "xperf_path", return_value="xperf"),
                mock.patch.object(
                    MODULE,
                    "active_xperf_loggers",
                    side_effect=[
                        "",
                        "Logger Name : NT Kernel Logger\n"
                        f"Log Filename : {expected_path}.foreign",
                    ],
                ),
                mock.patch.object(
                    MODULE,
                    "run_command",
                    side_effect=MODULE.QualificationError("start collision"),
                ),
                mock.patch.object(MODULE, "cancel_windows_audio_trace") as cancel,
            ):
                with self.assertRaises(MODULE.QualificationError):
                    MODULE.start_windows_audio_trace(
                        Path(directory),
                        MODULE.time.monotonic() + 10.0,
                        MODULE.time.monotonic() + 20.0,
                    )
            sessions = cancel.call_args.args[0]
            self.assertFalse(sessions.kernel_owned)
            self.assertFalse(sessions.audio_owned)

    def test_existing_kernel_trace_is_not_cancelled(self):
        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(MODULE, "xperf_path", return_value="xperf"),
                mock.patch.object(
                    MODULE,
                    "active_xperf_loggers",
                    return_value="Logger Name : NT Kernel Logger",
                ),
                mock.patch.object(MODULE, "cancel_windows_audio_trace") as cancel,
            ):
                with self.assertRaises(MODULE.QualificationError):
                    MODULE.start_windows_audio_trace(
                        Path(directory),
                        MODULE.time.monotonic() + 10.0,
                        MODULE.time.monotonic() + 20.0,
                    )
            cancel.assert_not_called()

    def test_missing_external_evidence_stays_unobserved(self):
        status, evidence = MODULE.external_status(
            None, "test-evidence", {"status": "verified"}, {}
        )
        self.assertEqual("not_observed", status)
        self.assertIsNone(evidence)

    def test_invalid_external_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "external.json"
            path.write_text(json.dumps({"status": "unverified"}), encoding="utf-8")
            with self.assertRaises(MODULE.QualificationError):
                MODULE.external_status(
                    path, "test-evidence", {"status": "verified"}, {}
                )

    def test_formal_mode_requires_exact_commit(self):
        args = MODULE.parse_args(
            [
                "--binary", sys.executable,
                "--output-dir", "out",
                "--mode", "formal",
            ]
        )
        with self.assertRaises(MODULE.QualificationError):
            MODULE.run_catalog(args)

    def test_formal_mode_requires_live_system_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            args = MODULE.parse_args(
                [
                    "--binary", sys.executable,
                    "--output-dir", directory,
                    "--mode", "formal",
                    "--expected-commit", "a" * 40,
                ]
            )
            with self.assertRaises(MODULE.QualificationError):
                MODULE.run_catalog(args)

    def test_lock_is_a_noop_off_windows(self):
        with mock.patch.object(MODULE.sys, "platform", "linux"):
            MODULE.lock_qualification_file(0, True)

    def test_failed_run_replaces_stale_manifest_with_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            manifest = output / "m12-manifest.json"
            manifest.write_text('{"status":"passed"}\n', encoding="utf-8")
            exit_code = MODULE.main(
                [
                    "--binary", str(output / "missing.exe"),
                    "--output-dir", str(output),
                ]
            )
            self.assertEqual(2, exit_code)
            self.assertFalse(manifest.exists())
            failure = json.loads(
                (output / "m12-failure.json").read_text(encoding="utf-8")
            )
            self.assertEqual("failed", failure["status"])


if __name__ == "__main__":
    unittest.main()
