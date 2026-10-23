from __future__ import annotations

import argparse
import copy
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from datetime import datetime
from functools import cache
from pathlib import Path, PurePosixPath
from typing import Any, Iterator

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import schema_archive
from release.verify_pack_reader import verify_companion_reader, verify_pack_reader

REPO_ROOT = SCRIPT_DIR.parent
SCHEMA_ROOT_PATH = "HarkenSchema"
CONFIG_PATH = ".scripts/studio-schema-release.json"
RELEASE_SCOPE_CONFIG_PATH = ".scripts/studio-schema-release-scopes.json"
TREE_NAMES = ("Editor", "Pack")
SCHEMA_NAMES = {"Editor": "ProjectSerializedSchema.json", "Pack": "PackManifestSchema.json"}
STUDIO_BINARIES = ("HarkenStudio.exe", "HarkenStudio.app/Contents/MacOS/HarkenStudio")
EDIT_PATHS = {"/content/metadata/description", "/content/name"}


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def canonical_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def json_equal(left: Any, right: Any) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, dict) or isinstance(right, dict):
        return isinstance(left, dict) and isinstance(right, dict) and set(left) == set(right) \
            and all(json_equal(left[key], right[key]) for key in left)
    if isinstance(left, list) or isinstance(right, list):
        return isinstance(left, list) and isinstance(right, list) and len(left) == len(right) \
            and all(json_equal(a, b) for a, b in zip(left, right))
    return left == right


def read_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def run_git(args: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], cwd=REPO_ROOT, text=True, encoding="utf-8",
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=check)


def revision_bytes(revision: str, path: str) -> bytes | None:
    if revision == "WORKTREE":
        target = REPO_ROOT / path
        return target.read_bytes() if target.is_file() else None
    result = subprocess.run(["git", "show", f"{revision}:{path}"], cwd=REPO_ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    return result.stdout if result.returncode == 0 else None


def revision_json(revision: str, path: str) -> dict[str, Any] | None:
    data = revision_bytes(revision, path)
    if data is None:
        return None
    value = json.loads(data)
    if not isinstance(value, dict):
        raise ValueError(f"{revision}:{path} must contain a JSON object")
    return value


def revision_paths(revision: str) -> list[str]:
    if revision == "WORKTREE":
        return sorted(set(run_git(["ls-files"]).stdout.splitlines()) |
                      set(run_git(["ls-files", "--others", "--exclude-standard"]).stdout.splitlines()))
    return run_git(["ls-tree", "-r", "--name-only", revision]).stdout.splitlines()


def changed_files(base: str, head: str) -> list[str]:
    args = ["diff", "--name-only", base]
    if head != "WORKTREE":
        args.append(head)
    paths = set(run_git([*args, "--"]).stdout.splitlines())
    if head == "WORKTREE":
        paths.update(run_git(["ls-files", "--others", "--exclude-standard"]).stdout.splitlines())
    return sorted(paths)


@contextmanager
def schema_root_at(revision: str) -> Iterator[Path]:
    if revision == "WORKTREE":
        yield REPO_ROOT / SCHEMA_ROOT_PATH
        return
    temp_root = REPO_ROOT / ".temp"
    temp_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="schema-audit-", dir=temp_root) as directory:
        root = Path(directory)
        for path in revision_paths(revision):
            if path.startswith(SCHEMA_ROOT_PATH + "/"):
                data = revision_bytes(revision, path)
                if data is not None:
                    destination = root / path.removeprefix(SCHEMA_ROOT_PATH + "/")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(data)
        yield root


def tree_summary(root: Path) -> dict[str, Any]:
    result = {}
    for tree in TREE_NAMES:
        data = schema_archive.load_tree(tree, root)
        result[tree] = {"generation": data["currentVersion"],
                        "sha256": digest(canonical_bytes(data["current"]))}
    return result


def release_scope_config(revision: str) -> dict[str, list[str]]:
    config = revision_json(revision, RELEASE_SCOPE_CONFIG_PATH)
    if not config or config.get("scopeMapVersion") != 2:
        raise ValueError("Studio Schema release scope map is missing or unsupported")
    trees = config.get("trees")
    if not isinstance(trees, dict) or set(trees) != set(TREE_NAMES):
        raise ValueError("Studio Schema release scope map must contain Editor and Pack")
    for tree, patterns in trees.items():
        if not isinstance(patterns, list) or not patterns or any(
                not isinstance(pattern, str) or not pattern for pattern in patterns):
            raise ValueError(f"Invalid release scope patterns for {tree}")
    return trees


def review_scopes(base: str, head: str) -> list[dict[str, Any]]:
    paths = changed_files(base, head)
    return [{"scope": tree, "files": matched}
            for tree, patterns in release_scope_config(head).items()
            if (matched := [path for path in paths
                            if path.startswith(f"HarkenSchema/{tree}/")
                            or any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)])]


def source_fingerprint(revision: str) -> str:
    patterns = [pattern for values in release_scope_config(revision).values() for pattern in values]
    files = {}
    for path in revision_paths(revision):
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns):
            data = revision_bytes(revision, path)
            if data is not None:
                files[path] = digest(data.replace(b"\r\n", b"\n"))
    return digest(canonical_bytes(files))


