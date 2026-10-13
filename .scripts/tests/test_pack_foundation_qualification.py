from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("pack_foundation_qualification.py")
SPEC = importlib.util.spec_from_file_location(
    "pack_foundation_qualification", SCRIPT_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def case(metrics: dict[str, float], footprint: int = 1024) -> dict:
    return {
        "metrics": {
            name: {"median": value} for name, value in metrics.items()
        },
        "resident_footprint_bytes": footprint,
    }


class PackFoundationQualificationTest(unittest.TestCase):
    def test_build_identity_requires_commit_configuration_and_clean_binary(
        self,
    ) -> None:
        runtime = {
            "build_commit": "abc123",
            "build_dirty": False,
            "build_configuration": "RelWithDebInfo",
        }
        studio = {
            "buildCommit": "abc123",
            "buildDirty": False,
            "buildConfiguration": "RelWithDebInfo",
        }
        self.assertTrue(MODULE.runtime_build_identity_matches(
            runtime, "abc123", "RelWithDebInfo"))
        self.assertTrue(MODULE.studio_build_identity_matches(
            studio, "abc123", "RelWithDebInfo"))

        for key, value in (
            ("build_commit", "old"),
            ("build_dirty", True),
            ("build_configuration", "Release"),
        ):
            changed = dict(runtime)
            changed[key] = value
            with self.subTest(runtime_key=key):
                self.assertFalse(MODULE.runtime_build_identity_matches(
                    changed, "abc123", "RelWithDebInfo"))

        for key, value in (
            ("buildCommit", "old"),
            ("buildDirty", True),
            ("buildConfiguration", "Release"),
        ):
            changed = dict(studio)
            changed[key] = value
            with self.subTest(studio_key=key):
                self.assertFalse(MODULE.studio_build_identity_matches(
                    changed, "abc123", "RelWithDebInfo"))

    def test_runtime_gates_pin_frozen_thresholds(self) -> None:
        small = {
            "inspect_us": 1.0,
            "load_init_us": 1.0,
            "content_cold_us": 40_000.0,
            "content_warm_us": 45_000.0,
            "replace_us": 79_999.0,
            "unload_us": 19_999.0,
            "post_loaded_us": 100.0,
            "post_unloaded_us": 10.0,
        }
        large = {
            "inspect_us": 49_999.0,
            "load_init_us": 99_999.0,
            "content_cold_us": 1.0,
            "content_warm_us": 1.0,
            "replace_us": 1.0,
            "unload_us": 1.0,
            "post_loaded_us": 299.0,
            "post_unloaded_us": 29.0,
        }
        gates = MODULE.runtime_gates(
            {
                "1000-64": case(small),
                "50000-1024": case(large, 8 << 20),
            }
        )
        self.assertTrue(all(item["passed"] for item in gates))

        large["post_loaded_us"] = 300.0
        gates = MODULE.runtime_gates(
            {
                "1000-64": case(small),
                "50000-1024": case(large, (8 << 20) + 1),
            }
        )
        failed = {item["name"] for item in gates if not item["passed"]}
        self.assertEqual(
            {
                "post_loaded_50k_to_1k_ratio",
                "resident_1024_body_plus_index_bytes",
            },
            failed,
        )

    def test_metric_summary_rejects_three_times_median(self) -> None:
        metrics, unstable = MODULE.metric_summary(
            [{"value": value} for value in (10.0, 10.0, 31.0)],
            ("value",),
        )
        self.assertEqual(["value"], unstable)
        self.assertFalse(metrics["value"]["stable"])

    def test_metric_summary_rejects_non_finite_and_negative_values(self) -> None:
        for value in (float("inf"), float("nan"), -1.0):
            with self.subTest(value=value):
                metrics, unstable = MODULE.metric_summary(
                    [{"value": 1.0}, {"value": 1.0}, {"value": value}],
                    ("value",),
                )
                self.assertEqual(["value"], unstable)
                self.assertFalse(metrics["value"]["stable"])


if __name__ == "__main__":
    unittest.main()
