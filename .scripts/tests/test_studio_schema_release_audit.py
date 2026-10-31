from __future__ import annotations

import copy
import importlib.util
import io
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from contextlib import contextmanager, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("studio_schema_release_audit", REPO_ROOT / ".scripts/studio_schema_release_audit.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def documents(root: Path) -> dict:
    return {
        "root": (root / "Fixture.hsp", {"harkenDocument": {"id": "root", "type": "project.root", "studioVersion": "1.0", "packSchemaVersion": 1},
            "content": {"metadata": {"name": "Original"},
                        "extensions": {"future": {"flag": True, "integer": 9007199254740993}}}}),
        "action": (root / "Actions/Trigger.vp", {"harkenDocument": {"id": "action", "type": "project.action"},
            "content": {"name": "Trigger", "type": "Action", "extensions": {"future": [True, None, {}, []]}}}),
        "master": (root / "Busses/Master.vp", {"harkenDocument": {"id": "master", "type": "project.bus"},
            "content": {"name": "Master", "parentRef": {"kind": "root"}, "extensions": {}}}),
    }


def edited_documents(before: dict, pack_schema_version: int = 2) -> tuple[dict, dict]:
    after = copy.deepcopy(before)
    edits = []
    for identity, (_, document) in after.items():
        header = document["harkenDocument"]
        for field in ("studioVersion", "savedStudioVersion"):
            if field in header:
                header[field] = "1.1"
        if identity == "root" and "packSchemaVersion" in header:
            header["packSchemaVersion"] = pack_schema_version
        if identity == "master":
            continue
        path = "/content/metadata/name" if identity == "root" else "/content/name"
        parent, key = MODULE.pointer_parent(document, path)
        original = parent[key]
        parent[key] += "_schema_edit"
        edits.append({"documentId": identity, "jsonPath": path, "before": original, "after": parent[key]})
    return after, {"version": "1.1", "packSchemaVersion": pack_schema_version, "edits": edits}


def write_documents(values: dict) -> None:
    for path, document in values.values():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document), encoding="utf-8")


class SchemaCompatibilityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.editor = MODULE.schema_archive.load_tree("Editor")["current"]
        self.pack = MODULE.schema_archive.load_tree("Pack")["current"]

    def test_unconstrained_metadata_addition_is_only_a_candidate(self) -> None:
        candidate = copy.deepcopy(self.editor)
        candidate["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["futureNote"] = {}
        self.assertEqual(MODULE.same_generation_errors("Editor", self.editor, candidate), [])
        with patch.object(MODULE, "published_releases", return_value={}):
            with self.assertRaises(ValueError):
                MODULE.verify_release_readers(Path("missing"), {})

    def test_typed_optional_metadata_still_narrows_old_arbitrary_values(self) -> None:
        for field in ({"type": "string"}, {"const": True}, {"enum": [1, 2]}):
            candidate = copy.deepcopy(self.editor)
            candidate["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["futureNote"] = field
            self.assertTrue(MODULE.same_generation_errors("Editor", self.editor, candidate))

    def test_optional_behavior_is_not_metadata(self) -> None:
        candidate = copy.deepcopy(self.editor)
        candidate["x-harken-documentTypes"][0]["schema"]["properties"]["content"]["properties"]["futureBehavior"] = {}
        self.assertTrue(MODULE.same_generation_errors("Editor", self.editor, candidate))

    def test_existing_contract_semantics_and_required_fields_cannot_change(self) -> None:
        candidate = copy.deepcopy(self.editor)
        candidate["x-harken-documentTypes"][0]["schema"]["properties"]["content"]["required"].remove("name")
        self.assertTrue(MODULE.same_generation_errors("Editor", self.editor, candidate))
        candidate = copy.deepcopy(self.editor)
        candidate["x-harken-projectRules"] = []
        self.assertTrue(MODULE.same_generation_errors("Editor", self.editor, candidate))

    def test_pack_tracking_changes_reuse_generation_but_behavior_does_not(self) -> None:
        candidate = copy.deepcopy(self.pack)
        for field in ("readers", "writers", "validators"):
            candidate["x-harken-format"][field] = ["MovedSource"]
        self.assertEqual(MODULE.same_generation_errors("Pack", self.pack, candidate), [])
        candidate["$defs"]["identity"]["properties"]["future"] = {}
        self.assertTrue(MODULE.same_generation_errors("Pack", self.pack, candidate))
        candidate = copy.deepcopy(self.pack)
        candidate["x-harken-format"]["versionLocator"] = "other"
        self.assertTrue(MODULE.same_generation_errors("Pack", self.pack, candidate))

    def test_fixed_normal_edits_preserve_raw_content(self) -> None:
        for historical in (False, True):
            with self.subTest(historical=historical):
                before = documents(Path("Fixture"))
                if historical:
                    before["root"][1]["harkenDocument"].pop("packSchemaVersion")
                    before["root"][1]["harkenDocument"].pop("studioVersion")
                    before["root"][1]["content"]["metadata"]["modifiedDate"] = "2026-09-07T00:00:00Z"
                    for _, document in before.values():
                        document["harkenDocument"]["savedStudioVersion"] = "1.0"
                after, report = edited_documents(before)
                if historical:
                    after["root"][1]["content"]["metadata"]["modifiedDate"] = "2026-10-01T00:00:00Z"
                MODULE.verify_edit(before, after, report)
                for identity, key, value in (("root", "savedStudioVersion" if historical else "studioVersion", "1.0"),
                                              ("root", "packSchemaVersion", 1),
                                              ("action", "savedStudioVersion", "1.0"),
                                              ("master", "packSchemaVersion", 2)):
                    with self.subTest(document=identity, field=key):
                        damaged = copy.deepcopy(after)
                        damaged[identity][1]["harkenDocument"][key] = value
                        with self.assertRaises(ValueError):
                            MODULE.verify_edit(before, damaged, report)

    def test_action_rename_revision_and_history_are_checked_exactly(self) -> None:
        self.check_action_rename_history(legacy=True)

    def test_guid_action_rename_revision_and_history_are_checked_exactly(self) -> None:
        self.check_action_rename_history(legacy=False)

    def check_action_rename_history(self, legacy: bool) -> None:
        before = documents(Path("Fixture"))
        before["action"][1]["content"].update({
            "guid": "action", "nameBindingRevision": 1})
        created = {"name": "Trigger", "objectGuid": "action",
                   "nameBindingRevision": 1, "event": "created"}
        if legacy:
            before["action"][1]["content"]["runtimeStableId"] = 5
            created["runtimeStableId"] = "5"
        before["root"][1]["content"]["actionPublishedNameHistory"] = [created]
        after, report = edited_documents(before)
        after["action"][1]["content"]["nameBindingRevision"] = 2
        renamed = {"name": "Trigger_schema_edit", "objectGuid": "action",
                   "nameBindingRevision": 2, "event": "renamed_from", "priorName": "Trigger"}
        if legacy:
            renamed["runtimeStableId"] = "5"
        after["root"][1]["content"]["actionPublishedNameHistory"].append(renamed)
        MODULE.verify_edit(before, after, report)
        for history in ([], [created], [renamed], [created, renamed, renamed],
                        [{**created, "name": "Changed"}, renamed]):
            with self.subTest(history=history):
                damaged = copy.deepcopy(after)
                damaged["root"][1]["content"]["actionPublishedNameHistory"] = history
                with self.assertRaises(ValueError):
                    MODULE.verify_edit(before, damaged, report)
        for key, value in (("name", "Wrong"), ("objectGuid", "other"),
                           ("runtimeStableId", "6"), ("nameBindingRevision", 3),
                           ("event", "created"), ("priorName", "Wrong")):
            with self.subTest(field=key):
                damaged = copy.deepcopy(after)
                damaged["root"][1]["content"]["actionPublishedNameHistory"][-1][key] = value
                with self.assertRaises(ValueError):
                    MODULE.verify_edit(before, damaged, report)
        for revision in (1, 3, True):
            with self.subTest(revision=revision):
                damaged = copy.deepcopy(after)
                damaged["action"][1]["content"]["nameBindingRevision"] = revision
                with self.assertRaises(ValueError):
                    MODULE.verify_edit(before, damaged, report)

    def test_unknown_content_loss_and_boolean_to_number_change_are_rejected(self) -> None:
        before = documents(Path("Fixture"))
        for replacement in ({}, {"future": {"flag": 1, "integer": 9007199254740993}}):
            after, report = edited_documents(before)
            after["root"][1]["content"]["extensions"] = replacement
            with self.assertRaises(ValueError):
                MODULE.verify_edit(before, after, report)

    def test_equivalent_json_numbers_survive_normal_serialization(self) -> None:
        before = documents(Path("Fixture"))
        before["root"][1]["content"]["extensions"]["future"]["number"] = 1.0
        after, report = edited_documents(before)
        after["root"][1]["content"]["extensions"]["future"]["number"] = 1
        MODULE.verify_edit(before, after, report)

    def test_large_integer_rounding_is_rejected(self) -> None:
        before = documents(Path("Fixture"))
        after, report = edited_documents(before)
        after["root"][1]["content"]["extensions"]["future"]["integer"] = float(9007199254740993)
        with self.assertRaises(ValueError):
            MODULE.verify_edit(before, after, report)

    def test_derived_paths_and_attenuation_name_are_checked_exactly(self) -> None:
        before = documents(Path("Fixture"))
        content = before["action"][1]["content"]
        content.update({"parentRef": {"kind": "item", "id": "master"}, "objectPath": "\\Master\\Trigger"})
        before["master"][1]["content"]["objectPath"] = "\\Master"
        attenuation = copy.deepcopy(before["action"][1])
        attenuation["harkenDocument"].update({"id": "attenuation", "type": "project.attenuation"})
        attenuation["content"].update({"name": "Curve", "parentRef": {"kind": "root"}, "objectPath": "\\Curve",
                                         "components": {"attenuation": {"name": "Curve", "points": [1, 2]}}})
        before["attenuation"] = (Path("Fixture/Attenuations/Curve.vp"), attenuation)
        after, report = edited_documents(before)
        after["action"][1]["content"]["objectPath"] = "\\Master\\Trigger_schema_edit"
        after["attenuation"][1]["content"]["objectPath"] = "\\Curve_schema_edit"
        after["attenuation"][1]["content"]["components"]["attenuation"]["name"] = "Curve_schema_edit"
        MODULE.verify_edit(before, after, report)
        after["attenuation"][1]["content"]["components"]["attenuation"]["points"] = []
        with self.assertRaises(ValueError):
            MODULE.verify_edit(before, after, report)

    def test_historical_metadata_injection_respects_old_boundaries(self) -> None:
        historical = copy.deepcopy(self.editor)
        for item in historical["x-harken-documentTypes"]:
            item["schema"]["properties"]["harkenDocument"]["additionalProperties"] = False
            content = item["schema"]["properties"]["content"]
            if "$ref" in content:
                content = historical["$defs"][content["$ref"].removeprefix("#/$defs/")]
            content["properties"].pop("extensions", None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = documents(root)
            for _, document in original.values():
                document["content"].pop("extensions", None)
            write_documents(original)
            MODULE.inject_metadata(root / "Fixture.hsp", historical)
            result = MODULE.project_documents(root / "Fixture.hsp")
            for identity, (_, document) in result.items():
                self.assertNotIn("schemaCompatibilityMetadata", document["harkenDocument"])
                if identity != "root":
                    self.assertNotIn("extensions", document["content"])

    def test_probe_cannot_exempt_arbitrary_fields(self) -> None:
        before = documents(Path("Fixture"))
        after, report = edited_documents(before)
        report["edits"].append({"documentId": "root", "jsonPath": "/content/extensions"})
        with self.assertRaises(ValueError):
            MODULE.verify_edit(before, after, report)

    def test_every_eligible_document_must_be_edited(self) -> None:
        before = documents(Path("Fixture"))
        before["second"] = (Path("Fixture/Actions/Second.vp"), copy.deepcopy(before["action"][1]))
        before["second"][1]["harkenDocument"]["id"] = "second"
        after, report = edited_documents(before)
        report["edits"] = [edit for edit in report["edits"] if edit["documentId"] != "second"]
        with self.assertRaises(ValueError):
            MODULE.verify_edit(before, after, report)

    def test_probe_cannot_claim_arbitrary_rename_or_bad_timestamp(self) -> None:
        before = documents(Path("Fixture"))
        after, report = edited_documents(before)
        report["edits"][0]["after"] = "Discarded name"
        with self.assertRaises(ValueError):
            MODULE.verify_edit(before, after, report)
        for historical in (False, True):
            with self.subTest(historical=historical):
                if historical:
                    before["root"][1]["content"]["metadata"]["modifiedDate"] = "2026-09-07T00:00:00Z"
                after, report = edited_documents(before)
                after["root"][1]["content"]["metadata"]["modifiedDate"] = "invalid"
                with self.assertRaises(ValueError):
                    MODULE.verify_edit(before, after, report)

    def test_recovery_copies_are_preserved_and_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before = documents(root)
            write_documents(before)
            backup = root / ".harken/backup/Fixture.hsp"
            backup.parent.mkdir(parents=True)
            backup.write_text(json.dumps(before["root"][1]), encoding="utf-8")
            self.assertEqual(set(MODULE.project_documents(root / "Fixture.hsp")), set(before))
            self.assertTrue(backup.is_file())

    def test_candidate_metadata_names_are_exercised(self) -> None:
        for content_extensions in (False, True):
            with self.subTest(content_extensions=content_extensions):
                candidate = copy.deepcopy(self.editor)
                for entry in candidate["x-harken-documentTypes"]:
                    entry["schema"]["properties"]["harkenDocument"]["properties"]["futureNote"] = {}
                    content = entry["schema"]["properties"]["content"]
                    if "$ref" in content:
                        content = candidate["$defs"][content["$ref"].removeprefix("#/$defs/")]
                    if content_extensions:
                        content["properties"]["extensions"] = {
                            "type": "object", "additionalProperties": True,
                            "properties": {"futureExtension": {}}}
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    original = documents(root)
                    for _, document in original.values():
                        document["content"].pop("extensions", None)
                    write_documents(original)
                    MODULE.inject_metadata(root / "Fixture.hsp", candidate)
                    for _, value in MODULE.project_documents(root / "Fixture.hsp").values():
                        self.assertIn("futureNote", value["harkenDocument"])
                        if content_extensions:
                            self.assertIn("futureExtension", value["content"]["extensions"])
                        else:
                            self.assertNotIn("extensions", value["content"])
                        self.assertIn("schemaCompatibilityMetadata", value["harkenDocument"])


class GitHubPublicationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tag = "release-123-abcdef"
        self.package = {"id": 10, "name": "0.7.0-stable-windows-x64-build.zip", "state": "uploaded"}
        self.catalog_asset = {"id": 11, "name": "harken-manifest.json", "state": "uploaded"}
        self.release = {"tag_name": self.tag, "draft": False, "published_at": "2026-09-20T12:59:49Z",
                        "assets": [self.package, self.catalog_asset]}
        self.entry = {"version": "0.7.0", "releaseTag": self.tag,
                      "file": "releases/harken/windows-x64/0.7.0/" + self.package["name"]}
        self.catalog = {"schema_version": 1, "channels": {"windows-x64": [self.entry]}}

    def test_publisher_tag_is_discovered_from_catalog_and_uploaded_package(self) -> None:
        with patch.object(MODULE, "github_json", side_effect=[[[], [self.release]], self.catalog]) as query:
            self.assertEqual(MODULE.github_studio_release_tags({self.tag}), {self.tag})
        self.assertEqual(query.call_args_list[1].args,
                         (MODULE.REPO_ROOT, "repos/{owner}/{repo}/releases/assets/11", True))

    def test_hub_only_draft_and_unrelated_releases_are_not_baselines(self) -> None:
        for release in ({**self.release, "assets": [self.catalog_asset]},
                        {**self.release, "draft": True},
                        {**self.release, "published_at": None},
                        {**self.release, "tag_name": "other"}):
            with self.subTest(release=release):
                with patch.object(MODULE, "github_json", return_value=[[release]]) as query:
                    self.assertEqual(MODULE.github_studio_release_tags({self.tag}), set())
                    query.assert_called_once()

    def test_catalog_reference_must_belong_to_this_release_and_have_an_uploaded_asset(self) -> None:
        for entry in ({**self.entry, "releaseTag": "older-release"},
                      {**self.entry, "file": "releases/harken/missing.zip"}):
            with self.subTest(entry=entry):
                catalog = {**self.catalog, "channels": {"windows-x64": [entry]}}
                with patch.object(MODULE, "github_json", side_effect=[[[self.release]], catalog]):
                    self.assertEqual(MODULE.github_studio_release_tags({self.tag}), set())
        release = {**self.release, "assets": [{**self.package, "state": "new"}, self.catalog_asset]}
        with patch.object(MODULE, "github_json", return_value=[[release]]):
            self.assertEqual(MODULE.github_studio_release_tags({self.tag}), set())

    def test_invalid_catalog_fails_instead_of_dropping_the_baseline(self) -> None:
        for catalog in ([], {"schema_version": 2},
                        {**self.catalog, "channels": {"windows-x64": {}}},
                        {**self.catalog, "channels": {"windows-x64": [None]}},
                        {**self.catalog, "channels": {"windows-x64": [{"file": None}]}}):
            with self.subTest(catalog=catalog):
                with patch.object(MODULE, "github_json", side_effect=[[[self.release]], catalog]):
                    with self.assertRaises(ValueError):
                        MODULE.github_studio_release_tags({self.tag})

    def test_github_queries_paginate_releases_and_download_catalog_content(self) -> None:
        MODULE.github_json.cache_clear()
        self.addCleanup(MODULE.github_json.cache_clear)
        replies = [SimpleNamespace(stdout=json.dumps([[self.release]])),
                   SimpleNamespace(stdout=json.dumps(self.catalog))]
        with patch.object(MODULE.subprocess, "run", side_effect=replies) as query:
            self.assertEqual(MODULE.github_studio_release_tags({self.tag}), {self.tag})
            self.assertEqual(MODULE.github_studio_release_tags({self.tag}), {self.tag})
        self.assertEqual(query.call_count, 2)
        self.assertEqual(query.call_args_list[0].args[0],
                         ["gh", "api", "repos/{owner}/{repo}/releases?per_page=100", "--paginate", "--slurp"])
        self.assertEqual(query.call_args_list[1].args[0],
                         ["gh", "api", "repos/{owner}/{repo}/releases/assets/11", "--header", "Accept: application/octet-stream"])
        self.assertEqual(query.call_args_list[0].kwargs["cwd"], MODULE.REPO_ROOT)

    def test_github_failure_is_not_treated_as_an_empty_release_history(self) -> None:
        MODULE.github_json.cache_clear()
        self.addCleanup(MODULE.github_json.cache_clear)
        for error in (FileNotFoundError("gh"), subprocess.CalledProcessError(1, ["gh", "api"]),
                      subprocess.TimeoutExpired(["gh", "api"], 60)):
            with self.subTest(error=error):
                with patch.object(MODULE.subprocess, "run", side_effect=error):
                    with self.assertRaises(ValueError):
                        MODULE.github_studio_release_tags({self.tag})


class ReleaseLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        shutil.copytree(REPO_ROOT / "HarkenSchema", self.root / "HarkenSchema")
        (self.root / ".scripts").mkdir()
        shutil.copy2(REPO_ROOT / MODULE.RELEASE_SCOPE_CONFIG_PATH, self.root / MODULE.RELEASE_SCOPE_CONFIG_PATH)
        (self.root / "VERSION").write_text("1.0.0", encoding="utf-8")
        self.patch_root = patch.object(MODULE, "REPO_ROOT", self.root)
        self.patch_root.start()
        self.addCleanup(self.patch_root.stop)
        self.published_tags = set()
        publication = patch.object(MODULE, "github_studio_release_tags", side_effect=lambda tags: tags & self.published_tags)
        publication.start()
        self.addCleanup(publication.stop)
        MODULE.run_git(["init", "--quiet"])
        self.commit()

    def commit(self) -> None:
        MODULE.run_git(["add", "."])
        MODULE.run_git(["-c", "user.name=Schema Test", "-c", "user.email=schema@example.invalid", "commit", "--quiet", "--allow-empty", "-m", "fixture"])

    def synchronize_current(self) -> None:
        for tree in MODULE.TREE_NAMES:
            loaded = MODULE.schema_archive.load_tree(tree, self.root / "HarkenSchema")
            path = self.root / "HarkenSchema" / tree / str(loaded["currentVersion"]) / MODULE.SCHEMA_NAMES[tree]
            path.write_text(json.dumps(loaded["current"]), encoding="utf-8")

    def publish_tag(self, tag: str) -> None:
        self.synchronize_current()
        self.commit()
        MODULE.run_git(["tag", tag])
        self.published_tags.add(tag)

    def test_development_current_and_version_are_not_frozen(self) -> None:
        path = self.root / "HarkenSchema/Editor/current/ProjectSerializedSchema.json"
        source = json.loads(path.read_text(encoding="utf-8"))
        source["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["newNote"] = {}
        path.write_text(json.dumps(source), encoding="utf-8")
        self.assertEqual(MODULE.audit("HEAD", "WORKTREE")["errors"], [])
        (self.root / "VERSION").write_text("1.0.1", encoding="utf-8")
        self.assertEqual(MODULE.audit("HEAD", "WORKTREE")["errors"], [])

    def test_publication_identity_is_rebuilt_from_the_frozen_tag(self) -> None:
        self.publish_tag("studio-first")
        commit = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
        trees = MODULE.tree_summary(self.root / "HarkenSchema")
        (self.root / "VERSION").write_text("1.1.0", encoding="utf-8")
        self.commit()
        record = MODULE.published_releases()["studio-first"]
        self.assertEqual(record["product"], "HarkenStudio")
        self.assertEqual(record["version"], "1.0.0")
        self.assertEqual(record["gitCommit"], commit)
        self.assertEqual(record["trees"], trees)

    def test_formal_contract_checks_require_current_to_match_its_numbered_archive(self) -> None:
        self.synchronize_current()
        self.assertEqual(MODULE.verify_current_release()["errors"], [])
        path = self.root / "HarkenSchema/Editor/current/ProjectSerializedSchema.json"
        source = json.loads(path.read_text(encoding="utf-8"))
        source["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["newNote"] = {}
        path.write_text(json.dumps(source), encoding="utf-8")
        self.assertTrue(MODULE.verify_current_release()["errors"])
        self.assertEqual(MODULE.release_schema_errors(self.root / "HarkenSchema"), [])

    def test_current_tag_is_excluded_without_losing_prior_same_generation(self) -> None:
        self.publish_tag("first")
        self.publish_tag("second")
        self.assertEqual(set(MODULE.required_baselines(self.root / "HarkenSchema", "second")), {"first"})

    def test_unrelated_tag_does_not_become_a_release(self) -> None:
        MODULE.run_git(["tag", "internal-build"])
        self.assertEqual(MODULE.published_releases(), {})

    def test_publisher_tag_freezes_contracts_and_requires_its_reader(self) -> None:
        tag = "release-123-abcdef"
        self.publish_tag(tag)
        self.commit()
        MODULE.run_git(["tag", "hub-only"])
        with patch.object(MODULE, "github_studio_release_tags", return_value={tag}):
            self.assertEqual(set(MODULE.published_releases()), {tag})
            self.assertEqual(set(MODULE.required_baselines(self.root / "HarkenSchema", "studio-first")), {tag})
            self.assertEqual(MODULE.release_schema_errors(self.root / "HarkenSchema"), [])
            with self.assertRaisesRegex(ValueError, re.escape(tag)):
                MODULE.verify_release_readers(Path("missing"), {})
            version = MODULE.schema_archive.load_tree("Editor", self.root / "HarkenSchema")["currentVersion"]
            path = self.root / f"HarkenSchema/Editor/{version}/ProjectSerializedSchema.json"
            source = json.loads(path.read_text(encoding="utf-8"))
            source["x-harken-documentTypes"][0]["schema"]["properties"]["content"]["required"].remove("name")
            path.write_text(json.dumps(source), encoding="utf-8")
            errors = MODULE.release_schema_errors(self.root / "HarkenSchema")
            self.assertTrue(any(tag in error for error in errors))
            version = MODULE.schema_archive.load_tree("Pack", self.root / "HarkenSchema")["currentVersion"]
            path = self.root / f"HarkenSchema/Pack/{version}/PackManifestSchema.json"
            source = json.loads(path.read_text(encoding="utf-8"))
            source["$defs"]["identity"]["properties"]["future"] = {}
            path.write_text(json.dumps(source), encoding="utf-8")
            errors = MODULE.release_schema_errors(self.root / "HarkenSchema")
            self.assertTrue(any(tag in error for error in errors))

    def test_verifying_a_published_commit_excludes_itself_but_not_prior_readers(self) -> None:
        first = "release-123-first"
        second = "release-124-second"
        self.publish_tag(first)
        self.publish_tag(second)
        with patch.object(MODULE, "github_studio_release_tags", return_value={first, second}):
            commit = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
            self.assertEqual(set(MODULE.required_baselines(self.root / "HarkenSchema", "studio-next", commit)), {first})
            (self.root / "VERSION").write_text("1.0.1", encoding="utf-8")
            self.commit()
            commit = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
            self.assertEqual(set(MODULE.required_baselines(self.root / "HarkenSchema", "studio-next", commit)), {first, second})

    def test_unrelated_tag_at_a_published_commit_is_not_a_baseline(self) -> None:
        self.publish_tag("studio-first")
        MODULE.run_git(["tag", "internal-build"])
        with patch.object(MODULE, "github_studio_release_tags", return_value={"studio-first"}) as discover:
            self.assertEqual(set(MODULE.published_releases()), {"studio-first"})
            discover.assert_called_once_with({"studio-first", "internal-build"})

    def test_same_generation_rejects_a_lost_published_metadata_property(self) -> None:
        path = self.root / "HarkenSchema/Editor/current/ProjectSerializedSchema.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        source = copy.deepcopy(original)
        source["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["publishedNote"] = {}
        path.write_text(json.dumps(source), encoding="utf-8")
        version = MODULE.schema_archive.load_tree("Editor", self.root / "HarkenSchema")["currentVersion"]
        contract = MODULE.schema_archive.materialize_source("Editor", source, version)
        (self.root / f"HarkenSchema/Editor/{version}/ProjectSerializedSchema.json").write_text(json.dumps(contract), encoding="utf-8")
        self.publish_tag("latest")
        path.write_text(json.dumps(original), encoding="utf-8")
        self.assertEqual(MODULE.release_schema_errors(self.root / "HarkenSchema"), [])
        contract = MODULE.schema_archive.materialize_source("Editor", original, version)
        (self.root / f"HarkenSchema/Editor/{version}/ProjectSerializedSchema.json").write_text(json.dumps(contract), encoding="utf-8")
        self.assertTrue(any("removed" in error for error in MODULE.release_schema_errors(self.root / "HarkenSchema")))

    def test_sdk_source_changes_are_not_a_generation_hash_lock(self) -> None:
        reader = self.root / "HarkenIntegration/Unity/Harken/Runtime/Sdk/PackCompanion.cs"
        reader.parent.mkdir(parents=True)
        reader.write_text("old reader", encoding="utf-8")
        self.publish_tag("sdk-baseline")
        reader.write_text("changed reader", encoding="utf-8")
        self.assertEqual(MODULE.release_schema_errors(self.root / "HarkenSchema"), [])

    def test_release_archive_can_return_to_a_main_with_newer_development(self) -> None:
        self.publish_tag("first")
        path = self.root / "HarkenSchema/Editor/current/ProjectSerializedSchema.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        released = copy.deepcopy(original)
        released["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["releasedNote"] = {}
        path.write_text(json.dumps(released), encoding="utf-8")
        self.publish_tag("second")
        release_commit = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
        version = MODULE.schema_archive.load_tree("Editor", self.root / "HarkenSchema")["currentVersion"]
        archive = self.root / f"HarkenSchema/Editor/{version}/ProjectSerializedSchema.json"
        release_archive = archive.read_bytes()
        MODULE.run_git(["reset", "--hard", "first"])
        original["x-harken-documentTypes"][0]["schema"]["properties"]["content"]["required"].remove("name")
        path.write_text(json.dumps(original), encoding="utf-8")
        reader = self.root / "HarkenIntegration/Unity/Harken/Runtime/Sdk/PackCompanion.cs"
        reader.parent.mkdir(parents=True)
        reader.write_text("new development", encoding="utf-8")
        self.commit()
        main_commit = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
        archive.write_bytes(release_archive)
        self.commit()
        self.assertNotEqual(MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip(), release_commit)
        self.assertEqual(MODULE.audit(main_commit, "HEAD")["errors"], [])
        self.assertTrue(MODULE.verify_current_release("HEAD")["errors"])

    def test_retired_generation_still_keeps_latest_published_contract(self) -> None:
        source_root = self.root / "HarkenSchema"
        path = source_root / "Editor/current/ProjectSerializedSchema.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        changed = copy.deepcopy(original)
        changed["x-harken-documentTypes"][0]["schema"]["properties"]["harkenDocument"]["properties"]["releasedNote"] = {}
        path.write_text(json.dumps(changed), encoding="utf-8")
        original_version = MODULE.schema_archive.load_tree("Editor", source_root)["currentVersion"]
        (source_root / f"Editor/{original_version}/ProjectSerializedSchema.json").write_text(
            json.dumps(MODULE.schema_archive.materialize_source("Editor", changed, original_version)), encoding="utf-8")
        self.publish_tag("retired")
        for tree in MODULE.TREE_NAMES:
            loaded = MODULE.schema_archive.load_tree(tree, source_root)
            version = loaded["currentVersion"] + 1
            loaded["manifest"]["published"].append(version)
            loaded["manifest"]["supportedHistorical"] = []
            (source_root / tree / "manifest.json").write_text(json.dumps(loaded["manifest"]), encoding="utf-8")
            target = source_root / tree / str(version) / MODULE.SCHEMA_NAMES[tree]
            target.parent.mkdir()
            target.write_text(json.dumps(MODULE.schema_archive.materialize_source(tree, loaded["currentSource"], version)), encoding="utf-8")
        (source_root / f"Editor/{original_version}/ProjectSerializedSchema.json").write_text(
            json.dumps(MODULE.schema_archive.materialize_source("Editor", original, original_version)), encoding="utf-8")
        self.assertEqual(MODULE.required_baselines(source_root), {})
        self.assertTrue(any("removed" in error for error in MODULE.release_schema_errors(source_root)))

    def test_published_generations_cannot_be_removed_after_advancing_both_trees(self) -> None:
        source_root = self.root / "HarkenSchema"
        self.publish_tag("published")
        for tree in MODULE.TREE_NAMES:
            loaded = MODULE.schema_archive.load_tree(tree, source_root)
            version = loaded["currentVersion"] + 1
            loaded["manifest"]["published"] = [version]
            loaded["manifest"]["supportedHistorical"] = []
            (source_root / tree / "manifest.json").write_text(json.dumps(loaded["manifest"]), encoding="utf-8")
            target = source_root / tree / str(version) / MODULE.SCHEMA_NAMES[tree]
            target.parent.mkdir()
            target.write_text(json.dumps(MODULE.schema_archive.materialize_source(tree, loaded["currentSource"], version)), encoding="utf-8")
            for published_version in loaded["published"]:
                shutil.rmtree(source_root / tree / str(published_version))
        self.assertEqual(MODULE.required_baselines(source_root), {})
        errors = MODULE.release_schema_errors(source_root)
        for tree in MODULE.TREE_NAMES:
            self.assertTrue(any(f"Published {tree} generation" in error and "cannot be removed" in error for error in errors))

    def test_formal_reader_must_match_committed_sources(self) -> None:
        head = MODULE.run_git(["rev-parse", "HEAD"]).stdout.strip()
        reader = {"gitCommit": head, "gitDirty": False}
        MODULE.require_current_reader({"currentReader": reader})
        for changed in ({**reader, "gitDirty": True}, {**reader, "gitCommit": "old"}):
            with self.assertRaises(ValueError):
                MODULE.require_current_reader({"currentReader": changed})
        (self.root / "VERSION").write_text("1.0.1", encoding="utf-8")
        with self.assertRaises(ValueError):
            MODULE.require_current_reader({"currentReader": reader})
        MODULE.run_git(["add", "VERSION"])
        with self.assertRaises(ValueError):
            MODULE.require_current_reader({"currentReader": reader})

    def test_changed_persistence_paths_are_reviewed_without_internal_format_scopes(self) -> None:
        editor_paths = ["HarkenStudio/Source/Core/ProjectSchemaValidator.cpp",
                        "HarkenStudio/Source/Core/MigrationManager.cpp",
                        "HarkenStudio/Source/Core/ProjectMigration.cpp",
                        "HarkenStudio/Source/Core/ProjectMigration.h",
                        "HarkenStudio/Source/Tooling/Harkenizer/Core/ProjectLoader.cpp"]
        pack_paths = ["HarkenIntegration/Unity/Harken/Runtime/Sdk/PackCompanion.cs",
                      "HarkenStudio/Source/Tooling/Harkenizer/Core/CanonicalPackSetExport.cpp",
                      "HarkenStudio/Source/Tooling/Harkenizer/Core/ProjectPackExporter.cpp"]
        with patch.object(MODULE, "changed_files", return_value=editor_paths + pack_paths):
            scopes = MODULE.review_scopes("HEAD", "WORKTREE")
        self.assertEqual({value["scope"] for value in scopes}, {"Editor", "Pack"})
        for tree, paths in (("Editor", editor_paths), ("Pack", pack_paths)):
            files = next(value["files"] for value in scopes if value["scope"] == tree)
            self.assertEqual(set(files), set(paths))


class BaselinePackageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bundle = MODULE.schema_archive.build_bundle()
        binary = self.root / "HarkenStudio.exe"
        binary.write_bytes(b"released reader")
        for name, value in self.bundle["installed"].items():
            path = self.root / "schema" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        self.manifest = {"product": "HarkenStudio", "gitCommit": "a" * 40, "gitDirty": False,
                         "version": "1.0", "buildId": "released", "files": []}
        self.protect_files()

    def protect_files(self) -> None:
        self.manifest["files"] = [{"path": path.relative_to(self.root).as_posix(), "size": path.stat().st_size,
                                   "sha256": MODULE.digest(path.read_bytes())}
                                  for path in self.root.rglob("*") if path.is_file() and path.name != "package-manifest.json"]
        (self.root / "package-manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def check(self) -> tuple:
        @contextmanager
        def original_root(_):
            yield REPO_ROOT / "HarkenSchema"
        with patch.object(MODULE, "run_git", return_value=SimpleNamespace(stdout="a" * 40)), \
                patch.object(MODULE, "schema_root_at", original_root):
            return MODULE.checked_baseline_package("release", self.root)

    def test_old_generated_snapshot_identity_is_not_recomputed_by_new_codegen(self) -> None:
        manifest = copy.deepcopy(self.bundle["manifest"])
        manifest["generated"][0]["sha256"] = "b" * 64
        manifest["snapshotHash"] = MODULE.schema_archive.snapshot_hash(manifest)
        (self.root / "schema/build.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.protect_files()
        _, package = self.check()
        self.assertEqual(package["_schemaManifest"]["snapshotHash"], manifest["snapshotHash"])

    def test_tampered_binary_is_rejected(self) -> None:
        (self.root / "HarkenStudio.exe").write_bytes(b"different reader")
        with self.assertRaises(ValueError):
            self.check()

    def test_dirty_or_different_source_commit_is_rejected(self) -> None:
        self.manifest["gitDirty"] = True
        self.protect_files()
        with self.assertRaises(ValueError):
            self.check()

    def test_unprotected_schema_content_is_rejected(self) -> None:
        (self.root / "schema/extra.json").write_text("{}", encoding="utf-8")
        with self.assertRaises(ValueError):
            self.check()

    def test_unsafe_package_paths_are_rejected(self) -> None:
        self.manifest["files"][0]["path"] = "../outside"
        (self.root / "package-manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        with self.assertRaises(ValueError):
            self.check()

    def test_duplicate_baseline_inputs_are_rejected(self) -> None:
        path = self.root / "readers.json"
        path.write_text(json.dumps({"release": str(self.root)}), encoding="utf-8")
        with self.assertRaises(ValueError):
            MODULE.parse_baseline_readers([f"release={self.root}"], path)


class ReaderGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "HarkenSchema"
        shutil.copytree(REPO_ROOT / "HarkenSchema", self.source)
        bundle = MODULE.schema_archive.build_bundle(self.source)
        self.pack_schema_version = bundle["manifest"]["trees"]["Pack"]["currentVersion"]
        self.binary = self.root / "current/HarkenStudio.exe"
        self.old_binary = self.root / "baseline/HarkenStudio.exe"
        self.pack_reader = self.root / "HarkenAbiFidelity.dll"
        self.pack_root = self.root / "pack"
        self.pack_root.mkdir()
        native_name = "HarkenCore.dll" if MODULE.os.name == "nt" else "libHarkenCore.dylib" \
            if MODULE.sys.platform == "darwin" else "libHarkenCore.so"
        for path in (self.binary, self.old_binary, self.pack_reader,
                     self.binary.parent / native_name, self.pack_root / "Init.bytes", self.pack_root / "Init.json",
                     self.pack_root / "Play.bytes", self.pack_root / "Play.json",
                     self.pack_root / "English/Play.bytes", self.pack_root / "English/Play.json"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        self.calls = []

    def probe(self, binary: Path, request: dict, work: Path, expected: dict, package: dict | None = None) -> dict:
        self.calls.append((binary, request["mode"]))
        edits = []
        if request["mode"] == "create":
            root = Path(request["outputDirectory"])
            write_documents(documents(root))
            project = root / "Fixture.hsp"
        else:
            project = Path(request["projectPath"])
            if request["mode"] == "edit":
                after, changed = edited_documents(MODULE.project_documents(project),
                                                  expected["trees"]["Pack"]["currentVersion"])
                write_documents(after)
                edits = changed["edits"]
        return {"buildId": "fixture", "version": "1.1", "gitCommit": "a" * 40, "gitDirty": False,
                "editorSchemaVersion": expected["trees"]["Editor"]["currentVersion"],
                "packSchemaVersion": expected["trees"]["Pack"]["currentVersion"],
                "schemaSnapshotHash": expected["snapshotHash"], "projectPath": str(project), "edits": edits}

    def verify(self, old_version: int | None = None, sdk_version: int | None = None,
               native_returncode: int = 0, native_output: str | None = None) -> tuple:
        if sdk_version is None:
            sdk_version = self.pack_schema_version
        expected = copy.deepcopy(MODULE.schema_archive.build_bundle(self.source)["manifest"])
        if old_version is not None:
            expected["trees"]["Editor"]["currentVersion"] = old_version
        baselines = {"baseline": self.old_binary.parent} if old_version is not None else {}
        package = {"_schemaManifest": expected, "buildId": "released"}
        if native_output is None:
            native_output = f"[PackCompanion] native publication PASS.\n[PackCompanion] schemaVersion {sdk_version}\n"
        native = SimpleNamespace(returncode=native_returncode, stdout=native_output, stderr="")
        with patch.object(MODULE, "required_baselines", return_value={tag: {} for tag in baselines}), \
                patch.object(MODULE, "release_schema_errors", return_value=[]), \
                patch.object(MODULE, "run_git", return_value=SimpleNamespace(returncode=0, stdout="a" * 40)), \
                patch.object(MODULE, "checked_baseline_package", return_value=(self.old_binary, package)), \
                patch.object(MODULE, "run_probe", side_effect=self.probe), \
                patch.object(MODULE.subprocess, "run", return_value=native) as native_run:
            report = MODULE.verify_release_readers(self.binary, baselines, self.pack_reader, self.pack_root, self.source)
            return report, native_run.call_args_list

    def test_old_same_generation_editor_and_current_companion_reader_execute(self) -> None:
        report, calls = self.verify(old_version=MODULE.schema_archive.load_tree("Editor", self.source)["currentVersion"])
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].args[0], ["dotnet", str(self.pack_reader), "--require-native",
                                          "--companion-root", str(self.pack_root)])
        self.assertIn((self.old_binary, "edit"), self.calls)
        self.assertEqual(set(report["pack"]["fixtureSha256"]), {
            "Init.bytes", "Init.json", "Play.bytes", "Play.json", "English/Play.bytes", "English/Play.json"})

    def test_pack_generation_does_not_require_unsupported_editor_baselines(self) -> None:
        releases = {"pack-only": {"trees": {"Editor": {"generation": -1},
                                          "Pack": {"generation": self.pack_schema_version}}}}
        with patch.object(MODULE, "published_releases", return_value=releases):
            self.assertEqual(MODULE.required_baselines(self.source), {})

    def test_incomplete_companion_pair_stops_before_current_reader(self) -> None:
        (self.pack_root / "English/Play.json").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete companion pair"):
            self.verify()
        self.assertEqual(self.calls, [])

    def test_native_pass_cannot_certify_a_different_companion_generation(self) -> None:
        with self.assertRaises(ValueError):
            self.verify(sdk_version=self.pack_schema_version - 1)
        with self.assertRaises(ValueError):
            self.verify(sdk_version=self.pack_schema_version + 1)

    def test_previous_success_cannot_replace_executing_readers_and_native_checks(self) -> None:
        report, _ = self.verify()
        self.assertEqual(report["errors"], [])
        with self.assertRaisesRegex(ValueError, "Native Pack gate failed"):
            self.verify(native_output=f"[PackCompanion] schemaVersion {self.pack_schema_version}\n")
        previous_calls = len(self.calls)
        with self.assertRaises(ValueError):
            self.verify(native_returncode=1)
        self.assertGreater(len(self.calls), previous_calls)
        with patch.object(self, "probe", side_effect=ValueError("reader failed")):
            with self.assertRaisesRegex(ValueError, "reader failed"):
                self.verify()

    def test_missing_promised_package_stops_before_current_reader(self) -> None:
        with patch.object(MODULE, "required_baselines", return_value={"missing": {}}), \
                patch.object(MODULE, "run_probe") as probe:
            with self.assertRaises(ValueError):
                MODULE.verify_release_readers(self.binary, {}, self.pack_reader, self.pack_root, self.source)
            probe.assert_not_called()


class ReleaseCommandTest(unittest.TestCase):
    def run_command(self, *arguments: str) -> tuple[int, str]:
        output = io.StringIO()
        with patch.object(MODULE.sys, "argv", ["audit", "--verify-readers", "--reader", "studio",
                "--pack-reader", "HarkenAbiFidelity.dll", "--pack-root", "packs", *arguments]), \
                redirect_stdout(output):
            result = MODULE.main()
        return result, output.getvalue()

    def test_unprepared_sources_cannot_reach_binary_verification(self) -> None:
        with patch.object(MODULE, "verify_current_release", return_value={
                "status": "failed", "errors": ["unprepared contracts"]}), \
                patch.object(MODULE, "verify_release_readers") as readers:
            code, output = self.run_command()
        self.assertEqual(code, 1)
        self.assertIn("unprepared contracts", output)
        readers.assert_not_called()

    def test_live_failures_cannot_be_replaced_by_a_previous_success(self) -> None:
        with patch.object(MODULE, "verify_current_release", side_effect=lambda: {
                "status": "passed", "errors": []}), \
                patch.object(MODULE, "verify_release_readers", side_effect=[
                    {"errors": []}, ValueError("native check failed"), {"errors": []}]) as readers, \
                patch.object(MODULE, "require_current_reader", side_effect=[
                    None, ValueError("reader belongs to another commit")]):
            self.assertEqual(self.run_command()[0], 0)
            for message in ("native check failed", "reader belongs to another commit"):
                code, output = self.run_command()
                self.assertEqual(code, 1)
                self.assertIn(message, output)
        self.assertEqual(readers.call_count, 3)

    def test_live_verification_cannot_claim_another_revision(self) -> None:
        with patch.object(MODULE, "verify_release_readers") as readers:
            self.assertEqual(self.run_command("--head", "HEAD~1")[0], 1)
        readers.assert_not_called()


if __name__ == "__main__":
    unittest.main()
