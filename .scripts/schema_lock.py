from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
SCHEMA_TOOL_DIR = SCRIPT_DIR.parent / "HarkenStudio" / "Source" / "Tooling" / "SchemaArchive"
if str(SCHEMA_TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(SCHEMA_TOOL_DIR))

import schema_archive
import schema_release_audit as audit

REPO_ROOT = SCRIPT_DIR.parent
SCHEMA_ROOT = REPO_ROOT / "HarkenStudio" / "Schema"
PACK_BINARY_ROOT = REPO_ROOT / "HarkenEngine" / "Schema" / "Pack"


def candidate_roots(candidate_root: Path) -> tuple[Path, Path]:
    return candidate_root / audit.SCHEMA_ROOT_PATH, candidate_root / audit.PACK_BINARY_ROOT_PATH


def stage_candidate(target: Path, modes: dict[str, str], source_root: Path | None = None,
                    pack_binary_root: Path | None = None) -> dict[str, Any]:
    source_root = (source_root or SCHEMA_ROOT).resolve()
    pack_binary_root = (pack_binary_root or PACK_BINARY_ROOT).resolve()
    target = target.resolve()
    if target.exists() or any(target.is_relative_to(source) or source.is_relative_to(target)
                              for source in (source_root, pack_binary_root)):
        raise ValueError("Candidate must be a new directory outside the source Schema trees")
    trees = {tree: schema_archive.load_tree(tree, source_root) for tree in audit.TREE_NAMES}
    pack = audit.load_pack_binary(pack_binary_root)
    if pack is None:
        raise ValueError(f"{audit.PACK_BINARY} tree is missing")
    if set(modes) != set(audit.TREE_NAMES) or any(mode not in {"same", "next"} for mode in modes.values()):
        raise ValueError("Choose same or next independently for Editor and Pack")
    studio_target, pack_target = candidate_roots(target)
    shutil.copytree(source_root, studio_target)
    shutil.copytree(pack_binary_root, pack_target)
    for tree, data in trees.items():
        manifest = dict(data["manifest"])
        version = max(manifest["published"])
        development = manifest.pop("developmentVersion", None)
        if development is not None and modes[tree] != "next":
            raise ValueError(f"{tree} development generation must be staged with next")
        if modes[tree] == "next":
            version += 1
            manifest["published"] = [*manifest["published"], version]
            if tree == "Editor":
                manifest["supportedHistorical"] = sorted(set(manifest["supportedHistorical"]) | {version - 1})
        contract = schema_archive.materialize_source(tree, data["currentSource"], version)
        directory = schema_archive.TREE_DIRECTORIES[tree]
        destination = studio_target / directory / str(version) / audit.SCHEMA_NAMES[tree]
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(schema_archive.canonical_bytes(contract))
        (studio_target / directory / "manifest.json").write_text(audit.canonical_text(manifest), encoding="utf-8")
    manifest = dict(pack["manifest"])
    version = max(manifest["published"])
    if modes["Pack"] == "next":
        version += 1
        manifest["published"] = [*manifest["published"], version]
    frozen = pack_target / str(version)
    shutil.rmtree(frozen, ignore_errors=True)
    frozen.mkdir()
    for name, data in pack["current"].items():
        (frozen / name).write_bytes(data)
    (pack_target / "manifest.json").write_text(audit.canonical_text(manifest), encoding="utf-8")
    bundle = schema_archive.build_bundle(studio_target)
    return {"candidateRoot": str(target), "trees": audit.tree_summary(studio_target, pack_target),
            "schemaSnapshotHash": bundle["manifest"]["snapshotHash"]}


def pack_binary_updates(candidate_root: Path) -> dict[Path, bytes | None]:
    previous = audit.load_pack_binary(PACK_BINARY_ROOT)
    candidate = audit.load_pack_binary(candidate_roots(candidate_root)[1])
    if previous is None or candidate is None:
        raise ValueError(f"{audit.PACK_BINARY} tree is missing")
    if previous["current"] != candidate["current"]:
        raise ValueError(f"{audit.PACK_BINARY} candidate is stale; current changed after staging")
    old_version, version = previous["currentVersion"], candidate["currentVersion"]
    expected_published = previous["manifest"]["published"]
    if version == old_version + 1:
        expected_published = [*expected_published, version]
    elif version != old_version:
        raise ValueError(f"{audit.PACK_BINARY} next generation must be max(published) + 1")
    if candidate["manifest"]["published"] != expected_published \
            or candidate["manifest"]["supportedHistorical"] != previous["manifest"]["supportedHistorical"]:
        raise ValueError(f"{audit.PACK_BINARY} candidate changed its historical support or publication set")
    for historical, files in previous["published"].items():
        if historical != version and candidate["published"].get(historical) != files:
            raise ValueError(f"{audit.PACK_BINARY} candidate rewrote historical generation {historical}")
    if candidate["published"][version] != candidate["current"]:
        raise ValueError(f"{audit.PACK_BINARY} candidate does not freeze current into generation {version}")
    frozen = PACK_BINARY_ROOT / str(version)
    updates: dict[Path, bytes | None] = {path: None for path in frozen.glob("*")
                                         if path.name not in candidate["current"]}
    updates.update({frozen / name: data for name, data in candidate["current"].items()})
    updates[PACK_BINARY_ROOT / "manifest.json"] = audit.canonical_text(candidate["manifest"]).encode("utf-8")
    return updates


