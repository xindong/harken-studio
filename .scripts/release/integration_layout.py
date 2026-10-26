import argparse
import hashlib
import json
import shutil
import stat
from pathlib import Path


PRIMARY_DIRECTORY = "HarkenIntegration"
LEGACY_DIRECTORY = "HarkenSDK"
UNITY_PACKAGE = Path("Unity/Harken")


def package_files(root: Path) -> dict[str, str]:
    def linked(path: Path) -> bool:
        return path.is_symlink() or bool(getattr(path.lstat(), "st_file_attributes", 0)
                                        & stat.FILE_ATTRIBUTE_REPARSE_POINT)

    if not root.is_dir() or linked(root):
        raise ValueError(f"Integration package directory is missing or linked: {root}")
    result = {}
    for path in sorted(root.rglob("*")):
        if linked(path):
            raise ValueError(f"Integration package contains a linked path: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f"Integration package contains an unsupported entry: {path}")
        with path.open("rb") as stream:
            digest = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
            result[path.relative_to(root).as_posix()] = digest.hexdigest()
    return result


def verify(directory: Path) -> None:
    primary = directory / PRIMARY_DIRECTORY / UNITY_PACKAGE
    prefix = UNITY_PACKAGE.as_posix() + "/"
    current_files = {name: digest for name, digest in package_files(directory / PRIMARY_DIRECTORY).items()
                     if name.startswith(prefix)}
    if prefix + "package.json" not in current_files:
        raise ValueError("Integration package manifest is missing")
    identity = json.loads((primary / "package.json").read_text(encoding="utf-8"))
    if not isinstance(identity, dict) or identity.get("name") != "com.harken.core":
        raise ValueError("Integration package identity must be com.harken.core")
    if current_files != package_files(directory / LEGACY_DIRECTORY):
        raise ValueError("Integration package and legacy Hub compatibility copy differ")


def stage(source: Path, destination: Path) -> None:
    package_files(source)
    source = source.resolve(strict=True)
    destination = destination.resolve()
    if source == destination or source.is_relative_to(destination) or destination.is_relative_to(source):
        raise ValueError("Integration source and deployment directories must be separate")
    for name in (PRIMARY_DIRECTORY, LEGACY_DIRECTORY):
        if (destination / name).exists() or (destination / name).is_symlink():
            raise ValueError(f"Integration deployment directory already exists: {destination / name}")
    primary = destination / PRIMARY_DIRECTORY
    shutil.copytree(source, primary)
    shutil.copytree(primary / UNITY_PACKAGE, destination / LEGACY_DIRECTORY / UNITY_PACKAGE)
    verify(destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    if args.source:
        stage(args.source, args.destination)
    else:
        verify(args.destination)


if __name__ == "__main__":
    main()
