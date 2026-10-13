#!/usr/bin/env python3
"""Build a deterministic Debug-only compilation database for naming tooling."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path, PurePosixPath
from typing import Any

from cpp_source_scope import (
    TRANSLATION_UNIT_SCOPES,
    eligible_translation_units,
    membership_details,
    path_sort_key,
    translation_unit_scope,
)


REPO_ROOT = Path(__file__).resolve().parents[1]
REQUIRED_SCOPES = TRANSLATION_UNIT_SCOPES
PROVENANCE_SCHEMA_VERSION = 1
CONFIG_DEFINE = re.compile(
    r"CMAKE_INTDIR=(?:\\?\")?(Debug|Release|RelWithDebInfo|MinSizeRel)(?:\\?\")?",
    re.IGNORECASE,
)
TARGET_FROM_OUTPUT = re.compile(
    r"(?:^|/)CMakeFiles/([^/]+)\.dir/Debug(?:/|$)", re.IGNORECASE
)
CONFIG_FROM_OUTPUT = re.compile(
    r"(?:^|/)CMakeFiles/[^/]+\.dir/"
    r"(Debug|Release|RelWithDebInfo|MinSizeRel)(?:/|$)",
    re.IGNORECASE,
)


class CompilationDatabaseError(RuntimeError):
    pass


def _absolute_path(value: str, base: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    return path.resolve(strict=False)


def _normalized_output(value: str) -> str:
    return posixpath.normpath(value.replace("\\", "/"))


def _repo_relative(path: Path, repo_root: Path) -> PurePosixPath | None:
    try:
        return PurePosixPath(path.relative_to(repo_root).as_posix())
    except ValueError:
        return None


def tooling_provenance_path(output_db: Path) -> Path:
    return output_db.with_name(output_db.name + ".provenance.json")


def source_database_provenance(
    source_db: Path, content: bytes | None = None
) -> dict[str, str | int]:
    source_db = source_db.resolve(strict=False)
    if content is None:
        try:
            content = source_db.read_bytes()
        except OSError as error:
            raise CompilationDatabaseError(
                f"cannot read compilation database {source_db}: {error}"
            ) from error
    return {
        "schema_version": PROVENANCE_SCHEMA_VERSION,
        "source_db": source_db.as_posix(),
        "source_sha256": hashlib.sha256(content).hexdigest(),
    }


def _configuration(entry: dict[str, str]) -> str | None:
    command_match = CONFIG_DEFINE.search(entry["command"])
    command_config = command_match.group(1).casefold() if command_match else None
    output_match = CONFIG_FROM_OUTPUT.search(entry["output"])
    output_config = output_match.group(1).casefold() if output_match else None
    if command_config and output_config and command_config != output_config:
        raise CompilationDatabaseError(
            "command and output disagree about configuration for " + entry["file"]
        )
    return command_config or output_config


def _target(entry: dict[str, str]) -> str | None:
    match = TARGET_FROM_OUTPUT.search(entry["output"])
    return match.group(1).casefold() if match else None


def _preferred_target(scope: str, relative: PurePosixPath) -> str:
    """Return the owning target selected for known multi-target source files."""
    folded = tuple(part.casefold() for part in relative.parts)
    if scope == "runtime":
        return "harken_core"
    if scope == "runtime_tests":
        return "harken_runtime_tests"
    if scope == "runtime_benchmarks":
        if relative.name.casefold() == "meterscanbenchmark.cpp":
            return "harken_meter_benchmark"
        return "harken_benchmarks"
    if scope == "editor_tests":
        return "harken_editor_tests"
    if "remote" in folded:
        return "harken_remote"
    if "host" in folded:
        return "harken_host"
    if "packc" in folded:
        return "harken_packc"
    raise CompilationDatabaseError(
        f"no target precedence is defined for duplicated tooling file {relative}"
    )


def _signature(entry: dict[str, str]) -> tuple[str, str, str]:
    return entry["directory"], entry["command"], entry["output"]


def _choose_entry(
    file_path: Path,
    entries: list[dict[str, str]],
    scope: str,
    repo_root: Path,
) -> dict[str, str]:
    signatures: dict[tuple[str, str, str], dict[str, str]] = {}
    for entry in entries:
        signatures.setdefault(_signature(entry), entry)
    unique = list(signatures.values())
    if len(unique) == 1:
        return unique[0]

    relative = _repo_relative(file_path, repo_root)
    if relative is None:
        raise CompilationDatabaseError(f"source is outside repository: {file_path}")
    preferred = _preferred_target(scope, relative)
    preferred_entries = [entry for entry in unique if _target(entry) == preferred]
    preferred_signatures = {_signature(entry) for entry in preferred_entries}
    if len(preferred_signatures) != 1:
        targets = sorted({_target(entry) or "<unknown>" for entry in unique})
        raise CompilationDatabaseError(
            f"ambiguous Debug commands for {relative}; expected one {preferred} "
            f"command, found targets: {', '.join(targets)}"
        )
    return preferred_entries[0]


def _load_entries(
    source_db: Path, repo_root: Path
) -> tuple[list[dict[str, str]], bytes]:
    try:
        content = source_db.read_bytes()
        document: Any = json.loads(content.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CompilationDatabaseError(
            f"cannot read compilation database {source_db}: {error}"
        ) from error
    if not isinstance(document, list):
        raise CompilationDatabaseError("compilation database root must be a JSON array")

    selected: list[dict[str, str]] = []
    for index, raw in enumerate(document):
        if not isinstance(raw, dict):
            raise CompilationDatabaseError(f"entry {index} is not an object")
        missing = {
            field
            for field in ("directory", "command", "file", "output")
            if not isinstance(raw.get(field), str) or not raw[field]
        }
        if missing:
            raise CompilationDatabaseError(
                f"entry {index} is missing string fields: {', '.join(sorted(missing))}"
            )
        directory = _absolute_path(raw["directory"], source_db.parent)
        file_path = _absolute_path(raw["file"], directory)
        scope = translation_unit_scope(file_path, repo_root)
        if scope is None:
            continue
        normalized = {
            "directory": directory.as_posix(),
            "command": raw["command"],
            "file": file_path.as_posix(),
            "output": _normalized_output(raw["output"]),
            "_scope": scope,
        }
        if _configuration(normalized) == "debug":
            selected.append(normalized)
    return selected, content


def _path_key(path: Path) -> str:
    return os.path.normcase(str(path.resolve(strict=False)))


def _write_json_atomic(path: Path, document: Any) -> None:
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=path.parent,
            prefix=path.name + ".",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            json.dump(document, temporary, indent=2, ensure_ascii=False)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, path)
    except OSError as error:
        if temporary_name:
            try:
                Path(temporary_name).unlink(missing_ok=True)
            except OSError:
                # Cleanup is best-effort and must not hide the primary write error.
                pass
        raise CompilationDatabaseError(
            f"cannot atomically write {path}: {error}"
        ) from error


def create_tooling_database(
    source_db: Path, output_db: Path, repo_root: Path = REPO_ROOT
) -> list[dict[str, str]]:
    source_db = source_db.resolve(strict=False)
    output_db = output_db.resolve(strict=False)
    repo_root = repo_root.resolve(strict=False)
    if not source_db.is_file():
        raise CompilationDatabaseError(f"source database does not exist: {source_db}")
    if source_db == output_db:
        raise CompilationDatabaseError("source and output databases must be different")

    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    display_paths: dict[str, Path] = {}
    loaded_entries, source_content = _load_entries(source_db, repo_root)
    for entry in loaded_entries:
        path = Path(entry["file"])
        key = _path_key(path)
        grouped[key].append(entry)
        display_paths[key] = path

    inventory = eligible_translation_units(repo_root)
    inventory_paths = {_path_key(path): path for path in inventory}
    missing_keys = sorted(
        inventory_paths.keys() - grouped.keys(),
        key=lambda key: path_sort_key(inventory_paths[key]),
    )
    unexpected_keys = sorted(
        grouped.keys() - inventory_paths.keys(),
        key=lambda key: path_sort_key(display_paths[key]),
    )
    if missing_keys or unexpected_keys:
        raise CompilationDatabaseError(
            "Debug source membership differs from eligible inventory; "
            + membership_details(
                [inventory_paths[key] for key in missing_keys],
                [display_paths[key] for key in unexpected_keys],
                repo_root,
            )
        )
    if not grouped:
        raise CompilationDatabaseError("no relevant Debug compilation commands found")

    result: list[dict[str, str]] = []
    covered: set[str] = set()
    for key in sorted(
        grouped, key=lambda item: path_sort_key(display_paths[item])
    ):
        entries = grouped[key]
        scope = entries[0]["_scope"]
        if any(entry["_scope"] != scope for entry in entries):
            raise CompilationDatabaseError(
                f"source classified into multiple scopes: {display_paths[key]}"
            )
        chosen = _choose_entry(display_paths[key], entries, scope, repo_root)
        covered.add(scope)
        result.append({
            field: chosen[field]
            for field in ("directory", "command", "file", "output")
        })

    missing_scopes = sorted(REQUIRED_SCOPES - covered)
    if missing_scopes:
        raise CompilationDatabaseError(
            "required Debug scope is absent: " + ", ".join(missing_scopes)
        )

    try:
        output_db.parent.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise CompilationDatabaseError(
            f"cannot create output directory {output_db.parent}: {error}"
        ) from error
    _write_json_atomic(output_db, result)
    _write_json_atomic(
        tooling_provenance_path(output_db),
        source_database_provenance(source_db, source_content),
    )
    return result


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        default=REPO_ROOT / "build-nc" / "compile_commands.json",
        help="source multi-configuration compilation database",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / ".temp" / "clang-tidy" / "compile_commands.json",
        help="Debug-only tooling database to replace atomically",
    )
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        entries = create_tooling_database(
            arguments.input, arguments.output, arguments.repo_root
        )
    except CompilationDatabaseError as error:
        print(f"tooling compilation database error: {error}", file=sys.stderr)
        return 1
    print(f"wrote {len(entries)} Debug commands to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