def same_generation_errors(tree: str, previous: dict[str, Any], current: dict[str, Any]) -> list[str]:
    if json_equal(previous, current):
        return []
    if tree == "Pack":
        previous, current = copy.deepcopy(previous), copy.deepcopy(current)
        for source in (previous, current):
            for field in ("readers", "writers", "validators"):
                source.get("x-harken-format", {}).pop(field, None)
        if json_equal(previous, current):
            return []
        return ["Pack sidecars have no ignorable extension boundary; structural or semantic changes require a new generation"]
    errors: list[str] = []
    boundaries = set()
    for index, item in enumerate(previous["x-harken-documentTypes"]):
        base = ("x-harken-documentTypes", str(index), "schema", "properties")
        boundaries.add((*base, "harkenDocument"))
        content = item["schema"]["properties"]["content"]
        content_path = (*base, "content")
        if "$ref" in content:
            content_path = tuple(content["$ref"].removeprefix("#/").split("/"))
        boundaries.add((*content_path, "properties", "extensions"))

    def walk(old: Any, new: Any, path: tuple[str, ...]) -> None:
        if json_equal(old, new):
            return
        if not isinstance(old, dict) or not isinstance(new, dict):
            if isinstance(old, list) and isinstance(new, list) and len(old) == len(new):
                for index, (left, right) in enumerate(zip(old, new)):
                    walk(left, right, (*path, str(index)))
                return
            errors.append("Existing schema behavior changed at /" + "/".join(path))
            return
        if set(old) - set(new):
            errors.append("Schema content removed at /" + "/".join(path))
        for key in set(new) - set(old):
            if key != "properties" or old.get("additionalProperties") is not True \
                    or path not in boundaries or any(value != {} for value in new[key].values()):
                errors.append("Schema content added outside metadata at /" + "/".join((*path, key)))
        for key in set(old) & set(new):
            if key == "properties" and isinstance(old[key], dict) and isinstance(new[key], dict):
                for name in set(new[key]) - set(old[key]):
                    if path not in boundaries or old.get("additionalProperties") is not True \
                            or name in new.get("required", []) or new[key][name] != {}:
                        errors.append("Optional content is not an ignorable metadata extension at /" +
                                      "/".join((*path, key, name)))
                for name in set(old[key]) - set(new[key]):
                    errors.append("Schema property removed at /" + "/".join((*path, key, name)))
                for name in set(old[key]) & set(new[key]):
                    walk(old[key][name], new[key][name], (*path, key, name))
            else:
                walk(old[key], new[key], (*path, key))

    walk(previous, current, ())
    return sorted(set(errors))


