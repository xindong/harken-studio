from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "runtime_rt_evidence.py"
SPEC = importlib.util.spec_from_file_location("runtime_rt_evidence", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

TEST_COMMIT = "a" * 40


def report(
    cell: str,
    device: str,
    workers: int,
) -> dict:
    controls = MODULE.CONTROL_MATRIX[cell]
    callback = device == "real" and cell in ("device", "all")
    publication = cell in ("publication", "all")
    worker = device == "null" and workers > 0 and cell in ("worker", "all")
    return {
        "controls": controls,
        "contractDurationStatus": "passed",
        "captureIntegrityStatus": "passed",
        "coverageStatus": "passed",
        "qualificationInputStatus": "ready",
        "build": {
            "gitCommit": TEST_COMMIT,
            "gitDirty": False,
            "buildInfoGitCommit": TEST_COMMIT,
            "buildInfoGitDirty": False,
            "trackedWorktreeClean": True,
            "untrackedEntriesPresent": False,
            "configuration": "Release",
            "generator": "Visual Studio 17 2022",
            "compilerId": "MSVC",
            "compilerVersion": "19.42",
            "compilerPath": "cl.exe",
            "generatorToolset": "v143,version=14.42",
            "vsToolsetVersion": "14.42",
            "windowsSdk": "10.0.26100.0",
            "msvcFullVersion": 194234444,
        },
        "profile": {
            "device": device,
            "seconds": 60.0,
            "sampleRate": 48000,
            "blockFrames": 256,
            "minCallbackFrames": 64,
            "channels": 2,
            "voices": 8,
            "workers": workers,
            "meterVariables": 0,
            "audibilityParking": False,
        },
        "backend": {
            "valid": 1 if device == "real" and callback else 0,
            "code": 1 if device == "real" and callback else 0,
            "name": "WASAPI" if device == "real" and callback else "",
            "deviceName": "Test Device" if device == "real" and callback else "",
            "clientSampleRate": 48000 if device == "real" and callback else 0,
            "clientChannels": 2 if device == "real" and callback else 0,
            "clientFormat": 5 if device == "real" and callback else 0,
            "requestedPeriodFrames": 0,
            "requestedPeriods": 0,
            "internalSampleRate": 48000 if device == "real" and callback else 0,
            "internalChannels": 2 if device == "real" and callback else 0,
            "internalFormat": 5 if device == "real" and callback else 0,
            "internalPeriodFrames": 960 if device == "real" and callback else 0,
            "internalPeriods": 2 if device == "real" and callback else 0,
        },
        "lifecycle": {
            "init": 0,
            "setup": 0,
            "update": 0,
            "snapshot": 0,
            "term": 0,
            "detach": True,
        },
        "storage": {
            "timelineOverflow": 0,
            "workerBarrierOverflow": 0,
            "timelineCount": 100 if callback or publication else 0,
            "workerBarrierCount": 100 if worker else 0,
        },
        "rawRecords": {
            "timelineCount": 100 if callback or publication else 0,
            "workerCount": 100 if worker else 0,
        },
        "identity": {
            "mismatchCount": 0,
            "generationOverflowCount": 0,
            "workerCoordinatorMismatchCount": 0,
            "concurrentCallbackCount": 0,
        },
        "timingBoundary": MODULE.TIMING_BOUNDARY,
        "callbackTiming": {
            "count": 100 if callback else 0,
            "invalidCount": 0,
            "deadlineMisses": 0,
            "p50Us": 101.0 if cell == "all" else 100.0,
            "p99Us": 205.0 if cell == "all" else 200.0,
        },
        "mixPublicationTiming": {
            "count": 100 if publication else 0,
            "invalidCount": 0,
            "deadlineMisses": 0,
        },
        "workerBarrier": {
            "count": 100 if worker else 0,
            "invalidCount": 0,
            "faultCount": 0,
            "serialFallbackCount": 0,
            "incompleteScratchCount": 0,
            "identityMismatchMask": 0,
        },
        "shortestObservedCallbackPeriodUs": (
            20000.0 if callback else 0.0
        ),
        "callbackShape": {
            "frameCounts": {"960": 100} if callback else {},
            "mixBlockCounts": {"3": 75, "4": 25} if callback else {},
            "stagingReadBefore": {"0": 100} if callback else {},
            "stagingAvailBefore": {"0": 100} if callback else {},
            "stagingReadAfter": {"192": 100} if callback else {},
            "stagingAvailAfter": {"256": 100} if callback else {},
        },
        "publicationShape": {
            "count": 100 if publication else 0,
            "meterArmedCount": 0,
            "sourceCursorPublishedCount": 0,
        },
        "semantic": {
            "outputHashValid": device == "null",
            "outputHash": 123 if device == "null" else None,
            "outputSignalMetricsValid": device == "null",
            "outputPeakAbs": 0.01 if device == "null" else None,
            "outputRms": 0.001 if device == "null" else None,
            "outputNonzeroSampleCount": (
                1000 if device == "null" else 0
            ),
            "outputSampleCount": 2000 if device == "null" else 0,
            "outputNonFiniteSampleCount": 0,
            "pumpedBlocks": 11250,
            "snapshotBlockIndex": 11249,
            "activeVoiceCount": 8,
            "voiceHighWater": 8,
            "arenaOverflowCount": 0,
            "renderWorkerCount": workers,
            "meterRingOverflowCount": 0,
        },
    }


class RuntimeRtEvidenceTest(unittest.TestCase):
    def make_inputs(self):
        null_reports = {
            cell: report(cell, "null", 2)
            for cell in MODULE.CONTROL_MATRIX
        }
        real_reports = {
            cell: report(cell, "real", 0)
            for cell in MODULE.CONTROL_MATRIX
        }
        observer = {
            "scope": "qualification-process-thread-observer-primitive",
            "claimBoundary": (
                "QPC-plus-callback-ownership-and-fixed-slot-cost-"
                "not-total-shipping-delta"
            ),
            "build": {
                **real_reports["device"]["build"],
                "gitDirty": False,
            },
            "iterations": 200000,
            "warmupRecords": 1000,
            "measuredRecords": 199000,
            "timelineCount": 200000,
            "storageBytes": 15000000,
            "storageLimitBytes": 16 * 1024 * 1024,
            "timelineOverflow": 0,
            "identityMismatchCount": 0,
            "concurrentCallbackCount": 0,
            "detached": True,
            "p50Us": 0.1,
            "p99Us": 0.2,
            "p99_9Us": 0.3,
            "maxUs": 1.0,
        }
        compileout = {
            "status": "passed",
            "git": {
                "commit": TEST_COMMIT,
                "trackedWorktreeClean": True,
            },
        }
        overhead_trials = {
            "baselineBefore": report("device", "real", 0),
            "frontFirst": report("all", "real", 0),
            "frontSecond": report("all", "real", 0),
            "baselineAfter": report("device", "real", 0),
        }
        worker_baseline_before = report("device", "real", 2)
        worker_baseline_after = report("device", "real", 2)
        worker_front_first = report("device", "real", 2)
        worker_front_second = report("device", "real", 2)
        for front in (worker_front_first, worker_front_second):
            front["controls"] = {
                "deviceCallback": True,
                "mixPublication": False,
                "workerBarrier": True,
            }
            front["workerBarrier"]["count"] = 100
            front["storage"]["workerBarrierCount"] = 100
            front["rawRecords"]["workerCount"] = 100
        worker_overhead_trials = {
            "baselineBefore": worker_baseline_before,
            "frontFirst": worker_front_first,
            "frontSecond": worker_front_second,
            "baselineAfter": worker_baseline_after,
        }
        capacity_profiles = {}
        for name, voices in (
            ("light", 8),
            ("medium", 32),
            ("heavy", 64),
        ):
            capacity = report("device", "real", 0)
            capacity["profile"]["voices"] = voices
            capacity["semantic"]["activeVoiceCount"] = voices
            capacity["semantic"]["voiceHighWater"] = voices
            capacity_profiles[name] = capacity
        parking_diagnostic = report("all", "real", 0)
        parking_diagnostic["profile"]["audibilityParking"] = True
        parking_diagnostic["publicationShape"][
            "sourceCursorPublishedCount"
        ] = parking_diagnostic["mixPublicationTiming"]["count"]
        callback_closure = {
            "kind": "harken-runtime-source-callback-closure",
            "verdict": "SOURCE_CLOSURE_CONSTRUCTED_WITH_OPEN_BLOCKERS",
            "analysisLevel": "source-ast",
            "safetyVerdict": "UNRESOLVED",
            "git": {
                "commit": TEST_COMMIT,
                "trackedWorktreeClean": True,
            },
            "dynamicEdges": {
                "status": "PASS",
                "unknownCalls": [],
            },
            "closure": {
                "digest": "closure-digest",
                "nodeCount": 10,
                "edgeCount": 20,
            },
            "knownBlockers": [
                *[
                    {
                        "id": blocker,
                        "name": "test blocker",
                        "status": "OPEN",
                    }
                    for blocker in (
                        "RT-B001",
                        "RT-B002",
                        "RT-B003",
                        "RT-B004",
                        "RT-B006",
                        "RT-B008",
                    )
                ],
                {
                    "id": "RT-B007",
                    "name": "test blocker",
                    "status": "NOT_DETECTED",
                },
            ],
        }
        return (
            null_reports,
            real_reports,
            observer,
            compileout,
            overhead_trials,
            worker_overhead_trials,
            capacity_profiles,
            parking_diagnostic,
            callback_closure,
        )

    def test_valid_matrix_passes_infrastructure_not_production(self):
        inputs = self.make_inputs()
        manifest = MODULE.build_manifest(*inputs, etw=None)
        self.assertEqual("passed", manifest["m2ProbeMechanicsStatus"])
        self.assertEqual("blocked", manifest["m2InfrastructureStatus"])
        self.assertEqual(
            "unverified",
            manifest["publicationIncrementalCallback"][
                "attributableOverheadStatus"
            ],
        )
        self.assertEqual(
            "blocked", manifest["productionQualificationStatus"]
        )
        self.assertEqual(
            "unverified",
            manifest["claimBoundary"]["totalProbeVsShippingOffDelta"],
        )

    def test_etw_closes_only_authoritative_external_facets(self):
        inputs = self.make_inputs()
        manifest = MODULE.build_manifest(
            *inputs,
            etw={
                "xrunStatus": "verified",
                "audioGlitchCount": 0,
                "environmentIdentityStatus": "verified",
            },
        )
        self.assertEqual(
            "blocked", manifest["productionQualificationStatus"]
        )
        self.assertEqual(
            "passed", manifest["externalRealtimeFacetsStatus"]
        )
        self.assertEqual("passed", manifest["m2InfrastructureStatus"])

    def test_all_off_callback_samples_are_rejected(self):
        inputs = self.make_inputs()
        inputs[1]["off"]["callbackTiming"]["count"] = 1
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_null_semantic_drift_is_rejected(self):
        inputs = self.make_inputs()
        inputs[0]["all"]["semantic"]["outputHash"] = 999
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_silent_null_output_is_rejected(self):
        inputs = self.make_inputs()
        inputs[0]["all"]["semantic"]["outputNonzeroSampleCount"] = 0
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_real_voice_loss_is_rejected(self):
        inputs = self.make_inputs()
        inputs[1]["all"]["semantic"]["activeVoiceCount"] = 7
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_missing_source_identity_boolean_is_rejected(self):
        inputs = self.make_inputs()
        del inputs[0]["off"]["build"]["gitDirty"]
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_wrong_source_identity_type_is_rejected(self):
        inputs = self.make_inputs()
        inputs[1]["device"]["build"]["untrackedEntriesPresent"] = "false"
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_stale_callback_closure_contract_is_rejected(self):
        inputs = self.make_inputs()
        inputs[8]["kind"] = "harken-runtime-callback-closure"
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_deadline_miss_is_rejected(self):
        inputs = self.make_inputs()
        inputs[1]["all"]["callbackTiming"]["deadlineMisses"] = 1
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_bffb_tail_noise_blocks_infrastructure(self):
        inputs = self.make_inputs()
        inputs[4]["frontSecond"]["callbackTiming"]["p99Us"] = 230.0
        manifest = MODULE.build_manifest(*inputs, etw=None)
        self.assertEqual(
            "failed",
            manifest["publicationIncrementalCallback"][
                "numericThresholdStatus"
            ],
        )
        self.assertEqual(
            "unverified",
            manifest["publicationIncrementalCallback"][
                "attributableOverheadStatus"
            ],
        )
        self.assertEqual("blocked", manifest["m2InfrastructureStatus"])

    def test_worker_bffb_tail_noise_blocks_infrastructure(self):
        inputs = self.make_inputs()
        inputs[5]["frontSecond"]["callbackTiming"]["p99Us"] = 230.0
        manifest = MODULE.build_manifest(*inputs, etw=None)
        self.assertEqual(
            "failed",
            manifest["workerIncrementalCallback"][
                "numericThresholdStatus"
            ],
        )
        self.assertEqual(
            "unverified",
            manifest["workerIncrementalCallback"][
                "attributableOverheadStatus"
            ],
        )
        self.assertEqual("blocked", manifest["m2InfrastructureStatus"])

    def test_verified_environment_preserves_numeric_failure(self):
        inputs = self.make_inputs()
        inputs[4]["frontSecond"]["callbackTiming"]["p99Us"] = 230.0
        manifest = MODULE.build_manifest(
            *inputs,
            etw={
                "xrunStatus": "verified",
                "audioGlitchCount": 0,
                "environmentIdentityStatus": "verified",
            },
        )
        self.assertEqual(
            "failed",
            manifest["publicationIncrementalCallback"][
                "attributableOverheadStatus"
            ],
        )
        self.assertEqual("blocked", manifest["m2InfrastructureStatus"])

    def test_drifted_environment_blocks_numeric_pass(self):
        inputs = self.make_inputs()
        manifest = MODULE.build_manifest(
            *inputs,
            etw={
                "xrunStatus": "verified",
                "audioGlitchCount": 0,
                "environmentIdentityStatus": "drifted",
            },
        )
        self.assertEqual(
            "passed",
            manifest["publicationIncrementalCallback"][
                "numericThresholdStatus"
            ],
        )
        self.assertEqual(
            "failed",
            manifest["publicationIncrementalCallback"][
                "attributableOverheadStatus"
            ],
        )
        self.assertEqual("blocked", manifest["m2InfrastructureStatus"])

    def test_capacity_profile_drift_is_rejected(self):
        inputs = self.make_inputs()
        inputs[6]["heavy"]["semantic"]["activeVoiceCount"] = 63
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_parking_diagnostic_requires_publication_records(self):
        inputs = self.make_inputs()
        inputs[7]["mixPublicationTiming"]["count"] = 0
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_parking_diagnostic_requires_source_cursor_flag(self):
        inputs = self.make_inputs()
        inputs[7]["publicationShape"][
            "sourceCursorPublishedCount"
        ] = 0
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_bffb_workload_distribution_drift_is_rejected(self):
        inputs = self.make_inputs()
        inputs[4]["frontSecond"]["callbackShape"]["mixBlockCounts"] = {
            "3": 70,
            "4": 30,
        }
        with self.assertRaises(MODULE.EvidenceError):
            MODULE.build_manifest(*inputs, etw=None)

    def test_raw_artifact_rejects_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "records.jsonl"
            path.write_text(
                json.dumps({"sequence": 0}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.EvidenceError):
                MODULE.raw_artifact_record(path, "test.raw", 2)

    def test_raw_artifact_accepts_contiguous_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "records.jsonl"
            path.write_text(
                "\n".join(
                    json.dumps({"sequence": index})
                    for index in range(2)
                )
                + "\n",
                encoding="utf-8",
            )
            artifact = MODULE.raw_artifact_record(
                path, "test.raw", 2
            )
            self.assertEqual("test.raw", artifact["role"])


if __name__ == "__main__":
    unittest.main()
