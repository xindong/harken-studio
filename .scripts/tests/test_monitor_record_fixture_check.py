from __future__ import annotations

import hashlib
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
RECORD_SCRIPT = (
    REPO_ROOT
    / "HarkenEngine"
    / "Source"
    / "Runtime"
    / "api"
    / "monitor"
    / "fixtures"
    / "generate_record_fixtures.py"
)
MODULE = load_script_module("generate_record_fixtures", RECORD_SCRIPT)


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class MonitorRecordFixtureCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="monitor-record-"))
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.root = self.temp / "monitor"
        self.manifest = self.root / "manifest.json"
        self.m0 = MODULE.container_bytes()
        self.lifecycle = MODULE.lifecycle_container_bytes()
        self._write_base_tree()

    def _write_file(self, relative: str, data: bytes) -> None:
        path = self.root.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def _write_base_tree(self) -> None:
        nested = b'{"kind":"gap"}\n'
        files = [
            ("legal_event.bin", self.m0, "legal-event", None),
            ("lifecycle/legal_event.bin", self.lifecycle, "legal-event", "m2-1"),
            ("lifecycle/gap_variable_control.json", nested, "gap-variable-control", "m2-1"),
            ("lifecycle/gap_queue_cursor.json", nested, "gap-queue-cursor", "m2-1"),
            (
                "lifecycle/gap_playable_world_generation.json",
                nested,
                "gap-playable-world-generation",
                "m2-1",
            ),
            ("lineage/gap_layer_g21.json", nested, "gap-layer", "m2-2"),
        ]
        entries = []
        for relative, data, kind, producer in files:
            self._write_file(relative, data)
            entry = {
                "path": relative,
                "sha256": _sha(data),
                "isProvisional": False,
                "kind": kind,
                "schemaVersion": 1,
            }
            if producer is not None:
                entry["producerPackage"] = producer
            entries.append(entry)
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(
            json.dumps({"schemaVersion": 1, "isProvisional": False, "files": entries}, indent=2)
            + "\n",
            encoding="utf-8",
        )
        self._write_file("plan/lease.bin", b"plan-not-owned\n")
        self._write_file("remote/handshake_disabled.bin", b"remote-not-owned\n")
        self._write_file("session/legal.bin", b"session-not-owned\n")

    def _check(self) -> None:
        MODULE.check_artifacts(
            self.m0,
            _sha(self.m0),
            self.lifecycle,
            _sha(self.lifecycle),
            out=self.root,
            manifest=self.manifest,
        )

    def test_nested_present_and_foreign_trees_are_ignored(self) -> None:
        self._check()

    def test_nested_missing_fails_closed(self) -> None:
        (self.root / "lineage" / "gap_layer_g21.json").unlink()
        with self.assertRaises(SystemExit) as raised:
            self._check()
        self.assertIn("lineage/gap_layer_g21.json", str(raised.exception))

    def test_nested_bytes_drift_fails_closed(self) -> None:
        nested = self.root / "lineage" / "gap_layer_g21.json"
        nested.write_bytes(b'{"kind":"corrupted"}\n')
        with self.assertRaises(SystemExit) as raised:
            self._check()
        self.assertIn("lineage/gap_layer_g21.json", str(raised.exception))

    def test_nested_extra_fails_closed(self) -> None:
        self._write_file("lineage/extra.json", b"{}\n")
        with self.assertRaises(SystemExit) as raised:
            self._check()
        self.assertIn("lineage/extra.json", str(raised.exception))

    def test_windows_separators_normalize_to_posix(self) -> None:
        payload = json.loads(self.manifest.read_text(encoding="utf-8"))
        for item in payload["files"]:
            if item["path"] == "lineage/gap_layer_g21.json":
                item["path"] = r"lineage\gap_layer_g21.json"
        self.manifest.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        self._check()

    def test_absolute_posix_path_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            MODULE.manifest_relative_path("/lineage/gap_layer_g21.json")

    def test_windows_drive_path_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            MODULE.manifest_relative_path(r"C:\lineage\gap_layer_g21.json")


if __name__ == "__main__":
    unittest.main()