@cache
def github_json(root: Path, endpoint: str, asset: bool = False) -> Any:
    args = ["gh", "api", endpoint]
    args.extend(["--header", "Accept: application/octet-stream"] if asset else ["--paginate", "--slurp"])
    try:
        result = subprocess.run(args, cwd=root, capture_output=True, text=True, encoding="utf-8",
                                check=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        raise ValueError("Cannot discover published Studio baselines through GitHub; "
                         "check gh authentication and repository access") from error
    return json.loads(result.stdout)


def github_studio_release_tags(tags: set[str]) -> set[str]:
    published = set()
    for page in github_json(REPO_ROOT, "repos/{owner}/{repo}/releases?per_page=100"):
        for release in page:
            tag = release["tag_name"]
            if tag not in tags or release["draft"] or not release["published_at"]:
                continue
            assets = {asset["name"]: asset for asset in release["assets"] if asset["state"] == "uploaded"}
            catalog_asset = assets.get("harken-manifest.json")
            if not catalog_asset or not any(name.endswith(".zip") for name in assets):
                continue
            catalog = github_json(REPO_ROOT, f"repos/{{owner}}/{{repo}}/releases/assets/{catalog_asset['id']}", True)
            if not isinstance(catalog, dict) or catalog.get("schema_version") != 1 \
                    or not isinstance(catalog.get("channels"), dict):
                raise ValueError(f"Published Studio catalog is invalid: {tag}")
            for entries in catalog["channels"].values():
                if not isinstance(entries, list):
                    raise ValueError(f"Published Studio catalog channel is invalid: {tag}")
                for entry in entries:
                    if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
                        raise ValueError(f"Published Studio catalog entry is invalid: {tag}")
                    if entry.get("releaseTag") != tag:
                        continue
                    name = PurePosixPath(entry["file"]).name
                    if name.endswith(".zip") and name in assets:
                        published.add(tag)
    return published


def published_releases() -> dict[str, dict[str, Any]]:
    releases = {}
    candidates = {}
    for tag in run_git(["tag", "--list"]).stdout.splitlines():
        record = revision_json(tag, CONFIG_PATH)
        if record and record.get("product") == "HarkenStudio" \
                and record.get("auditVersion") == 4 and record.get("publication") == "released":
            if record.get("releaseTag") == tag:
                releases[tag] = record
            else:
                candidates[tag] = record
    if candidates:
        for tag in github_studio_release_tags(set(candidates)):
            releases[tag] = candidates[tag]
    return releases


def required_baselines(root: Path, exclude_tag: str | None = None,
                      exclude_commit: str | None = None) -> dict[str, dict[str, Any]]:
    editor = schema_archive.load_tree("Editor", root)
    pack = schema_archive.load_tree("Pack", root)
    versions = set(editor["manifest"]["supportedHistorical"]) | {editor["currentVersion"]}
    return {tag: record for tag, record in published_releases().items()
            if tag != exclude_tag and (exclude_commit is None or
                run_git(["rev-parse", f"refs/tags/{tag}^{{commit}}"]).stdout.strip() != exclude_commit)
            and (record.get("trees", {}).get("Editor", {}).get("generation") in versions
            or record.get("trees", {}).get("Pack", {}).get("generation") == pack["currentVersion"])}


def release_schema_errors(root: Path, exclude_tag: str | None = None) -> list[str]:
    errors = []
    current = {tree: schema_archive.load_tree(tree, root) for tree in TREE_NAMES}
    for tag in published_releases():
        if tag == exclude_tag:
            continue
        with schema_root_at(tag) as previous_root:
            for tree in TREE_NAMES:
                previous = schema_archive.load_tree(tree, previous_root)
                for version, published in previous["published"].items():
                    if version not in current[tree]["published"]:
                        errors.append(f"{tag}: Published {tree} generation {version} cannot be removed")
                        continue
                    target = current[tree]["current"] if version == current[tree]["currentVersion"] \
                        else current[tree]["published"][version]
                    source = previous["current"] if version == previous["currentVersion"] else published
                    errors.extend(f"{tag} {tree}/{version}: {error}" for error in same_generation_errors(tree, source, target))
    return errors


def verify_current_release(head_revision: str = "WORKTREE") -> dict[str, Any]:
    errors = []
    with schema_root_at(head_revision) as root:
        bundle = schema_archive.build_bundle(root)
        managed = revision_bytes(head_revision, schema_archive.UNITY_PACK_SCHEMA_PATH)
        if managed is None or managed.replace(b"\r\n", b"\n") != schema_archive.unity_pack_schema(bundle).encode("utf-8"):
            errors.append("Unity Pack schema does not match the release generation")
        record = revision_json(head_revision, CONFIG_PATH)
        if not record or record.get("product") != "HarkenStudio" \
                or record.get("auditVersion") != 4 or record.get("publication") != "released":
            errors.append("Formal Studio release has no verified release receipt")
        else:
            if record.get("trees") != tree_summary(root):
                errors.append("Formal Schema release receipt does not match current contracts")
            if record.get("sourceFingerprint") != source_fingerprint(head_revision):
                errors.append("Formal Schema release receipt does not match reader/writer sources")
            verification = record.get("verification")
            if not isinstance(verification, dict):
                verification = {}
            pack = verification.get("pack")
            if not isinstance(pack, dict):
                pack = {}
            committed = revision_json("HEAD" if head_revision == "WORKTREE" else head_revision, CONFIG_PATH)
            release_commit = None
            if not errors and json_equal(record, committed):
                release_commit = run_git(["rev-parse", "HEAD" if head_revision == "WORKTREE" else head_revision]).stdout.strip()
            baselines = sorted(required_baselines(root, record.get("releaseTag"), release_commit))
            readers = verification.get("readers")
            reader_tags = [reader["tag"] for reader in readers
                           if isinstance(reader, dict) and isinstance(reader.get("tag"), str)] \
                if isinstance(readers, list) else []
            if verification.get("protocol") != "schema-roundtrip-v1" \
                    or verification.get("errors") != [] \
                    or verification.get("schemaSnapshotHash") != bundle["manifest"]["snapshotHash"] \
                    or not isinstance(readers, list) or len(reader_tags) != len(readers) \
                    or len(reader_tags) != len(set(reader_tags)) or set(reader_tags) != set(baselines) | {"current"} \
                    or pack.get("status") != "passed" \
                    or type(pack.get("schemaVersion")) is not int \
                    or pack.get("schemaVersion") != bundle["manifest"]["trees"]["Pack"]["currentVersion"]:
                errors.append("Formal Schema release receipt has no controlled reader/writer and native Pack checks")
            if verification.get("baselineTags") != baselines:
                errors.append("Formal Schema release receipt does not cover all promised releases")
            tag = record.get("releaseTag")
            if not isinstance(tag, str) or not tag:
                errors.append("Formal Schema release receipt has no release tag")
            elif run_git(["rev-parse", "--verify", f"refs/tags/{tag}"], check=False).returncode == 0:
                tagged = run_git(["rev-parse", f"refs/tags/{tag}^{{commit}}"]).stdout.strip()
                head = run_git(["rev-parse", "HEAD" if head_revision == "WORKTREE" else head_revision]).stdout.strip()
                if tagged != head:
                    errors.append("Published release tag cannot be reused for another commit")
        for tree in TREE_NAMES:
            data = schema_archive.load_tree(tree, root)
            if data["published"][data["currentVersion"]] != data["current"]:
                errors.append(f"{tree} current has not been synchronized to its release generation")
        errors.extend(release_schema_errors(root, record.get("releaseTag") if record else None))
    return {"auditVersion": 4, "decision": "formal-release", "headRevision": head_revision,
            "status": "failed" if errors else "passed", "errors": errors}


def audit(base_revision: str | None, head_revision: str = "WORKTREE") -> dict[str, Any]:
    with schema_root_at(head_revision) as root:
        schema_archive.build_bundle(root)
    scopes = review_scopes(base_revision, head_revision) if base_revision else []
    errors = []
    if base_revision and published_releases() and any(
            re.match(r"HarkenSchema/(Editor|Pack)/(\d+/|manifest\.json$)", path)
            for path in changed_files(base_revision, head_revision)):
        errors.extend(verify_current_release(head_revision)["errors"])
    return {"auditVersion": 4, "decision": "development", "headRevision": head_revision,
            "reviewScopes": scopes, "status": "failed" if errors else "passed", "errors": errors}


def run_probe(binary: Path, request: dict[str, Any], directory: Path,
              expected: dict[str, Any], package: dict[str, Any] | None = None) -> dict[str, Any]:
    request_path = directory / "request.json"
    request_path.write_text(canonical_text({"requestVersion": 1, **request}), encoding="utf-8")
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    result = subprocess.run([str(binary), "--schema-roundtrip", str(request_path)],
                            cwd=REPO_ROOT, env=environment, capture_output=True,
                            text=True, encoding="utf-8", timeout=180, check=False)
    if result.returncode != 0:
        raise ValueError(f"Reader {binary} failed: {result.stderr.strip()} {result.stdout.strip()}")
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"Reader {binary} did not return the fixed probe report") from error
    if not isinstance(report, dict) or report.get("reportVersion") != 1 \
            or report.get("ok") is not True or report.get("product") != "HarkenStudio" \
            or report.get("mode") != request["mode"]:
        raise ValueError(f"Reader {binary} returned an invalid probe report")
    if report.get("schemaSnapshotHash") != expected["snapshotHash"] \
            or type(report.get("editorSchemaVersion")) is not int \
            or type(report.get("packSchemaVersion")) is not int \
            or report.get("editorSchemaVersion") != expected["trees"]["Editor"]["currentVersion"] \
            or report.get("packSchemaVersion") != expected["trees"]["Pack"]["currentVersion"]:
        raise ValueError(f"Reader {binary} does not match its Schema inputs")
    for field in ("buildId", "version", "gitCommit"):
        if not isinstance(report.get(field), str) or not report[field]:
            raise ValueError(f"Reader {binary} has no {field}")
        if package is not None and report[field] != package.get(field):
            raise ValueError(f"Reader {binary} does not match its package {field}")
    if type(report.get("gitDirty")) is not bool or package is not None and report["gitDirty"] != package.get("gitDirty"):
        raise ValueError(f"Reader {binary} does not match its package source state")
    project_path = Path(report.get("projectPath", "")).resolve()
    if request["mode"] == "create":
        output_root = Path(request["outputDirectory"]).resolve()
        if not project_path.is_relative_to(output_root):
            raise ValueError("Probe created its fixture outside the requested directory")
    elif project_path != Path(request["projectPath"]).resolve():
        raise ValueError("Probe reported a different project")
    if not project_path.is_file():
        raise ValueError("Probe did not produce a project")
    return report


