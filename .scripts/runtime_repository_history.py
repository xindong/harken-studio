from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        fail(
            "command failed (%d): %s\n%s\n%s"
            % (
                result.returncode,
                subprocess.list2cmdline(command),
                result.stdout,
                result.stderr,
            )
        )
    return result


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail("cannot import module: %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def git_info(root: Path) -> dict[str, Any]:
    commit = run(["git", "rev-parse", "HEAD"], cwd=root).stdout.strip()
    status = run(
        [
            "git",
            "status",
            "--porcelain=v1",
            "--untracked-files=no",
        ],
        cwd=root,
    ).stdout.strip()
    return {
        "root": str(root),
        "commit": commit,
        "trackedWorktreeClean": not status,
        "trackedStatus": status.splitlines(),
    }


def find_dumpbin() -> Path:
    vs_root = Path(
        r"C:\Program Files\Microsoft Visual Studio\2022\Community"
    )
    candidates = sorted(
        vs_root.glob(
            "VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
        ),
        reverse=True,
    )
    if not candidates:
        fail("dumpbin.exe was not found")
    return candidates[0]


def public_exports(
    dumpbin: Path, dll: Path, abi_module: Any
) -> tuple[list[str], str]:
    output = run([str(dumpbin), "/exports", str(dll)]).stdout
    exports = sorted(
        name
        for name in abi_module.parse_dumpbin_exports(output)
        if name.startswith("Vn_")
    )
    digest = hashlib.sha256(
        ("\n".join(exports) + "\n").encode("utf-8")
    ).hexdigest()
    return exports, digest


def verify_no_runtime_import(dumpbin: Path, client: Path) -> None:
    output = run([str(dumpbin), "/imports", str(client)]).stdout
    if "harkencore.dll" in output.lower():
        fail("history client imports a runtime DLL: %s" % client)


def verify_x64_pe(dumpbin: Path, binary: Path) -> str:
    output = run([str(dumpbin), "/headers", str(binary)]).stdout
    if "machine (x64)" not in output.lower():
        fail("binary is not an x64 PE image: %s" % binary)
    return "x86_64"


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def require_old_subset(
    label: str,
    old: dict[str, Any],
    current: dict[str, Any],
) -> None:
    if not isinstance(old, dict) or not isinstance(current, dict):
        fail("%s comparison requires dictionaries" % label)
    for key, value in old.items():
        if key not in current:
            fail("%s removed old key %s" % (label, key))
        if current[key] != value:
            fail("%s changed old key %s" % (label, key))


def index_functions(
    label: str,
    functions: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    if not isinstance(functions, list):
        fail("%s functions must be a list" % label)
    indexed: dict[str, dict[str, Any]] = {}
    for function in functions:
        if not isinstance(function, dict):
            fail("%s function entry must be an object" % label)
        name = function.get("name")
        if not isinstance(name, str) or not name:
            fail("%s function entry has no valid name" % label)
        if name in indexed:
            fail("%s contains duplicate function %s" % (label, name))
        indexed[name] = function
    return indexed


def verify_header_prefixes(
    old_root: Path,
    current_root: Path,
    abi_module: Any,
) -> dict[str, Any]:
    relative = Path(
        "HarkenEngine/Source/Runtime/api/include/harken/Harken.h"
    )
    old = abi_module.parse_header_contract(
        (old_root / relative).read_text(encoding="utf-8")
    )
    current = abi_module.parse_header_contract(
        (current_root / relative).read_text(encoding="utf-8")
    )
    old_functions = index_functions("old", old["functions"])
    current_functions = index_functions(
        "current", current["functions"]
    )
    require_old_subset(
        "functions", old_functions, current_functions
    )
    for key in (
        "defines",
        "enum_values",
        "scalar_typedefs",
        "callbacks",
    ):
        require_old_subset(key, old[key], current[key])

    for name, old_fields in old["structs"].items():
        if name not in current["structs"]:
            fail("current header removed struct %s" % name)
        current_fields = current["structs"][name]
        if current_fields[: len(old_fields)] != old_fields:
            fail("current header changed prefix of %s" % name)

    added_functions = sorted(
        set(current_functions) - set(old_functions)
    )
    if added_functions != ["Vn_ApplyBusTopology"]:
        fail("unexpected public function delta: %r" % added_functions)
    return {
        "oldFunctionCount": len(old["functions"]),
        "currentFunctionCount": len(current["functions"]),
        "addedFunctions": added_functions,
        "oldStructCount": len(old["structs"]),
        "currentStructCount": len(current["structs"]),
        "status": "PASS",
    }


def verify_wire_layouts(
    old_root: Path,
    current_root: Path,
    wire_module: Any,
) -> dict[str, Any]:
    relative = Path(
        "HarkenEngine/Source/Tooling/Remote/wire/"
        "harken_wire_table.json"
    )
    old_table = load_json(old_root / relative)
    current_table = load_json(current_root / relative)
    old_layouts = wire_module.compute_layouts(old_table["types"])
    current_layouts = wire_module.compute_layouts(
        current_table["types"]
    )

    for name, old_layout in old_layouts.items():
        if name not in current_layouts:
            fail("current wire model removed type %s" % name)
        current_layout = current_layouts[name]
        current_offsets = dict(current_layout.fields)
        for field, offset in old_layout.fields:
            if current_offsets.get(field) != offset:
                fail(
                    "wire offset changed for %s.%s" % (name, field)
                )

    expected_sizes = {
        "oldConfig": old_layouts["VnConfig"].size,
        "currentConfig": current_layouts["VnConfig"].size,
        "oldSnapshot": old_layouts["VnSnapshot"].size,
        "currentSnapshot": current_layouts["VnSnapshot"].size,
    }
    if expected_sizes != {
        "oldConfig": 264,
        "currentConfig": 280,
        "oldSnapshot": 3536,
        "currentSnapshot": 4008,
    }:
        fail("unexpected history layout sizes: %r" % expected_sizes)

    old_opcodes = {
        entry["name"]: entry.get("opcode")
        for entry in old_table["functions"]
    }
    current_opcodes = {
        entry["name"]: entry.get("opcode")
        for entry in current_table["functions"]
    }
    require_old_subset("wire opcode", old_opcodes, current_opcodes)
    return {**expected_sizes, "status": "PASS"}


def file_record(path: Path) -> dict[str, Any]:
    if not path.is_file():
        fail("required file does not exist: %s" % path)
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def command_record(
    command: list[str],
    cwd: Path,
    result: subprocess.CompletedProcess[str],
) -> dict[str, Any]:
    stdout = result.stdout
    stderr = result.stderr
    return {
        "argv": command,
        "cwd": str(cwd),
        "returnCode": result.returncode,
        "stdout": stdout,
        "stdoutSha256": hashlib.sha256(
            stdout.encode("utf-8")
        ).hexdigest(),
        "stderr": stderr,
        "stderrSha256": hashlib.sha256(
            stderr.encode("utf-8")
        ).hexdigest(),
    }


def cache_values(cache: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in cache.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        key = declaration.split(":", 1)[0]
        values[key] = value
    return values


def cache_record(
    build: Path,
    expected: dict[str, str] | None = None,
    path_keys: set[str] | None = None,
) -> dict[str, Any]:
    cache = build / "CMakeCache.txt"
    record = file_record(cache)
    values = cache_values(cache)
    if expected is not None:
        path_keys = path_keys or set()
        for key, expected_value in expected.items():
            actual = values.get(key)
            if actual is None:
                fail("CMake cache is missing %s: %s" % (key, cache))
            if key in path_keys:
                actual_path = Path(actual).resolve()
                expected_path = Path(expected_value).resolve()
                if actual_path != expected_path:
                    fail(
                        "CMake cache %s path mismatch: %s != %s"
                        % (key, actual_path, expected_path)
                    )
            elif actual != expected_value:
                fail(
                    "CMake cache %s mismatch: %r != %r"
                    % (key, actual, expected_value)
                )
    reported_keys = (
        "CMAKE_GENERATOR",
        "CMAKE_GENERATOR_PLATFORM",
        "CMAKE_GENERATOR_TOOLSET",
        "CMAKE_HOME_DIRECTORY",
        "HARKEN_RUNTIME_ONLY",
        "HARKEN_BUILD_CORE",
        "HARKEN_BUILD_EDITOR",
        "HARKEN_BUILD_HUB",
        "HARKEN_BUILD_TOOLS",
        "HARKEN_BUILD_SHARED",
        "HARKEN_BUILD_RUNTIME_UNIT_TESTS",
        "HARKEN_BUILD_EDITOR_UNIT_TESTS",
        "HARKEN_BUILD_HUB_UNIT_TESTS",
        "HARKEN_BUILD_RUNTIME_BENCHMARKS",
        "HARKEN_BUILD_RT_QUALIFICATION",
        "HARKEN_PUBLIC_INCLUDE_DIR",
        "HARKEN_BINDING_PIN_DIR",
    )
    return {
        **record,
        "values": {
            key: values[key]
            for key in reported_keys
            if key in values
        },
    }


def tracked_source_index(root: Path) -> dict[str, Any]:
    tracked = run(
        ["git", "ls-files", "-z"], cwd=root
    ).stdout.split("\0")
    relative_paths = sorted(path for path in tracked if path)
    files: dict[str, Any] = {}
    digest = hashlib.sha256()
    total_bytes = 0
    for relative in relative_paths:
        path = root / relative
        if not path.is_file():
            fail("tracked source is not a regular file: %s" % path)
        size = path.stat().st_size
        file_hash = sha256(path)
        total_bytes += size
        files[relative] = {
            "bytes": size,
            "sha256": file_hash,
        }
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(size).encode("ascii"))
        digest.update(b"\0")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\n")
    tree = run(
        ["git", "rev-parse", "HEAD^{tree}"], cwd=root
    ).stdout.strip()
    return {
        "gitTree": tree,
        "fileCount": len(files),
        "totalBytes": total_bytes,
        "indexSha256": digest.hexdigest(),
        "files": files,
    }


def parse_cmake_set_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    pattern = re.compile(
        r'^set\(([A-Za-z0-9_]+)\s+"?([^"\)]*)"?\)$'
    )
    for line in path.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        match = pattern.match(line.strip())
        if match:
            values[match.group(1)] = match.group(2)
    return values


def single_file(root: Path, pattern: str) -> Path:
    matches = sorted(root.glob(pattern))
    if len(matches) != 1:
        fail(
            "expected one %s under %s, found %d"
            % (pattern, root, len(matches))
        )
    return matches[0]


def toolchain_record(build: Path) -> dict[str, Any]:
    compiler_file = single_file(
        build / "CMakeFiles", "*/CMakeCXXCompiler.cmake"
    )
    system_file = single_file(
        build / "CMakeFiles", "*/CMakeSystem.cmake"
    )
    project_file = build / "harken_core_shared.vcxproj"
    compiler_values = parse_cmake_set_file(compiler_file)
    system_values = parse_cmake_set_file(system_file)
    required_compiler = {
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_COMPILER_ARCHITECTURE_ID",
        "CMAKE_CXX_COMPILER_LINKER",
        "CMAKE_CXX_COMPILER_LINKER_ID",
        "CMAKE_CXX_COMPILER_LINKER_VERSION",
    }
    missing = sorted(required_compiler - set(compiler_values))
    if missing:
        fail("compiler identity is incomplete: %r" % missing)
    xml_root = ET.parse(project_file).getroot()
    project_values: dict[str, set[str]] = {
        "PlatformToolset": set(),
        "WindowsTargetPlatformVersion": set(),
    }
    for element in xml_root.iter():
        name = element.tag.rsplit("}", 1)[-1]
        if name in project_values and element.text:
            project_values[name].add(element.text.strip())
    normalized_project: dict[str, str] = {}
    for name, values in project_values.items():
        if len(values) != 1:
            fail(
                "generated project has ambiguous %s: %r"
                % (name, sorted(values))
            )
        normalized_project[name] = next(iter(values))
    compiler = Path(
        compiler_values["CMAKE_CXX_COMPILER"]
    ).resolve()
    linker = Path(
        compiler_values["CMAKE_CXX_COMPILER_LINKER"]
    ).resolve()
    return {
        "compilerFile": file_record(compiler_file),
        "systemFile": file_record(system_file),
        "projectFile": file_record(project_file),
        "compiler": {
            **{
                key: compiler_values[key]
                for key in sorted(required_compiler)
            },
            "binary": file_record(compiler),
            "linkerBinary": file_record(linker),
        },
        "system": system_values,
        "generatedProject": normalized_project,
    }


def runtime_build_options(root: Path) -> dict[str, str]:
    options = {
        "HARKEN_BUILD_SHARED": "ON",
        "HARKEN_BUILD_RUNTIME_UNIT_TESTS": "OFF",
        "HARKEN_BUILD_EDITOR_UNIT_TESTS": "OFF",
        "HARKEN_BUILD_HUB": "OFF",
        "HARKEN_BUILD_RUNTIME_BENCHMARKS": "OFF",
    }
    cmake_source = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if re.search(r"\boption\s*\(\s*HARKEN_BUILD_CORE\b", cmake_source):
        options.update({
            "HARKEN_BUILD_CORE": "ON",
            "HARKEN_BUILD_EDITOR": "OFF",
            "HARKEN_BUILD_TOOLS": "OFF",
            "HARKEN_BUILD_HUB_UNIT_TESTS": "OFF",
            "HARKEN_BUILD_RT_QUALIFICATION": "OFF",
        })
    else:
        options["HARKEN_RUNTIME_ONLY"] = "ON"
    return options


def runtime_configure_command(root: Path, build: Path) -> list[str]:
    return [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build),
        "-G",
        "Visual Studio 17 2022",
        "-A",
        "x64",
        "-U",
        "HARKEN_RUNTIME_ONLY",
        *(f"-D{key}={value}" for key, value in runtime_build_options(root).items()),
    ]


def rebuild_runtime(
    root: Path, build: Path, configuration: str
) -> dict[str, Any]:
    configure_command = runtime_configure_command(root, build)
    configure = run(configure_command, cwd=root)
    build_command = [
        "cmake",
        "--build",
        str(build),
        "--config",
        configuration,
        "--target",
        "harken_core_shared",
        "--clean-first",
        "--parallel",
    ]
    compiled = run(build_command, cwd=root)
    cache = cache_record(
        build,
        {
            "CMAKE_HOME_DIRECTORY": str(root),
            "CMAKE_GENERATOR": "Visual Studio 17 2022",
            "CMAKE_GENERATOR_PLATFORM": "x64",
            **runtime_build_options(root),
        },
        {"CMAKE_HOME_DIRECTORY"},
    )
    return {
        "configure": command_record(
            configure_command, root, configure
        ),
        "compile": command_record(build_command, root, compiled),
        "cache": cache,
        "toolchain": toolchain_record(build),
    }


def rebuild_client(
    source: Path,
    build: Path,
    include: Path,
    binding_pin: Path,
    executable: Path,
    configuration: str,
) -> dict[str, Any]:
    expected_executable = (
        build / configuration / "harken_abi_history_client.exe"
    ).resolve()
    if executable != expected_executable:
        fail(
            "history client path does not match its build: %s"
            % executable
        )
    configure_command = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Visual Studio 17 2022",
        "-A",
        "x64",
        "-DHARKEN_PUBLIC_INCLUDE_DIR=%s" % include,
        "-DHARKEN_BINDING_PIN_DIR=%s" % binding_pin,
    ]
    configure = run(configure_command, cwd=source)
    build_command = [
        "cmake",
        "--build",
        str(build),
        "--config",
        configuration,
        "--target",
        "harken_abi_history_client",
        "--clean-first",
        "--parallel",
    ]
    compiled = run(build_command, cwd=source)
    cache = cache_record(
        build,
        {
            "CMAKE_HOME_DIRECTORY": str(source),
            "CMAKE_GENERATOR": "Visual Studio 17 2022",
            "CMAKE_GENERATOR_PLATFORM": "x64",
            "HARKEN_PUBLIC_INCLUDE_DIR": str(include),
            "HARKEN_BINDING_PIN_DIR": str(binding_pin),
        },
        {
            "CMAKE_HOME_DIRECTORY",
            "HARKEN_PUBLIC_INCLUDE_DIR",
            "HARKEN_BINDING_PIN_DIR",
        },
    )
    return {
        "configure": command_record(
            configure_command, source, configure
        ),
        "compile": command_record(build_command, source, compiled),
        "cache": cache,
    }


