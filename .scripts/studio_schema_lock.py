from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import schema_archive
import studio_schema_release_audit as audit

REPO_ROOT = SCRIPT_DIR.parent
SCHEMA_ROOT = REPO_ROOT / "HarkenSchema"


def stage_candidate(target: Path, modes: dict[str, str], source_root: Path | None = None) -> dict[str, Any]:
    source_root = (source_root or SCHEMA_ROOT).resolve()
    target = target.resolve()
    if target.exists() or target.is_relative_to(source_root) or source_root.is_relative_to(target):
        raise ValueError("Candidate must be a new directory outside the source Schema tree")
    trees = {tree: schema_archive.load_tree(tree, source_root) for tree in audit.TREE_NAMES}
    if set(modes) != set(audit.TREE_NAMES) or any(mode not in {"same", "next"} for mode in modes.values()):
        raise ValueError("Choose same or next independently for Editor and Pack")
    shutil.copytree(source_root, target)
    for tree, data in trees.items():
        manifest = dict(data["manifest"])
        version = max(manifest["published"])
        if modes[tree] == "next":
            version += 1
            manifest["published"] = [*manifest["published"], version]
            if tree == "Editor":
                manifest["supportedHistorical"] = sorted(set(manifest["supportedHistorical"]) | {version - 1})
        contract = schema_archive.materialize_source(tree, data["currentSource"], version)
        destination = target / tree / str(version) / audit.SCHEMA_NAMES[tree]
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(schema_archive.canonical_bytes(contract))
        (target / tree / "manifest.json").write_text(audit.canonical_text(manifest), encoding="utf-8")
    bundle = schema_archive.build_bundle(target)
    return {"candidateRoot": str(target), "trees": audit.tree_summary(target),
            "schemaSnapshotHash": bundle["manifest"]["snapshotHash"]}


def candidate_updates(candidate_root: Path) -> dict[Path, bytes]:
    candidate_root = candidate_root.resolve()
    if candidate_root == SCHEMA_ROOT.resolve() or candidate_root.is_relative_to(SCHEMA_ROOT.resolve()):
        raise ValueError("Archive requires an isolated candidate Schema tree")
    updates = {}
    for tree in audit.TREE_NAMES:
        previous = schema_archive.load_tree(tree, SCHEMA_ROOT)
        candidate = schema_archive.load_tree(tree, candidate_root)
        if previous["currentSource"] != candidate["currentSource"]:
            raise ValueError(f"{tree} candidate is stale; current changed after staging")
        old_version, version = previous["currentVersion"], candidate["currentVersion"]
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
        updates[SCHEMA_ROOT / tree / "manifest.json"] = audit.canonical_text(candidate["manifest"]).encode("utf-8")
        updates[SCHEMA_ROOT / tree / str(version) / audit.SCHEMA_NAMES[tree]] = schema_archive.canonical_bytes(candidate["current"])
    return updates


def archive_candidate(candidate_root: Path) -> dict[str, Any]:
    updates = candidate_updates(candidate_root)
    originals = {path: path.read_bytes() if path.exists() else None for path in updates}
    try:
        for path, data in updates.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        bundle = schema_archive.build_bundle(SCHEMA_ROOT)
        for tree, data in bundle["trees"].items():
            if data["published"][data["currentVersion"]] != data["current"]:
                raise ValueError(f"{tree} archive does not match current")
        report = {"trees": audit.tree_summary(SCHEMA_ROOT),
                  "schemaSnapshotHash": bundle["manifest"]["snapshotHash"]}
    except Exception:
        for path, original in originals.items():
            if original is not None:
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