def project_documents(project_path: Path) -> dict[str, tuple[Path, dict[str, Any]]]:
    result = {}
    paths = [project_path]
    for bucket in ("Actions", "Attenuations", "Busses", "Layers", "Variables"):
        paths.extend((project_path.parent / bucket).rglob("*.vp"))
    for path in paths:
        if path.is_file() and not any(part.startswith(".") for part in path.relative_to(project_path.parent).parts):
            if path.is_symlink():
                raise ValueError("Compatibility fixtures must not contain symlinks")
            document = read_object(path)
            identity = document.get("harkenDocument", {}).get("id")
            if not isinstance(identity, str) or not identity or identity in result:
                raise ValueError("Compatibility fixture has a missing or duplicate document ID")
            result[identity] = (path, document)
    if not result:
        raise ValueError("Compatibility fixture is empty")
    return result


def pointer_parent(document: dict[str, Any], path: str) -> tuple[dict[str, Any], str]:
    keys = path.removeprefix("/").split("/")
    parent = document
    for key in keys[:-1]:
        parent = parent[key]
    return parent, keys[-1]


def documents_equal(left: dict[str, tuple[Path, dict[str, Any]]],
                    right: dict[str, tuple[Path, dict[str, Any]]]) -> bool:
    return set(left) == set(right) and all(left[key][0] == right[key][0]
           and json_equal(left[key][1], right[key][1]) for key in left)


