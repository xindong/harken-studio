from __future__ import annotations

import copy
import hashlib
import json
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "schema_archive.py"
MODULE = runpy.run_path(str(SCRIPT))
SCHEMA_ROOT = REPO_ROOT / "HarkenSchema"
GENERATED_NAMES = {
    "ProjectSchemaGenerated.h",
    "ProjectSchemaValidator.generated.h",
    "ProjectSchemaValidator.generated.cpp",
    "SchemaBuildGenerated.h",
}
EDITOR_VERSION = MODULE["load_tree"]("Editor")["currentVersion"]
PACK_VERSION = MODULE["load_tree"]("Pack")["currentVersion"]


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(MODULE["canonical_bytes"](value))


def copy_schema(directory: str) -> Path:
    root = Path(directory) / "HarkenSchema"
    shutil.copytree(SCHEMA_ROOT, root)
    return root


class SchemaArchiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bundle = MODULE["build_bundle"]()

    def test_current_sources_are_complete_without_product_integers(self) -> None:
        editor = MODULE["load_tree"]("Editor")
        current = editor["currentSource"]
        self.assertNotIn("projectSchemaVersion", current)
        self.assertEqual(current["$id"], "harken://schema/Editor/current")
        self.assertIn("$defs", current)
        self.assertIn("x-harken-projectRules", current)
        for item in current["x-harken-documentTypes"]:
            slot = item["schema"]["properties"]["harkenDocument"]["properties"]["schemaVersion"]
            self.assertEqual(slot, {"type": "integer"})
        self.assertEqual(editor["current"]["projectSchemaVersion"], EDITOR_VERSION)
        pack = MODULE["load_tree"]("Pack")
        self.assertNotIn("formatVersion", pack["currentSource"]["x-harken-format"])
        self.assertNotIn("schema_version", pack["currentSource"].get("properties", {}))
        self.assertNotIn("schema_version", pack["current"].get("properties", {}))
        self.assertEqual(pack["current"]["x-harken-format"]["formatVersion"], PACK_VERSION)
        next_pack = MODULE["materialize_source"]("Pack", pack["currentSource"], PACK_VERSION + 1)
        self.assertEqual(pack["currentSource"]["$defs"]["schemaVersion"], {"type": "integer"})
        self.assertEqual(next_pack["$defs"]["schemaVersion"]["const"],
                         next_pack["x-harken-format"]["formatVersion"])
        for branch in next_pack["oneOf"]:
            document = next_pack["$defs"][branch["$ref"].removeprefix("#/$defs/")] if "$ref" in branch else branch
            self.assertEqual(document["properties"]["schemaVersion"], {"$ref": "#/$defs/schemaVersion"})
            self.assertIn("schemaVersion", document["required"])

    def test_current_rejects_embedded_generations(self) -> None:
        for tree, top, slot in (
            ("Editor", "projectSchemaVersion", "schemaVersion"),
            ("Pack", "formatVersion", "schema_version"),
        ):
            current = MODULE["load_tree"](tree)["currentSource"]
            invalid = copy.deepcopy(current)
            if tree == "Editor":
                invalid[top] = 2
            else:
                invalid["x-harken-format"][top] = 1
            with self.assertRaises(ValueError):
                MODULE["materialize_source"](tree, invalid, 2)
            invalid = copy.deepcopy(current)
            if tree == "Editor":
                invalid["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"][slot]["const"] = 2
            else:
                invalid.setdefault("properties", {})[slot] = {"type": "integer", "const": 1}
            with self.assertRaises(ValueError):
                MODULE["materialize_source"](tree, invalid, 2)

        pack = MODULE["load_tree"]("Pack")
        invalid = copy.deepcopy(pack["currentSource"])
        invalid["$defs"]["schemaVersion"]["const"] = PACK_VERSION
        with self.assertRaises(ValueError):
            MODULE["materialize_source"]("Pack", invalid, PACK_VERSION)
        invalid = copy.deepcopy(pack["current"])
        invalid["$defs"]["schemaVersion"]["const"] = PACK_VERSION + 1
        with self.assertRaises(ValueError):
            MODULE["validate_pack_source"](invalid)

    def test_schema_deprecated_marker_requires_boolean(self) -> None:
        MODULE["validate_schema_language"]({"type": "string", "deprecated": True}, "field")
        with self.assertRaises(ValueError):
            MODULE["validate_schema_language"]({"type": "string", "deprecated": "true"}, "field")

    def test_field_groups_require_model_and_explicit_inheritance_values(self) -> None:
        loaded = MODULE["load_tree"]("Editor")
        source = loaded["current"]
        for document in source["x-harken-documentTypes"]:
            header = document["schema"]["properties"]["harkenDocument"]
            self.assertEqual(header["properties"]["propertyModel"]["const"], "fieldGroupsV1")
            self.assertIn("propertyModel", header["required"])
        state = source["$defs"]["fieldGroupSharedMainOutput"]
        self.assertEqual(state["if"]["properties"]["inherit"], {"const": False})
        self.assertEqual(state["then"]["required"], ["values"])
        for invalid in ({"if": {}}, {"then": {}}, {"if": [], "then": {}}):
            with self.assertRaises(ValueError):
                MODULE["validate_schema_language"](invalid, "state")

    def test_manifest_cannot_hide_invalid_support_or_another_version_authority(self) -> None:
        original = MODULE["load_tree"]("Editor")["manifest"]
        invalid_fields = (
            {"manifestVersion": True}, {"tip": "3"}, {"published": []},
            {"published": [2, 1]}, {"published": [1, 1, 2]}, {"published": [True, 2]},
            {"supportedHistorical": [2]}, {"supportedHistorical": [EDITOR_VERSION]},
            {"currentVersion": 2}, {"rootHash": "0" * 64},
        )
        for change in invalid_fields:
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    MODULE["validate_tree_manifest"]("Editor", {**original, **change})
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            (root / "Editor" / str(EDITOR_VERSION + 1)).mkdir()
            with self.assertRaises(ValueError):
                MODULE["build_bundle"](root)

    def test_published_directory_must_match_its_complete_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            current = MODULE["load_tree"]("Editor", root)["currentSource"]
            path = root / "Editor" / str(EDITOR_VERSION) / "ProjectSerializedSchema.json"
            write_json(path, MODULE["materialize_source"]("Editor", current, EDITOR_VERSION + 1))
            with self.assertRaises(ValueError):
                MODULE["build_bundle"](root)

    def test_current_can_change_without_bumping_generation_or_published_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            before = MODULE["build_bundle"](root)
            published = root / "Editor" / str(EDITOR_VERSION) / "ProjectSerializedSchema.json"
            published_bytes = published.read_bytes()
            current_path = root / "Editor" / "current" / "ProjectSerializedSchema.json"
            current = read_json(current_path)
            current["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["savedStudioVersion"]["minLength"] = 2
            write_json(current_path, current)
            after = MODULE["build_bundle"](root)
            self.assertEqual(after["manifest"]["trees"], before["manifest"]["trees"])
            self.assertNotEqual(after["manifest"]["snapshotHash"], before["manifest"]["snapshotHash"])
            self.assertNotEqual(after["generated"]["ProjectSchemaValidator.generated.cpp"], before["generated"]["ProjectSchemaValidator.generated.cpp"])
            self.assertEqual(published.read_bytes(), published_bytes)
            self.assertNotEqual(after["installed"][f"Editor/{EDITOR_VERSION}/ProjectSerializedSchema.json"], MODULE["canonical_bytes"](read_json(published)))
            write_json(published, after["trees"]["Editor"]["current"])
            synchronized = MODULE["build_bundle"](root)
            self.assertEqual(after["manifest"], synchronized["manifest"])
            self.assertEqual(after["generated"], synchronized["generated"])

    def test_historical_validation_is_explicit_and_reference_files_do_not_imply_support(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            path = root / "Editor" / "manifest.json"
            manifest = read_json(path)
            historical_version = EDITOR_VERSION
            current_version = historical_version + 1
            manifest["published"].append(current_version)
            manifest["supportedHistorical"] = []
            current = read_json(root / "Editor/current/ProjectSerializedSchema.json")
            write_json(root / f"Editor/{current_version}/ProjectSerializedSchema.json", MODULE["materialize_source"]("Editor", current, current_version))
            write_json(path, manifest)
            bundle = MODULE["build_bundle"](root)
            self.assertEqual([source["projectSchemaVersion"] for source in MODULE["read_project_sources"](root)], [current_version])
            self.assertIn(f"Editor/{historical_version}/ProjectSerializedSchema.json", bundle["installed"])
            generated = bundle["generated"]["ProjectSchemaValidator.generated.cpp"].decode("ascii")
            self.assertNotIn(f"validateProjectV{historical_version}", generated)
            self.assertIn(f"validateProjectV{current_version}", generated)
            self.assertIn("kProjectSchemaVersions[] = {" + str(current_version) + "}", bundle["generated"]["ProjectSchemaGenerated.h"].decode("ascii"))
            historical_path = root / "Editor" / str(historical_version) / "ProjectSerializedSchema.json"
            historical = read_json(historical_path)
            historical["$defs"]["uuid"]["maxLength"] = 36
            write_json(historical_path, historical)
            revised = MODULE["build_bundle"](root)
            self.assertEqual(bundle["generated"]["ProjectSchemaValidator.generated.cpp"], revised["generated"]["ProjectSchemaValidator.generated.cpp"])
            self.assertNotEqual(bundle["manifest"]["snapshotHash"], revised["manifest"]["snapshotHash"])
            manifest["supportedHistorical"] = [historical_version]
            write_json(path, manifest)
            supported = MODULE["build_bundle"](root)
            self.assertIn(f"validateProjectV{historical_version}", supported["generated"]["ProjectSchemaValidator.generated.cpp"].decode("ascii"))

    def test_editor_and_pack_materialize_their_own_generation(self) -> None:
        for changed_tree in ("Editor", "Pack"):
            with self.subTest(tree=changed_tree), tempfile.TemporaryDirectory() as directory:
                root = copy_schema(directory)
                loaded = MODULE["load_tree"](changed_tree, root)
                current = loaded["currentVersion"]
                manifest = loaded["manifest"]
                manifest["published"].append(current + 1)
                manifest["supportedHistorical"].append(current)
                write_json(root / changed_tree / "manifest.json", manifest)
                name = MODULE["TREE_SCHEMA_NAMES"][changed_tree]
                write_json(root / changed_tree / str(current + 1) / name, MODULE["materialize_source"](changed_tree, loaded["currentSource"], current + 1))
                bundle = MODULE["build_bundle"](root)
                editor_current = MODULE["load_tree"]("Editor", root)["currentVersion"]
                pack_current = MODULE["load_tree"]("Pack", root)["currentVersion"]
                expected = {
                    "Editor": current + 1 if changed_tree == "Editor" else editor_current,
                    "Pack": current + 1 if changed_tree == "Pack" else pack_current,
                }
                for tree, version in expected.items():
                    self.assertEqual(bundle["manifest"]["trees"][tree]["currentVersion"], version)
                    installed = json.loads(bundle["installed"][f"{tree}/{version}/{MODULE['TREE_SCHEMA_NAMES'][tree]}"])
                    self.assertEqual(MODULE["source_version"](tree, installed), version)

    def test_snapshot_binds_effective_inputs_installation_and_generated_code(self) -> None:
        bundle = self.bundle
        manifest = bundle["manifest"]
        self.assertEqual(manifest["snapshotHash"], MODULE["snapshot_hash"](manifest))
        self.assertEqual(json.loads(bundle["installed"]["build.json"]), manifest)
        for field, files in (("files", bundle["installed"]), ("generated", bundle["generated"])):
            entries = manifest[field]
            self.assertEqual(len({entry["path"] for entry in entries}), len(entries))
            for entry in entries:
                data = files[entry["path"]]
                self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(entry["size"], len(data))
        self.assertEqual({entry["path"] for entry in manifest["files"]}, set(bundle["installed"]) - {"build.json"})
        self.assertEqual({entry["path"] for entry in manifest["generated"]}, GENERATED_NAMES - {"SchemaBuildGenerated.h"})
        input_paths = {entry["path"] for entry in manifest["inputs"]}
        expected_inputs = set()
        for tree in ("Editor", "Pack"):
            loaded = MODULE["load_tree"](tree)
            name = MODULE["TREE_SCHEMA_NAMES"][tree]
            expected_inputs.update({f"{tree}/manifest.json", f"{tree}/current/{name}"})
            expected_inputs.update(f"{tree}/{version}/{name}" for version in loaded["manifest"]["published"] if version != loaded["currentVersion"])
        self.assertEqual(input_paths, expected_inputs)
        self.assertIn(manifest["snapshotHash"], bundle["generated"]["SchemaBuildGenerated.h"].decode("ascii"))
        changed = copy.deepcopy(manifest)
        changed["generated"][0]["sha256"] = "0" * 64
        self.assertNotEqual(MODULE["snapshot_hash"](changed), manifest["snapshotHash"])

    def test_generation_is_reproducible_across_json_whitespace_and_line_endings(self) -> None:
        self.assertEqual(self.bundle, MODULE["build_bundle"]())
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            for path in root.rglob("*.json"):
                value = read_json(path)
                path.write_bytes(json.dumps(value, indent=4, sort_keys=False).replace("\n", "\r\n").encode("utf-8"))
            rebuilt = MODULE["build_bundle"](root)
            for field in ("manifest", "installed", "generated", "trees"):
                self.assertEqual(self.bundle[field], rebuilt[field])

    def test_publish_contains_only_integer_generations_and_replaces_stale_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "schema"
            stale = target / "Editor" / "current"
            stale.mkdir(parents=True)
            (stale / "stale.json").write_text("{}", encoding="utf-8")
            for version in (1, 2):
                write_json(target / "Editor" / str(version) / "ProjectSerializedSchema.json", {})
            MODULE["publish_bundle"](self.bundle, target)
            files = {path.relative_to(target).as_posix(): path.read_bytes() for path in target.rglob("*") if path.is_file()}
            self.assertEqual(files, MODULE["installed_files"](self.bundle))
            self.assertFalse(any(path.name == "current" for path in target.rglob("*")))
            self.assertEqual({path.name for path in (target / "Editor").iterdir()}, {str(v) for v in MODULE["load_tree"]("Editor")["manifest"]["published"]})
            self.assertEqual({path.name for path in (target / "Pack").iterdir()}, {str(v) for v in MODULE["load_tree"]("Pack")["manifest"]["published"]})
            source = copy_schema(directory)
            staged = MODULE["build_bundle"](source)
            with self.assertRaises(ValueError):
                MODULE["publish_bundle"](staged, source / "schema")
        for target in (SCHEMA_ROOT, SCHEMA_ROOT / "schema"):
            with self.assertRaises(ValueError):
                MODULE["publish_bundle"](self.bundle, target)

    def test_generated_files_are_checked_before_publish(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            generated = Path(directory) / "Core"
            MODULE["write_generated_code"](generated, self.bundle)
            self.assertEqual({path.name for path in generated.iterdir()}, GENERATED_NAMES)
            self.assertEqual(MODULE["check_generated"](generated, self.bundle), [])
            for name in GENERATED_NAMES:
                path = generated / name
                data = path.read_bytes()
                path.write_bytes(data + b" ")
                self.assertEqual(len(MODULE["check_generated"](generated, self.bundle)), 1)
                path.write_bytes(data)
            target = Path(directory) / "schema"
            target.mkdir()
            sentinel = target / "previous-package"
            sentinel.write_text("preserved", encoding="utf-8")
            (generated / "SchemaBuildGenerated.h").write_text("stale", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--check-generated", str(generated), "--publish", str(target)],
                cwd=REPO_ROOT, capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserved")
        with self.assertRaises(ValueError):
            MODULE["write_generated_code"](REPO_ROOT / "HarkenStudio" / "Source" / "Core" / "generated", self.bundle)

    def test_two_processes_can_publish_the_same_snapshot_to_one_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "schema"
            old = target / "versions" / "2"
            old.mkdir(parents=True)
            for index in range(50):
                (old / f"old-{index}.json").write_text("{}", encoding="utf-8")
            worker = (
                "import runpy,sys; from pathlib import Path; "
                "module=runpy.run_path(sys.argv[1]); bundle=module['build_bundle'](); "
                "print('ready',flush=True); sys.stdin.readline(); "
                "[module['publish_bundle'](bundle,Path(sys.argv[2])) for _ in range(12)]"
            )
            processes = [subprocess.Popen(
                [sys.executable, "-c", worker, str(SCRIPT), str(target)],
                cwd=REPO_ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            ) for _ in range(2)]
            try:
                for process in processes:
                    self.assertEqual(process.stdout.readline().strip(), "ready")
                for process in processes:
                    process.stdin.write("publish\n")
                    process.stdin.flush()
                for process in processes:
                    stdout, stderr = process.communicate(timeout=30)
                    self.assertEqual(process.returncode, 0, stdout + stderr)
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
            actual = {path.relative_to(target).as_posix(): path.read_bytes() for path in target.rglob("*") if path.is_file()}
            self.assertEqual(actual, self.bundle["installed"])
            self.assertEqual({path.name for path in target.iterdir()}, {"Editor", "Pack", "build.json"})
            self.assertFalse(any(path.name.startswith(".schema-publish-") for path in Path(directory).iterdir()))

    def test_staged_candidate_cli_uses_its_source_tree_for_generation_and_publish(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = copy_schema(directory)
            loaded = MODULE["load_tree"]("Editor", root)
            loaded["manifest"]["published"].append(EDITOR_VERSION + 1)
            loaded["manifest"]["supportedHistorical"].append(EDITOR_VERSION)
            write_json(root / "Editor" / "manifest.json", loaded["manifest"])
            write_json(root / "Editor" / str(EDITOR_VERSION + 1) / "ProjectSerializedSchema.json", MODULE["materialize_source"]("Editor", loaded["currentSource"], EDITOR_VERSION + 1))
            generated = Path(directory) / "Core"
            target = Path(directory) / "schema"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--schema-root", str(root), "--generate-code", str(generated), "--publish", str(target)],
                cwd=REPO_ROOT, capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = MODULE["build_bundle"](root)
            self.assertEqual(read_json(target / "build.json"), expected["manifest"])
            self.assertEqual(MODULE["check_generated"](generated, expected), [])
            self.assertFalse((SCHEMA_ROOT / "Editor" / str(EDITOR_VERSION + 1)).exists())

    def test_v3_extension_boundaries_are_preserved(self) -> None:
        for source in MODULE["read_project_sources"]():
            for item in source["x-harken-documentTypes"]:
                document = item["schema"]
                header = document["properties"]["harkenDocument"]
                self.assertFalse(document["additionalProperties"])
                self.assertTrue(header["additionalProperties"])
                self.assertIn("savedStudioVersion", header["required"])
                self.assertEqual(header["properties"]["savedStudioVersion"], {"minLength": 1, "type": "string"})
                content = document["properties"]["content"]
                if "$ref" in content:
                    content = source["$defs"][content["$ref"][len("#/$defs/"):]]
                self.assertFalse(content["additionalProperties"])
                self.assertEqual(content["properties"]["extensions"], {"additionalProperties": True, "type": "object"})

    def test_header_version_mismatch_and_patch_inputs_are_rejected(self) -> None:
        source = MODULE["load_source"]()
        source["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["schemaVersion"]["const"] = EDITOR_VERSION + 1
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"](source)
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"]({"schemaSourceVersion": 3, "documentTypePatches": []})

    def test_unknown_rules_and_unresolved_refs_fail_generation(self) -> None:
        source = MODULE["load_source"]()
        unknown = copy.deepcopy(source)
        unknown["x-harken-projectRules"].append({"rule": "notImplemented"})
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"](unknown)
        duplicate = copy.deepcopy(source)
        duplicate["x-harken-projectRules"].append(copy.deepcopy(source["x-harken-projectRules"][0]))
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"](duplicate)
        unresolved = copy.deepcopy(source)
        unresolved["x-harken-documentTypes"][0]["schema"] = {"$ref": "#/$defs/Missing"}
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"](unresolved)

    def test_unsupported_schema_keywords_and_pack_versions_fail_generation(self) -> None:
        source = MODULE["load_source"]()
        source["$defs"]["uuid"]["anyOf"] = []
        with self.assertRaises(ValueError):
            MODULE["validate_project_source"](source)
        pack = MODULE["read_pack_sources"]()[0]
        pack.setdefault("properties", {})["schema_version"] = {"type": "integer", "const": 6}
        with self.assertRaises(ValueError):
            MODULE["validate_pack_source"](pack)
        pack = MODULE["read_pack_sources"]()[0]
        pack["$defs"]["guid"]["anyOf"] = []
        with self.assertRaises(ValueError):
            MODULE["validate_pack_source"](pack)

    def test_one_of_requires_schema_alternatives(self) -> None:
        schema = {"oneOf": [{"type": "object", "required": ["platforms"]},
                            {"type": "object", "required": ["platformId", "packs"]}]}
        MODULE["validate_schema_language"](schema, "Pack")
        for invalid in ([], {}, [None]):
            with self.assertRaises(ValueError):
                MODULE["validate_schema_language"]({"oneOf": invalid}, "Pack")

if __name__ == "__main__":
    unittest.main()
