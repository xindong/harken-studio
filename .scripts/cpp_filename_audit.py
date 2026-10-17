#!/usr/bin/env python3
"""Enforce project C++ filename spelling and exact include-path case."""

from __future__ import annotations

import argparse
import os
import posixpath
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path, PurePosixPath


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (
    "HarkenEngine/Source/Runtime/",
    "HarkenEngine/Source/Tooling/",
    "HarkenEngine/Tool/",
    "HarkenEngine/Test/",
    "HarkenStudio/Test/",
    "HarkenHub/Test/",
    "HarkenSDK/Test/",
)
INCLUDE_SCAN_ROOTS = (
    "HarkenEngine/Source/",
    "HarkenEngine/Tool/",
    "HarkenStudio/Source/",
    "HarkenEngine/Test/",
    "HarkenStudio/Test/",
    "HarkenHub/Test/",
    "HarkenSDK/Test/",
)
DISCOVERY_ROOTS = tuple(sorted(set(SOURCE_ROOTS + INCLUDE_SCAN_ROOTS)))
DISCOVERY_EXCLUDED_PREFIXES = (
    ".git/",
    ".temp/",
    ".claude/worktrees/",
)
OLD_PATH_EXCLUDED_PREFIXES = (
    "HarkenEngine/ThirdParty/",
    "HarkenStudio/ThirdParty/",
)
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}
PROJECT_HEADER_PREFIXES = ("harken/", "harken_remote/")
PASCAL_FILENAME = re.compile(r"^[A-Z][A-Za-z0-9]*\.(?:h|cpp)$")
INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
TOKEN_BOUNDARY = r"A-Za-z0-9_"


def git_files(*arguments: str) -> list[str]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z", *arguments], cwd=REPO_ROOT
    )
    paths = {
        posixpath.normpath(path.decode("utf-8").replace("\\", "/"))
        for path in output.split(b"\0")
        if path
    }
    return sorted(path for path in paths if path != ".")


def is_generated(path: str) -> bool:
    return "generated" in {part.lower() for part in PurePosixPath(path).parts}


def is_discovery_excluded(path: str) -> bool:
    if path.startswith(DISCOVERY_EXCLUDED_PREFIXES):
        return True
    parts = {part.casefold() for part in PurePosixPath(path).parts}
    if "thirdparty" in parts or "third_party" in parts:
        return True
    return any(
        part == "build"
        or part.startswith(("build-", "build_", "build."))
        or part.startswith("cmake-build-")
        for part in parts
    )


def source_files(tracked_files: list[str], untracked_files: list[str]) -> list[str]:
    return sorted(
        {
            path
            for path in (*tracked_files, *untracked_files)
            if path.startswith(DISCOVERY_ROOTS) and not is_discovery_excluded(path)
        }
    )


def text_audit_files(
    tracked_files: list[str], untracked_files: list[str]
) -> list[str]:
    return sorted(
        {
            path
            for path in (*tracked_files, *untracked_files)
            if not is_discovery_excluded(path)
            and not is_generated(path)
            and not path.startswith(OLD_PATH_EXCLUDED_PREFIXES)
        }
    )


def actual_disk_path(path: str, directory_cache: dict[Path, dict[str, list[str]]]) -> str | None:
    current = REPO_ROOT
    actual_parts: list[str] = []
    for requested in PurePosixPath(path).parts:
        entries = directory_cache.get(current)
        if entries is None:
            entries = defaultdict(list)
            try:
                for entry in os.scandir(current):
                    entries[entry.name.casefold()].append(entry.name)
            except OSError:
                return None
            directory_cache[current] = entries
        matches = entries.get(requested.casefold(), [])
        if len(matches) != 1:
            return None
        actual = matches[0]
        actual_parts.append(actual)
        current = current / actual
    return "/".join(actual_parts)


def migration_key(path: str) -> str:
    parts = path.replace("\\", "/").split("/")
    if not parts:
        return ""
    parts[-1] = parts[-1].replace("_", "")
    return "/".join(part.casefold() for part in parts)


def build_suffix_indexes(
    actual_files: list[str],
) -> tuple[dict[str, set[str]], dict[str, set[str]], dict[str, str]]:
    casefold_suffixes: dict[str, set[str]] = defaultdict(set)
    migration_suffixes: dict[str, set[str]] = defaultdict(set)
    full_casefold: dict[str, str] = {}
    for path in actual_files:
        full_casefold[path.casefold()] = path
        parts = PurePosixPath(path).parts
        for index in range(len(parts)):
            suffix = "/".join(parts[index:])
            casefold_suffixes[suffix.casefold()].add(suffix)
            migration_suffixes[migration_key(suffix)].add(suffix)
    return casefold_suffixes, migration_suffixes, full_casefold


def resolve_include(
    includer: str,
    delimiter: str,
    token: str,
    casefold_suffixes: dict[str, set[str]],
    migration_suffixes: dict[str, set[str]],
    full_casefold: dict[str, str],
) -> tuple[str | None, bool]:
    normalized_token = token.replace("\\", "/")
    if delimiter == '"':
        relative = posixpath.normpath(
            posixpath.join(posixpath.dirname(includer), normalized_token)
        )
        target = full_casefold.get(relative.casefold())
        if target is not None:
            expected = posixpath.relpath(target, posixpath.dirname(includer))
            return expected, False

    candidates = casefold_suffixes.get(normalized_token.casefold(), set())
    if not candidates:
        candidates = migration_suffixes.get(migration_key(normalized_token), set())
    if len(candidates) == 1:
        return next(iter(candidates)), False
    return None, len(candidates) > 1