def verify_edit(before: dict[str, tuple[Path, dict[str, Any]]],
                after: dict[str, tuple[Path, dict[str, Any]]], report: dict[str, Any]) -> None:
    if set(before) != set(after):
        raise ValueError("Reader changed the set of project document IDs")
    edits = report.get("edits")
    if not isinstance(edits, list) or not edits:
        raise ValueError("Reader did not perform normal project edits")
    expected = {identity: copy.deepcopy(value[1]) for identity, value in before.items()}
    roots = [document["content"] for document in expected.values()
             if document["harkenDocument"]["type"] == "project.root"]
    if len(roots) != 1:
        raise ValueError("Compatibility fixture must contain exactly one project root")
    edited = set()
    for edit in edits:
        identity, path = edit.get("documentId"), edit.get("jsonPath")
        if identity not in expected or path not in EDIT_PATHS or (identity, path) in edited:
            raise ValueError("Reader reported an unapproved edit")
        document = expected[identity]
        is_root = document["harkenDocument"]["type"] == "project.root"
        if is_root != (path == "/content/metadata/description"):
            raise ValueError("Reader edited an unexpected document kind")
        parent, key = pointer_parent(document, path)
        original = parent.get(key, "")
        suffix = "\nSchema compatibility edit" if is_root else "_schema_edit"
        if not isinstance(original, str) or edit.get("before") != original or edit.get("after") != original + suffix:
            raise ValueError("Reader did not perform the fixed normal edit")
        parent[key] = edit["after"]
        if document["harkenDocument"]["type"] == "project.action" and "nameBindingRevision" in parent:
            revision = parent["nameBindingRevision"]
            if type(revision) is not int or revision < 1:
                raise ValueError("Fixture Action has an invalid name binding revision")
            parent["nameBindingRevision"] = revision + 1
            history = {
                "name": edit["after"], "objectGuid": parent["guid"],
                "nameBindingRevision": revision + 1,
                "event": "renamed_from", "priorName": original,
            }
            if "runtimeStableId" in parent:
                history["runtimeStableId"] = str(parent["runtimeStableId"])
            roots[0].setdefault("actionPublishedNameHistory", []).append(history)
        edited.add((identity, path))
    required_edits = set()
    for identity, (_, document) in before.items():
        kind = document["harkenDocument"]["type"]
        content = document["content"]
        if kind == "project.root":
            required_edits.add((identity, "/content/metadata/description"))
        elif content.get("type") != "Folder" and not (
                kind == "project.bus" and content.get("parentRef", {}).get("kind") == "root"):
            required_edits.add((identity, "/content/name"))
    if edited != required_edits:
        raise ValueError("Reader must edit every eligible project document")
    original_documents = {identity: value[1] for identity, value in before.items()}

    def object_path(identity: str, values: dict[str, Any], visiting: set[str]) -> str:
        if identity in visiting or identity not in values:
            raise ValueError("Fixture object path has a missing or cyclic parent")
        content = values[identity]["content"]
        reference = content.get("parentRef", {})
        if reference.get("kind") == "root":
            return "\\" + content["name"]
        if reference.get("kind") != "item":
            raise ValueError("Fixture object path has an unsupported parent")
        return object_path(reference.get("id"), values, visiting | {identity}) + "\\" + content["name"]

    for identity, document in expected.items():
        saved = after[identity][1]
        document["harkenDocument"]["savedStudioVersion"] = report["version"]
        content = document["content"]
        if "objectPath" in content:
            if original_documents[identity]["content"]["objectPath"] != object_path(identity, original_documents, set()):
                raise ValueError("Fixture object path does not match its authored parent chain")
            content["objectPath"] = object_path(identity, expected, set())
        if document["harkenDocument"]["type"] == "project.attenuation":
            component = content.get("components", {}).get("attenuation", {})
            if "name" in component:
                if component["name"] != original_documents[identity]["content"]["name"]:
                    raise ValueError("Fixture attenuation name does not match its document")
                component["name"] = content["name"]
        if document["harkenDocument"]["type"] == "project.root":
            metadata = document["content"]["metadata"]
            if "modifiedDate" in metadata:
                value = saved.get("content", {}).get("metadata", {}).get("modifiedDate")
                if not isinstance(value, str):
                    raise ValueError("Saved project has no modification time")
                try:
                    datetime.fromisoformat(value.replace("Z", "+00:00"))
                except ValueError as error:
                    raise ValueError("Saved project has an invalid modification time") from error
                metadata["modifiedDate"] = value
        if not json_equal(document, saved):
            raise ValueError(f"Reader lost or changed unedited project content in {identity}")


def inject_metadata(project_path: Path, source: dict[str, Any]) -> None:
    metadata = {"text": "Unedited author metadata", "values": [None, True, 9007199254740993,
                 -2.5, {}, [], {"nested": "preserved"}]}
    for path, document in project_documents(project_path).values():
        kind = document["harkenDocument"]["type"]
        schema = next(item["schema"] for item in source["x-harken-documentTypes"] if item["type"] == kind)
        header = schema["properties"]["harkenDocument"]
        if header.get("additionalProperties") is True:
            document["harkenDocument"]["schemaCompatibilityMetadata"] = metadata
            for name, field in header["properties"].items():
                if field == {}:
                    document["harkenDocument"][name] = metadata
        content = schema["properties"]["content"]
        if "$ref" in content:
            content = source["$defs"][content["$ref"].removeprefix("#/$defs/")]
        extension = content.get("properties", {}).get("extensions", {})
        if extension.get("additionalProperties") is True:
            document["content"].setdefault("extensions", {})["schemaCompatibilityMetadata"] = metadata
            for name, field in extension.get("properties", {}).items():
                if field == {}:
                    document["content"]["extensions"][name] = metadata
        path.write_text(canonical_text(document), encoding="utf-8")