def expected_managed_project(old_root: Path) -> Path:
    return (
        old_root / "HarkenTest/Binding/AbiFidelity.csproj"
    ).resolve()


def managed_output_index(output: Path) -> dict[str, Any]:
    if not output.is_dir():
        fail("managed output directory does not exist: %s" % output)
    files = sorted(path for path in output.iterdir() if path.is_file())
    if not files:
        fail("managed output directory is empty: %s" % output)
    return {
        path.name: file_record(path)
        for path in files
    }


def verify_generators(root: Path, *, include_manifest: bool) -> list[str]:
    scripts = [
        Path(
            "HarkenEngine/Source/Tooling/Binding/gen_binding.py"
        ),
        Path(
            "HarkenEngine/Source/Tooling/Remote/wire/gen_wire.py"
        ),
    ]
    if include_manifest:
        scripts.append(
            Path(
                "HarkenEngine/Source/Tooling/Abi/"
                "gen_abi_manifest.py"
            )
        )
    checked: list[str] = []
    for relative in scripts:
        run(
            [sys.executable, str(root / relative), "--check"],
            cwd=root,
        )
        checked.append(str(relative))
    return checked


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--old-root", type=Path, required=True)
    parser.add_argument("--current-root", type=Path, required=True)
    parser.add_argument("--old-build", type=Path, required=True)
    parser.add_argument("--current-build", type=Path, required=True)
    parser.add_argument("--old-client", type=Path, required=True)
    parser.add_argument("--current-client", type=Path, required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--old-commit", required=True)
    parser.add_argument("--current-commit", required=True)
    parser.add_argument(
        "--old-managed-project", type=Path, required=True
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    started = datetime.now(timezone.utc)
    old_root = args.old_root.resolve()
    current_root = args.current_root.resolve()
    old_build = args.old_build.resolve()
    current_build = args.current_build.resolve()
    old_client = args.old_client.resolve()
    current_client = args.current_client.resolve()
    old_managed_project = args.old_managed_project.resolve()
    if args.configuration != "Release":
        fail("repository-history fixture requires Release configuration")
    required_managed_project = expected_managed_project(old_root)
    if old_managed_project != required_managed_project:
        fail(
            "old managed project must be %s"
            % required_managed_project
        )
    file_record(old_managed_project)

    old_git = git_info(old_root)
    current_git = git_info(current_root)
    if old_git["commit"] != args.old_commit:
        fail("old commit does not match requested fixture")
    if current_git["commit"] != args.current_commit:
        fail("current commit does not match requested fixture")
    if not old_git["trackedWorktreeClean"]:
        fail("old worktree has tracked modifications")
    if not current_git["trackedWorktreeClean"]:
        fail("current worktree has tracked modifications")
    old_source_index = tracked_source_index(old_root)
    current_source_index = tracked_source_index(current_root)
    old_generator_checks = verify_generators(
        old_root, include_manifest=False
    )
    current_generator_checks = verify_generators(
        current_root, include_manifest=True
    )

    current_abi_script = (
        current_root
        / "HarkenEngine/Source/Tooling/Abi/gen_abi_manifest.py"
    )
    abi_module = load_module(
        "history_current_abi", current_abi_script
    )
    wire_module = load_module(
        "history_current_wire",
        current_root
        / "HarkenEngine/Source/Tooling/Remote/wire/gen_wire.py",
    )

    header_result = verify_header_prefixes(
        old_root, current_root, abi_module
    )
    layout_result = verify_wire_layouts(
        old_root, current_root, wire_module
    )

    config = args.configuration
    old_build_evidence = rebuild_runtime(
        old_root, old_build, config
    )
    current_build_evidence = rebuild_runtime(
        current_root, current_build, config
    )
    old_toolchain = old_build_evidence["toolchain"]
    current_toolchain = current_build_evidence["toolchain"]
    for key in (
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_COMPILER_ARCHITECTURE_ID",
        "CMAKE_CXX_COMPILER_LINKER_ID",
        "CMAKE_CXX_COMPILER_LINKER_VERSION",
    ):
        if (
            old_toolchain["compiler"][key]
            != current_toolchain["compiler"][key]
        ):
            fail("old/current toolchain mismatch for %s" % key)
    if (
        old_toolchain["generatedProject"]
        != current_toolchain["generatedProject"]
    ):
        fail("old/current generated project toolchain mismatch")

    client_source = (
        current_root / "HarkenEngine/Test/E2E/AbiHistoryClient"
    )
    old_client_build = old_client.parent.parent
    current_client_build = current_client.parent.parent
    old_client_build_evidence = rebuild_client(
        client_source,
        old_client_build,
        old_root / "HarkenEngine/Source/Runtime/api/include",
        old_root
        / "HarkenEngine/Source/Tooling/Binding/generated",
        old_client,
        config,
    )
    current_client_build_evidence = rebuild_client(
        client_source,
        current_client_build,
        current_root / "HarkenEngine/Source/Runtime/api/include",
        current_root
        / "HarkenEngine/Source/Tooling/Binding/generated",
        current_client,
        config,
    )

    old_dll = (
        old_build / "bin" / config / "HarkenCore.dll"
    ).resolve()
    current_dll = (
        current_build / "bin" / config / "HarkenCore.dll"
    ).resolve()
    dumpbin = find_dumpbin()
    old_exports, old_export_hash = public_exports(
        dumpbin, old_dll, abi_module
    )
    current_exports, current_export_hash = public_exports(
        dumpbin, current_dll, abi_module
    )

    old_table = load_json(
        old_root
        / "HarkenEngine/Source/Tooling/Remote/wire/"
        "harken_wire_table.json"
    )
    current_table = load_json(
        current_root
        / "HarkenEngine/Source/Tooling/Remote/wire/"
        "harken_wire_table.json"
    )
    expected_old = sorted(
        entry["name"] for entry in old_table["functions"]
    )
    expected_current = sorted(
        entry["name"] for entry in current_table["functions"]
    )
    if old_exports != expected_old:
        fail("old DLL exports do not match old wire table")
    if current_exports != expected_current:
        fail("current DLL exports do not match current wire table")
    if sorted(set(current_exports) - set(old_exports)) != [
        "Vn_ApplyBusTopology"
    ]:
        fail("unexpected binary export delta")

    verify_no_runtime_import(dumpbin, old_client)
    verify_no_runtime_import(dumpbin, current_client)
    old_dll_machine = verify_x64_pe(dumpbin, old_dll)
    current_dll_machine = verify_x64_pe(dumpbin, current_dll)
    old_client_machine = verify_x64_pe(dumpbin, old_client)
    current_client_machine = verify_x64_pe(
        dumpbin, current_client
    )
    old_to_new = run(
        [
            str(old_client),
            "old-client-new-runtime",
            str(current_dll),
        ]
    )
    current_to_old = run(
        [
            str(current_client),
            "current-client-old-runtime",
            str(old_dll),
        ]
    )

    # The historical managed client predates the artifact rename. Give only
    # that ABI fixture the new binary under its original loading contract.
    managed_native_dir = current_build / "history-managed-native"
    managed_native_dir.mkdir(parents=True, exist_ok=True)
    managed_native = managed_native_dir / "HarkenCore.dll"
    shutil.copyfile(current_dll, managed_native)
    environment = os.environ.copy()
    environment["HARKEN_NATIVE_DIR"] = str(managed_native_dir)
    managed_build_command = [
        "dotnet",
        "build",
        str(old_managed_project),
        "-c",
        config,
        "--nologo",
        "--no-incremental",
    ]
    managed_build = run(
        managed_build_command,
        cwd=old_root,
        env=environment,
    )
    managed_output_dir = (
        old_managed_project.parent
        / "bin"
        / config
        / "net9.0"
    )
    managed_assembly = (
        managed_output_dir / "HarkenAbiFidelity.dll"
    ).resolve()
    managed_run_command = [
        "dotnet",
        str(managed_assembly),
        "--require-native",
    ]
    managed_run = run(
        managed_run_command,
        cwd=old_root,
        env=environment,
    )
    managed = {
        "result": "PASS",
        "project": file_record(old_managed_project),
        "sourceRoot": str(old_root),
        "relevantEnvironment": {
            "HARKEN_NATIVE_DIR": str(managed_native_dir),
        },
        "historicalFilenameAlias": file_record(managed_native),
        "build": command_record(
            managed_build_command, old_root, managed_build
        ),
        "run": command_record(
            managed_run_command, old_root, managed_run
        ),
        "outputs": managed_output_index(managed_output_dir),
    }

    report = {
        "kind": "harken-runtime-repository-history-compatibility",
        "schemaVersion": 2,
        "classification": "repository-history-not-released",
        "startedUtc": started.isoformat(),
        "finishedUtc": datetime.now(timezone.utc).isoformat(),
        "host": {
            "node": platform.node(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": sys.version,
        },
        "old": {
            **old_git,
            "sources": old_source_index,
            "generatorChecks": old_generator_checks,
            "build": old_build_evidence,
            "dll": {
                **file_record(old_dll),
                "machine": old_dll_machine,
                "publicExportCount": len(old_exports),
                "publicExportSha256": old_export_hash,
                "publicExports": old_exports,
            },
        },
        "current": {
            **current_git,
            "sources": current_source_index,
            "generatorChecks": current_generator_checks,
            "build": current_build_evidence,
            "dll": {
                **file_record(current_dll),
                "machine": current_dll_machine,
                "publicExportCount": len(current_exports),
                "publicExportSha256": current_export_hash,
                "publicExports": current_exports,
            },
        },
        "clients": {
            "source": file_record(
                current_root
                / "HarkenEngine/Test/E2E/AbiHistoryClient/"
                "AbiHistoryClient.cpp"
            ),
            "old": {
                **file_record(old_client),
                "machine": old_client_machine,
                "build": old_client_build_evidence,
            },
            "current": {
                **file_record(current_client),
                "machine": current_client_machine,
                "build": current_client_build_evidence,
            },
            "harkenCoreImportAudit": "PASS",
        },
        "contractDiff": {
            "header": header_result,
            "wireLayout": layout_result,
            "binaryAddedExports": sorted(
                set(current_exports) - set(old_exports)
            ),
            "oldExportsMissingFromCurrent": sorted(
                set(old_exports) - set(current_exports)
            ),
            "status": "PASS",
        },
        "cases": {
            "oldClientToCurrentRuntime": {
                "result": "PASS",
                "stdout": old_to_new.stdout.strip(),
            },
            "currentDefaultToOldRuntime": {
                "result": "EXPECTED_REFUSAL",
                "downlevelResult": "PASS",
                "stdout": current_to_old.stdout.strip(),
            },
            "oldManagedSdkToCurrentRuntime": managed,
        },
        "releaseProvenance": {
            "status": "EXTERNAL_BLOCKED",
            "reason": (
                "No immutable shipped bundle, release tag, signature, "
                "or channel record is available."
            ),
        },
        "tools": {
            "cmake": {
                "version": run(
                    ["cmake", "--version"]
                ).stdout.splitlines()[0],
                "binary": file_record(
                    Path(shutil.which("cmake") or "").resolve()
                ),
            },
            "dotnet": {
                "version": run(
                    ["dotnet", "--version"]
                ).stdout.strip(),
                "info": run(["dotnet", "--info"]).stdout,
                "binary": file_record(
                    Path(shutil.which("dotnet") or "").resolve()
                ),
            },
            "dumpbin": file_record(dumpbin),
            "git": file_record(
                Path(shutil.which("git") or "").resolve()
            ),
            "python": file_record(Path(sys.executable).resolve()),
        },
        "verdict": "PASS",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print("wrote:", args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print("error:", exc, file=sys.stderr)
        raise SystemExit(1)
