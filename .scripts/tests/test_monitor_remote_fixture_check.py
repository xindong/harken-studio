from __future__ import annotations

import json
import unittest
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
REMOTE_SCRIPT = (
    REPO_ROOT
    / "HarkenEngine"
    / "Source"
    / "Runtime"
    / "api"
    / "monitor"
    / "fixtures"
    / "generate_remote_fixtures.py"
)
MODULE = load_script_module("generate_remote_fixtures", REMOTE_SCRIPT)
REMOTE_DIR = (
    REPO_ROOT / "HarkenEngine" / "Test" / "UnitTest" / "monitor" / "remote"
)
ABI_PATH = (
    REPO_ROOT
    / "HarkenEngine"
    / "Source"
    / "Tooling"
    / "Abi"
    / "generated"
    / "harken_abi_manifest.g.json"
)


class MonitorRemoteFixtureCheckTest(unittest.TestCase):
    def test_compare_sessions_is_required_independent_of_count(self) -> None:
        names = MODULE.parse_monitor_functions(
            {
                "contract": {
                    "nativeAbi": {
                        "functions": [
                            {"name": "Vn_MonitorPoll"},
                            {"name": "Vn_MonitorCompareSessions"},
                        ]
                    }
                }
            }
        )
        self.assertEqual(
            ["Vn_MonitorPoll", "Vn_MonitorCompareSessions"],
            names,
        )

    def test_missing_compare_sessions_fails_even_when_count_matches_old_pin(self) -> None:
        functions = [{"name": "Vn_Monitor%02d" % index} for index in range(19)]
        with self.assertRaises(SystemExit) as raised:
            MODULE.parse_monitor_functions(
                {"contract": {"nativeAbi": {"functions": functions}}}
            )
        self.assertIn("Vn_MonitorCompareSessions", str(raised.exception))

    def test_event_session_apis_do_not_shift_monitor_capability_bits(self) -> None:
        names = MODULE.parse_monitor_functions(
            {
                "contract": {
                    "nativeAbi": {
                        "functions": [
                            {"name": "Vn_MonitorOpenSession"},
                            {"name": "Vn_MonitorOpenEventSession"},
                            {"name": "Vn_MonitorSealEventSession"},
                            {"name": "Vn_MonitorGetEventSessionStatus"},
                            {"name": "Vn_MonitorCloseSession"},
                            {"name": "Vn_MonitorCompareSessions"},
                            {"name": "Vn_MonitorFutureGate"},
                        ]
                    }
                }
            }
        )
        self.assertEqual(
            [
                "Vn_MonitorOpenSession",
                "Vn_MonitorCloseSession",
                "Vn_MonitorCompareSessions",
                "Vn_MonitorFutureGate",
            ],
            names,
        )

    def test_empty_or_duplicate_monitor_names_fail_closed(self) -> None:
        with self.assertRaises(SystemExit):
            MODULE.parse_monitor_functions({"contract": {"nativeAbi": {"functions": []}}})
        with self.assertRaises(SystemExit):
            MODULE.parse_monitor_functions(
                {
                    "contract": {
                        "nativeAbi": {
                            "functions": [
                                {"name": "Vn_MonitorCompareSessions"},
                                {"name": "Vn_MonitorCompareSessions"},
                            ]
                        }
                    }
                }
            )

    def test_checked_in_allowlist_matches_abi_and_keeps_compare(self) -> None:
        abi = json.loads(ABI_PATH.read_text(encoding="utf-8"))
        names = MODULE.parse_monitor_functions(abi)
        allowlist = json.loads(
            (REMOTE_DIR / "allowlist_monitor_functions.json").read_text(encoding="utf-8")
        )
        expected = json.loads(
            (REMOTE_DIR / "contract.expected.json").read_text(encoding="utf-8")
        )
        self.assertEqual(names, allowlist["names"])
        self.assertEqual(len(names), expected["monitorFunctionCount"])
        self.assertIn("Vn_MonitorCompareSessions", names)

    def test_abi_growth_without_allowlist_update_fails_check(self) -> None:
        files = MODULE.artifacts()
        grown = dict(files)
        grown["allowlist_monitor_functions.json"] = json.dumps(
            {
                "schemaVersion": 1,
                "producingPackage": "m0-5",
                "kind": "monitor-functions",
                "names": json.loads(files["allowlist_monitor_functions.json"])["names"]
                + ["Vn_MonitorFutureGate"],
            },
            indent=2,
        ) + "\n"
        with self.assertRaises(SystemExit):
            MODULE.check_artifacts(grown)
        self.assertEqual(
            (REMOTE_DIR / "allowlist_monitor_functions.json").read_bytes(),
            MODULE.as_bytes(files["allowlist_monitor_functions.json"]),
        )

    def test_handshake_and_opcode_allowlist_stay_independent(self) -> None:
        files = MODULE.artifacts()
        self.assertEqual(
            (REMOTE_DIR / "allowlist_wire_opcodes.json").read_bytes(),
            MODULE.as_bytes(files["allowlist_wire_opcodes.json"]),
        )
        self.assertEqual(
            (REMOTE_DIR / "handshake_disabled.bin").read_bytes(),
            files["handshake_disabled.bin"],
        )


if __name__ == "__main__":
    unittest.main()
