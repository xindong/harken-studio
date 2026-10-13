#!/usr/bin/env python3
"""Run the C++ function/method naming gate on an explicit first-party scope.

Example for the Batch 1 declaration while parsing all Runtime test translation
units that may include it:

  py -3 .scripts/cpp_naming_check.py --scope HarkenEngine/Test \
      --diagnostic-path HarkenEngine/Test/Common/TestUtil.h
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Iterable

from build_tooling_compile_db import (
    CompilationDatabaseError,
    source_database_provenance,
    tooling_provenance_path,
)
from cpp_source_scope import (
    CPP_SOURCE_SUFFIXES,
    eligible_translation_units,
    is_excluded_parts,
    membership_details,
    relative_parts,
    sorted_paths,
    translation_unit_scope,
)


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE_DB = REPO_ROOT / "build-nc" / "compile_commands.json"
DEFAULT_TOOLING_DB = REPO_ROOT / ".temp" / "clang-tidy" / "compile_commands.json"
DEFAULT_CONFIG = REPO_ROOT / ".clang-tidy"
DEDUPE_SCRIPT = REPO_ROOT / ".scripts" / "build_tooling_compile_db.py"
CONFIGURE_OUTPUT_TIMESTAMP_TOLERANCE_NS = 1_000_000_000
CPP_DIAGNOSTIC_SUFFIXES = CPP_SOURCE_SUFFIXES | frozenset(
    {".h", ".hh", ".hpp", ".hxx", ".inl"}
)


class NamingCheckError(RuntimeError):
    pass


def _resolved(value: str | Path, root: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = root / path
    return path.resolve(strict=False)


def _relative_kind(path: Path, root: Path) -> str | None:
    scope = translation_unit_scope(path, root)
    if scope in {"runtime", "tooling"}:
        return "engine"
    if scope in {"runtime_tests", "runtime_benchmarks"}:
        return "runtime_test"
    if scope == "editor_tests":
        return "editor_test"
    parts = relative_parts(path, root)
    if parts is not None and not is_excluded_parts(parts):
        if parts[:2] == ("harkenengine", "test"):
            return "runtime_test"
        if parts[:2] == ("harkenstudio", "test"):
            return "editor_test"
    return None


def _executable(path: Path) -> bool:
    return path.is_file() and (os.name == "nt" or os.access(path, os.X_OK))


def _environment_value(environment: dict[str, str], name: str) -> str | None:
    folded_name = name.casefold()
    return next(
        (value for key, value in environment.items() if key.casefold() == folded_name),
        None,
    )


def _clang_tidy_candidates(installation: Path) -> tuple[Path, Path]:
    return (
        installation / "VC/Tools/Llvm/x64/bin/clang-tidy.exe",
        installation / "VC/Tools/Llvm/bin/clang-tidy.exe",
    )


def _known_visual_studio_candidates(environment: dict[str, str]) -> list[Path]:
    roots = []
    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        value = _environment_value(environment, variable)
        if value:
            roots.append(Path(value) / "Microsoft Visual Studio" / "2022")
    candidates: list[Path] = []
    for root in roots:
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            installation = root / edition
            candidates.extend(_clang_tidy_candidates(installation))
    return candidates


def _vswhere_installations(environment: dict[str, str]) -> list[Path]:
    candidates = []
    program_files_x86 = _environment_value(environment, "ProgramFiles(x86)")
    if program_files_x86:
        candidates.append(
            Path(program_files_x86)
            / "Microsoft Visual Studio"
            / "Installer"
            / "vswhere.exe"
        )
    located = shutil.which("vswhere")
    if located:
        candidates.append(Path(located))
    vswhere = next((path for path in candidates if _executable(path)), None)
    if vswhere is None:
        return []
    try:
        result = subprocess.run(
            [
                str(vswhere),
                "-all",
                "-products",
                "*",
                "-property",
                "installationPath",
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return []
    if result.returncode != 0:
        return []
    return [Path(line.strip()) for line in result.stdout.splitlines() if line.strip()]


def find_clang_tidy(
    explicit: str | None,
    *,
    environment: dict[str, str] | None = None,
    path_lookup: Callable[[str], str | None] = shutil.which,
    known_candidates: Iterable[Path] | None = None,
    vs_installations: Iterable[Path] | None = None,
) -> Path:
    environment = dict(os.environ if environment is None else environment)
    if explicit:
        explicit_path = Path(explicit).expanduser()
        if _executable(explicit_path):
            return explicit_path.resolve()
        located = path_lookup(explicit)
        if located and _executable(Path(located)):
            return Path(located).resolve()
        raise NamingCheckError(f"explicit clang-tidy was not found: {explicit}")

    located = path_lookup("clang-tidy")
    if located and _executable(Path(located)):
        return Path(located).resolve()

    candidates = list(
        _known_visual_studio_candidates(environment)
        if known_candidates is None
        else known_candidates
    )
    installations = list(
        _vswhere_installations(environment)
        if vs_installations is None
        else vs_installations
    )
    for installation in installations:
        candidates.extend(_clang_tidy_candidates(installation))
    for candidate in candidates:
        if _executable(candidate):
            return candidate.resolve()
    raise NamingCheckError(
        "clang-tidy was not found via --clang-tidy, PATH, Visual Studio LLVM, "
        "or vswhere"
    )


def _read_provenance(path: Path) -> dict[str, object] | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    return document if isinstance(document, dict) else None


def _source_provenance(source_db: Path) -> dict[str, str | int]:
    try:
        return source_database_provenance(source_db)
    except CompilationDatabaseError as error:
        raise NamingCheckError(str(error)) from error


def _require_fresh_source_database(source_db: Path) -> None:
    markers = [
        path
        for path in (
            source_db.parent / "CMakeCache.txt",
            source_db.parent / "build.ninja",
        )
        if path.is_file()
    ]
    if not markers:
        raise NamingCheckError(
            f"CMake configure marker is missing beside {source_db}; rerun CMake "
            "configure to regenerate compile_commands.json"
        )
    marker = max(markers, key=lambda path: path.stat().st_mtime_ns)
    if (
        source_db.stat().st_mtime_ns + CONFIGURE_OUTPUT_TIMESTAMP_TOLERANCE_NS
        < marker.stat().st_mtime_ns
    ):
        raise NamingCheckError(
            f"source compilation database predates {marker}; rerun CMake configure "
            "to regenerate compile_commands.json"
        )


def ensure_tooling_database(source_db: Path, tooling_db: Path) -> None:
    if not source_db.is_file():
        raise NamingCheckError(f"source compilation database is missing: {source_db}")
    _require_fresh_source_database(source_db)
    provenance_file = tooling_provenance_path(tooling_db)
    expected_provenance = _source_provenance(source_db)
    needs_refresh = (
        not tooling_db.is_file()
        or tooling_db.stat().st_mtime_ns < source_db.stat().st_mtime_ns
        or _read_provenance(provenance_file) != expected_provenance
    )
    if needs_refresh:
        if not DEDUPE_SCRIPT.is_file():
            raise NamingCheckError(f"database generator is missing: {DEDUPE_SCRIPT}")
        try:
            result = subprocess.run(
                [
                    sys.executable,
                    str(DEDUPE_SCRIPT),
                    "--input",
                    str(source_db),
                    "--output",
                    str(tooling_db),
                    "--repo-root",
                    str(REPO_ROOT),
                ],
                cwd=REPO_ROOT,
                check=False,
            )
        except OSError as error:
            raise NamingCheckError(
                f"cannot regenerate tooling database: {error}"
            ) from error
        if result.returncode != 0:
            raise NamingCheckError("tooling database regeneration failed")
    if not tooling_db.is_file():
        raise NamingCheckError(f"tooling database is missing: {tooling_db}")
    if tooling_db.stat().st_mtime_ns < source_db.stat().st_mtime_ns:
        raise NamingCheckError("tooling database is older than the source database")
    current_provenance = (
        _source_provenance(source_db) if needs_refresh else expected_provenance
    )
    if _read_provenance(provenance_file) != current_provenance:
        raise NamingCheckError(
            "tooling database provenance does not match the requested source "
            "compilation database"
        )


def load_tooling_files(tooling_db: Path, root: Path) -> list[Path]:
    try:
        document = json.loads(tooling_db.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise NamingCheckError(f"cannot read tooling database: {error}") from error
    if not isinstance(document, list) or not document:
        raise NamingCheckError("tooling database must be a non-empty JSON array")
    files: set[Path] = set()
    for index, entry in enumerate(document):
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise NamingCheckError(f"tooling database entry {index} has no file")
        path = _resolved(entry["file"], root)
        if translation_unit_scope(path, root) is None:
            raise NamingCheckError(f"tooling database contains excluded file: {path}")
        if path.suffix.casefold() not in CPP_SOURCE_SUFFIXES or not path.is_file():
            raise NamingCheckError(f"tooling source is missing or unsupported: {path}")
        files.add(path)
    if not files:
        raise NamingCheckError("tooling database contains no source files")
    return sorted_paths(files)


def select_translation_units(
    scopes: Iterable[str], available: list[Path], root: Path
) -> list[Path]:
    selected: set[Path] = set()
    available_set = set(available)
    inventory: set[Path] | None = None
    for scope in scopes:
        path = _resolved(scope, root)
        if not path.exists():
            raise NamingCheckError(f"scope does not exist: {path}")
        if _relative_kind(path, root) not in {"engine", "runtime_test", "editor_test"}:
            raise NamingCheckError(f"scope is outside the naming gate: {path}")
        if path.is_file():
            if path not in available:
                raise NamingCheckError(f"scope has no tooling command: {path}")
            selected.add(path)
            continue
        if inventory is None:
            inventory = set(eligible_translation_units(root))
        expected = {
            candidate for candidate in inventory if candidate.is_relative_to(path)
        }
        scoped_available = {
            candidate for candidate in available_set if candidate.is_relative_to(path)
        }
        missing = sorted_paths(expected - scoped_available)
        unexpected = sorted_paths(scoped_available - expected)
        if missing or unexpected:
            raise NamingCheckError(
                f"directory scope membership differs for {path}; "
                + membership_details(missing, unexpected, root)
            )
        selected.update(expected)
    if not selected:
        raise NamingCheckError("the requested scope selected no translation units")
    return sorted_paths(selected)


def resolve_diagnostic_paths(paths: Iterable[str], root: Path) -> list[Path]:
    resolved: set[Path] = set()
    for value in paths:
        path = _resolved(value, root)
        if not path.exists():
            raise NamingCheckError(f"diagnostic path does not exist: {path}")
        if _relative_kind(path, root) is None:
            raise NamingCheckError(f"diagnostic path is outside the naming gate: {path}")
        candidates = [path] if path.is_file() else path.rglob("*")
        for candidate in candidates:
            if (
                candidate.is_file()
                and candidate.suffix.casefold() in CPP_DIAGNOSTIC_SUFFIXES
                and _relative_kind(candidate, root) is not None
            ):
                resolved.add(candidate.resolve())
    if not resolved:
        raise NamingCheckError("the requested diagnostic scope contains no C++ files")
    return sorted_paths(resolved)


def _line_count(path: Path) -> int:
    try:
        with path.open("rb") as source:
            chunks = iter(lambda: source.read(65536), b"")
            count = sum(chunk.count(b"\n") for chunk in chunks)
    except OSError as error:
        raise NamingCheckError(f"cannot read diagnostic file {path}: {error}") from error
    return max(1, count + 1)


def _header_filter(paths: Iterable[Path]) -> str:
    headers = [path for path in paths if path.suffix.casefold() not in CPP_SOURCE_SUFFIXES]
    if not headers:
        return "^$"
    patterns = [re.escape(path.as_posix()).replace("/", r"[/\\]") for path in headers]
    return "^(?:" + "|".join(patterns) + ")$"


def build_clang_tidy_command(
    clang_tidy: Path,
    tooling_db: Path,
    config: Path,
    translation_units: list[Path],
    diagnostic_paths: list[Path],
) -> list[str]:
    if not translation_units:
        raise NamingCheckError("no translation units were provided")
    if not diagnostic_paths:
        raise NamingCheckError("no diagnostic paths were provided")
    translation_unit_set = set(translation_units)
    unselected_sources = [
        path
        for path in diagnostic_paths
        if path.suffix.casefold() in CPP_SOURCE_SUFFIXES
        and path not in translation_unit_set
    ]
    if unselected_sources:
        raise NamingCheckError(
            "diagnostic source has no selected tooling command: "
            + ", ".join(str(path) for path in unselected_sources)
        )
    line_filter = [
        {"name": path.as_posix(), "lines": [[1, _line_count(path)]]}
        for path in diagnostic_paths
    ]
    return [
        str(clang_tidy),
        "-p",
        str(tooling_db.parent),
        f"--config-file={config}",
        f"--header-filter={_header_filter(diagnostic_paths)}",
        "--line-filter=" + json.dumps(line_filter, separators=(",", ":")),
        "--quiet",
        *(str(path) for path in translation_units),
    ]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scope",
        action="append",
        required=True,
        help="first-party source file or directory to select from the tooling DB",
    )
    parser.add_argument(
        "--diagnostic-path",
        action="append",
        default=[],
        help=(
            "file or directory whose lines may emit diagnostics; defaults to the "
            "selected translation units"
        ),
    )
    parser.add_argument("--clang-tidy", help="explicit clang-tidy executable")
    parser.add_argument("--source-db", type=Path, default=DEFAULT_SOURCE_DB)
    parser.add_argument("--tooling-db", type=Path, default=DEFAULT_TOOLING_DB)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    source_db = _resolved(arguments.source_db, REPO_ROOT)
    tooling_db = _resolved(arguments.tooling_db, REPO_ROOT)
    config = _resolved(arguments.config, REPO_ROOT)
    try:
        if not config.is_file():
            raise NamingCheckError(f"clang-tidy configuration is missing: {config}")
        clang_tidy = find_clang_tidy(arguments.clang_tidy)
        ensure_tooling_database(source_db, tooling_db)
        available = load_tooling_files(tooling_db, REPO_ROOT)
        translation_units = select_translation_units(
            arguments.scope, available, REPO_ROOT
        )
        diagnostic_paths = (
            resolve_diagnostic_paths(arguments.diagnostic_path, REPO_ROOT)
            if arguments.diagnostic_path
            else translation_units
        )
        command = build_clang_tidy_command(
            clang_tidy, tooling_db, config, translation_units, diagnostic_paths
        )
        result = subprocess.run(command, cwd=REPO_ROOT, check=False)
    except (NamingCheckError, OSError) as error:
        print(f"C++ naming check error: {error}", file=sys.stderr)
        return 2
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