def checked_baseline_package(tag: str, root: Path) -> tuple[Path, dict[str, Any]]:
    root = root.resolve()
    manifest = read_object(root / "package-manifest.json")
    if manifest.get("product") != "HarkenStudio":
        raise ValueError("Baseline package is not a HarkenStudio release")
    if any(not isinstance(manifest.get(field), str) or not manifest[field] for field in ("version", "buildId")):
        raise ValueError("Baseline package has no release build identity")
    expected_commit = run_git(["rev-parse", f"refs/tags/{tag}^{{commit}}"]).stdout.strip()
    if manifest.get("gitCommit") != expected_commit or manifest.get("gitDirty") is not False:
        raise ValueError(f"Baseline package does not match the clean source commit of {tag}")
    records = manifest.get("files")
    if not isinstance(records, list) or not records:
        raise ValueError(f"Baseline package {tag} has no protected files")
    protected = set()
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Baseline package contains an invalid file record")
        name = record.get("path")
        if not isinstance(name, str) or not name or "\\" in name:
            raise ValueError("Baseline package has an invalid file path")
        path = root / name
        if name in protected or Path(name).is_absolute() or ".." in Path(name).parts \
                or path.is_symlink() or not path.resolve().is_relative_to(root) or not path.is_file():
            raise ValueError("Baseline package contains an unsafe, duplicate or missing file")
        if path.stat().st_size != record.get("size") or digest(path.read_bytes()) != record.get("sha256"):
            raise ValueError(f"Baseline package file does not match its manifest: {name}")
        protected.add(name)
    binaries = [root / name for name in STUDIO_BINARIES if name in protected]
    if len(binaries) != 1:
        raise ValueError(f"Baseline package {tag} must protect exactly one Studio binary")
    for path in root.rglob("*"):
        if path.is_file() and ("schema" in path.relative_to(root).parts) \
                and path.relative_to(root).as_posix() not in protected:
            raise ValueError("Baseline package contains unprotected Schema content")
    schema_dir = binaries[0].parent / "schema"
    previous_manifest = read_object(schema_dir / "build.json")
    if previous_manifest.get("snapshotHash") != schema_archive.snapshot_hash(previous_manifest):
        raise ValueError("Baseline package has an invalid Schema snapshot identity")
    with schema_root_at(tag) as previous_root:
        expected = schema_archive.build_bundle(previous_root)
        if previous_manifest.get("trees") != expected["manifest"]["trees"]:
            raise ValueError("Baseline package generations do not match its tag")
        source_paths = {name for name in expected["installed"] if name != "build.json"}
        installed_paths = {path.relative_to(schema_dir).as_posix() for path in schema_dir.rglob("*") if path.is_file()}
        if installed_paths != source_paths | {"build.json"}:
            raise ValueError("Baseline package has an unexpected Schema installation tree")
        for name in source_paths:
            if read_object(schema_dir / name) != json.loads(expected["installed"][name]):
                raise ValueError("Baseline package Schema content does not match its tag")
    manifest["_schemaManifest"] = previous_manifest
    manifest["_protectedFiles"] = protected
    return binaries[0], manifest


def parse_baseline_readers(values: list[str], source: Path | None = None) -> dict[str, Path]:
    result = {}
    if source is not None:
        for tag, value in read_object(source).items():
            if not isinstance(tag, str) or not tag or not isinstance(value, str) or not value:
                raise ValueError("Baseline reader map must contain tag-to-directory strings")
            result[tag] = Path(value).resolve()
    for value in values:
        tag, separator, directory = value.partition("=")
        if not separator or not tag or not directory or tag in result:
            raise ValueError("Baseline readers must be unique tag=directory entries")
        result[tag] = Path(directory).resolve()
    return result


def require_current_reader(report: dict[str, Any]) -> None:
    reader = report.get("currentReader", {})
    if not isinstance(reader, dict) or reader.get("gitDirty") is not False \
            or reader.get("gitCommit") != run_git(["rev-parse", "HEAD"]).stdout.strip():
        raise ValueError("Formal release verification requires a clean reader built from HEAD")
    if source_fingerprint("WORKTREE") != source_fingerprint("HEAD"):
        raise ValueError("Commit reader/writer source changes before building a formal release candidate")