def read_utf8(path: str) -> str | None:
    try:
        data = (REPO_ROOT / Path(path)).read_bytes()
    except OSError:
        return None
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def load_manifest(path: Path) -> dict[str, str]:
    mapping: dict[str, str] = {}
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "old_path\tnew_path":
        raise ValueError(f"{path}: expected old_path/new_path TSV header")
    for line_number, line in enumerate(lines[1:], start=2):
        columns = line.split("\t")
        if len(columns) != 2:
            raise ValueError(f"{path}:{line_number}: expected two TSV columns")
        old_path, new_path = columns
        old_name = PurePosixPath(old_path).name
        new_name = PurePosixPath(new_path).name
        if old_name in mapping and mapping[old_name] != new_name:
            raise ValueError(f"{path}:{line_number}: duplicate old basename {old_name}")
        mapping[old_name] = new_name
    return mapping


def audit_old_paths(files: list[str], manifest: Path) -> list[str]:
    mapping = load_manifest(manifest)
    if not mapping:
        return []
    alternatives = "|".join(
        re.escape(name) for name in sorted(mapping, key=lambda name: (-len(name), name))
    )
    pattern = re.compile(
        rf"(?<![{TOKEN_BOUNDARY}])({alternatives})(?![{TOKEN_BOUNDARY}])"
    )
    errors: list[str] = []
    try:
        manifest_path = manifest.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
    except ValueError:
        manifest_path = None
    for path in files:
        if path == manifest_path or path.startswith(OLD_PATH_EXCLUDED_PREFIXES):
            continue
        text = read_utf8(path)
        if text is None:
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            for match in pattern.finditer(line):
                old_name = match.group(1)
                errors.append(
                    f"{path}:{line_number}: obsolete filename {old_name}; "
                    f"use {mapping[old_name]}"
                )
    return errors


def run(manifest: Path | None) -> int:
    tracked_files = git_files()
    untracked_files = git_files("--others", "--exclude-standard")
    files = source_files(tracked_files, untracked_files)
    text_files = text_audit_files(tracked_files, untracked_files)
    directory_cache: dict[Path, dict[str, list[str]]] = {}
    actual_files: list[str] = []
    errors: list[str] = []

    for path in files:
        actual = actual_disk_path(path, directory_cache)
        if actual is None:
            errors.append(f"{path}: repository path is missing or case-ambiguous on disk")
            continue
        actual_files.append(actual)
        if path != actual:
            errors.append(f"{path}: Git path case differs from disk path {actual}")

    for path in actual_files:
        if not path.startswith(SOURCE_ROOTS) or is_generated(path):
            continue
        suffix = PurePosixPath(path).suffix
        if suffix not in {".h", ".cpp"}:
            continue
        name = PurePosixPath(path).name
        if PASCAL_FILENAME.fullmatch(name) is None:
            errors.append(
                f"{path}: project-owned C++ filename must be underscore-free PascalCase"
            )

    casefold_suffixes, migration_suffixes, full_casefold = build_suffix_indexes(actual_files)
    for path in actual_files:
        if not path.startswith(INCLUDE_SCAN_ROOTS):
            continue
        if path.startswith("HarkenEngine/ThirdParty/") or is_generated(path):
            continue
        if PurePosixPath(path).suffix.lower() not in CPP_SUFFIXES:
            continue
        text = read_utf8(path)
        if text is None:
            errors.append(f"{path}: cannot read source as UTF-8")
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            match = INCLUDE_LINE.match(line)
            if match is None:
                continue
            delimiter, token = match.groups()
            expected, ambiguous = resolve_include(
                path,
                delimiter,
                token,
                casefold_suffixes,
                migration_suffixes,
                full_casefold,
            )
            if ambiguous:
                errors.append(f"{path}:{line_number}: ambiguous project include {token}")
                continue
            if expected is None:
                if token.replace("\\", "/").startswith(PROJECT_HEADER_PREFIXES):
                    errors.append(f"{path}:{line_number}: unresolved project include {token}")
                continue
            if token != expected:
                errors.append(
                    f"{path}:{line_number}: include path {token} must match disk path {expected}"
                )

    if manifest is not None:
        errors.extend(audit_old_paths(text_files, manifest))

    if errors:
        for error in sorted(set(errors)):
            print(error)
        print(f"C++ filename/include audit failed with {len(set(errors))} error(s).")
        return 1
    print("C++ filename/include audit completed with no findings.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--migration-manifest",
        type=Path,
        help="Optional old_path/new_path TSV used to reject obsolete filename references.",
    )
    args = parser.parse_args()
    manifest = args.migration_manifest
    if manifest is not None and not manifest.is_absolute():
        manifest = REPO_ROOT / manifest
    try:
        return run(manifest)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"C++ filename/include audit could not run: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
