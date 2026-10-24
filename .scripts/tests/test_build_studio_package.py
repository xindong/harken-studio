import importlib.util
import json
import shutil
import tempfile
import unittest
from unittest import mock
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "release" / "build_studio_package.py"


def load_module():
    spec = importlib.util.spec_from_file_location("build_studio_package", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildStudioPackageTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        staging_output = self.module.release_evidence.package_output
        self.evidence = mock.patch.object(self.module, "release_evidence").start()
        self.evidence.package_output.side_effect = staging_output
        self.addCleanup(mock.patch.stopall)
        self.evidence.prepare.return_value = Path("release-evidence")
        self.evidence.finalize.side_effect = lambda directory, artifacts, payload: [
            {"sha256": self.module.sha256_file(path), "size": path.stat().st_size} for path in artifacts]
        (REPO_ROOT / ".temp").mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT / ".temp")
        self.root = Path(self.tmp.name)
        self.input_dir = self.root / "deployed"
        self.output_dir = self.root / "out"
        self.self_test_channel = "stable"
        self.verify_release_candidate = self.module.verify_release_candidate
        self.module.verify_release_candidate = lambda args, self_test: None
        def self_test(binary):
            schema = self.module.verify_schema_directory(binary.parent / "schema")
            return {
                "ok": True,
                "product": "HarkenStudio",
                "version": "1.4.0",
                "buildId": "run12.abcd1234",
                "channel": self.self_test_channel,
                "capabilitiesVersion": 2,
                "editorSchemaVersion": schema["trees"]["Editor"]["currentVersion"],
                "schemaArchiveOk": True,
                "schemaSnapshotHash": schema["snapshotHash"],
                "packSchemaVersion": schema["trees"]["Pack"]["currentVersion"],
                "gitCommit": "b" * 40,
                "gitDirty": False,
            }
        self.module.run_studio_self_test = self_test

    def tearDown(self):
        self.tmp.cleanup()

    def publish_schema(self, target):
        bundle = self.module.schema_bundle()
        for relative, content in bundle["installed"].items():
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)

    def publish_legal(self, binary_dir, legal_dir):
        names = self.module.collect_legal_texts.assemble("studio", legal_dir, "6.8.3")
        self.legal_paths = {(legal_dir / name).relative_to(self.input_dir).as_posix() for name in names}
        for name in ("LICENSE.md", "NOTICE.md"):
            relative = Path("HarkenSDK/Unity/Harken") / name
            destination = binary_dir / relative
            shutil.copyfile(REPO_ROOT / relative, destination)
            self.legal_paths.add(destination.relative_to(self.input_dir).as_posix())

    def make_input(self, with_binary=True, sdk_version="0.2.0"):
        (self.input_dir / "data").mkdir(parents=True)
        if with_binary:
            (self.input_dir / "HarkenStudio.exe").write_bytes(b"stub-binary")
        (self.input_dir / "data" / "asset.bin").write_bytes(b"payload" * 100)
        self.publish_schema(self.input_dir / "schema")
        unity = self.input_dir / "HarkenSDK" / "Unity" / "Harken"
        unity.mkdir(parents=True)
        (unity / "package.json").write_text(
            json.dumps({"name": "com.harken.core", "version": sdk_version}),
            encoding="utf-8",
        )
        self.publish_legal(self.input_dir, self.input_dir / "Legal")

    def run_build(self, extra_args=None):
        args = [
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "1.4.0",
            "--build-id", "run12.abcd1234",
        ] + (extra_args or [])
        return self.module.build_package(self.module.parse_args(args))

    def test_evidence_failure_prevents_archive_creation(self):
        self.make_input()
        self.evidence.prepare.side_effect = ValueError("missing sources")
        with self.assertRaisesRegex(ValueError, "missing sources"):
            self.run_build()
        self.assertEqual(list(self.output_dir.glob("*.zip")), [])

    def test_finalization_failure_removes_the_incomplete_package_and_allows_retry(self):
        self.make_input()
        finalize = self.evidence.finalize.side_effect
        self.evidence.finalize.side_effect = ValueError("payload changed")
        with self.assertRaisesRegex(ValueError, "payload changed"):
            self.run_build()
        self.assertEqual(list(self.output_dir.iterdir()), [])
        self.evidence.finalize.side_effect = finalize
        summary = self.run_build()
        self.assertTrue(Path(summary["package"]).is_file())

    def test_package_contains_payload_and_manifests(self):
        self.make_input()
        summary = self.run_build()

        package = Path(summary["package"])
        self.assertTrue(package.exists())
        self.evidence.finalize.assert_called_once()
        self.assertEqual(self.evidence.finalize.call_args.args[1][0].name, package.name)
        self.assertEqual(package.name, "1.4.0-run12.abcd1234.zip")
        schema_files = {
            path.relative_to(self.input_dir).as_posix()
            for path in (self.input_dir / "schema").rglob("*")
            if path.is_file()
        }
        self.assertEqual(summary["fileCount"], 3 + len(schema_files) + len(self.legal_paths))

        with zipfile.ZipFile(package) as archive:
            names = set(archive.namelist())
            self.assertIn("HarkenStudio.exe", names)
            self.assertIn("data/asset.bin", names)
            self.assertIn("package-manifest.json", names)
            self.assertIn("capabilities.json", names)
            archived_schema_files = {name for name in names if name.startswith("schema/")}
            self.assertEqual(archived_schema_files, schema_files)
            for relative in schema_files:
                self.assertEqual(archive.read(relative), (self.input_dir / relative).read_bytes())

            manifest_content = archive.read("package-manifest.json")
            reference = self.evidence.prepare.call_args.kwargs["payload_manifest"]
            self.assertEqual(reference["artifact"], package.name)
            self.assertEqual(reference["path"], "package-manifest.json")
            self.assertEqual(reference["sha256"], self.module.hashlib.sha256(manifest_content).hexdigest())
            manifest = json.loads(manifest_content)
            self.assertEqual(manifest["version"], "1.4.0")
            self.assertEqual(manifest["buildId"], "run12.abcd1234")
            self.assertEqual(manifest["gitCommit"], "b" * 40)
            self.assertFalse(manifest["gitDirty"])
            self.assertEqual(manifest["channel"], "stable")
            by_path = {f["path"]: f for f in manifest["files"]}
            self.assertEqual(set(by_path), schema_files | self.legal_paths | {
                    "HarkenStudio.exe",
                    "data/asset.bin",
                    "HarkenSDK/Unity/Harken/package.json",
                })
            self.assertEqual(by_path["HarkenStudio.exe"]["size"], len(b"stub-binary"))
            self.assertEqual(manifest["components"], [{
                "product": "HarkenSDK",
                "version": "0.2.0",
                "artifacts": {
                    "unity": "HarkenSDK/Unity/Harken",
                },
            }])

            capabilities = json.loads(archive.read("capabilities.json"))
            self.assertEqual(capabilities, {
                "studioVersion": "1.4.0",
                "buildId": "run12.abcd1234",
                "capabilitiesVersion": 2,
            })

            for name in ("package-manifest.json", "capabilities.json"):
                info = archive.getinfo(name)
                self.assertEqual(info.create_system, 3)
                self.assertEqual((info.external_attr >> 16) & 0o7777, 0o644)

    def test_manifest_hashes_match_payload(self):
        self.make_input()
        summary = self.run_build()

        with zipfile.ZipFile(summary["package"]) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            for record in manifest["files"]:
                source = self.input_dir / record["path"]
                self.assertEqual(
                    record["sha256"], self.module.sha256_file(source), record["path"]
                )

        self.assertEqual(
            summary["sha256"], self.module.sha256_file(Path(summary["package"]))
        )

    def test_missing_legal_material_prevents_package_creation(self):
        self.make_input()
        for relative in (
            "Legal/third-party/Qt6-THIRD-PARTY-NOTICES.txt",
            "HarkenSDK/Unity/Harken/LICENSE.md",
            "HarkenSDK/Unity/Harken/NOTICE.md",
        ):
            with self.subTest(path=relative):
                path = self.input_dir / relative
                content = path.read_bytes()
                path.unlink()
                with self.assertRaises(SystemExit):
                    self.run_build()
                self.assertFalse(self.output_dir.exists())
                path.write_bytes(content)

    def test_invalid_sdk_versions_are_rejected(self):
        self.make_input()
        plugin = self.input_dir / "HarkenSDK/Unity" / "Harken" / "package.json"
        for identity in (
            {},
            {"name": "com.harken.core", "version": ""},
            {"name": "com.harken.core", "version": "   "},
            {"name": "com.harken.core", "version": "0.2.0 "},
            {"name": "com.harken.core", "version": None},
            {"name": "com.harken.core", "version": 2},
        ):
            with self.subTest(identity=identity):
                plugin.write_text(json.dumps(identity), encoding="utf-8")
                with self.assertRaises(SystemExit):
                    self.run_build()

    def test_missing_unity_sdk_is_rejected(self):
        self.make_input()
        (self.input_dir / "HarkenSDK/Unity" / "Harken" / "package.json").unlink()
        with self.assertRaises(SystemExit):
            self.run_build()

    def test_macos_sdk_component_paths_are_recorded(self):
        app = self.input_dir / "HarkenStudio.app" / "Contents" / "MacOS"
        app.mkdir(parents=True)
        (app / "HarkenStudio").write_bytes(b"stub-binary")
        self.publish_schema(app / "schema")
        unity = app / "HarkenSDK" / "Unity" / "Harken"
        unity.mkdir(parents=True)
        (unity / "package.json").write_text(
            json.dumps({"version": "0.2.0"}), encoding="utf-8")
        self.publish_legal(app, app.parent / "Resources" / "Legal")
        summary = self.run_build()
        with zipfile.ZipFile(summary["package"]) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            artifacts = manifest["components"][0]["artifacts"]
            self.assertEqual(
                artifacts["unity"],
                "HarkenStudio.app/Contents/MacOS/HarkenSDK/Unity/Harken",
            )

    def test_missing_binary_is_rejected(self):
        self.make_input(with_binary=False)
        with self.assertRaises(SystemExit):
            self.run_build()

    def test_missing_schema_archive_is_rejected(self):
        self.make_input()
        shutil.rmtree(self.input_dir / "schema")
        with self.assertRaises(SystemExit):
            self.run_build()

    def test_tampered_schema_archive_is_rejected(self):
        self.make_input()
        manifest = self.input_dir / "schema" / "build.json"
        manifest.write_text("{}\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            self.run_build()

    def test_development_tip_changes_do_not_require_a_numbered_snapshot_update(self):
        self.make_input()
        source = self.root / "HarkenSchema"
        shutil.copytree(self.module.SCHEMA_SOURCE, source)
        self.module.SCHEMA_SOURCE = source
        path = source / "Editor/current/ProjectSerializedSchema.json"
        current = json.loads(path.read_text(encoding="utf-8"))
        for document in current["x-harken-documentTypes"]:
            document["schema"]["properties"]["harkenDocument"]["properties"]["futureMetadata"] = {}
        path.write_text(json.dumps(current), encoding="utf-8")
        generation = self.module.schema_archive.load_tree("Editor", source)["currentVersion"]
        relative = f"Editor/{generation}/ProjectSerializedSchema.json"
        numbered = source / relative
        numbered_before = numbered.read_bytes()
        with self.assertRaises(SystemExit):
            self.run_build()
        self.publish_schema(self.input_dir / "schema")
        self.self_test_channel = "dev"
        summary = self.run_build(["--channel", "dev"])
        self.assertEqual(numbered.read_bytes(), numbered_before)
        with zipfile.ZipFile(summary["package"]) as archive:
            installed = json.loads(archive.read(f"schema/{relative}"))
            for document in installed["x-harken-documentTypes"]:
                self.assertIn("futureMetadata", document["schema"]["properties"]["harkenDocument"]["properties"])
            self.assertFalse(any("current" in Path(name).parts for name in archive.namelist()))

    def test_reference_snapshot_cannot_be_swapped_with_another_generation(self):
        self.make_input()
        version = self.module.schema_archive.load_tree("Editor", self.module.SCHEMA_SOURCE)["currentVersion"]
        target = self.input_dir / f"schema/Editor/{version}/ProjectSerializedSchema.json"
        source = json.loads((self.module.SCHEMA_SOURCE / "Editor/current/ProjectSerializedSchema.json").read_text(encoding="utf-8"))
        obsolete = self.module.schema_archive.materialize_source("Editor", source, version + 1)
        target.write_text(json.dumps(obsolete), encoding="utf-8")
        with self.assertRaises(SystemExit):
            self.run_build()

    def test_unexpected_schema_directories_are_rejected(self):
        self.make_input()
        schema = json.loads((self.input_dir / "schema/build.json").read_text(encoding="utf-8"))
        editor_generation = schema["trees"]["Editor"]["currentVersion"]
        pack_generation = schema["trees"]["Pack"]["currentVersion"]
        for relative in ("Editor/current", f"Editor/{editor_generation + 1}",
                         f"Pack/{pack_generation + 1}", "independent"):
            with self.subTest(relative=relative):
                path = self.input_dir / "schema" / relative
                path.mkdir()
                with self.assertRaises(SystemExit):
                    self.run_build()
                path.rmdir()

    def test_existing_artifacts_cannot_be_overwritten(self):
        self.make_input()
        summary = self.run_build()
        package = Path(summary["package"])
        original = package.read_bytes()
        (self.input_dir / "data/asset.bin").write_bytes(b"changed payload")
        with self.assertRaises(SystemExit):
            self.run_build()
        self.assertEqual(package.read_bytes(), original)

    def test_formal_packages_reject_unprepared_release_contracts(self):
        self.make_input()
        self.module.verify_release_candidate = self.verify_release_candidate
        for error in (
            "Editor current has not been synchronized to its release generation",
            "Published Pack generation cannot be removed",
        ):
            with self.subTest(error=error), mock.patch.object(
                self.module.studio_schema_release_audit,
                "verify_current_release",
                return_value={"errors": [error]},
            ):
                with self.assertRaisesRegex(SystemExit, error):
                    self.run_build()
                self.assertFalse(self.output_dir.exists())

    def test_formal_packages_require_clean_binary_from_head(self):
        self.make_input()
        self.module.verify_release_candidate = self.verify_release_candidate
        audit = self.module.studio_schema_release_audit
        self_test = self.module.run_studio_self_test
        with (
            mock.patch.object(audit, "verify_current_release", return_value={"errors": []}),
            mock.patch.object(audit, "run_git", return_value=mock.Mock(stdout="b" * 40, returncode=0)),
        ):
            for channel in ("stable", "beta"):
                self.self_test_channel = channel
                for field, value in (("gitDirty", True), ("gitCommit", "a" * 40)):
                    with self.subTest(channel=channel, field=field):
                        self.module.run_studio_self_test = lambda binary: {
                            **self_test(binary), field: value,
                        }
                        with self.assertRaises(SystemExit):
                            self.run_build(["--channel", channel])
                        self.assertFalse(self.output_dir.exists())

    def test_formal_packages_reject_source_changes_after_build(self):
        self.make_input()
        self.module.verify_release_candidate = self.verify_release_candidate
        audit = self.module.studio_schema_release_audit
        with (
            mock.patch.object(audit, "verify_current_release", return_value={"errors": []}),
            mock.patch.object(audit, "run_git", return_value=mock.Mock(stdout="b" * 40, returncode=1)),
        ):
            with self.assertRaises(SystemExit):
                self.run_build()
        self.assertFalse(self.output_dir.exists())

    def test_development_packages_do_not_run_formal_release_checks(self):
        self.make_input()
        self.module.verify_release_candidate = self.verify_release_candidate
        audit = self.module.studio_schema_release_audit
        with (
            mock.patch.object(audit, "verify_current_release", side_effect=AssertionError("formal contracts required")),
            mock.patch.object(audit, "require_current_reader", side_effect=AssertionError("formal reader required")),
        ):
            for channel in ("dev", "nightly"):
                with self.subTest(channel=channel):
                    self.self_test_channel = channel
                    self.output_dir = self.root / channel
                    summary = self.run_build(["--channel", channel])
                    self.assertTrue(Path(summary["package"]).is_file())

    def test_formal_packages_verify_contracts_and_identity_without_a_receipt(self):
        self.make_input()
        self.module.verify_release_candidate = self.verify_release_candidate
        audit = self.module.studio_schema_release_audit
        with (
            mock.patch.object(audit, "verify_current_release", return_value={"errors": []}),
            mock.patch.object(audit, "run_git", return_value=mock.Mock(stdout="b" * 40, returncode=0)),
            mock.patch.object(audit, "verify_release_readers", side_effect=AssertionError("development reader validation requested")),
            mock.patch.object(audit.subprocess, "run", side_effect=AssertionError("external build or test requested")),
        ):
            for channel in ("stable", "beta"):
                with self.subTest(channel=channel):
                    self.self_test_channel = channel
                    self.output_dir = self.root / channel
                    summary = self.run_build(["--channel", channel])
                    with zipfile.ZipFile(summary["package"]) as archive:
                        manifest = json.loads(archive.read("package-manifest.json"))
                    self.assertEqual(manifest["gitCommit"], "b" * 40)
                    self.assertFalse(manifest["gitDirty"])
                    self.assertEqual(manifest["channel"], channel)

    def test_beta_channel_is_recorded(self):
        self.make_input()
        self.self_test_channel = "beta"
        summary = self.run_build(["--channel", "beta"])
        with zipfile.ZipFile(summary["package"]) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertEqual(manifest["channel"], "beta")

    def test_unknown_build_channel_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_build(["--channel", "preview"])

    def test_old_self_test_protocol_is_rejected(self):
        self.make_input()
        report = self.module.run_studio_self_test(self.input_dir / "HarkenStudio.exe")
        current = report.pop("editorSchemaVersion")
        report["capabilitiesVersion"] = 1
        report["projectSchema"] = {
            "current": current,
            "openableAsIs": [current],
            "upgradableFrom": [1],
        }
        self.module.run_studio_self_test = lambda binary: report
        with self.assertRaises(SystemExit):
            self.run_build()
        self.assertFalse(self.output_dir.exists())

    def test_binary_archive_report_must_match_packaged_contract(self):
        self.make_input()
        self_test = self.module.run_studio_self_test
        report = self_test(self.input_dir / "HarkenStudio.exe")
        editor_generation = report["editorSchemaVersion"]
        pack_generation = report["packSchemaVersion"]
        for field, value in (
            ("product", "HarkenHub"),
            ("product", None),
            ("capabilitiesVersion", 1),
            ("capabilitiesVersion", 2.0),
            ("capabilitiesVersion", "2"),
            ("capabilitiesVersion", None),
            ("editorSchemaVersion", editor_generation + 1),
            ("editorSchemaVersion", 3.0),
            ("editorSchemaVersion", "3"),
            ("editorSchemaVersion", None),
            ("schemaSnapshotHash", "0" * 64),
            ("packSchemaVersion", pack_generation + 1),
            ("packSchemaVersion", True),
            ("schemaArchiveOk", False),
        ):
            with self.subTest(field=field, value=value):
                self.module.run_studio_self_test = lambda binary: {
                    **self_test(binary), field: value,
                }
                with self.assertRaises(SystemExit):
                    self.run_build()
                self.assertFalse(self.output_dir.exists())

    def test_schema_capability_overrides_are_not_accepted(self):
        for option in ("--readable-schema-min", "--readable-schema-max", "--writable-schema"):
            with self.subTest(option=option):
                with self.assertRaises(SystemExit) as error:
                    self.run_build([option, "2"])
                self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