def candidate_updates(candidate_root: Path) -> dict[Path, bytes | None]:
    candidate_root = candidate_root.resolve()
    if any(candidate_root == source.resolve() or candidate_root.is_relative_to(source.resolve())
           for source in (SCHEMA_ROOT, PACK_BINARY_ROOT)):
        raise ValueError("Archive requires an isolated candidate Schema tree")
    studio_candidate = candidate_roots(candidate_root)[0]
    updates: dict[Path, bytes | None] = {}
    for tree in audit.TREE_NAMES:
        previous = schema_archive.load_tree(tree, SCHEMA_ROOT)
        candidate = schema_archive.load_tree(tree, studio_candidate)
        if previous["currentSource"] != candidate["currentSource"]:
            raise ValueError(f"{tree} candidate is stale; current changed after staging")
        old_version, version = max(previous["manifest"]["published"]), candidate["currentVersion"]
        if "developmentVersion" in candidate["manifest"]:
            raise ValueError(f"{tree} candidate must freeze its development generation")
        expected_published = previous["manifest"]["published"]
        expected_history = previous["manifest"]["supportedHistorical"]
        if version == old_version + 1:
            expected_published = [*expected_published, version]
            if tree == "Editor":
                expected_history = sorted(set(expected_history) | {old_version})
        elif version != old_version:
            raise ValueError(f"{tree} next generation must be max(published) + 1")
        if candidate["manifest"]["published"] != expected_published \
                or candidate["manifest"]["supportedHistorical"] != expected_history:
            raise ValueError(f"{tree} candidate changed its historical support or publication set")
        for historical, contract in previous["published"].items():
            if historical != version and candidate["published"].get(historical) != contract:
                raise ValueError(f"{tree} candidate rewrote historical generation {historical}")
        if candidate["published"][version] != candidate["current"]:
            raise ValueError(f"{tree} candidate does not materialize current into generation {version}")
        directory = SCHEMA_ROOT / schema_archive.TREE_DIRECTORIES[tree]
        updates[directory / "manifest.json"] = audit.canonical_text(candidate["manifest"]).encode("utf-8")
        updates[directory / str(version) / audit.SCHEMA_NAMES[tree]] = schema_archive.canonical_bytes(candidate["current"])
    updates.update(pack_binary_updates(candidate_root))
    return updates


def archive_candidate(candidate_root: Path) -> dict[str, Any]:
    updates = candidate_updates(candidate_root)
    originals = {path: path.read_bytes() if path.exists() else None for path in updates}
    try:
        for path, data in updates.items():
            if data is None:
                path.unlink()
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        bundle = schema_archive.build_bundle(SCHEMA_ROOT)
        for tree, data in bundle["trees"].items():
            if data["published"][data["currentVersion"]] != data["current"]:
                raise ValueError(f"{tree} archive does not match current")
        pack = audit.load_pack_binary(PACK_BINARY_ROOT)
        if pack is None or pack["published"][pack["currentVersion"]] != pack["current"]:
            raise ValueError(f"{audit.PACK_BINARY} archive does not match current")
        report = {"trees": audit.tree_summary(SCHEMA_ROOT, PACK_BINARY_ROOT),
                  "schemaSnapshotHash": bundle["manifest"]["snapshotHash"]}
    except Exception:
        for path, original in originals.items():
            if original is not None:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(original)
            elif path.exists():
                path.unlink()
                if path.parent.name.isdigit() and not any(path.parent.iterdir()):
                    path.parent.rmdir()
        raise
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    stage = commands.add_parser("stage")
    stage.add_argument("--candidate-root", type=Path, required=True)
    stage.add_argument("--editor", choices=("same", "next"), default="same")
    stage.add_argument("--pack", choices=("same", "next"), default="same")
    archive = commands.add_parser("archive")
    archive.add_argument("--candidate-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "stage":
            report = stage_candidate(args.candidate_root, {"Editor": args.editor, "Pack": args.pack})
        else:
            report = archive_candidate(args.candidate_root)
        print(audit.canonical_text(report), end="")
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
