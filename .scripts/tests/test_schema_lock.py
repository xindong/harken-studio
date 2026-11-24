from __future__ import annotations

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("schema_lock", REPO_ROOT / ".scripts/schema_lock.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class SchemaLockTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "HarkenStudio" / "Schema"
        self.pack_binary = self.root / "HarkenEngine" / "Schema" / "Pack"
        self.candidate = self.root / "candidate"
        self.staged = self.candidate / "HarkenStudio" / "Schema"
        self.staged_pack_binary = self.candidate / "HarkenEngine" / "Schema" / "Pack"
        self.editor_version = MODULE.schema_archive.load_tree("Editor")["currentVersion"]
        self.editor_history = MODULE.schema_archive.load_tree("Editor")["manifest"]["supportedHistorical"]
        shutil.copytree(REPO_ROOT / "HarkenStudio" / "Schema", self.source)
        shutil.copytree(REPO_ROOT / "HarkenEngine" / "Schema" / "Pack", self.pack_binary)
        for name, value in (("SCHEMA_ROOT", self.source), ("PACK_BINARY_ROOT", self.pack_binary),
                            ("REPO_ROOT", self.root)):
            active = patch.object(MODULE, name, value)
            active.start()
            self.addCleanup(active.stop)

    def add_metadata(self) -> dict:
        path = self.source / "Project/current/ProjectSerializedSchema.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["futureNote"] = {}
        path.write_text(json.dumps(value), encoding="utf-8")
        return value

    def stage(self, editor: str = "same", pack: str = "same") -> dict:
        return MODULE.stage_candidate(self.candidate, {"Editor": editor, "Pack": pack})

    def snapshot(self) -> dict:
        return {path.relative_to(self.root).as_posix(): path.read_bytes()
                for path in self.root.rglob("*") if path.is_file() and not path.is_relative_to(self.candidate)}

    def test_same_generation_stages_changed_tip_without_touching_published_sources(self) -> None:
        source = self.add_metadata()
        before = self.snapshot()
        result = self.stage()
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(result["trees"]["Editor"]["generation"], self.editor_version)
        staged = MODULE.schema_archive.load_tree("Editor", self.staged)
        self.assertEqual(staged["currentSource"], source)
        self.assertEqual(staged["published"][self.editor_version], staged["current"])
        self.assertNotEqual(MODULE.schema_archive.load_tree("Editor", self.source)["published"][self.editor_version], staged["current"])

    def test_pack_advances_independently_and_current_remains_unnumbered(self) -> None:
        current = MODULE.schema_archive.load_tree("Pack", self.source)["currentVersion"]
        result = self.stage(pack="next")
        self.assertEqual(result["trees"]["Editor"]["generation"], self.editor_version)
        self.assertEqual(result["trees"]["Pack"]["generation"], current + 1)
        pack = MODULE.schema_archive.load_tree("Pack", self.staged)
        self.assertNotIn("formatVersion", pack["currentSource"]["x-harken-format"])
        self.assertEqual(pack["current"]["$defs"]["packMetadataVersion"]["const"], current + 1)
        self.assertFalse((self.source / "PackMetadata" / str(current + 1)).exists())
        binary = MODULE.audit.load_pack_binary(self.staged_pack_binary)
        self.assertEqual(result["trees"]["PackBinary"]["generation"], binary["currentVersion"])
        self.assertEqual(binary["published"][binary["currentVersion"]], binary["current"])
        self.assertEqual(binary["currentVersion"], MODULE.audit.load_pack_binary(self.pack_binary)["currentVersion"] + 1)

    def test_next_editor_generation_preserves_latest_previous_snapshot(self) -> None:
        source = self.add_metadata()
        latest = MODULE.schema_archive.materialize_source("Editor", source, self.editor_version)
        (self.source / f"Project/{self.editor_version}/ProjectSerializedSchema.json").write_text(json.dumps(latest), encoding="utf-8")
        result = self.stage(editor="next")
        tree = MODULE.schema_archive.load_tree("Editor", self.staged)
        self.assertEqual(result["trees"]["Editor"]["generation"], self.editor_version + 1)
        self.assertEqual(tree["published"][self.editor_version], latest)
        self.assertEqual(tree["manifest"]["supportedHistorical"], sorted(set(self.editor_history) | {self.editor_version}))
        self.assertEqual(tree["currentSource"], source)
        self.assertNotIn("projectSchemaVersion", source)
        self.assertFalse((self.source / f"Project/{self.editor_version + 1}").exists())
        self.assertFalse(any("migration" in path.as_posix().lower() for path in self.candidate.rglob("*")))

    def test_candidate_is_buildable_before_repository_generation_changes(self) -> None:
        self.stage(editor="next")
        bundle = MODULE.schema_archive.build_bundle(self.staged)
        self.assertIn(f"Editor/{self.editor_version + 1}/ProjectSerializedSchema.json", bundle["installed"])
        self.assertEqual(bundle["manifest"]["trees"]["Editor"]["currentVersion"], self.editor_version + 1)
        self.assertEqual(MODULE.schema_archive.load_tree("Editor", self.source)["currentVersion"], self.editor_version)

    def test_existing_or_in_source_candidate_destination_is_rejected(self) -> None:
        self.candidate.mkdir()
        with self.assertRaises(ValueError):
            self.stage()
        with self.assertRaises(ValueError):
            MODULE.stage_candidate(self.source / "candidate", {"Editor": "same", "Pack": "same"})

    def test_current_changed_after_staging_is_rejected(self) -> None:
        self.stage()
        self.add_metadata()
        with self.assertRaises(ValueError):
            MODULE.candidate_updates(self.candidate)

    def test_candidate_cannot_rewrite_historical_snapshot(self) -> None:
        self.stage(editor="next")
        path = self.staged / f"Project/{self.editor_version}/ProjectSerializedSchema.json"
        contract = json.loads(path.read_text(encoding="utf-8"))
        contract["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["rewritten"] = {}
        path.write_text(json.dumps(contract), encoding="utf-8")
        with self.assertRaises(ValueError):
            MODULE.candidate_updates(self.candidate)

    def test_same_generation_archive_updates_snapshot_and_keeps_tip(self) -> None:
        source = self.add_metadata()
        self.stage()
        result = MODULE.archive_candidate(self.candidate)
        tree = MODULE.schema_archive.load_tree("Editor", self.source)
        self.assertEqual(tree["currentSource"], source)
        self.assertEqual(tree["published"][self.editor_version], tree["current"])
        self.assertEqual(result["trees"]["Editor"]["generation"], self.editor_version)
        self.assertFalse((self.source / f"Project/{self.editor_version + 1}").exists())

    def test_failed_archive_validation_rolls_back_all_contracts(self) -> None:
        self.stage(editor="next", pack="next")
        before = self.snapshot()
        with patch.object(MODULE.schema_archive, "build_bundle", side_effect=ValueError("invalid archive")):
            with self.assertRaisesRegex(ValueError, "invalid archive"):
                MODULE.archive_candidate(self.candidate)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.source / f"Project/{self.editor_version + 1}").exists())

    def test_archive_only_updates_manifests_and_numbered_contracts(self) -> None:
        self.stage(editor="next", pack="next")
        before = self.snapshot()
        result = MODULE.archive_candidate(self.candidate)
        after = self.snapshot()
        changed = {path for path in before.keys() | after.keys() if before.get(path) != after.get(path)}
        expected = set()
        binary = result["trees"].pop("PackBinary")
        expected.add("HarkenEngine/Schema/Pack/manifest.json")
        expected.update(f"HarkenEngine/Schema/Pack/{binary['generation']}/{path.name}"
                        for path in (self.pack_binary / "current").iterdir())
        for tree, summary in result["trees"].items():
            directory = MODULE.schema_archive.TREE_DIRECTORIES[tree]
            expected.add(f"HarkenStudio/Schema/{directory}/manifest.json")
            expected.add(f"HarkenStudio/Schema/{directory}/{summary['generation']}/{MODULE.audit.SCHEMA_NAMES[tree]}")
        self.assertEqual(changed, expected)
        self.assertEqual(result["schemaSnapshotHash"], MODULE.schema_archive.build_bundle(self.source)["manifest"]["snapshotHash"])


if __name__ == "__main__":
    unittest.main()
