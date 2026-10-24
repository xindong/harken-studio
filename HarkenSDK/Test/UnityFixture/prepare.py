#!/usr/bin/env python3
"""Copy repository-owned audio fixtures into an existing Unity test project."""

import argparse
import hashlib
import shutil
from pathlib import Path


FIXTURE_ROOT = Path(__file__).resolve().parent
REPOSITORY = FIXTURE_ROOT.parents[2]
DATA_NAMES = (
    "U4Original", "U4Renamed", "U4OtherProject", "NameFirstPacks",
    "PlatformContractPacks", "SpatialPacks", "CompanionPacks", "VariableNames",
)


def digest(path: Path) -> bytes:
    return hashlib.sha256(path.read_bytes()).digest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path,
                        help="Existing dedicated Unity project directory")
    parser.add_argument("--check", action="store_true",
                        help="Verify installed fixture bytes without writing")
    args = parser.parse_args()
    project = args.project.resolve()
    if not (project / "Assets").is_dir() or not (project / "ProjectSettings/ProjectVersion.txt").is_file():
        parser.error("--project must name an existing Unity project with Assets and ProjectSettings/ProjectVersion.txt")
    sources = [(FIXTURE_ROOT / "Data" / name, name) for name in DATA_NAMES]
    copies = []
    dest_roots = []
    for source_root, name in sources:
        files = sorted(path for path in source_root.rglob("*") if path.is_file() and path.suffix != ".meta")
        if not files:
            parser.error(f"Repository fixture is missing: {source_root}")
        dest_root = project / "Assets/StreamingAssets/Audio" / name
        dest_roots.append(dest_root)
        for source in files:
            target = dest_root / source.relative_to(source_root)
            copies.append((source, target))
    wanted = {target for _, target in copies}
    extras = []
    for dest_root in dest_roots:
        if not dest_root.is_dir():
            continue
        for path in dest_root.rglob("*"):
            if not path.is_file():
                continue
            keep = path in wanted or (path.suffix == ".meta" and
                                     (path.with_suffix("") in wanted or path.with_suffix("").is_dir()))
            if not keep:
                extras.append(path)
    failures = []
    for source, target in copies:
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.is_file() or digest(source) != digest(target):
                shutil.copyfile(source, target)
        if not target.is_file() or digest(source) != digest(target):
            failures.append(str(target.relative_to(project)))
    if not args.check:
        for extra in extras:
            extra.unlink()
    elif extras:
        failures.extend(str(path.relative_to(project)) for path in extras)
    if failures:
        parser.exit(1, "Missing or mismatched fixture files:\n" + "\n".join(failures) + "\n")
    print(f"Verified {len(copies)} fixture files in {project}; SDK and native plugins were not changed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