def verify_release_readers(current_binary: Path, baseline_readers: dict[str, Path],
                           pack_reader: Path | None = None, pack_root: Path | None = None,
                           schema_root: Path | None = None, release_tag: str | None = None) -> dict[str, Any]:
    root = schema_root or REPO_ROOT / SCHEMA_ROOT_PATH
    bundle = schema_archive.build_bundle(root)
    if release_tag is None and root.resolve() == (REPO_ROOT / SCHEMA_ROOT_PATH).resolve():
        record = revision_json("WORKTREE", CONFIG_PATH) or {}
        release_tag = record.get("releaseTag")
    required = required_baselines(root, release_tag)
    missing = set(required) - set(baseline_readers)
    if missing:
        raise ValueError("Missing promised baseline package(s): " + ", ".join(sorted(missing)))
    if set(baseline_readers) - set(required):
        raise ValueError("Unexpected baseline packages were supplied")
    errors = release_schema_errors(root, release_tag)
    if errors:
        raise ValueError("; ".join(errors))
    if pack_reader is None or pack_root is None:
        raise ValueError("Formal release requires the compiled native Pack gate and its Pack fixture")
    current_binary = current_binary.resolve()
    pack_reader, pack_root = pack_reader.resolve(), pack_root.resolve()
    if not current_binary.is_file() or not pack_reader.is_file() or pack_reader.name != "HarkenAbiFidelity.dll":
        raise ValueError("Formal release requires the Studio binary and HarkenAbiFidelity.dll")
    companion = bundle["trees"]["Pack"]["current"]["x-harken-format"]["formatId"] == "generated.pack-companion"
    if companion:
        fixture_files = sorted(path.relative_to(pack_root).as_posix() for path in pack_root.rglob("*")
                               if path.is_file() and path.suffix in {".bytes", ".json"})
        if not {"Init.bytes", "Init.json"}.issubset(fixture_files) \
                or not any(name.endswith(".bytes") and Path(name).name != "Init.bytes" for name in fixture_files):
            raise ValueError("Native Pack fixture must contain Init and Action companion pairs")
    else:
        fixture_files = ["Init.pack", "Content.pack"]
        if not all((pack_root / name).is_file() for name in fixture_files):
            raise ValueError("Native Pack fixture must contain Init.pack and Content.pack")
    platform = "macOS" if sys.platform == "darwin" else "x86_64"
    native_directory = current_binary.parent / "HarkenSDK/Unity/Harken/Runtime/Plugins" / platform
    native_name = "HarkenCore.dll" if os.name == "nt" else "libHarkenCore.dylib" if sys.platform == "darwin" else "libHarkenCore.so"
    if not (native_directory / native_name).is_file():
        native_directory = current_binary.parent
    if not (native_directory / native_name).is_file():
        raise ValueError("Studio package does not contain the native library required by the Pack gate")
    result: dict[str, Any] = {"protocol": "schema-roundtrip-v1", "errors": [],
        "schemaSnapshotHash": bundle["manifest"]["snapshotHash"],
        "baselineTags": sorted(required), "readers": []}
    temp_root = REPO_ROOT / ".temp"
    temp_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="schema-readers-", dir=temp_root) as directory:
        work = Path(directory)
        current_report = run_probe(current_binary, {"mode": "create", "outputDirectory": str(work / "current")},
                                   work, bundle["manifest"])
        result["currentReader"] = {key: current_report[key] for key in ("buildId", "version", "gitCommit", "gitDirty")}
        current_project = Path(current_report["projectPath"])
        editor = schema_archive.load_tree("Editor", root)
        inject_metadata(current_project, editor["current"])
        readers = [("current", current_binary, bundle["manifest"], None)]
        for tag, package_root in sorted(baseline_readers.items()):
            binary, manifest = checked_baseline_package(tag, package_root)
            readers.append((tag, binary, manifest["_schemaManifest"], manifest))
        for index, (tag, binary, expected, package) in enumerate(readers):
            checks = []
            if package is not None and expected["trees"]["Pack"]["currentVersion"] == bundle["manifest"]["trees"]["Pack"]["currentVersion"]:
                sdk = binary.parent / "HarkenSDK/Unity/Harken"
                if companion:
                    sources = [*sdk.glob("Runtime/Generated/*.cs"), *sdk.glob("Runtime/Sdk/*.cs")]
                    if not sources or any(path.relative_to(baseline_readers[tag].resolve()).as_posix()
                                          not in package["_protectedFiles"] for path in sources):
                        raise ValueError("Baseline package does not protect its Pack reader sources")
                    parser = verify_companion_reader(sdk, pack_root, expected["trees"]["Pack"]["currentVersion"],
                                                     native_directory, work)
                else:
                    reader_source = sdk / "Runtime/Sdk/PackManifest.cs"
                    if reader_source.relative_to(baseline_readers[tag].resolve()).as_posix() not in package["_protectedFiles"]:
                        raise ValueError("Baseline package does not protect its Pack parser")
                    parser = verify_pack_reader(reader_source, pack_root, expected["trees"]["Pack"]["currentVersion"], work)
                result.setdefault("packReaders", []).append({"tag": tag, **parser})
                checks.append("old-pack-parser-reads-current-sidecars")
            old_editor_version = expected["trees"]["Editor"]["currentVersion"]
            if old_editor_version != current_report["editorSchemaVersion"] \
                    and old_editor_version not in editor["manifest"]["supportedHistorical"]:
                result["readers"].append({"tag": tag, "buildId": package["buildId"],
                    "schemaSnapshotHash": expected["snapshotHash"], "checks": checks})
                continue
            old_report = run_probe(binary, {"mode": "create", "outputDirectory": str(work / f"old-{index}")},
                                   work, expected, package)
            old_project = Path(old_report["projectPath"])
            if old_report["editorSchemaVersion"] != current_report["editorSchemaVersion"]:
                with schema_root_at(tag) as previous_root:
                    inject_metadata(old_project, schema_archive.load_tree("Editor", previous_root)["current"])
                before = project_documents(old_project)
                for mode in ("migrate-only", "migrate"):
                    migration = run_probe(current_binary, {"mode": mode, "projectPath": str(old_project)},
                                          work, bundle["manifest"])
                    if migration.get("fromVersion") != old_report["editorSchemaVersion"]:
                        raise ValueError("Migration did not consume the promised historical generation")
                    after = project_documents(old_project)
                    if set(before) != set(after) or any(
                            "extensions" in left[1]["content"] and not json_equal(
                                left[1]["content"]["extensions"], after[identity][1]["content"].get("extensions"))
                            or "schemaCompatibilityMetadata" in left[1]["harkenDocument"] and not json_equal(
                                left[1]["harkenDocument"]["schemaCompatibilityMetadata"],
                                after[identity][1]["harkenDocument"].get("schemaCompatibilityMetadata"))
                            for identity, left in before.items()):
                        raise ValueError("Migration lost author metadata or document identities")
                    run_probe(current_binary, {"mode": "read", "projectPath": str(old_project)}, work, bundle["manifest"])
                    if not documents_equal(after, project_documents(old_project)):
                        raise ValueError("Reading migrated project data modified its documents")
                    if mode == "migrate-only":
                        run_probe(current_binary, {"mode": "rollback", "projectPath": str(old_project)}, work, bundle["manifest"])
                        if not documents_equal(before, project_documents(old_project)):
                            raise ValueError("Migration rollback did not restore original project documents")
                result["readers"].append({"tag": tag, "buildId": old_report["buildId"],
                    "schemaSnapshotHash": old_report["schemaSnapshotHash"], "checks": checks + ["migration-preserves-metadata", "new-reads-migrated", "migration-rollback-restores-original"]})
                continue
            before = project_documents(old_project)
            run_probe(current_binary, {"mode": "read", "projectPath": str(old_project)}, work, bundle["manifest"])
            if not documents_equal(before, project_documents(old_project)):
                raise ValueError("Reading old project data modified its documents")
            copy_root = work / f"edit-{index}"
            shutil.copytree(current_project.parent, copy_root)
            project = copy_root / current_project.name
            before = project_documents(project)
            edit_report = run_probe(binary, {"mode": "edit", "projectPath": str(project)}, work, expected, package)
            verify_edit(before, project_documents(project), edit_report)
            before = project_documents(project)
            run_probe(current_binary, {"mode": "read", "projectPath": str(project)}, work, bundle["manifest"])
            if not documents_equal(before, project_documents(project)):
                raise ValueError("Reopening edited data modified its documents")
            result["readers"].append({"tag": tag, "buildId": old_report["buildId"],
                "schemaSnapshotHash": old_report["schemaSnapshotHash"],
                "checks": checks + ["new-reads-old", "old-edits-new", "new-reopens-preserved-content"]})
    environment = dict(os.environ, HARKEN_NATIVE_DIR=str(native_directory))
    arguments = ["--companion-root", str(pack_root)] if companion else ["--require-native", "--pack-root", str(pack_root)]
    pack_result = subprocess.run(["dotnet", str(pack_reader), *arguments],
                                 cwd=REPO_ROOT, env=environment, capture_output=True, text=True,
                                 encoding="utf-8", timeout=180, check=False)
    pack_version = bundle["manifest"]["trees"]["Pack"]["currentVersion"]
    passed = "[PackCompanion] native publication PASS." if companion else "[PackNative] shared Init/content fixture PASS."
    version_line = f"[PackCompanion] schemaVersion {pack_version}" if companion else f"[PackSidecar] schema_version {pack_version}"
    if pack_result.returncode != 0 or passed not in pack_result.stdout.splitlines() \
            or version_line not in pack_result.stdout.splitlines():
        raise ValueError(f"Native Pack gate failed: {pack_result.stdout} {pack_result.stderr}")
    result["pack"] = {"status": "passed", "schemaVersion": pack_version,
                      "readerSha256": digest(pack_reader.read_bytes()),
                      "fixtureSha256": {name: digest((pack_root / name).read_bytes())
                                        for name in fixture_files}}
    return result


def markdown_report(report: dict[str, Any]) -> str:
    lines = [f"Studio Schema {report.get('decision', 'verification')}: {report.get('status', 'passed')}", ""]
    for item in report.get("reviewScopes", []):
        lines.append(f"- {item['scope']}: {len(item['files'])} changed file(s)")
    lines.extend(f"- {error}" for error in report.get("errors", []))
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base")
    parser.add_argument("--head", default="WORKTREE")
    parser.add_argument("--verify-current", action="store_true")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    args = parser.parse_args()
    try:
        report = verify_current_release(args.head) if args.verify_current else audit(args.base, args.head)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report = {"auditVersion": 4, "decision": "error", "status": "failed", "errors": [str(error)]}
    for path, content in ((args.json_out, canonical_text(report)), (args.markdown_out, markdown_report(report))):
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    print(markdown_report(report), end="")
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
