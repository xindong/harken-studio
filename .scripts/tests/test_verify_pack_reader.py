from __future__ import annotations

import copy
import hashlib
import json
import runpy
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE = runpy.run_path(str(REPO_ROOT / ".scripts" / "release" / "verify_pack_reader.py"))
READER = REPO_ROOT / "HarkenSDK" / "Unity" / "Harken" / "Runtime" / "Sdk" / "PackManifest.cs"
SCHEMA = runpy.run_path(str(REPO_ROOT / ".scripts" / "schema_archive.py"))
PACK_VERSION = SCHEMA["load_tree"]("Pack")["currentVersion"]


def expected_report() -> dict:
    return {
        "schemaVersion": PACK_VERSION,
        "ok": True,
        "indexEntries": [{"id": "11111111-2222-3333-4444-555555555555", "name": "Compatibility", "basePlatform": "Windows", "path": "Compatibility"}],
        "manifestEntries": [
            {"name": "Init", "packStableId": "11", "path": "Init.pack", "platformId": "11111111-2222-3333-4444-555555555555", "contentHash": str(MODULE["fnv1a64"](b"init fixture")), "dependencies": []},
            {"name": "Content", "packStableId": "22", "path": "Content.pack", "platformId": "11111111-2222-3333-4444-555555555555", "contentHash": str(MODULE["fnv1a64"](b"content fixture")), "dependencies": ["11"]},
        ],
        "packPaths": ["Compatibility/Init.pack", "Compatibility/Content.pack"],
    }


class VerifyPackReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        (REPO_ROOT / ".temp").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="pack-reader-test-", dir=REPO_ROOT / ".temp")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.packs = self.root / "packs"
        self.packs.mkdir()
        (self.packs / "Init.pack").write_bytes(b"init fixture")
        (self.packs / "Content.pack").write_bytes(b"content fixture")
        self.work = self.root / "work"

    def verify(self, version: int = PACK_VERSION, source: Path = READER) -> dict:
        return MODULE["verify_pack_reader"](source, self.packs, version, self.work)

    @unittest.skipUnless(shutil.which("dotnet"), ".NET SDK is required for the real reader compilation")
    def test_real_parser_reads_index_manifest_and_paths_and_rejects_wrong_generation(self) -> None:
        old_source = self.root / "PackManifest.cs"
        original = READER.read_bytes() + b"\n"
        old_source.write_bytes(original)
        report = self.verify(source=old_source)
        self.assertEqual({key: report[key] for key in expected_report()}, expected_report())
        self.assertEqual(report["readerSha256"], hashlib.sha256(original).hexdigest())
        self.assertNotEqual(report["readerSha256"], hashlib.sha256(READER.read_bytes()).hexdigest())
        with self.assertRaises(ValueError):
            self.verify(version=PACK_VERSION + 1, source=old_source)
        self.assertEqual(old_source.read_bytes(), original)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_original_source_and_supplied_pack_files_reach_the_controlled_harness(self) -> None:
        commands = []

        def run(command, **options):
            commands.append(command)
            self.assertEqual(options["cwd"], REPO_ROOT)
            work = Path(command[2]).parent if command[1] == "build" else Path(command[1]).parents[3]
            self.assertTrue(work.is_relative_to(self.work))
            self.assertEqual((work / "PackManifest.cs").read_bytes(), READER.read_bytes())
            self.assertEqual(
                (work / "JsonParser.cs").read_bytes(),
                MODULE["JSON_PARSER"].read_bytes(),
            )
            project = ET.parse(work / "PackReader.proj")
            self.assertIn("JsonParser.cs", [item.get("Include") for item in project.findall(".//Compile")])
            self.assertEqual((work / "export" / "Compatibility" / "Init.pack").read_bytes(), b"init fixture")
            self.assertEqual((work / "export" / "Compatibility" / "Content.pack").read_bytes(), b"content fixture")
            if command[1] == "build":
                self.assertIsNotNone(ET.parse(work / "NuGet.Config").find("packageSources/clear"))
                return subprocess.CompletedProcess(command, 0, "", "")
            return subprocess.CompletedProcess(command, 0, json.dumps(expected_report()), "")

        with patch.object(MODULE["subprocess"], "run", side_effect=run):
            report = self.verify()
        self.assertEqual(len(commands), 2)
        self.assertEqual(commands[0][:2], ["dotnet", "build"])
        self.assertEqual(Path(commands[1][1]).name, "PackReader.dll")
        self.assertEqual(report["fixtureSha256"], {
            "Init.pack": hashlib.sha256(b"init fixture").hexdigest(),
            "Content.pack": hashlib.sha256(b"content fixture").hexdigest(),
        })

    def test_success_exit_cannot_hide_wrong_names_ids_paths_or_report_types(self) -> None:
        bad_reports = []
        for field, value in (("schemaVersion", True), ("schemaVersion", PACK_VERSION + 1), ("ok", False),
                             ("ok", 1), ("packPaths", ["../Init.pack", "Compatibility/Content.pack"])):
            bad_reports.append({**expected_report(), field: value})
        for field, key, value in (("manifestEntries", "name", "Other"),
                                  ("manifestEntries", "packStableId", "22"),
                                  ("indexEntries", "path", "OtherPlatform")):
            report = copy.deepcopy(expected_report())
            report[field][0][key] = value
            bad_reports.append(report)
        for report in bad_reports:
            with self.subTest(report=report):
                results = [subprocess.CompletedProcess([], 0, "", ""),
                           subprocess.CompletedProcess([], 0, json.dumps(report), "")]
                with patch.object(MODULE["subprocess"], "run", side_effect=results):
                    with self.assertRaises(ValueError):
                        self.verify()

    def test_compile_failure_missing_json_and_failed_process_are_rejected(self) -> None:
        cases = (
            [subprocess.CompletedProcess([], 1, "compile failed", "")],
            [subprocess.CompletedProcess([], 0, "", ""), subprocess.CompletedProcess([], 0, "not JSON", "")],
            [subprocess.CompletedProcess([], 0, "", ""), subprocess.CompletedProcess([], 1, json.dumps(expected_report()), "")],
        )
        for results in cases:
            with self.subTest(results=results):
                with patch.object(MODULE["subprocess"], "run", side_effect=results):
                    with self.assertRaises(ValueError):
                        self.verify()

    def test_missing_inputs_and_unsafe_work_directory_fail_before_compilation(self) -> None:
        with patch.object(MODULE["subprocess"], "run") as run:
            for version in (0, -1, True):
                with self.assertRaises(ValueError):
                    self.verify(version=version)
            with self.assertRaises(ValueError):
                self.verify(source=self.root / "missing.cs")
            with self.assertRaises(ValueError):
                MODULE["verify_pack_reader"](READER, self.packs, PACK_VERSION, REPO_ROOT)
            (self.packs / "Content.pack").write_bytes(b"")
            with self.assertRaises(ValueError):
                self.verify()
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
