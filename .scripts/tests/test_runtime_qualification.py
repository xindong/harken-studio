from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "runtime_qualification.py"
)
SPEC = importlib.util.spec_from_file_location(
    "runtime_qualification", SCRIPT_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
MEMORY_SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "runtime_memory_observer.py"
)
MEMORY_SPEC = importlib.util.spec_from_file_location(
    "runtime_memory_observer",
    MEMORY_SCRIPT_PATH,
)
assert MEMORY_SPEC is not None and MEMORY_SPEC.loader is not None
MEMORY_MODULE = importlib.util.module_from_spec(MEMORY_SPEC)
MEMORY_SPEC.loader.exec_module(MEMORY_MODULE)
COMPRESSOR_SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "runtime_compressor_characterization.py"
)
COMPRESSOR_SPEC = importlib.util.spec_from_file_location(
    "runtime_compressor_characterization",
    COMPRESSOR_SCRIPT_PATH,
)
assert COMPRESSOR_SPEC is not None and COMPRESSOR_SPEC.loader is not None
COMPRESSOR_MODULE = importlib.util.module_from_spec(COMPRESSOR_SPEC)
COMPRESSOR_SPEC.loader.exec_module(COMPRESSOR_MODULE)


class RuntimeQualificationTest(unittest.TestCase):
    def test_memory_observer_parses_runtime_rows(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "benchmark.log"
            output.write_text(
                "\n".join(
                    [
                        "diagnostic",
                        json.dumps(
                            {
                                "benchmark": "phase1",
                                "status": "matrix",
                                "voices": [16, 64],
                                "layouts": ["stereo", "7.1"],
                            }
                        ),
                        json.dumps(
                            {
                                "benchmark": "phase1",
                                "status": "measured",
                                "sample_rate": 48000,
                                "block_frames": 256,
                                "voices": 16,
                                "layout": "stereo",
                                "arena_high_water_bytes": 4096,
                                "arena_overflow_count": 0,
                            }
                        ),
                        json.dumps(
                            {
                                "benchmark": "phase1",
                                "status": "measured",
                                "layout": "7.1",
                                "voices": 64,
                                "arena_high_water_bytes": 8192,
                                "arena_overflow_count": 0,
                            }
                        ),
                        json.dumps(
                            {
                                "benchmark": "phase1",
                                "status": "complete",
                                "measured_rows": 2,
                            }
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            arena, capacities = (
                MEMORY_MODULE.parse_runtime_output(output)
            )
            self.assertEqual(8192, arena["high_water_bytes"])
            self.assertEqual(0, arena["overflow_count"])
            self.assertEqual(64, capacities["voices"])
            self.assertEqual(2, capacities["measured_rows"])
            self.assertEqual(
                ["7.1", "stereo"],
                capacities["layouts"],
            )
            inventory = MEMORY_MODULE.static_allocation_inventory(
                capacities
            )
            self.assertTrue(
                MODULE.REQUIRED_STATIC_ALLOCATION_CATEGORIES.issubset(
                    {item["name"] for item in inventory}
                )
            )

    def test_compressor_characterization_is_strict_but_diagnostic(
        self,
    ) -> None:
        contract = {
            "performance_contract_vintage": 2,
            "performance_gate_scope": "finite-cost-regression-only",
            "scoped_cost_gate_applicable": False,
            "scoped_cost_gate_passed": False,
            "deadline_misses_diagnostic_only": True,
            "deadline_miss_hard_fail_applicable": False,
            "deadline_reliability_claimed": False,
            "deadline_observation_status": "clear",
            "scoped_gate_meaning": (
                "finite-cost-regression-only-not-hard-realtime-"
                "or-glitch-free"
            ),
        }
        matrix = {
            **contract,
            "benchmark": "compressor-matrix",
            "status": "matrix",
            "sample_rate": 48000,
            "block_frames": 256,
            "channels": 8,
            "warmup_blocks": 2000,
            "measured_blocks": 10000,
            "runs": 5,
            "workers": [0, 1, 2, 4],
            "canonical_counts": False,
            "process_pinned": True,
            "priority_set": True,
            "expected_scoped_scenario_worker_mask": "00001f11",
            "expected_scoped_rows": 7,
        }
        measured = []
        for scenario, workers in (
            COMPRESSOR_MODULE.EXPECTED_SCENARIO_WORKERS.items()
        ):
            for worker in workers:
                for run in range(1, 6):
                    measured.append(
                        {
                            **contract,
                            "benchmark": "compressor-matrix",
                            "status": "measured",
                            "scenario": scenario,
                            "workers": worker,
                            "run": run,
                            "build": "release",
                            "sample_rate": 48000,
                            "block_frames": 256,
                            "channels": 8,
                            "warmup_blocks": 2000,
                            "measured_blocks": 10000,
                            "process_pinned": True,
                            "priority_set": True,
                            "setup_valid": True,
                            "setup_error": "none",
                            "output_finite": True,
                            "nonfinite_output_sample_count": 0,
                            "arena_overflow_count": 0,
                            "dynamic_route_refusal_count": 0,
                            "dynamic_plan_retire_overflow_count": 0,
                            "worker_identity": (
                                "serial-base"
                                if worker == 0
                                else "identical"
                            ),
                            "deadline_misses": 0,
                            "deadline_us": 5333.333,
                            "max_us": 3000.0,
                            "median_us": 1000.0,
                            "p99_us": 1200.0,
                            "p99_budget_us": 1066.666,
                            (
                                "absolute_callback_budget_"
                                "diagnostic_applicable"
                            ): scenario != "inactive-route-platform",
                            (
                                "absolute_callback_budget_"
                                "diagnostic_passed"
                            ): False,
                        }
                    )
        complete = {
            **contract,
            "benchmark": "compressor-matrix",
            "status": "complete",
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
            "missing_runtime_evidence": (
                COMPRESSOR_MODULE.EXPECTED_MISSING_EVIDENCE
            ),
        }
        valid_rows = [matrix, *measured, complete]
        issues, summary = COMPRESSOR_MODULE.validate_rows(valid_rows)
        self.assertEqual([], issues)
        self.assertEqual(35, summary["measured_rows"])
        self.assertGreater(
            len(summary["absolute_budget_failures"]),
            0,
        )
        self.assertEqual(
            [],
            COMPRESSOR_MODULE.validate_process_contract(1, 0),
        )
        for exit_code in (0, 2, 3, -1):
            with self.subTest(exit_code=exit_code):
                self.assertTrue(
                    COMPRESSOR_MODULE.validate_process_contract(
                        exit_code,
                        0,
                    )
                )
        self.assertTrue(
            COMPRESSOR_MODULE.validate_process_contract(1, 1)
        )

        invalid_cases = {
            "duplicate complete": [*valid_rows, complete],
            "missing measured": [matrix, *measured[:-1], complete],
            "setup failure": [
                matrix,
                {**measured[0], "setup_valid": False},
                *measured[1:],
                complete,
            ],
            "non-finite": [
                matrix,
                {**measured[0], "p99_us": float("nan")},
                *measured[1:],
                complete,
            ],
            "identity drift": [
                matrix,
                {**measured[0], "worker_identity": "drift"},
                *measured[1:],
                complete,
            ],
        }
        for name, rows in invalid_cases.items():
            with self.subTest(name=name):
                case_issues, _ = (
                    COMPRESSOR_MODULE.validate_rows(rows)
                )
                self.assertTrue(case_issues)

    def test_compressor_paired_evidence_requires_both_leaf_gates(
        self,
    ) -> None:
        def performance_contract(
            applicable: bool,
            passed: bool,
        ) -> dict[str, object]:
            return {
                "performance_contract_vintage": 2,
                "performance_gate_scope": "finite-cost-regression-only",
                "scoped_cost_gate_applicable": applicable,
                "scoped_cost_gate_passed": passed,
                "deadline_misses_diagnostic_only": True,
                "deadline_miss_hard_fail_applicable": False,
                "deadline_reliability_claimed": False,
                "deadline_observation_status": "clear",
                "scoped_gate_meaning": (
                    "finite-cost-regression-only-not-hard-realtime-"
                    "or-glitch-free"
                ),
            }

        def paired_rows(
            scenario: str,
            mask: str,
        ) -> list[dict[str, object]]:
            rows: list[dict[str, object]] = [
                {
                    **performance_contract(False, False),
                    "benchmark": "compressor-matrix-paired",
                    "status": "matrix",
                    "order": "B-F-F-B",
                    "pairs": 5,
                    "warmup_blocks_per_session": 2000,
                    "measured_blocks_per_session": 10000,
                    "process_pinned": True,
                    "priority_set": True,
                    "canonical_counts": True,
                    "expected_scoped_scenario_worker_mask": mask,
                    "expected_scoped_rows": 1,
                }
            ]
            for pair in range(1, 6):
                for role in ("B1", "F1", "F2", "B2"):
                    rows.append(
                        {
                            **performance_contract(False, False),
                            "benchmark": "compressor-matrix-paired",
                            "status": "session",
                            "scenario": scenario,
                            "pair": pair,
                            "role": role,
                            "workers": 0,
                            "sample_rate": 48000,
                            "block_frames": 256,
                            "channels": 8,
                            "warmup_blocks": 2000,
                            "measured_blocks": 10000,
                            "process_pinned": True,
                            "priority_set": True,
                            (
                                "callback_thread_priority_gate_passed"
                            ): True,
                            "output_finite": True,
                            "nonfinite_output_sample_count": 0,
                            "arena_overflow_count": 0,
                            "dynamic_route_refusal_count": 0,
                            "dynamic_plan_retire_overflow_count": 0,
                            "setup_valid": True,
                            "setup_error": "none",
                            "deadline_misses": 0,
                            "median_us": 100.0,
                            "p99_us": 200.0,
                            "max_us": 300.0,
                        }
                    )
                rows.append(
                    {
                        **performance_contract(True, True),
                        "benchmark": "compressor-matrix-paired",
                        "status": "measured",
                        "scenario": scenario,
                        "pair": pair,
                        "workers": 0,
                        "order": "B-F-F-B",
                        "sample_rate": 48000,
                        "block_frames": 256,
                        "channels": 8,
                        "process_pinned": True,
                        "priority_set": True,
                        "callback_thread_priority_gate_passed": True,
                        "warmup_blocks_per_session": 2000,
                        "measured_blocks_per_session": 10000,
                        "matched_delta_blocks": 10000,
                        "paired_delta_within_budget": True,
                        "quartet_zero_deadline_misses": True,
                        "quartet_absolute_p99_below_deadline": True,
                        "all_sessions_output_finite": True,
                        "nonfinite_output_sample_count": 0,
                        "scoped_gate_applicable": True,
                        "aggregate_delta_conditions_met": True,
                        "aggregate_delta_gate_passed": True,
                        "scoped_gate_passed": True,
                        "scoped_gate_status": "passed",
                        "hashes_stable": True,
                        "same_init_config": True,
                        "setup_phase_aligned": True,
                        "setup_valid": True,
                        "paired_delta_median_us": 100.0,
                        "paired_delta_p99_us": 200.0,
                        "paired_delta_max_us": 300.0,
                        "paired_delta_budget_us": 1000.0,
                        "feature_absolute_p99_us": 500.0,
                        "baseline_absolute_p99_us": 300.0,
                        "feature_first_absolute_deadline_misses": 0,
                        "feature_second_absolute_deadline_misses": 0,
                        "baseline_first_absolute_deadline_misses": 0,
                        "baseline_second_absolute_deadline_misses": 0,
                    }
                )
            rows.extend(
                [
                    {
                        **performance_contract(True, True),
                        "benchmark": "compressor-matrix-paired",
                        "status": "aggregate",
                        "scenario": scenario,
                        "workers": 0,
                        "pairs": 5,
                        "all_sessions_output_finite": True,
                        "nonfinite_output_sample_count": 0,
                        "process_pinned": True,
                        "priority_set": True,
                        "scoped_gate_applicable": True,
                        "setup_valid": True,
                        "scoped_gate_passed": True,
                        "scoped_gate_status": "passed",
                        "max_pair_delta_p99_us": 300.0,
                    },
                    {
                        **performance_contract(True, True),
                        "benchmark": "compressor-matrix-paired",
                        "status": "complete",
                        "setup_valid": True,
                        "process_pinned": True,
                        "priority_set": True,
                        "canonical_counts": True,
                        (
                            "expected_scoped_scenario_worker_mask"
                        ): mask,
                        (
                            "observed_scoped_scenario_worker_mask"
                        ): mask,
                        "expected_scoped_rows": 1,
                        "observed_scoped_rows": 1,
                        "scoped_coverage_complete": True,
                        "all_sessions_output_finite": True,
                        "nonfinite_output_sample_count": 0,
                        (
                            "required_paired_scenario_worker_mask"
                        ): "00000011",
                        "required_paired_coverage_complete": False,
                        "scoped_gate_applicable": True,
                        "scoped_gate_passed": True,
                        "scoped_gate_status": "passed",
                        "overall_runtime_gate_passed": False,
                        "overall_runtime_gate_status": (
                            "unverified-full-matrix-and-external-"
                            "evidence-required"
                        ),
                    },
                ]
            )
            return rows

        with tempfile.TemporaryDirectory() as directory:
            run_root = Path(directory)
            logs = run_root / "logs"
            logs.mkdir()
            records = []
            contracts = (
                (
                    "perf-compressor-voice-self",
                    "voice-self-64",
                    "00000001",
                ),
                (
                    "perf-compressor-bus-routed",
                    "bus-routed-16x4",
                    "00000010",
                ),
            )
            for index, (kind, scenario, mask) in enumerate(contracts):
                stdout = logs / f"{index}.stdout.log"
                stderr = logs / f"{index}.stderr.log"
                stdout.write_text(
                    "\n".join(
                        json.dumps(row)
                        for row in paired_rows(scenario, mask)
                    )
                    + "\n",
                    encoding="utf-8",
                )
                stderr.write_bytes(b"")
                records.append(
                    {
                        "kind": kind,
                        "stdout": {
                            "path": str(stdout.relative_to(run_root)),
                            "size": stdout.stat().st_size,
                        },
                        "stderr": {
                            "path": str(stderr.relative_to(run_root)),
                            "size": 0,
                        },
                    }
                )
            self.assertEqual(
                [],
                MODULE.validate_compressor_paired_evidence(
                    run_root,
                    records,
                ),
            )
            rows = [
                json.loads(line)
                for line in (
                    run_root / records[0]["stdout"]["path"]
                ).read_text(encoding="utf-8").splitlines()
            ]
            session = next(
                row
                for row in rows
                if row["status"] == "session"
                and row["pair"] == 1
                and row["role"] == "B1"
            )
            session["deadline_misses"] = 1
            session["deadline_observation_status"] = "unclassified"
            measured = next(
                row
                for row in rows
                if row["status"] == "measured" and row["pair"] == 1
            )
            measured["quartet_zero_deadline_misses"] = False
            measured["baseline_first_absolute_deadline_misses"] = 1
            measured["deadline_observation_status"] = "unclassified"
            next(
                row for row in rows if row["status"] == "aggregate"
            )["deadline_observation_status"] = "unclassified"
            next(
                row for row in rows if row["status"] == "complete"
            )["deadline_observation_status"] = "unclassified"
            (run_root / records[0]["stdout"]["path"]).write_text(
                "\n".join(json.dumps(row) for row in rows) + "\n",
                encoding="utf-8",
            )
            self.assertEqual(
                [],
                MODULE.validate_compressor_paired_evidence(
                    run_root,
                    records,
                ),
            )
            rows[-1]["scoped_gate_passed"] = False
            (run_root / records[0]["stdout"]["path"]).write_text(
                "\n".join(json.dumps(row) for row in rows) + "\n",
                encoding="utf-8",
            )
            violations = MODULE.validate_compressor_paired_evidence(
                run_root,
                records,
            )
            self.assertTrue(
                any(
                    item["rule"]
                    == "paired_complete_scoped_gate_passed"
                    for item in violations
                )
            )

    def test_compressor_report_preserves_expected_child_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_root = Path(directory)
            source_root = run_root / "source"
            release = run_root / "build" / "vs" / "bin" / "Release"
            perf = run_root / "perf"
            harness = run_root / "inputs" / "harness"
            for path in (source_root, release, perf, harness):
                path.mkdir(parents=True)
            binary = release / "harken_benchmarks.exe"
            binary.write_bytes(b"qualified-binary")
            observer = (
                harness / "runtime_compressor_characterization.py"
            )
            observer.write_bytes(COMPRESSOR_SCRIPT_PATH.read_bytes())
            raw_stdout = (
                perf / "compressor-characterization.stdout.log"
            )
            raw_stderr = (
                perf / "compressor-characterization.stderr.log"
            )
            raw_stdout.write_text("{}\n", encoding="utf-8")
            raw_stderr.write_bytes(b"")
            report_path = (
                perf / "compressor-characterization.json"
            )
            boundary = {
                "characterization_complete": True,
                "finite_cost_regression_certified": False,
                "hard_realtime_certified": False,
                "glitch_free_certified": False,
                "full_runtime_gate_closed": False,
            }
            report = {
                "schema_version": 1,
                "claim_id": "M1-compressor-characterization",
                "commit_sha": "a" * 40,
                "validation_passed": True,
                "validation_issues": [],
                "binary": {
                    "path": str(binary.resolve()),
                    "sha256": MODULE.sha256_file(binary),
                    "post_sha256": MODULE.sha256_file(binary),
                    "unchanged": True,
                    "argv": [
                        str(binary.resolve()),
                        "--filter",
                        "compressor-matrix",
                    ],
                    "cwd": str(source_root.resolve()),
                    "exit_code": 1,
                },
                "observer": {
                    "path": str(observer.resolve()),
                    "sha256": MODULE.sha256_file(observer),
                    "duration_seconds": 1.0,
                },
                "raw_evidence": {
                    "stdout": {
                        "path": str(raw_stdout.resolve()),
                        "sha256": MODULE.sha256_file(raw_stdout),
                        "size": raw_stdout.stat().st_size,
                    },
                    "stderr": {
                        "path": str(raw_stderr.resolve()),
                        "sha256": MODULE.sha256_file(raw_stderr),
                        "size": 0,
                    },
                },
                "result": {
                    "measured_rows": 35,
                    "deadline_misses": 0,
                    "absolute_budget_failures": [],
                    "max_callback_deadline_fraction": 0.5,
                    "full_runtime_gate_status": (
                        "unverified-full-matrix-and-external-"
                        "evidence-required"
                    ),
                },
                "claim_boundary": boundary,
            }
            report_path.write_text(
                json.dumps(report) + "\n",
                encoding="utf-8",
            )
            manifest = {
                "commit_sha": "a" * 40,
                "source_root": str(source_root.resolve()),
                "qualification_harness": {
                    "files": [
                        {
                            "path": str(
                                observer.relative_to(run_root)
                            ),
                            "sha256": MODULE.sha256_file(observer),
                            "size": observer.stat().st_size,
                        }
                    ]
                },
            }
            artifacts = []
            for role, path in (
                ("report", report_path),
                ("raw-stdout", raw_stdout),
                ("raw-stderr", raw_stderr),
            ):
                artifacts.append(
                    {
                        "role": role,
                        "path": str(path.relative_to(run_root)),
                        "sha256": MODULE.sha256_file(path),
                        "size": path.stat().st_size,
                    }
                )
            records = [
                {
                    "kind": "perf-compressor-matrix",
                    "artifacts": artifacts,
                }
            ]
            self.assertEqual(
                [],
                MODULE.validate_compressor_evidence(
                    manifest,
                    run_root,
                    records,
                ),
            )
            alternate_report = perf / "alternate-report.json"
            alternate_report.write_bytes(report_path.read_bytes())
            canonical_report_artifact = records[0]["artifacts"][0]
            records[0]["artifacts"][0] = {
                "role": "report",
                "path": str(alternate_report.relative_to(run_root)),
                "sha256": MODULE.sha256_file(alternate_report),
                "size": alternate_report.stat().st_size,
            }
            alternate_violations = (
                MODULE.validate_compressor_evidence(
                    manifest,
                    run_root,
                    records,
                )
            )
            self.assertTrue(
                any(
                    item["rule"] == "compressor_report_path"
                    for item in alternate_violations
                )
            )
            records[0]["artifacts"][0] = canonical_report_artifact
            report["binary"]["exit_code"] = 0
            report_path.write_text(
                json.dumps(report) + "\n",
                encoding="utf-8",
            )
            violations = MODULE.validate_compressor_evidence(
                manifest,
                run_root,
                records,
            )
            self.assertTrue(
                any(
                    item["rule"]
                    == "compressor_expected_child_exit"
                    for item in violations
                )
            )

    def test_memory_report_role_cannot_substitute_another_path(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_root = Path(directory)
            memory = run_root / "memory"
            release = run_root / "build" / "vs" / "bin" / "Release"
            harness = run_root / "inputs" / "harness"
            for path in (memory, release, harness):
                path.mkdir(parents=True)
            binary = release / "harken_benchmarks.exe"
            binary.write_bytes(b"qualified-binary")
            observer = harness / "runtime_memory_observer.py"
            observer.write_bytes(MEMORY_SCRIPT_PATH.read_bytes())
            stdout = memory / "observer.stdout.log"
            stderr = memory / "observer.stderr.log"
            stdout.write_text("{}\n", encoding="utf-8")
            stderr.write_bytes(b"")
            alternate = memory / "alternate-memory.json"
            alternate.write_text(
                json.dumps(
                    {
                        "commit_sha": "b" * 40,
                        "binary": {},
                        "observer": {},
                        "config": {},
                        "process_bytes": {},
                        "working_set_bytes": {},
                        "arena": {},
                        "capacities": {},
                        "static_allocations": [],
                        "limitations": [],
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            manifest = {
                "commit_sha": "b" * 40,
                "evidence_policy": {
                    "entries": [
                        {
                            "kind": "memory-baseline",
                            "execution_kind": "subprocess",
                        }
                    ]
                },
                "qualification_harness": {
                    "files": [
                        {
                            "path": str(
                                observer.relative_to(run_root)
                            ),
                            "sha256": MODULE.sha256_file(observer),
                            "size": observer.stat().st_size,
                        }
                    ]
                },
            }
            records = [
                {
                    "kind": "memory-baseline",
                    "artifacts": [
                        {
                            "role": "report",
                            "path": str(alternate.relative_to(run_root)),
                        },
                        {
                            "role": "raw-stdout",
                            "path": str(stdout.relative_to(run_root)),
                        },
                        {
                            "role": "raw-stderr",
                            "path": str(stderr.relative_to(run_root)),
                        },
                    ],
                }
            ]
            violations = MODULE.validate_memory_evidence(
                manifest,
                run_root,
                records,
            )
            self.assertTrue(
                any(
                    item["rule"] == "memory_report_path"
                    for item in violations
                )
            )

    def test_ctest_coverage_rejects_filters_and_count_mismatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_root = Path(directory)
            logs = run_root / "logs"
            tests = run_root / "tests"
            logs.mkdir()
            tests.mkdir()
            inventory = logs / "inventory.log"
            inventory.write_text(
                "Test #1: A\nTest #2: B\nTotal Tests: 2\n",
                encoding="utf-8",
            )
            for name in ("debug", "release"):
                (tests / f"{name}.junit.xml").write_text(
                    '<testsuites tests="2"></testsuites>\n',
                    encoding="utf-8",
                )
            records = [
                {
                    "kind": "ctest-inventory",
                    "stdout": {"path": "logs/inventory.log"},
                },
                {
                    "kind": "ctest-debug",
                    "argv": [
                        "ctest",
                        "-C",
                        "Debug",
                        "--output-junit",
                        str(tests / "debug.junit.xml"),
                    ],
                    "artifacts": [
                        {
                            "role": "junit",
                            "path": "tests/debug.junit.xml",
                        }
                    ],
                },
                {
                    "kind": "ctest-release",
                    "argv": [
                        "ctest",
                        "-C",
                        "Release",
                        "--output-junit",
                        str(tests / "release.junit.xml"),
                    ],
                    "artifacts": [
                        {
                            "role": "junit",
                            "path": "tests/release.junit.xml",
                        }
                    ],
                },
            ]
            self.assertEqual(
                [],
                MODULE.validate_ctest_coverage(run_root, records),
            )
            records[1]["argv"].extend(["-R", "A"])
            violations = MODULE.validate_ctest_coverage(
                run_root,
                records,
            )
            self.assertTrue(
                any(
                    item["rule"] == "unfiltered_ctest"
                    for item in violations
                )
            )
            records[1]["argv"] = [
                "ctest",
                "-C",
                "Debug",
                "--output-junit",
                str(tests / "debug.junit.xml"),
            ]
            (tests / "release.junit.xml").write_text(
                '<testsuites tests="1"></testsuites>\n',
                encoding="utf-8",
            )
            violations = MODULE.validate_ctest_coverage(
                run_root,
                records,
            )
            self.assertTrue(
                any(
                    item["rule"] == "junit_inventory_count"
                    for item in violations
                )
            )

    def test_require_under_accepts_child_and_rejects_sibling(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child = root / "evidence" / "run"
            sibling = root / "other"
            child.mkdir(parents=True)
            sibling.mkdir()
            self.assertEqual(
                child.resolve(),
                MODULE.require_under(child, root / "evidence"),
            )
            with self.assertRaises(ValueError):
                MODULE.require_under(sibling, root / "evidence")

    def test_prepare_run_and_finalize(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(
                ["git", "init"],
                cwd=repo,
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["git", "config", "user.email", "test@example.invalid"],
                cwd=repo,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Runtime Test"],
                cwd=repo,
                check=True,
            )
            (repo / "seed.txt").write_text("seed\n", encoding="utf-8")
            for name in ("A.txt", "b.txt", "Z.txt"):
                (repo / name).write_text(name + "\n", encoding="utf-8")
            (repo / "probe_module.py").write_text(
                "VALUE = 'qualified'\n",
                encoding="utf-8",
            )
            for relative in MODULE.REQUIRED_SOURCE_CONTRACT_PATHS:
                path = repo / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("contract input\n", encoding="utf-8")
            subprocess.run(
                ["git", "add", "-A"], cwd=repo, check=True
            )
            subprocess.run(
                ["git", "commit", "-m", "seed"],
                cwd=repo,
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["git", "remote", "add", "origin", str(repo)],
                cwd=repo,
                check=True,
            )
            subprocess.run(
                ["git", "update-ref", "refs/remotes/origin/main", "HEAD"],
                cwd=repo,
                check=True,
            )

            run_id = "test-run"
            original_policy = MODULE.M1_EVIDENCE_POLICY
            MODULE.M1_EVIDENCE_POLICY = {
                "policy_version": 1,
                "entries": [
                    {
                        "kind": "probe",
                        "execution_kind": "subprocess",
                        "count": 1,
                        "executable": Path(sys.executable).resolve().stem,
                    },
                    {
                        "kind": "raw",
                        "execution_kind": "subprocess",
                        "count": 1,
                        "executable": Path(sys.executable).resolve().stem,
                    },
                    {
                        "kind": "memory-baseline",
                        "execution_kind": "external",
                        "count": 1,
                    },
                    {
                        "kind": "configure-contract",
                        "execution_kind": "internal-verification",
                        "count": 1,
                    },
                    {
                        "kind": "vs-build-debug",
                        "execution_kind": "external",
                        "count": 0,
                    },
                ],
            }
            self.addCleanup(
                setattr,
                MODULE,
                "M1_EVIDENCE_POLICY",
                original_policy,
            )
            toolchain_dir = repo / "toolchain"
            toolchain_dir.mkdir()
            compiler_path = toolchain_dir / "cl.exe"
            linker_path = toolchain_dir / "link.exe"
            rc_path = toolchain_dir / "rc.exe"
            cmake_path = toolchain_dir / "cmake.exe"
            ctest_path = toolchain_dir / "ctest.exe"
            dotnet_path = toolchain_dir / "dotnet.exe"
            powercfg_path = toolchain_dir / "powercfg.exe"
            for path in (
                compiler_path,
                linker_path,
                rc_path,
                cmake_path,
                ctest_path,
                dotnet_path,
                powercfg_path,
            ):
                path.write_bytes(path.name.encode("ascii"))
            successful_probe = {"exit_code": 0, "stdout": "", "stderr": ""}
            toolchain = {
                "visual_studio": {
                    "installation_path": successful_probe,
                    "installation_version": successful_probe,
                },
                "msvc_toolset": "14.42.34433",
                "compiler_path": str(compiler_path),
                "compiler_sha256": MODULE.sha256_file(compiler_path),
                "compiler": successful_probe,
                "linker_path": str(linker_path),
                "linker_sha256": MODULE.sha256_file(linker_path),
                "linker": successful_probe,
                "windows_sdk": {
                    "version": "10.0.26100.0",
                    "bin_path": str(toolchain_dir),
                    "rc_sha256": MODULE.sha256_file(rc_path),
                },
            }
            original_toolchain_facts = MODULE.windows_toolchain_facts
            MODULE.windows_toolchain_facts = lambda _repo: toolchain
            self.addCleanup(
                setattr,
                MODULE,
                "windows_toolchain_facts",
                original_toolchain_facts,
            )
            tool_versions = {
                "cmake": (cmake_path, "cmake version 4.0.4"),
                "ctest": (ctest_path, "ctest version 4.0.4"),
                "dotnet": (dotnet_path, "10.0.200"),
                "powercfg": (powercfg_path, "Power Scheme GUID: test"),
                sys.executable: (
                    Path(sys.executable).resolve(),
                    "Python 3.13.9",
                ),
            }

            def fixture_tool_version(_repo, argv):
                path, stdout = tool_versions[argv[0]]
                return {
                    "argv": argv,
                    "exit_code": 0,
                    "stdout": stdout,
                    "stderr": "",
                    "resolved_path": str(path),
                    "sha256": MODULE.sha256_file(path),
                }

            original_tool_version = MODULE.tool_version
            MODULE.tool_version = fixture_tool_version
            self.addCleanup(
                setattr,
                MODULE,
                "tool_version",
                original_tool_version,
            )
            run_root = (
                repo
                / ".temp"
                / "runtime-qualification"
                / MODULE.git_text(repo, "rev-parse", "HEAD")
                / run_id
            )
            prepare_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "expected_commit": MODULE.git_text(
                        repo, "rev-parse", "HEAD"
                    ),
                    "run_id": run_id,
                    "run_root": str(run_root),
                },
            )()
            wrong_prepare_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "expected_commit": "0" * 40,
                    "run_id": run_id,
                    "run_root": str(run_root),
                },
            )()
            with self.assertRaises(RuntimeError):
                MODULE.prepare(wrong_prepare_args)
            self.assertFalse(run_root.exists())
            self.assertEqual(0, MODULE.prepare(prepare_args))
            with self.assertRaises(RuntimeError):
                MODULE.prepare(prepare_args)
            self.assertEqual(
                "seed\n",
                (run_root / "source" / "seed.txt").read_text(
                    encoding="utf-8"
                ),
            )
            source_status = subprocess.run(
                ["git", "status", "--porcelain=v1"],
                cwd=run_root / "source",
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual("", source_status.stdout)
            source_index = json.loads(
                (run_root / "source-inputs.sha256.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                5 + len(MODULE.REQUIRED_SOURCE_CONTRACT_PATHS),
                len(source_index),
            )

            command = [
                sys.executable,
                "-c",
                "import probe_module; print(probe_module.VALUE)",
            ]
            run_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "kind": "probe",
                    "cwd": None,
                    "command": command,
                },
            )()
            prepared = MODULE.load_manifest(run_root)
            frozen_harness = (
                run_root
                / prepared["qualification_harness"]["files"][0]["path"]
            )
            frozen_bytes = frozen_harness.read_bytes()
            frozen_harness.write_bytes(frozen_bytes + b"\n")
            with self.assertRaises(RuntimeError):
                MODULE.execute(run_args)
            frozen_harness.write_bytes(frozen_bytes)
            untampered_manifest = MODULE.load_manifest(run_root)
            tampered_manifest = json.loads(
                json.dumps(untampered_manifest)
            )
            tampered_manifest["evidence_policy"]["entries"].pop()
            MODULE.write_manifest(run_root, tampered_manifest)
            with self.assertRaises(RuntimeError):
                MODULE.execute(run_args)
            MODULE.write_manifest(run_root, untampered_manifest)

            generated_source_output = (
                run_root / "source" / ".temp" / "test-output.bin"
            )
            generated_source_output.parent.mkdir()
            generated_source_output.write_bytes(b"generated")
            outside_cwd_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "kind": "outside",
                    "cwd": str(repo),
                    "command": command,
                },
            )()
            with self.assertRaises(ValueError):
                MODULE.execute(outside_cwd_args)
            self.assertEqual(0, MODULE.execute(run_args))
            self.assertFalse(
                (run_root / "source" / "__pycache__").exists()
            )
            probe_record = MODULE.read_command_records(
                run_root / "commands.jsonl"
            )[-1]
            self.assertEqual(
                MODULE.FIXED_CHILD_ENVIRONMENT,
                {
                    key: probe_record["environment_overrides"][key]
                    for key in MODULE.FIXED_CHILD_ENVIRONMENT
                },
            )
            self.assertEqual(
                0,
                MODULE.execute(
                    type(
                        "Args",
                        (),
                        {
                            "repo_root": str(repo),
                            "run_root": str(run_root),
                            "kind": "raw",
                            "cwd": None,
                            "command": [
                                sys.executable,
                                "-c",
                                (
                                    "import sys; "
                                    "sys.stdout.buffer.write(b'\\xff')"
                                ),
                            ],
                        },
                    )()
                ),
            )
            raw_log = next(
                (run_root / "logs").glob("*-raw.stdout.log")
            )
            self.assertEqual(b"\xff", raw_log.read_bytes())
            release_dir = (
                run_root / "build" / "vs" / "bin" / "Release"
            )
            release_dir.mkdir(parents=True)
            memory_binary = release_dir / "harken_benchmarks.exe"
            memory_binary.write_bytes(b"benchmark")
            wrong_workers_record = {
                "kind": "perf-compressor-voice-self",
                "execution_kind": "subprocess",
                "argv": [
                    str(memory_binary),
                    "--filter",
                    "compressor-matrix",
                    "--case",
                    "voice-self",
                    "--workers",
                    "10",
                    "--paired",
                ],
                "executable": {
                    "resolved_path": str(memory_binary),
                },
            }
            self.assertTrue(
                any(
                    item["rule"] == "exact_command_tail"
                    for item in MODULE.validate_record_scope(
                        prepared,
                        run_root,
                        [wrong_workers_record],
                    )
                )
            )
            wrong_python_record = {
                "kind": "perf-compressor-matrix",
                "execution_kind": "subprocess",
                "cwd": prepared["source_root"],
                "argv": [
                    prepared["tools"]["python"]["resolved_path"],
                ],
                "executable": {
                    "resolved_path": prepared["tools"]["python"][
                        "resolved_path"
                    ],
                    "sha256": "0" * 64,
                },
            }
            self.assertTrue(
                any(
                    item["rule"] == "qualified_tool_identity"
                    for item in MODULE.validate_record_scope(
                        prepared,
                        run_root,
                        [wrong_python_record],
                    )
                )
            )
            wrong_benchmark_hash_record = {
                "kind": "perf-phase1",
                "execution_kind": "subprocess",
                "cwd": prepared["source_root"],
                "argv": [
                    str(memory_binary),
                    "--filter",
                    "phase1",
                ],
                "executable": {
                    "resolved_path": str(memory_binary),
                    "sha256": "0" * 64,
                },
            }
            self.assertTrue(
                any(
                    item["rule"] == "qualified_benchmark_hash"
                    for item in MODULE.validate_record_scope(
                        prepared,
                        run_root,
                        [wrong_benchmark_hash_record],
                    )
                )
            )
            external_artifact = (
                run_root / "memory" / "memory-baseline.json"
            )
            external_artifact.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "claim_id": "M1-memory",
                        "commit_sha": prepared["commit_sha"],
                        "binary": {
                            "path": str(memory_binary),
                            "sha256": MODULE.sha256_file(memory_binary),
                        },
                        "config": {"sample_rate": 48000},
                        "process_bytes": {
                            "initial_observation": 10,
                            "peak": 20,
                            "after_exit": 10,
                        },
                        "runtime_phase_bytes": {
                            "after_init": None,
                            "after_warmup": None,
                            "high_water": 20,
                            "after_term": None,
                        },
                        "arena": {
                            "high_water_bytes": 1024,
                            "overflow_count": 0,
                        },
                        "capacities": {"max_voices": 64},
                        "static_allocations": [
                            {"name": "arena", "bytes": 1024}
                        ],
                        "limitations": [
                            "Unit-test fixture with partial phase data."
                        ],
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            external_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "kind": "memory-baseline",
                    "cwd": None,
                    "status": "success",
                    "started_utc": "2026-01-01T00:00:00Z",
                    "ended_utc": "2026-01-01T00:00:01Z",
                    "artifact": [f"report={external_artifact}"],
                    "metadata": ["configuration=Release"],
                },
            )()
            self.assertEqual(0, MODULE.record_external(external_args))
            incomplete_vs_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "kind": "vs-build-debug",
                    "cwd": None,
                    "status": "success",
                    "started_utc": "2026-01-01T00:00:00Z",
                    "ended_utc": "2026-01-01T00:00:01Z",
                    "artifact": [f"output={external_artifact}"],
                    "metadata": [],
                },
            )()
            with self.assertRaises(ValueError):
                MODULE.record_external(incomplete_vs_args)

            native_dir = (
                run_root / "build" / "vs" / "bin" / "Release"
            )
            native_dir.mkdir(parents=True, exist_ok=True)
            native_dll = native_dir / "HarkenCore.dll"
            native_dll.write_bytes(b"native")
            overrides, native_inputs = MODULE.command_environment(
                [f"HARKEN_NATIVE_DIR={native_dir}"],
                run_root,
            )
            self.assertEqual(str(native_dir), overrides["HARKEN_NATIVE_DIR"])
            self.assertEqual(
                MODULE.sha256_file(native_dll),
                native_inputs[0]["sha256"],
            )
            with self.assertRaises(ValueError):
                MODULE.command_environment(
                    [
                        "HARKEN_NATIVE_DIR="
                        + str(run_root / "build" / "native")
                    ],
                    run_root,
                )

            manifest_path = run_root / "manifest.json"
            prepared_manifest = json.loads(
                manifest_path.read_text(encoding="utf-8")
            )
            toolchain = prepared_manifest["tools"]["windows_toolchain"]
            compiler_path = Path(toolchain["compiler_path"]).resolve()
            linker_path = Path(toolchain["linker_path"]).resolve()
            fake_build = run_root / "build" / "vs"
            compiler_dir = fake_build / "CMakeFiles" / "4.0.4"
            compiler_dir.mkdir(parents=True)
            contract = prepared_manifest["configure_contract"]
            (fake_build / "CMakeCache.txt").write_text(
                "\n".join(
                    f"{key}:STRING={value}"
                    for key, value in contract["cmake_cache"].items()
                )
                + "\n",
                encoding="utf-8",
            )
            (
                compiler_dir / "CMakeCXXCompiler.cmake"
            ).write_text(
                "\n".join(
                    [
                        f'set(CMAKE_CXX_COMPILER "{compiler_path}")',
                        f'set(CMAKE_LINKER "{linker_path}")',
                        'set(CMAKE_CXX_COMPILER_VERSION "test")',
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            (fake_build / "ALL_BUILD.vcxproj").write_text(
                (
                    "<WindowsTargetPlatformVersion>"
                    f"{contract['windows_sdk']}"
                    "</WindowsTargetPlatformVersion>\n"
                ),
                encoding="utf-8",
            )
            build_info = (
                fake_build
                / "generated"
                / "harken_buildinfo"
                / "harken_buildinfo.h"
            )
            build_info.parent.mkdir(parents=True)
            build_info.write_text(
                "\n".join(
                    [
                        (
                            "inline constexpr std::string_view "
                            "GitCommit        = "
                            f'"{prepared_manifest["commit_sha"]}";'
                        ),
                        (
                            "inline constexpr bool             "
                            "GitDirty         = false;"
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            verify_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "build_dir": str(fake_build),
                },
            )()
            self.assertEqual(0, MODULE.verify_configure(verify_args))
            wrong_verify_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                    "build_dir": str(run_root / "build" / "other"),
                },
            )()
            with self.assertRaises(RuntimeError):
                MODULE.verify_configure(wrong_verify_args)

            seed_path = run_root / "source" / "seed.txt"
            seed_path.write_text("tampered\n", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                MODULE.execute(run_args)
            seed_path.write_text("seed\n", encoding="utf-8")

            records = MODULE.read_command_records(
                run_root / "commands.jsonl"
            )
            raw_bytes = raw_log.read_bytes()
            raw_log.write_bytes(b"tampered")
            self.assertTrue(
                MODULE.validate_record_integrity(run_root, records)
            )
            raw_log.write_bytes(raw_bytes)

            finalize_args = type(
                "Args",
                (),
                {
                    "repo_root": str(repo),
                    "run_root": str(run_root),
                },
            )()
            self.assertEqual(0, MODULE.finalize(finalize_args))
            manifest = json.loads(
                (run_root / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(run_id, manifest["run_id"])
            self.assertIsNotNone(manifest["timestamp_end_utc"])
            self.assertEqual("finalized", manifest["state"])
            self.assertEqual("passed", manifest["qualification_status"])
            self.assertTrue(
                any(
                    item["path"].endswith(".stdout.log")
                    for item in manifest["artifacts"]
                )
            )
            self.assertTrue(
                any(
                    item["path"].replace("\\", "/").endswith(
                        "source/.temp/test-output.bin"
                    )
                    for item in manifest["artifacts"]
                )
            )
            with self.assertRaises(RuntimeError):
                MODULE.execute(run_args)
            with self.assertRaises(RuntimeError):
                MODULE.finalize(finalize_args)


if __name__ == "__main__":
    unittest.main()
