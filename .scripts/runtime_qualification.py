#!/usr/bin/env python3
"""Capture reproducible Runtime qualification evidence without shell expansion."""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as et
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1
ENV_ALLOWLIST = (
    "NUMBER_OF_PROCESSORS",
    "PROCESSOR_ARCHITECTURE",
    "PROCESSOR_IDENTIFIER",
    "SystemRoot",
    "TEMP",
    "TMP",
)
COMMAND_ENV_ALLOWLIST = ("HARKEN_NATIVE_DIR",)
FIXED_CHILD_ENVIRONMENT = {
    "PYTHONDONTWRITEBYTECODE": "1",
}
REQUIRED_SOURCE_CONTRACT_PATHS = (
    "CMakeLists.txt",
    "HarkenEngine/Source/Tooling/Binding/gen_binding.py",
    "HarkenEngine/Source/Tooling/Remote/wire/gen_wire.py",
    "HarkenEngine/Source/Tooling/Effects/gen_effect_registry.py",
    "HarkenSDK/Test/ContractTest/AbiFidelity.csproj",
)
SOURCE_VOLATILE_PREFIXES = (".temp",)
EXPECTED_TOOL_VERSIONS = {
    "cmake": "cmake version 4.0.4",
    "ctest": "ctest version 4.0.4",
    "dotnet": "10.0.200",
    "python": "Python 3.13.9",
}
REQUIRED_STATIC_ALLOCATION_CATEGORIES = {
    "BlockArena",
    "VoicePool",
    "VoiceLedger arrays",
    "Effect pools and delay lines",
    "Streaming rings and staging",
    "Bus storage",
    "Command and notification queues",
    "Topology plans",
    "Bindings, meter ring, and dynamic routes",
}
M1_EVIDENCE_POLICY = {
    "policy_version": 3,
    "entries": [
        {
            "kind": "configure",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "cmake",
            "required_argv_contains": [
                "Visual Studio 17 2022",
                "x64",
                "v143,version=14.42.34433",
                "-DCMAKE_SYSTEM_VERSION=10.0.26100.0",
                "-DHARKEN_BUILD_CORE=ON",
                "-DHARKEN_BUILD_TOOLS=ON",
                "-DHARKEN_BUILD_SHARED=ON",
                "-DHARKEN_BUILD_EDITOR=OFF",
                "-DHARKEN_BUILD_EDITOR_UNIT_TESTS=OFF",
                "-DHARKEN_BUILD_HUB=OFF",
                "-DHARKEN_BUILD_HUB_UNIT_TESTS=OFF",
                "-DHARKEN_BUILD_RUNTIME_UNIT_TESTS=ON",
                "-DHARKEN_BUILD_RUNTIME_BENCHMARKS=ON",
            ],
        },
        {
            "kind": "configure-contract",
            "execution_kind": "internal-verification",
            "count": 1,
        },
        {
            "kind": "vs-build-debug",
            "execution_kind": "external",
            "count": 1,
            "required_artifacts": [
                "build/vs/bin/Debug/harken_runtime_tests.exe",
                (
                    "build/vs/bin/Debug/"
                    "harken_render_worker_wake_fallback_tests.exe"
                ),
                "build/vs/bin/Debug/harken_shared_load_smoke.exe",
                "build/vs/bin/Debug/HarkenCore.dll",
                "build/vs/bin/Debug/HarkenCore.pdb",
            ],
        },
        {
            "kind": "vs-build-release",
            "execution_kind": "external",
            "count": 1,
            "required_artifacts": [
                "build/vs/bin/Release/harken_runtime_tests.exe",
                (
                    "build/vs/bin/Release/"
                    "harken_render_worker_wake_fallback_tests.exe"
                ),
                "build/vs/bin/Release/harken_shared_load_smoke.exe",
                "build/vs/bin/Release/HarkenCore.dll",
            ],
        },
        {
            "kind": "ctest-inventory",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "ctest",
            "required_argv_contains": ["-N", "Debug"],
            "required_stdout_contains": [
                "harken_shared_load_smoke",
                "effect_registry_manifest_check",
            ],
        },
        {
            "kind": "ctest-debug",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "ctest",
            "required_artifacts": ["tests/debug.junit.xml"],
            "required_recorded_artifact_roles": ["junit"],
            "required_argv_contains": [
                "Debug",
                "--output-on-failure",
                "--output-junit",
            ],
        },
        {
            "kind": "ctest-release",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "ctest",
            "required_artifacts": ["tests/release.junit.xml"],
            "required_recorded_artifact_roles": ["junit"],
            "required_argv_contains": [
                "Release",
                "--output-on-failure",
                "--output-junit",
            ],
        },
        {
            "kind": "generator-binding",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "python",
            "required_argv_contains": ["gen_binding.py", "--check"],
        },
        {
            "kind": "generator-wire",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "python",
            "required_argv_contains": ["gen_wire.py", "--check"],
        },
        {
            "kind": "generator-effects",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "python",
            "required_argv_contains": [
                "gen_effect_registry.py",
                "--check",
            ],
        },
        {
            "kind": "sdk-contract-managed",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "dotnet",
            "required_argv_contains": [
                "run",
                "ContractTest",
                "Release",
            ],
        },
        {
            "kind": "sdk-contract-native",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "dotnet",
            "required_environment": ["HARKEN_NATIVE_DIR"],
            "required_argv_contains": [
                "run",
                "ContractTest",
                "Release",
                "--require-native",
            ],
        },
        {
            "kind": "perf-phase1",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_benchmarks",
            "required_argv_contains": ["--filter", "phase1"],
        },
        {
            "kind": "perf-compressor-matrix",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "python",
            "required_argv_contains": [
                "runtime_compressor_characterization.py",
                "--filter",
                "compressor-matrix",
            ],
            "required_recorded_artifact_roles": [
                "report",
                "raw-stdout",
                "raw-stderr",
            ],
        },
        {
            "kind": "perf-compressor-voice-self",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_benchmarks",
            "required_argv_contains": [
                "--filter",
                "compressor-matrix",
                "--case",
                "voice-self",
                "--workers",
                "0",
                "--paired",
            ],
        },
        {
            "kind": "perf-compressor-bus-routed",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_benchmarks",
            "required_argv_contains": [
                "--filter",
                "compressor-matrix",
                "--case",
                "bus-routed",
                "--workers",
                "0",
                "--paired",
            ],
        },
        {
            "kind": "perf-resample",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_resample_benchmark",
        },
        {
            "kind": "perf-denormal",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_denormal_probe",
        },
        {
            "kind": "perf-meter",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "harken_meter_benchmark",
        },
        {
            "kind": "memory-baseline",
            "execution_kind": "subprocess",
            "count": 1,
            "executable": "python",
            "required_argv_contains": [
                "runtime_memory_observer.py",
                "--filter",
                "phase1",
            ],
            "required_recorded_artifact_roles": [
                "report",
                "raw-stdout",
                "raw-stderr",
            ],
        },
    ],
}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def finite_json_numbers(value: Any) -> bool:
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(
            finite_json_numbers(item) for item in value.values()
        )
    if isinstance(value, list):
        return all(finite_json_numbers(item) for item in value)
    return True


def run_capture(argv: list[str], cwd: Path) -> dict[str, Any]:
    completed = subprocess.run(
        argv,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return {
        "argv": argv,
        "exit_code": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def command_probe(repo_root: Path, argv: list[str]) -> dict[str, Any]:
    try:
        return tool_version(repo_root, argv)
    except OSError as error:
        return {
            "argv": argv,
            "exit_code": None,
            "stdout": "",
            "stderr": str(error),
        }


def git_text(repo_root: Path, *args: str) -> str:
    result = run_capture(["git", *args], repo_root)
    if result["exit_code"] != 0:
        raise RuntimeError(result["stderr"].strip() or "git command failed")
    return result["stdout"].strip()


def validate_commit_source_contract(
    repo_root: Path,
    commit: str,
) -> None:
    invalid = []
    for relative in REQUIRED_SOURCE_CONTRACT_PATHS:
        result = run_capture(
            ["git", "cat-file", "-t", f"{commit}:{relative}"],
            repo_root,
        )
        if result["exit_code"] != 0 or result["stdout"].strip() != "blob":
            invalid.append(relative)
    if invalid:
        raise RuntimeError(
            "qualification source contract paths are missing or not files: "
            f"{invalid}"
        )


def require_under(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    resolved.relative_to(parent.resolve())
    return resolved


def process_affinity_for_pid(pid: int) -> dict[str, Any]:
    if os.name != "nt":
        return {}
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    ]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetProcessAffinityMask.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.GetProcessAffinityMask.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    process = kernel32.OpenProcess(0x1000, False, pid)
    if not process:
        return {"query_error": ctypes.get_last_error()}
    try:
        process_mask = ctypes.c_size_t()
        system_mask = ctypes.c_size_t()
        if not kernel32.GetProcessAffinityMask(
            process,
            ctypes.byref(process_mask),
            ctypes.byref(system_mask),
        ):
            return {"query_error": ctypes.get_last_error()}
        return {
            "process_affinity_mask": f"0x{process_mask.value:x}",
            "system_affinity_mask": f"0x{system_mask.value:x}",
        }
    finally:
        kernel32.CloseHandle(process)


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_run_id() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def evidence_root(repo_root: Path, commit: str, run_id: str) -> Path:
    return (
        repo_root
        / ".temp"
        / "runtime-qualification"
        / commit
        / run_id
    )


def windows_host_facts() -> dict[str, Any]:
    if os.name != "nt":
        return {}
    from ctypes import wintypes

    class MemoryStatus(ctypes.Structure):
        _fields_ = [
            ("length", ctypes.c_ulong),
            ("memory_load", ctypes.c_ulong),
            ("total_physical", ctypes.c_ulonglong),
            ("available_physical", ctypes.c_ulonglong),
            ("total_page_file", ctypes.c_ulonglong),
            ("available_page_file", ctypes.c_ulonglong),
            ("total_virtual", ctypes.c_ulonglong),
            ("available_virtual", ctypes.c_ulonglong),
            ("available_extended_virtual", ctypes.c_ulonglong),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MemoryStatus)]
    kernel32.GlobalMemoryStatusEx.restype = wintypes.BOOL
    kernel32.GetCurrentProcess.argtypes = []
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.GetProcessAffinityMask.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.GetProcessAffinityMask.restype = wintypes.BOOL
    kernel32.GetPriorityClass.argtypes = [wintypes.HANDLE]
    kernel32.GetPriorityClass.restype = wintypes.DWORD
    memory = MemoryStatus()
    memory.length = ctypes.sizeof(memory)
    memory_ok = bool(kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)))

    process = kernel32.GetCurrentProcess()
    process_mask = ctypes.c_size_t()
    system_mask = ctypes.c_size_t()
    affinity_ok = bool(
        kernel32.GetProcessAffinityMask(
            process,
            ctypes.byref(process_mask),
            ctypes.byref(system_mask),
        )
    )

    processor_name = platform.processor()
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
        ) as key:
            processor_name = str(
                winreg.QueryValueEx(key, "ProcessorNameString")[0]
            ).strip()
    except OSError:
        pass

    return {
        "processor_name": processor_name,
        "total_physical_bytes": (
            memory.total_physical if memory_ok else None
        ),
        "process_affinity_mask": (
            f"0x{process_mask.value:x}" if affinity_ok else None
        ),
        "system_affinity_mask": (
            f"0x{system_mask.value:x}" if affinity_ok else None
        ),
        "process_priority_class": kernel32.GetPriorityClass(process),
    }


def windows_toolchain_facts(repo_root: Path) -> dict[str, Any]:
    if os.name != "nt":
        return {}

    program_files_x86 = Path(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    )
    vswhere = (
        program_files_x86
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    vs_path_probe = command_probe(
        repo_root,
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-property",
            "installationPath",
        ],
    )
    vs_version_probe = command_probe(
        repo_root,
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-property",
            "installationVersion",
        ],
    )
    installation_path = vs_path_probe["stdout"].strip()
    toolset = "14.42.34433"
    cl_path = (
        Path(installation_path)
        / "VC"
        / "Tools"
        / "MSVC"
        / toolset
        / "bin"
        / "Hostx64"
        / "x64"
        / "cl.exe"
    )
    link_path = cl_path.with_name("link.exe")
    sdk_version = "10.0.26100.0"
    sdk_bin = (
        program_files_x86
        / "Windows Kits"
        / "10"
        / "bin"
        / sdk_version
        / "x64"
    )
    return {
        "visual_studio": {
            "installation_path": vs_path_probe,
            "installation_version": vs_version_probe,
        },
        "msvc_toolset": toolset,
        "compiler_path": str(cl_path),
        "compiler_sha256": (
            sha256_file(cl_path) if cl_path.is_file() else None
        ),
        "compiler": command_probe(repo_root, [str(cl_path), "/Bv"]),
        "linker_path": str(link_path),
        "linker_sha256": (
            sha256_file(link_path) if link_path.is_file() else None
        ),
        "linker": command_probe(repo_root, [str(link_path), "/?"]),
        "windows_sdk": {
            "version": sdk_version,
            "bin_path": str(sdk_bin),
            "rc_sha256": (
                sha256_file(sdk_bin / "rc.exe")
                if (sdk_bin / "rc.exe").is_file()
                else None
            ),
        },
    }


def validate_toolchain_probe(toolchain: dict[str, Any]) -> None:
    if os.name != "nt":
        return
    visual_studio = toolchain["visual_studio"]
    if visual_studio["installation_path"]["exit_code"] != 0:
        raise RuntimeError("Visual Studio discovery failed")
    if visual_studio["installation_version"]["exit_code"] != 0:
        raise RuntimeError("Visual Studio version discovery failed")
    if not toolchain["compiler_sha256"]:
        raise RuntimeError("pinned MSVC compiler is missing")
    if not toolchain["linker_sha256"]:
        raise RuntimeError("pinned MSVC linker is missing")
    if not toolchain["windows_sdk"]["rc_sha256"]:
        raise RuntimeError("pinned Windows SDK is missing")


def materialize_source(
    repo_root: Path,
    run_root: Path,
    commit: str,
    submodules: list[str],
) -> Path:
    if submodules:
        raise RuntimeError(
            "submodules require an explicit exact-commit materialization policy"
        )
    source_root = run_root / "source"
    if source_root.exists():
        raise RuntimeError(f"source snapshot already exists: {source_root}")

    completed = subprocess.run(
        [
            "git",
            "worktree",
            "add",
            "--detach",
            str(source_root),
            commit,
        ],
        cwd=repo_root,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise RuntimeError(
            completed.stderr.strip() or "git worktree add failed"
        )
    return source_root


def write_source_index(
    source_root: Path,
    run_root: Path,
) -> dict[str, Any]:
    entries = []
    for path in sorted(source_root.rglob("*")):
        relative = path.relative_to(source_root)
        if relative.parts and relative.parts[0] == ".git":
            continue
        if (
            relative.parts
            and relative.parts[0] in SOURCE_VOLATILE_PREFIXES
        ):
            continue
        if path.is_file():
            entries.append(
                {
                    "path": relative.as_posix(),
                    "sha256": sha256_file(path),
                    "size": path.stat().st_size,
                }
            )
    index_path = run_root / "source-inputs.sha256.json"
    index_path.write_text(
        json.dumps(entries, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return {
        "path": str(index_path.relative_to(run_root)),
        "sha256": sha256_file(index_path),
        "file_count": len(entries),
    }


def verify_source_index(
    source_root: Path,
    run_root: Path,
    identity: dict[str, Any],
) -> None:
    index_path = run_root / identity["path"]
    if sha256_file(index_path) != identity["sha256"]:
        raise RuntimeError("source input index was modified")
    expected = {
        item["path"]: item
        for item in json.loads(index_path.read_text(encoding="utf-8"))
    }
    actual_paths = []
    for path in sorted(source_root.rglob("*")):
        relative = path.relative_to(source_root)
        if relative.parts and relative.parts[0] == ".git":
            continue
        if (
            relative.parts
            and relative.parts[0] in SOURCE_VOLATILE_PREFIXES
        ):
            continue
        if path.is_file():
            actual_paths.append(relative.as_posix())
    if set(actual_paths) != set(expected):
        raise RuntimeError("sealed source file set was modified")
    for relative, item in expected.items():
        path = source_root / Path(relative)
        if path.stat().st_size != item["size"]:
            raise RuntimeError(f"sealed source size changed: {relative}")
        if sha256_file(path) != item["sha256"]:
            raise RuntimeError(f"sealed source content changed: {relative}")


def verify_harness_identity(
    manifest: dict[str, Any],
    run_root: Path,
) -> None:
    current = Path(__file__).resolve()
    candidates = {
        Path(item["path"]).name: item
        for item in manifest["qualification_harness"]["files"]
    }
    expected = candidates.get(current.name)
    if expected is None:
        raise RuntimeError("qualification harness identity is missing")
    frozen = run_root / expected["path"]
    if not frozen.is_file() or sha256_file(frozen) != expected["sha256"]:
        raise RuntimeError("frozen qualification harness was modified")
    if sha256_file(current) != expected["sha256"]:
        raise RuntimeError(
            "active qualification harness differs from prepared harness"
        )


def verify_qualification_contract(
    manifest: dict[str, Any],
    run_root: Path,
) -> None:
    identity = manifest["qualification_contract"]
    path = run_root / identity["path"]
    if not path.is_file() or sha256_file(path) != identity["sha256"]:
        raise RuntimeError("frozen qualification contract was modified")
    contract = json.loads(path.read_text(encoding="utf-8"))
    if contract["commit_sha"] != manifest["commit_sha"]:
        raise RuntimeError("qualification commit contract was modified")
    if contract["evidence_policy"] != manifest["evidence_policy"]:
        raise RuntimeError("qualification evidence policy was modified")
    if contract["configure_contract"] != manifest["configure_contract"]:
        raise RuntimeError("qualification configure contract was modified")
    if (
        contract.get("fixed_child_environment")
        != manifest.get("fixed_child_environment")
        or contract.get("fixed_child_environment")
        != FIXED_CHILD_ENVIRONMENT
    ):
        raise RuntimeError(
            "qualification child environment contract was modified"
        )
    if contract["evidence_policy"] != M1_EVIDENCE_POLICY:
        raise RuntimeError(
            "active harness evidence policy differs from prepared contract"
        )
    toolchain = manifest["tools"]["windows_toolchain"]
    expected_toolchain = contract["toolchain_identity"]
    actual_toolchain = {
        "compiler_path": toolchain.get("compiler_path"),
        "compiler_sha256": toolchain.get("compiler_sha256"),
        "linker_path": toolchain.get("linker_path"),
        "linker_sha256": toolchain.get("linker_sha256"),
        "windows_sdk": toolchain.get("windows_sdk"),
    }
    if actual_toolchain != expected_toolchain:
        raise RuntimeError("qualification toolchain contract was modified")
    actual_tools = {
        name: manifest["tools"][name]
        for name in EXPECTED_TOOL_VERSIONS
    }
    if actual_tools != contract["tool_identities"]:
        raise RuntimeError("qualification tool identities were modified")


def capture_harness_identity(run_root: Path) -> dict[str, Any]:
    destination = run_root / "inputs" / "harness"
    destination.mkdir(parents=True)
    candidates = [
        Path(__file__).resolve(),
        Path(__file__).resolve().parent
        / "tests"
        / "test_runtime_qualification.py",
        Path(__file__).resolve().parent
        / "runtime_memory_observer.py",
        Path(__file__).resolve().parent
        / "runtime_compressor_characterization.py",
    ]
    files = []
    for source in candidates:
        if not source.is_file():
            raise FileNotFoundError(
                f"qualification harness input is missing: {source}"
            )
        target = destination / source.name
        shutil.copy2(source, target)
        files.append(
            {
                "path": str(target.relative_to(run_root)),
                "sha256": sha256_file(target),
                "size": target.stat().st_size,
            }
        )
    return {"files": files}


def load_manifest(run_root: Path) -> dict[str, Any]:
    path = run_root / "manifest.json"
    if not path.is_file():
        raise FileNotFoundError(f"missing manifest: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def write_manifest(run_root: Path, manifest: dict[str, Any]) -> None:
    path = run_root / "manifest.json"
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def tool_version(repo_root: Path, argv: list[str]) -> dict[str, Any]:
    result = run_capture(argv, repo_root)
    resolved_text = shutil.which(argv[0]) or ""
    resolved = Path(resolved_text) if resolved_text else Path()
    return {
        "argv": argv,
        "exit_code": result["exit_code"],
        "stdout": result["stdout"].strip(),
        "stderr": result["stderr"].strip(),
        "resolved_path": resolved_text,
        "sha256": (
            sha256_file(resolved)
            if resolved_text and resolved.is_file()
            else None
        ),
    }


def validate_tool_versions(tools: dict[str, Any]) -> None:
    for name, expected in EXPECTED_TOOL_VERSIONS.items():
        tool = tools[name]
        output = "\n".join(
            item
            for item in (tool["stdout"], tool["stderr"])
            if item
        )
        if tool["exit_code"] != 0:
            raise RuntimeError(f"{name} version probe failed")
        if expected not in output:
            raise RuntimeError(
                f"{name} version mismatch: expected {expected}, "
                f"observed {output}"
            )
        if not tool["sha256"]:
            raise RuntimeError(f"{name} executable identity is missing")


def prepare(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    commit = git_text(repo_root, "rev-parse", "HEAD")
    if commit != args.expected_commit:
        raise RuntimeError(
            f"HEAD {commit} does not match expected commit "
            f"{args.expected_commit}"
        )
    validate_commit_source_contract(repo_root, commit)
    run_id = args.run_id or default_run_id()
    run_root = (
        Path(args.run_root).resolve()
        if args.run_root
        else evidence_root(repo_root, commit, run_id)
    )
    require_under(
        run_root, repo_root / ".temp" / "runtime-qualification"
    )
    if run_root.exists():
        raise RuntimeError(f"run root already exists: {run_root}")
    run_root.mkdir(parents=True)
    for name in (
        "abi",
        "build",
        "inputs",
        "legacy",
        "logs",
        "memory",
        "perf",
        "rt",
        "tests",
    ):
        (run_root / name).mkdir()

    dirty_lines = git_text(repo_root, "status", "--porcelain=v1").splitlines()
    submodules = git_text(repo_root, "submodule", "status").splitlines()
    source_root = materialize_source(
        repo_root,
        run_root,
        commit,
        submodules,
    )
    source_index = write_source_index(source_root, run_root)
    harness_identity = capture_harness_identity(run_root)
    power_plan = command_probe(repo_root, ["powercfg", "/GETACTIVESCHEME"])
    toolchain = windows_toolchain_facts(repo_root)
    validate_toolchain_probe(toolchain)
    qualified_tools = {
        "cmake": tool_version(repo_root, ["cmake", "--version"]),
        "ctest": tool_version(repo_root, ["ctest", "--version"]),
        "dotnet": tool_version(repo_root, ["dotnet", "--version"]),
        "python": tool_version(
            repo_root,
            [sys.executable, "--version"],
        ),
    }
    validate_tool_versions(qualified_tools)
    evidence_policy = json.loads(json.dumps(M1_EVIDENCE_POLICY))
    configure_contract = {
        "generator": "Visual Studio 17 2022",
        "architecture": "x64",
        "toolset": "v143,version=14.42.34433",
        "windows_sdk": "10.0.26100.0",
        "runtime_only": True,
        "runtime_tests": True,
        "runtime_benchmarks": True,
        "source_root": str(source_root),
        "cmake_cache": {
            "CMAKE_GENERATOR": "Visual Studio 17 2022",
            "CMAKE_GENERATOR_PLATFORM": "x64",
            "CMAKE_GENERATOR_TOOLSET": "v143,version=14.42.34433",
            "CMAKE_HOME_DIRECTORY": str(source_root).replace("\\", "/"),
            "CMAKE_SYSTEM_VERSION": "10.0.26100.0",
            "HARKEN_BUILD_CORE": "ON",
            "HARKEN_BUILD_TOOLS": "ON",
            "HARKEN_BUILD_SHARED": "ON",
            "HARKEN_BUILD_EDITOR": "OFF",
            "HARKEN_BUILD_EDITOR_UNIT_TESTS": "OFF",
            "HARKEN_BUILD_HUB": "OFF",
            "HARKEN_BUILD_HUB_UNIT_TESTS": "OFF",
            "HARKEN_BUILD_RUNTIME_BENCHMARKS": "ON",
            "HARKEN_BUILD_RUNTIME_UNIT_TESTS": "ON",
        },
    }
    qualification_contract = {
        "schema_version": SCHEMA_VERSION,
        "commit_sha": commit,
        "evidence_policy": evidence_policy,
        "configure_contract": configure_contract,
        "fixed_child_environment": FIXED_CHILD_ENVIRONMENT,
        "toolchain_identity": {
            "compiler_path": toolchain.get("compiler_path"),
            "compiler_sha256": toolchain.get("compiler_sha256"),
            "linker_path": toolchain.get("linker_path"),
            "linker_sha256": toolchain.get("linker_sha256"),
            "windows_sdk": toolchain.get("windows_sdk"),
        },
        "tool_identities": qualified_tools,
    }
    contract_path = run_root / "inputs" / "qualification-contract.json"
    contract_path.write_text(
        json.dumps(qualification_contract, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    contract_identity = {
        "path": str(contract_path.relative_to(run_root)),
        "sha256": sha256_file(contract_path),
        "size": contract_path.stat().st_size,
    }
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "evidence_kind": "runtime_qualification",
        "claim_id": "M1",
        "state": "prepared",
        "qualification_status": "incomplete",
        "run_id": run_id,
        "commit_sha": commit,
        "branch": git_text(repo_root, "branch", "--show-current"),
        "upstream_main_sha": git_text(repo_root, "rev-parse", "origin/main"),
        "dirty_worktree": bool(dirty_lines),
        "dirty_paths": dirty_lines,
        "submodules": submodules,
        "timestamp_start_utc": utc_now(),
        "timestamp_end_utc": None,
        "repo_root": str(repo_root),
        "run_root": str(run_root),
        "source_root": str(source_root),
        "source_identity": {
            "commit_sha": commit,
            "tree_sha": git_text(repo_root, "rev-parse", f"{commit}^{{tree}}"),
            "index": source_index,
        },
        "qualification_harness": harness_identity,
        "qualification_contract": contract_identity,
        "evidence_policy": evidence_policy,
        "host": {
            "platform": platform.platform(),
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpu_count": os.cpu_count(),
            "windows": windows_host_facts(),
            "active_power_plan": power_plan,
        },
        "environment_allowlist": {
            key: os.environ.get(key) for key in ENV_ALLOWLIST
        },
        "tools": {
            **qualified_tools,
            "windows_toolchain": toolchain,
        },
        "configure_contract": configure_contract,
        "fixed_child_environment": FIXED_CHILD_ENVIRONMENT,
        "limitations": [
            "Untracked user files are recorded but are not modified.",
            "M1 NullBackend results do not qualify DeviceCallback or xrun behavior.",
            "Real-device traces with sampleRate zero are excluded from rate identity.",
        ],
        "artifacts": [],
    }
    write_manifest(run_root, manifest)
    (run_root / "commands.jsonl").touch(exist_ok=True)
    print(run_root)
    return 0


def next_sequence(commands_path: Path) -> int:
    if not commands_path.exists():
        return 1
    with commands_path.open("r", encoding="utf-8") as stream:
        return sum(1 for line in stream if line.strip()) + 1


def append_jsonl(path: Path, value: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(value, sort_keys=True) + "\n")


def executable_identity(argv0: str, cwd: Path) -> dict[str, Any]:
    candidate = Path(argv0)
    if candidate.is_absolute() or candidate.parent != Path("."):
        resolved = (cwd / candidate).resolve() if not candidate.is_absolute() else candidate.resolve()
        resolved_text = str(resolved)
    else:
        resolved_text = shutil.which(argv0) or ""
        resolved = Path(resolved_text) if resolved_text else Path()
    return {
        "requested": argv0,
        "resolved_path": resolved_text,
        "sha256": (
            sha256_file(resolved)
            if resolved_text and resolved.is_file()
            else None
        ),
    }


def command_environment(
    values: list[str],
    run_root: Path,
) -> tuple[dict[str, str], list[dict[str, Any]]]:
    overrides = {}
    inputs = []
    for value in values:
        if "=" not in value:
            raise ValueError("command environment must use key=value")
        key, item = value.split("=", 1)
        if key not in COMMAND_ENV_ALLOWLIST:
            raise ValueError(f"command environment key is not allowed: {key}")
        if key == "HARKEN_NATIVE_DIR":
            native_dir = require_under(Path(item), run_root)
            expected_native_dir = (
                run_root / "build" / "vs" / "bin" / "Release"
            ).resolve()
            if native_dir != expected_native_dir:
                raise ValueError(
                    "HARKEN_NATIVE_DIR must be the qualified Release "
                    f"directory: {expected_native_dir}"
                )
            native_dll = native_dir / "HarkenCore.dll"
            if not native_dll.is_file():
                raise FileNotFoundError(
                    f"native Runtime DLL is missing: {native_dll}"
                )
            item = str(native_dir)
            inputs.append(
                {
                    "role": "native-runtime",
                    "path": str(native_dll.relative_to(run_root)),
                    "sha256": sha256_file(native_dll),
                    "size": native_dll.stat().st_size,
                }
            )
        overrides[key] = item
    return overrides, inputs


def recorded_artifacts(
    values: list[str],
    run_root: Path,
) -> list[dict[str, Any]]:
    artifacts = []
    for value in values:
        if "=" not in value:
            raise ValueError("recorded artifact must use role=path")
        role, raw_path = value.split("=", 1)
        path = require_under(Path(raw_path), run_root)
        if not path.is_file():
            raise FileNotFoundError(f"recorded artifact missing: {path}")
        artifacts.append(
            {
                "role": role,
                "path": str(path.relative_to(run_root)),
                "sha256": sha256_file(path),
                "size": path.stat().st_size,
            }
        )
    return artifacts


def execute(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    run_root = require_under(
        Path(args.run_root),
        repo_root / ".temp" / "runtime-qualification",
    )
    manifest = load_manifest(run_root)
    if manifest["state"] == "finalized":
        raise RuntimeError("finalized evidence is immutable")
    verify_harness_identity(manifest, run_root)
    verify_qualification_contract(manifest, run_root)
    source_root = Path(manifest["source_root"])
    verify_source_index(
        source_root,
        run_root,
        manifest["source_identity"]["index"],
    )
    argv = list(args.command)
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        raise ValueError("run requires a command after --")
    command_cwd = require_under(
        Path(args.cwd) if args.cwd else source_root,
        run_root,
    )
    requested_environment, input_artifacts = command_environment(
        list(getattr(args, "env", [])),
        run_root,
    )
    environment_overrides = dict(FIXED_CHILD_ENVIRONMENT)
    environment_overrides.update(requested_environment)
    child_environment = os.environ.copy()
    child_environment.update(environment_overrides)

    commands_path = run_root / "commands.jsonl"
    sequence = next_sequence(commands_path)
    safe_kind = "".join(
        char if char.isalnum() or char in "-_" else "_" for char in args.kind
    )
    prefix = f"{sequence:04d}-{safe_kind}"
    stdout_path = run_root / "logs" / f"{prefix}.stdout.log"
    stderr_path = run_root / "logs" / f"{prefix}.stderr.log"

    started = utc_now()
    start_time = time.perf_counter()
    process = subprocess.Popen(
        argv,
        cwd=command_cwd,
        env=child_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    child_affinity = process_affinity_for_pid(process.pid)
    stdout_bytes, stderr_bytes = process.communicate()
    duration = time.perf_counter() - start_time
    ended = utc_now()
    stdout_path.write_bytes(stdout_bytes)
    stderr_path.write_bytes(stderr_bytes)
    artifact_capture_error = None
    try:
        output_artifacts = recorded_artifacts(
            list(getattr(args, "artifact", [])),
            run_root,
        )
    except (FileNotFoundError, OSError, ValueError) as error:
        output_artifacts = []
        artifact_capture_error = str(error)
    input_integrity_failed = False
    for item in input_artifacts:
        path = run_root / item["path"]
        post_hash = sha256_file(path) if path.is_file() else None
        item["post_sha256"] = post_hash
        item["unchanged"] = post_hash == item["sha256"]
        if not item["unchanged"]:
            input_integrity_failed = True
    if input_integrity_failed:
        effective_exit_code = 70
    elif artifact_capture_error:
        effective_exit_code = 71
    else:
        effective_exit_code = process.returncode

    record = {
        "sequence": sequence,
        "kind": args.kind,
        "execution_kind": "subprocess",
        "cwd": str(command_cwd),
        "argv": argv,
        "executable": executable_identity(argv[0], command_cwd),
        "child_process": child_affinity,
        "environment_overrides": environment_overrides,
        "input_artifacts": input_artifacts,
        "artifacts": output_artifacts,
        "artifact_capture_error": artifact_capture_error,
        "environment_allowlist": {
            key: os.environ.get(key) for key in ENV_ALLOWLIST
        },
        "start_utc": started,
        "end_utc": ended,
        "duration_seconds": duration,
        "exit_code": effective_exit_code,
        "stdout": {
            "path": str(stdout_path.relative_to(run_root)),
            "sha256": sha256_file(stdout_path),
            "size": stdout_path.stat().st_size,
        },
        "stderr": {
            "path": str(stderr_path.relative_to(run_root)),
            "sha256": sha256_file(stderr_path),
            "size": stderr_path.stat().st_size,
        },
    }
    append_jsonl(commands_path, record)
    manifest["state"] = "running"
    write_manifest(run_root, manifest)
    if hasattr(sys.stdout, "buffer"):
        sys.stdout.buffer.write(stdout_bytes)
        sys.stdout.buffer.flush()
    else:
        sys.stdout.write(
            stdout_bytes.decode(
                sys.stdout.encoding or "utf-8",
                errors="replace",
            )
        )
    if hasattr(sys.stderr, "buffer"):
        sys.stderr.buffer.write(stderr_bytes)
        sys.stderr.buffer.flush()
    else:
        sys.stderr.write(
            stderr_bytes.decode(
                sys.stderr.encoding or "utf-8",
                errors="replace",
            )
        )
    if artifact_capture_error:
        print(
            f"runtime qualification artifact error: "
            f"{artifact_capture_error}",
            file=sys.stderr,
        )
    return int(effective_exit_code)


def record_external(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    run_root = require_under(
        Path(args.run_root),
        repo_root / ".temp" / "runtime-qualification",
    )
    manifest = load_manifest(run_root)
    if manifest["state"] == "finalized":
        raise RuntimeError("finalized evidence is immutable")
    verify_harness_identity(manifest, run_root)
    verify_qualification_contract(manifest, run_root)
    source_root = Path(manifest["source_root"])
    verify_source_index(
        source_root,
        run_root,
        manifest["source_identity"]["index"],
    )
    command_cwd = require_under(
        Path(args.cwd) if args.cwd else source_root,
        run_root,
    )
    policy_entries = {
        item["kind"]: item
        for item in manifest["evidence_policy"]["entries"]
        if item["execution_kind"] == "external"
    }
    if args.kind not in policy_entries:
        raise ValueError(
            f"external evidence kind is not in frozen policy: {args.kind}"
        )
    started = dt.datetime.fromisoformat(
        args.started_utc.replace("Z", "+00:00")
    )
    ended = dt.datetime.fromisoformat(
        args.ended_utc.replace("Z", "+00:00")
    )
    if started.tzinfo is None or ended.tzinfo is None:
        raise ValueError("external timestamps must include a timezone")
    if ended < started:
        raise ValueError("external end timestamp precedes start timestamp")
    artifacts = []
    for value in args.artifact:
        if "=" not in value:
            raise ValueError("external artifact must use role=path")
        role, raw_path = value.split("=", 1)
        path = require_under(Path(raw_path), run_root)
        if not path.is_file():
            raise FileNotFoundError(f"external artifact missing: {path}")
        artifacts.append(
            {
                "role": role,
                "path": str(path.relative_to(run_root)),
                "sha256": sha256_file(path),
                "size": path.stat().st_size,
            }
        )
    metadata = {}
    for value in args.metadata:
        if "=" not in value:
            raise ValueError("metadata must use key=value")
        key, item = value.split("=", 1)
        metadata[key] = item
    if not artifacts:
        raise ValueError("external evidence requires at least one artifact")

    if args.kind in ("vs-build-debug", "vs-build-release"):
        configuration = (
            "Debug" if args.kind == "vs-build-debug" else "Release"
        )
        expected_solution = (
            run_root / "build" / "vs" / "HarkenCore.sln"
        ).resolve()
        required_metadata = {
            "solution": str(expected_solution),
            "configuration": configuration,
            "platform": "x64",
            "targets": "solution",
            "build_result": "succeeded",
        }
        for key, expected in required_metadata.items():
            if metadata.get(key) != expected:
                raise ValueError(
                    f"{args.kind} metadata {key} must equal {expected}"
                )
        expected_vs_instance = manifest["tools"]["windows_toolchain"][
            "visual_studio"
        ]["installation_path"]["stdout"].strip()
        if metadata.get("vs_instance") != expected_vs_instance:
            raise ValueError(
                f"{args.kind} vs_instance must equal "
                f"{expected_vs_instance}"
            )
        if not expected_solution.is_file():
            raise FileNotFoundError(
                f"qualified solution is missing: {expected_solution}"
            )
        roles = [item["role"] for item in artifacts]
        required_roles = ["output", "errors", "binary"]
        if configuration == "Debug":
            required_roles.append("pdb")
        for role in required_roles:
            if role not in roles:
                raise ValueError(
                    f"{args.kind} requires artifact role {role}"
                )
        config_dir = (
            run_root / "build" / "vs" / "bin" / configuration
        ).resolve()
        required_binary_paths = {
            (config_dir / "HarkenCore.dll").resolve(),
            (config_dir / "harken_runtime_tests.exe").resolve(),
            (
                config_dir
                / "harken_render_worker_wake_fallback_tests.exe"
            ).resolve(),
            (config_dir / "harken_shared_load_smoke.exe").resolve(),
        }
        recorded_binary_paths = {
            (run_root / item["path"]).resolve()
            for item in artifacts
            if item["role"] == "binary"
        }
        missing_binaries = sorted(
            str(path)
            for path in required_binary_paths - recorded_binary_paths
        )
        if missing_binaries:
            raise ValueError(
                f"{args.kind} missing qualified binaries: "
                f"{missing_binaries}"
            )
        if configuration == "Debug":
            required_pdb = (config_dir / "HarkenCore.pdb").resolve()
            recorded_pdb_paths = {
                (run_root / item["path"]).resolve()
                for item in artifacts
                if item["role"] == "pdb"
            }
            if required_pdb not in recorded_pdb_paths:
                raise ValueError(
                    f"{args.kind} missing qualified PDB: {required_pdb}"
                )
        for item in artifacts:
            path = run_root / item["path"]
            if item["role"] in ("output", "errors") and item["size"] == 0:
                raise ValueError(
                    f"{args.kind} {item['role']} artifact is empty"
                )
            if item["role"] == "binary" and path.suffix.lower() not in (
                ".dll",
                ".exe",
            ):
                raise ValueError(
                    f"{args.kind} binary artifact is not a DLL or EXE"
                )
            if item["role"] == "pdb" and path.suffix.lower() != ".pdb":
                raise ValueError(
                    f"{args.kind} pdb artifact is not a PDB"
                )
            if item["role"] in ("binary", "pdb"):
                if path.resolve().parent != config_dir:
                    raise ValueError(
                        f"{args.kind} {item['role']} artifact must be "
                        f"under {config_dir}"
                    )

    if args.kind == "memory-baseline":
        reports = [
            item for item in artifacts if item["role"] == "report"
        ]
        if len(reports) != 1:
            raise ValueError(
                "memory-baseline requires exactly one report artifact"
            )
        report_path = run_root / reports[0]["path"]
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise ValueError(
                f"memory-baseline report is not valid JSON: {error}"
            ) from error
        required_top_level = {
            "schema_version",
            "claim_id",
            "commit_sha",
            "binary",
            "config",
            "process_bytes",
            "runtime_phase_bytes",
            "arena",
            "capacities",
            "static_allocations",
            "limitations",
        }
        missing = sorted(required_top_level - set(report))
        if missing:
            raise ValueError(
                f"memory-baseline report missing fields: {missing}"
            )
        if report["schema_version"] != 1:
            raise ValueError("memory-baseline schema_version must be 1")
        if report["claim_id"] != "M1-memory":
            raise ValueError("memory-baseline claim_id must be M1-memory")
        if report["commit_sha"] != manifest["commit_sha"]:
            raise ValueError("memory-baseline commit does not match run")
        binary_path = require_under(
            Path(report["binary"].get("path", "")),
            run_root,
        )
        if not binary_path.is_file():
            raise FileNotFoundError(
                f"memory-baseline binary is missing: {binary_path}"
            )
        if report["binary"].get("sha256") != sha256_file(binary_path):
            raise ValueError("memory-baseline binary hash mismatch")
        process_bytes = report["process_bytes"]
        for key in ("initial_observation", "peak", "after_exit"):
            value = process_bytes.get(key)
            if not isinstance(value, int) or value < 0:
                raise ValueError(
                    f"memory-baseline process_bytes.{key} is invalid"
                )
        if process_bytes["peak"] < process_bytes["initial_observation"]:
            raise ValueError(
                "memory-baseline peak precedes before-launch baseline"
            )
        phases = report["runtime_phase_bytes"]
        for key in (
            "after_init",
            "after_warmup",
            "high_water",
            "after_term",
        ):
            value = phases.get(key)
            if value is not None and (
                not isinstance(value, int) or value < 0
            ):
                raise ValueError(
                    f"memory-baseline runtime_phase_bytes.{key} is invalid"
                )
        arena = report["arena"]
        for key in ("high_water_bytes", "overflow_count"):
            value = arena.get(key)
            if not isinstance(value, int) or value < 0:
                raise ValueError(
                    f"memory-baseline arena.{key} is invalid"
                )
        if not isinstance(report["capacities"], dict):
            raise ValueError("memory-baseline capacities must be an object")
        if not isinstance(report["static_allocations"], list):
            raise ValueError(
                "memory-baseline static_allocations must be an array"
            )
        if not report["limitations"] or not all(
            isinstance(item, str) for item in report["limitations"]
        ):
            raise ValueError(
                "memory-baseline limitations must be non-empty strings"
            )
    record = {
        "sequence": next_sequence(run_root / "commands.jsonl"),
        "kind": args.kind,
        "execution_kind": "external",
        "cwd": str(command_cwd),
        "argv": [],
        "environment_allowlist": {
            key: os.environ.get(key) for key in ENV_ALLOWLIST
        },
        "start_utc": args.started_utc,
        "end_utc": args.ended_utc,
        "duration_seconds": (ended - started).total_seconds(),
        "exit_code": 0 if args.status == "success" else 1,
        "artifacts": artifacts,
        "metadata": metadata,
    }
    append_jsonl(run_root / "commands.jsonl", record)
    manifest["state"] = "running"
    write_manifest(run_root, manifest)
    return record["exit_code"]


def parse_cmake_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def parse_cmake_set_file(path: Path) -> dict[str, str]:
    values = {}
    pattern = re.compile(
        r'^\s*set\(\s*([A-Za-z0-9_]+)\s+(?:"([^"]*)"|([^\s\)]+))'
    )
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        match = pattern.match(line)
        if match:
            values[match.group(1)] = match.group(2) or match.group(3)
    return values


def verify_configure(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    run_root = require_under(
        Path(args.run_root),
        repo_root / ".temp" / "runtime-qualification",
    )
    manifest = load_manifest(run_root)
    if manifest["state"] == "finalized":
        raise RuntimeError("finalized evidence is immutable")
    verify_harness_identity(manifest, run_root)
    verify_qualification_contract(manifest, run_root)
    verify_source_index(
        Path(manifest["source_root"]),
        run_root,
        manifest["source_identity"]["index"],
    )
    build_dir = require_under(Path(args.build_dir), run_root)
    expected_build_dir = (run_root / "build" / "vs").resolve()
    if build_dir != expected_build_dir:
        raise RuntimeError(
            f"configure verification requires {expected_build_dir}"
        )
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise FileNotFoundError(f"missing CMake cache: {cache_path}")
    cache = parse_cmake_cache(cache_path)
    contract = manifest["configure_contract"]
    expected_cache = dict(contract["cmake_cache"])
    mismatches = []
    for key, expected in expected_cache.items():
        actual = cache.get(key)
        if actual != expected:
            mismatches.append(
                {"key": key, "expected": expected, "actual": actual}
            )

    compiler_files = sorted(
        (build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
    )
    if len(compiler_files) != 1:
        mismatches.append(
            {
                "key": "CMakeCXXCompiler.cmake",
                "expected": "exactly one",
                "actual": len(compiler_files),
            }
        )
        compiler = {}
    else:
        compiler = parse_cmake_set_file(compiler_files[0])
    toolchain = manifest["tools"]["windows_toolchain"]
    actual_compiler_path = compiler.get("CMAKE_CXX_COMPILER", "")
    actual_linker_path = compiler.get("CMAKE_LINKER", "")
    expected_compiler_path = toolchain.get("compiler_path", "")
    expected_linker_path = toolchain.get("linker_path", "")
    path_pairs = (
        ("CMAKE_CXX_COMPILER", expected_compiler_path, actual_compiler_path),
        ("CMAKE_LINKER", expected_linker_path, actual_linker_path),
    )
    for key, expected, actual in path_pairs:
        if (
            not actual
            or Path(actual).resolve() != Path(expected).resolve()
        ):
            mismatches.append(
                {"key": key, "expected": expected, "actual": actual}
            )

    actual_compiler_hash = (
        sha256_file(Path(actual_compiler_path))
        if actual_compiler_path and Path(actual_compiler_path).is_file()
        else None
    )
    actual_linker_hash = (
        sha256_file(Path(actual_linker_path))
        if actual_linker_path and Path(actual_linker_path).is_file()
        else None
    )
    if actual_compiler_hash != toolchain.get("compiler_sha256"):
        mismatches.append(
            {
                "key": "compiler_sha256",
                "expected": toolchain.get("compiler_sha256"),
                "actual": actual_compiler_hash,
            }
        )
    if actual_linker_hash != toolchain.get("linker_sha256"):
        mismatches.append(
            {
                "key": "linker_sha256",
                "expected": toolchain.get("linker_sha256"),
                "actual": actual_linker_hash,
            }
        )

    sdk_tag = (
        f"<WindowsTargetPlatformVersion>{contract['windows_sdk']}"
        "</WindowsTargetPlatformVersion>"
    )
    vcxproj_files = sorted(build_dir.glob("*.vcxproj"))
    sdk_matches = [
        path
        for path in vcxproj_files
        if sdk_tag in path.read_text(encoding="utf-8", errors="strict")
    ]
    if not sdk_matches:
        mismatches.append(
            {
                "key": "WindowsTargetPlatformVersion",
                "expected": contract["windows_sdk"],
                "actual": None,
            }
        )

    build_info_path = (
        build_dir
        / "generated"
        / "harken_buildinfo"
        / "harken_buildinfo.h"
    )
    build_info_text = (
        build_info_path.read_text(encoding="utf-8", errors="strict")
        if build_info_path.is_file()
        else ""
    )
    expected_commit_line = (
        f'GitCommit        = "{manifest["commit_sha"]}"'
    )
    if expected_commit_line not in build_info_text:
        mismatches.append(
            {
                "key": "BuildInfo.GitCommit",
                "expected": manifest["commit_sha"],
                "actual": None,
            }
        )
    if "GitDirty         = false" not in build_info_text:
        mismatches.append(
            {
                "key": "BuildInfo.GitDirty",
                "expected": False,
                "actual": None,
            }
        )

    verification = {
        "schema_version": SCHEMA_VERSION,
        "build_dir": str(build_dir),
        "cache_path": str(cache_path),
        "expected_cache": expected_cache,
        "compiler_file": (
            str(compiler_files[0]) if len(compiler_files) == 1 else None
        ),
        "compiler_version": compiler.get("CMAKE_CXX_COMPILER_VERSION"),
        "compiler_path": actual_compiler_path,
        "compiler_sha256": actual_compiler_hash,
        "linker_path": actual_linker_path,
        "linker_sha256": actual_linker_hash,
        "sdk_project_match_count": len(sdk_matches),
        "build_info_path": str(build_info_path),
        "mismatches": mismatches,
        "status": "passed" if not mismatches else "failed",
    }
    output_path = run_root / "build" / "configure-verification.json"
    output_path.write_text(
        json.dumps(verification, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    record = {
        "sequence": next_sequence(run_root / "commands.jsonl"),
        "kind": "configure-contract",
        "execution_kind": "internal-verification",
        "cwd": str(build_dir),
        "argv": [],
        "environment_allowlist": {
            key: os.environ.get(key) for key in ENV_ALLOWLIST
        },
        "start_utc": utc_now(),
        "end_utc": utc_now(),
        "duration_seconds": None,
        "exit_code": 0 if not mismatches else 1,
        "artifacts": [
            {
                "path": str(output_path.relative_to(run_root)),
                "sha256": sha256_file(output_path),
                "size": output_path.stat().st_size,
            }
        ],
    }
    append_jsonl(run_root / "commands.jsonl", record)
    manifest["state"] = "running"
    write_manifest(run_root, manifest)
    if mismatches:
        for mismatch in mismatches:
            print(
                "configure contract mismatch: "
                f"{mismatch['key']} expected={mismatch['expected']} "
                f"actual={mismatch['actual']}",
                file=sys.stderr,
            )
        return 1
    print(output_path)
    return 0


def iter_artifacts(run_root: Path) -> Iterable[Path]:
    for path in sorted(run_root.rglob("*")):
        if not path.is_file() or path.name == "manifest.json":
            continue
        relative = path.relative_to(run_root)
        if relative.parts and relative.parts[0] == "source":
            if (
                len(relative.parts) < 2
                or relative.parts[1] not in SOURCE_VOLATILE_PREFIXES
            ):
                continue
        if relative.parts and relative.parts[0] == "build":
            retained_suffixes = {
                ".bin",
                ".cmake",
                ".csv",
                ".dll",
                ".etl",
                ".exe",
                ".filters",
                ".h",
                ".json",
                ".jsonl",
                ".lib",
                ".log",
                ".pdb",
                ".props",
                ".sln",
                ".targets",
                ".txt",
                ".vcxproj",
                ".xml",
            }
            if (
                path.suffix.lower() not in retained_suffixes
                and path.name != "CMakeCache.txt"
            ):
                continue
        yield path


def read_command_records(commands_path: Path) -> list[dict[str, Any]]:
    records = []
    with commands_path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if line.strip():
                records.append(json.loads(line))
    return records


def validate_record_integrity(
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    violations = []
    for record in records:
        descriptors = []
        for field in ("stdout", "stderr"):
            value = record.get(field)
            if value:
                descriptors.append((field, value, value.get("sha256")))
        for value in record.get("artifacts", []):
            descriptors.append(
                (
                    f"artifact:{value.get('role', '')}",
                    value,
                    value.get("sha256"),
                )
            )
        for value in record.get("input_artifacts", []):
            descriptors.append(
                (
                    f"input:{value.get('role', '')}",
                    value,
                    value.get("post_sha256") or value.get("sha256"),
                )
            )
        for role, descriptor, expected_hash in descriptors:
            raw_path = descriptor.get("path")
            path = run_root / Path(raw_path) if raw_path else None
            actual_hash = (
                sha256_file(path)
                if path is not None and path.is_file()
                else None
            )
            actual_size = (
                path.stat().st_size
                if path is not None and path.is_file()
                else None
            )
            if (
                actual_hash != expected_hash
                or actual_size != descriptor.get("size")
            ):
                violations.append(
                    {
                        "sequence": record["sequence"],
                        "kind": record["kind"],
                        "role": role,
                        "path": raw_path,
                        "expected_sha256": expected_hash,
                        "actual_sha256": actual_hash,
                        "expected_size": descriptor.get("size"),
                        "actual_size": actual_size,
                    }
                )
    return violations


def validate_evidence_policy(
    manifest: dict[str, Any],
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    violations = []
    by_kind: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        by_kind.setdefault(record["kind"], []).append(record)

    for entry in manifest["evidence_policy"]["entries"]:
        kind = entry["kind"]
        matches = by_kind.get(kind, [])
        expected_count = int(entry["count"])
        if len(matches) != expected_count:
            violations.append(
                {
                    "kind": kind,
                    "rule": "count",
                    "expected": expected_count,
                    "actual": len(matches),
                }
            )
            continue
        for record in matches:
            if record.get("execution_kind") != entry["execution_kind"]:
                violations.append(
                    {
                        "kind": kind,
                        "rule": "execution_kind",
                        "expected": entry["execution_kind"],
                        "actual": record.get("execution_kind"),
                    }
                )
            expected_executable = entry.get("executable")
            if expected_executable:
                resolved = record.get("executable", {}).get(
                    "resolved_path",
                    "",
                )
                actual_executable = (
                    Path(resolved).stem.lower() if resolved else ""
                )
                if actual_executable != expected_executable.lower():
                    violations.append(
                        {
                            "kind": kind,
                            "rule": "executable",
                            "expected": expected_executable,
                            "actual": actual_executable,
                        }
                    )
            argv = [str(item) for item in record.get("argv", [])]
            for token in entry.get("required_argv_contains", []):
                if not any(token in item for item in argv):
                    violations.append(
                        {
                            "kind": kind,
                            "rule": "argv_contains",
                            "expected": token,
                            "actual": argv,
                        }
                    )
            stdout_path = record.get("stdout", {}).get("path")
            stdout_text = ""
            if stdout_path:
                stdout_text = (run_root / stdout_path).read_bytes().decode(
                    "utf-8",
                    errors="replace",
                )
            for token in entry.get("required_stdout_contains", []):
                if token not in stdout_text:
                    violations.append(
                        {
                            "kind": kind,
                            "rule": "stdout_contains",
                            "expected": token,
                            "actual": stdout_path,
                        }
                    )
            overrides = record.get("environment_overrides", {})
            for key in entry.get("required_environment", []):
                if key not in overrides:
                    violations.append(
                        {
                            "kind": kind,
                            "rule": "environment",
                            "expected": key,
                            "actual": sorted(overrides),
                        }
                    )
            artifact_roles = {
                item.get("role") for item in record.get("artifacts", [])
            }
            for role in entry.get(
                "required_recorded_artifact_roles",
                [],
            ):
                if role not in artifact_roles:
                    violations.append(
                        {
                            "kind": kind,
                            "rule": "recorded_artifact_role",
                            "expected": role,
                            "actual": sorted(
                                item
                                for item in artifact_roles
                                if item is not None
                            ),
                        }
                    )
        for relative in entry.get("required_artifacts", []):
            path = run_root / Path(relative)
            if not path.is_file():
                violations.append(
                    {
                        "kind": kind,
                        "rule": "artifact",
                        "expected": relative,
                        "actual": None,
                    }
                )
    return violations


def validate_ctest_coverage(
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    violations = []
    by_kind = {record["kind"]: record for record in records}
    inventory = by_kind.get("ctest-inventory")
    if inventory is None:
        return violations
    inventory_path = run_root / inventory["stdout"]["path"]
    inventory_text = inventory_path.read_bytes().decode(
        "utf-8",
        errors="replace",
    )
    match = re.search(r"Total Tests:\s*(\d+)", inventory_text)
    if not match:
        return [
            {
                "kind": "ctest-inventory",
                "rule": "inventory_count",
                "expected": "Total Tests: <count>",
                "actual": None,
            }
        ]
    inventory_count = int(match.group(1))
    if inventory_count <= 0:
        violations.append(
            {
                "kind": "ctest-inventory",
                "rule": "inventory_count",
                "expected": "> 0",
                "actual": inventory_count,
            }
        )

    forbidden = {
        "-R",
        "--tests-regex",
        "-E",
        "--exclude-regex",
        "-I",
        "--tests-information",
        "-L",
        "--label-regex",
        "-LE",
        "--label-exclude",
        "-N",
        "--show-only",
    }
    for kind in ("ctest-debug", "ctest-release"):
        record = by_kind.get(kind)
        if record is None:
            continue
        argv = [str(item) for item in record.get("argv", [])]
        present_forbidden = sorted(forbidden.intersection(argv))
        if present_forbidden:
            violations.append(
                {
                    "kind": kind,
                    "rule": "unfiltered_ctest",
                    "expected": "no test filters",
                    "actual": present_forbidden,
                }
            )
        junit_items = [
            item
            for item in record.get("artifacts", [])
            if item.get("role") == "junit"
        ]
        if len(junit_items) != 1:
            continue
        junit_path = run_root / junit_items[0]["path"]
        try:
            output_index = argv.index("--output-junit")
            declared_junit = Path(argv[output_index + 1]).resolve()
        except (ValueError, IndexError):
            declared_junit = None
        if declared_junit != junit_path.resolve():
            violations.append(
                {
                    "kind": kind,
                    "rule": "junit_output_path",
                    "expected": str(junit_path.resolve()),
                    "actual": (
                        str(declared_junit)
                        if declared_junit is not None
                        else None
                    ),
                }
            )
        try:
            root = et.parse(junit_path).getroot()
            if "tests" in root.attrib:
                junit_count = int(root.attrib["tests"])
            else:
                junit_count = sum(
                    int(node.attrib.get("tests", "0"))
                    for node in root.findall(".//testsuite")
                )
        except (et.ParseError, ValueError):
            junit_count = -1
        if junit_count != inventory_count:
            violations.append(
                {
                    "kind": kind,
                    "rule": "junit_inventory_count",
                    "expected": inventory_count,
                    "actual": junit_count,
                }
            )
    return violations


def validate_record_scope(
    manifest: dict[str, Any],
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    violations = []
    source_root = Path(manifest["source_root"]).resolve()
    build_dir = (run_root / "build" / "vs").resolve()
    by_kind = {record["kind"]: record for record in records}
    tool_kinds = {
        "configure": "cmake",
        "ctest-inventory": "ctest",
        "ctest-debug": "ctest",
        "ctest-release": "ctest",
        "generator-binding": "python",
        "generator-wire": "python",
        "generator-effects": "python",
        "sdk-contract-managed": "dotnet",
        "sdk-contract-native": "dotnet",
        "perf-compressor-matrix": "python",
        "memory-baseline": "python",
    }
    for kind, tool_name in tool_kinds.items():
        record = by_kind.get(kind)
        if not record or record.get("execution_kind") != "subprocess":
            continue
        expected_tool = manifest["tools"][tool_name]
        actual_tool = record.get("executable", {})
        expected_path = Path(expected_tool["resolved_path"]).resolve()
        actual_path = actual_tool.get("resolved_path")
        if (
            not actual_path
            or Path(actual_path).resolve() != expected_path
            or actual_tool.get("sha256") != expected_tool["sha256"]
        ):
            violations.append(
                {
                    "kind": kind,
                    "rule": "qualified_tool_identity",
                    "expected": {
                        "path": str(expected_path),
                        "sha256": expected_tool["sha256"],
                    },
                    "actual": actual_tool,
                }
            )

    def has_exact_path(argv: list[str], expected: Path) -> bool:
        for value in argv:
            try:
                candidate = Path(value)
                if candidate.is_absolute() and candidate.resolve() == expected:
                    return True
            except OSError:
                continue
        return False

    configure = by_kind.get("configure")
    if configure:
        argv = [str(item) for item in configure.get("argv", [])]
        for role, expected in (
            ("source", source_root),
            ("build", build_dir),
        ):
            if not has_exact_path(argv, expected):
                violations.append(
                    {
                        "kind": "configure",
                        "rule": f"qualified_{role}_path",
                        "expected": str(expected),
                        "actual": argv,
                    }
                )

    for kind in ("ctest-inventory", "ctest-debug", "ctest-release"):
        record = by_kind.get(kind)
        if record:
            argv = [str(item) for item in record.get("argv", [])]
            if not has_exact_path(argv, build_dir):
                violations.append(
                    {
                        "kind": kind,
                        "rule": "qualified_test_dir",
                        "expected": str(build_dir),
                        "actual": argv,
                    }
                )

    generator_paths = {
        "generator-binding": (
            source_root
            / "HarkenEngine"
            / "Source"
            / "Tooling"
            / "Binding"
            / "gen_binding.py"
        ),
        "generator-wire": (
            source_root
            / "HarkenEngine"
            / "Source"
            / "Tooling"
            / "Remote"
            / "wire"
            / "gen_wire.py"
        ),
        "generator-effects": (
            source_root
            / "HarkenEngine"
            / "Source"
            / "Tooling"
            / "Effects"
            / "gen_effect_registry.py"
        ),
    }
    for kind, expected in generator_paths.items():
        record = by_kind.get(kind)
        if record:
            argv = [str(item) for item in record.get("argv", [])]
            if not has_exact_path(argv, expected.resolve()):
                violations.append(
                    {
                        "kind": kind,
                        "rule": "qualified_generator_path",
                        "expected": str(expected),
                        "actual": argv,
                    }
                )

    sdk_project = (
        source_root
        / "HarkenSDK"
        / "Test"
        / "ContractTest"
        / "AbiFidelity.csproj"
    ).resolve()
    sdk_artifacts = (run_root / "build" / "sdk").resolve()
    for kind in ("sdk-contract-managed", "sdk-contract-native"):
        record = by_kind.get(kind)
        if record:
            argv = [str(item) for item in record.get("argv", [])]
            if not has_exact_path(argv, sdk_project):
                violations.append(
                    {
                        "kind": kind,
                        "rule": "qualified_sdk_project",
                        "expected": str(sdk_project),
                        "actual": argv,
                    }
                )
    native_record = by_kind.get("sdk-contract-native")
    if native_record:
        expected_native_dir = (
            run_root / "build" / "vs" / "bin" / "Release"
        ).resolve()
        actual_native_dir = native_record.get(
            "environment_overrides",
            {},
        ).get("HARKEN_NATIVE_DIR")
        if (
            not actual_native_dir
            or Path(actual_native_dir).resolve() != expected_native_dir
        ):
            violations.append(
                {
                    "kind": "sdk-contract-native",
                    "rule": "qualified_native_dir",
                    "expected": str(expected_native_dir),
                    "actual": actual_native_dir,
                }
            )

    benchmark_kinds = {
        "perf-phase1": "harken_benchmarks.exe",
        "perf-compressor-voice-self": "harken_benchmarks.exe",
        "perf-compressor-bus-routed": "harken_benchmarks.exe",
        "perf-resample": "harken_resample_benchmark.exe",
        "perf-denormal": "harken_denormal_probe.exe",
        "perf-meter": "harken_meter_benchmark.exe",
    }
    release_bin = run_root / "build" / "vs" / "bin" / "Release"
    for kind, filename in benchmark_kinds.items():
        record = by_kind.get(kind)
        if record:
            actual_identity = record.get("executable", {})
            actual = actual_identity.get("resolved_path")
            expected = (release_bin / filename).resolve()
            if not actual or Path(actual).resolve() != expected:
                violations.append(
                    {
                        "kind": kind,
                        "rule": "qualified_benchmark_binary",
                        "expected": str(expected),
                        "actual": actual,
                    }
                )
            expected_hash = (
                sha256_file(expected) if expected.is_file() else None
            )
            if actual_identity.get("sha256") != expected_hash:
                violations.append(
                    {
                        "kind": kind,
                        "rule": "qualified_benchmark_hash",
                        "expected": expected_hash,
                        "actual": actual_identity.get("sha256"),
                    }
                )
    compressor_record = by_kind.get("perf-compressor-matrix")
    if (
        compressor_record
        and compressor_record.get("execution_kind") == "subprocess"
    ):
        frozen_observer = next(
            (
                run_root / item["path"]
                for item in manifest["qualification_harness"]["files"]
                if Path(item["path"]).name
                == "runtime_compressor_characterization.py"
            ),
            None,
        )
        expected_binary = (
            release_bin / "harken_benchmarks.exe"
        ).resolve()
        argv = [
            str(item) for item in compressor_record.get("argv", [])
        ]
        if frozen_observer is None or not has_exact_path(
            argv,
            frozen_observer.resolve(),
        ):
            violations.append(
                {
                    "kind": "perf-compressor-matrix",
                    "rule": "qualified_compressor_observer",
                    "expected": str(frozen_observer),
                    "actual": argv,
                }
            )
        if not has_exact_path(argv, expected_binary):
            violations.append(
                {
                    "kind": "perf-compressor-matrix",
                    "rule": "qualified_compressor_binary",
                    "expected": str(expected_binary),
                    "actual": argv,
                }
            )
    memory_record = by_kind.get("memory-baseline")
    if (
        memory_record
        and memory_record.get("execution_kind") == "subprocess"
    ):
        frozen_observer = next(
            (
                run_root / item["path"]
                for item in manifest["qualification_harness"]["files"]
                if Path(item["path"]).name
                == "runtime_memory_observer.py"
            ),
            None,
        )
        expected_binary = (
            release_bin / "harken_benchmarks.exe"
        ).resolve()
        argv = [
            str(item) for item in memory_record.get("argv", [])
        ]
        if frozen_observer is None or not has_exact_path(
            argv,
            frozen_observer.resolve(),
        ):
            violations.append(
                {
                    "kind": "memory-baseline",
                    "rule": "qualified_memory_observer",
                    "expected": str(frozen_observer),
                    "actual": argv,
                }
            )
        if not has_exact_path(argv, expected_binary):
            violations.append(
                {
                    "kind": "memory-baseline",
                    "rule": "qualified_memory_binary",
                    "expected": str(expected_binary),
                    "actual": argv,
                }
            )

    expected_tails: dict[str, list[str]] = {
        "configure": [
            "-S",
            str(source_root),
            "-B",
            str(build_dir),
            "-G",
            "Visual Studio 17 2022",
            "-A",
            "x64",
            "-T",
            "v143,version=14.42.34433",
            "-DCMAKE_SYSTEM_VERSION=10.0.26100.0",
            "-DHARKEN_BUILD_CORE=ON",
            "-DHARKEN_BUILD_TOOLS=ON",
            "-DHARKEN_BUILD_SHARED=ON",
            "-DHARKEN_BUILD_EDITOR=OFF",
            "-DHARKEN_BUILD_EDITOR_UNIT_TESTS=OFF",
            "-DHARKEN_BUILD_HUB=OFF",
            "-DHARKEN_BUILD_HUB_UNIT_TESTS=OFF",
            "-DHARKEN_BUILD_RUNTIME_UNIT_TESTS=ON",
            "-DHARKEN_BUILD_RUNTIME_BENCHMARKS=ON",
        ],
        "ctest-inventory": [
            "--test-dir",
            str(build_dir),
            "-C",
            "Debug",
            "-N",
        ],
        "ctest-debug": [
            "--test-dir",
            str(build_dir),
            "-C",
            "Debug",
            "--output-on-failure",
            "--output-junit",
            str((run_root / "tests" / "debug.junit.xml").resolve()),
        ],
        "ctest-release": [
            "--test-dir",
            str(build_dir),
            "-C",
            "Release",
            "--output-on-failure",
            "--output-junit",
            str((run_root / "tests" / "release.junit.xml").resolve()),
        ],
        "generator-binding": [
            str(generator_paths["generator-binding"].resolve()),
            "--check",
        ],
        "generator-wire": [
            str(generator_paths["generator-wire"].resolve()),
            "--check",
        ],
        "generator-effects": [
            str(generator_paths["generator-effects"].resolve()),
            "--check",
        ],
        "sdk-contract-managed": [
            "run",
            "--project",
            str(sdk_project),
            "--configuration",
            "Release",
            "--artifacts-path",
            str(sdk_artifacts),
        ],
        "sdk-contract-native": [
            "run",
            "--project",
            str(sdk_project),
            "--configuration",
            "Release",
            "--artifacts-path",
            str(sdk_artifacts),
            "--",
            "--require-native",
        ],
        "perf-phase1": ["--filter", "phase1"],
        "perf-compressor-voice-self": [
            "--filter",
            "compressor-matrix",
            "--case",
            "voice-self",
            "--workers",
            "0",
            "--paired",
        ],
        "perf-compressor-bus-routed": [
            "--filter",
            "compressor-matrix",
            "--case",
            "bus-routed",
            "--workers",
            "0",
            "--paired",
        ],
        "perf-resample": [],
        "perf-denormal": [],
        "perf-meter": [],
    }
    frozen_observer = next(
        (
            (run_root / item["path"]).resolve()
            for item in manifest["qualification_harness"]["files"]
            if Path(item["path"]).name == "runtime_memory_observer.py"
        ),
        None,
    )
    if frozen_observer is not None:
        expected_tails["memory-baseline"] = [
            str(frozen_observer),
            "--run-root",
            str(run_root.resolve()),
            "--commit",
            manifest["commit_sha"],
            "--binary",
            str(
                (
                    release_bin / "harken_benchmarks.exe"
                ).resolve()
            ),
            "--report",
            str(
                (
                    run_root / "memory" / "memory-baseline.json"
                ).resolve()
            ),
            "--stdout",
            str(
                (
                    run_root / "memory" / "observer.stdout.log"
                ).resolve()
            ),
            "--stderr",
            str(
                (
                    run_root / "memory" / "observer.stderr.log"
                ).resolve()
            ),
            "--sample-ms",
            "1",
            "--",
            "--filter",
            "phase1",
        ]
    frozen_compressor_observer = next(
        (
            (run_root / item["path"]).resolve()
            for item in manifest["qualification_harness"]["files"]
            if Path(item["path"]).name
            == "runtime_compressor_characterization.py"
        ),
        None,
    )
    if frozen_compressor_observer is not None:
        expected_tails["perf-compressor-matrix"] = [
            str(frozen_compressor_observer),
            "--run-root",
            str(run_root.resolve()),
            "--commit",
            manifest["commit_sha"],
            "--binary",
            str(
                (
                    release_bin / "harken_benchmarks.exe"
                ).resolve()
            ),
            "--report",
            str(
                (
                    run_root
                    / "perf"
                    / "compressor-characterization.json"
                ).resolve()
            ),
            "--stdout",
            str(
                (
                    run_root
                    / "perf"
                    / "compressor-characterization.stdout.log"
                ).resolve()
            ),
            "--stderr",
            str(
                (
                    run_root
                    / "perf"
                    / "compressor-characterization.stderr.log"
                ).resolve()
            ),
            "--",
            "--filter",
            "compressor-matrix",
        ]
    for kind, expected_tail in expected_tails.items():
        record = by_kind.get(kind)
        if not record or record.get("execution_kind") != "subprocess":
            continue
        actual_cwd = Path(record.get("cwd", "")).resolve()
        if actual_cwd != source_root:
            violations.append(
                {
                    "kind": kind,
                    "rule": "qualified_command_cwd",
                    "expected": str(source_root),
                    "actual": str(actual_cwd),
                }
            )
        actual_tail = [
            str(item) for item in record.get("argv", [])[1:]
        ]
        if actual_tail != expected_tail:
            violations.append(
                {
                    "kind": kind,
                    "rule": "exact_command_tail",
                    "expected": expected_tail,
                    "actual": actual_tail,
                }
            )
    return violations


def validate_compressor_evidence(
    manifest: dict[str, Any],
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    record = next(
        (
            item
            for item in records
            if item["kind"] == "perf-compressor-matrix"
        ),
        None,
    )
    if record is None:
        return []
    artifacts_by_role: dict[str, list[dict[str, Any]]] = {}
    for item in record.get("artifacts", []):
        artifacts_by_role.setdefault(item.get("role", ""), []).append(item)
    violations = []

    def require(condition: bool, rule: str, expected: Any, actual: Any) -> None:
        if not condition:
            violations.append(
                {
                    "kind": "perf-compressor-matrix",
                    "rule": rule,
                    "expected": expected,
                    "actual": actual,
                }
            )

    for role in ("report", "raw-stdout", "raw-stderr"):
        require(
            len(artifacts_by_role.get(role, [])) == 1,
            f"compressor_{role}_artifact",
            "exactly one",
            len(artifacts_by_role.get(role, [])),
        )
    reports = artifacts_by_role.get("report", [])
    if len(reports) != 1:
        return violations
    expected_report_path = (
        run_root / "perf" / "compressor-characterization.json"
    ).resolve()
    report_path = (run_root / reports[0]["path"]).resolve()
    require(
        report_path == expected_report_path,
        "compressor_report_path",
        str(expected_report_path),
        str(report_path),
    )
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as error:
        require(
            False,
            "compressor_report_json",
            "valid JSON",
            str(error),
        )
        return violations

    require(
        report.get("schema_version") == 1,
        "compressor_schema",
        1,
        report.get("schema_version"),
    )
    require(
        report.get("claim_id") == "M1-compressor-characterization",
        "compressor_claim",
        "M1-compressor-characterization",
        report.get("claim_id"),
    )
    require(
        report.get("commit_sha") == manifest["commit_sha"],
        "compressor_commit",
        manifest["commit_sha"],
        report.get("commit_sha"),
    )
    require(
        report.get("validation_passed") is True,
        "compressor_validation",
        True,
        report.get("validation_passed"),
    )
    require(
        report.get("validation_issues") == [],
        "compressor_validation_issues",
        [],
        report.get("validation_issues"),
    )

    expected_binary = (
        run_root
        / "build"
        / "vs"
        / "bin"
        / "Release"
        / "harken_benchmarks.exe"
    ).resolve()
    binary = report.get("binary", {})
    actual_binary_path = binary.get("path")
    require(
        bool(actual_binary_path)
        and Path(actual_binary_path).resolve() == expected_binary,
        "compressor_binary_path",
        str(expected_binary),
        actual_binary_path,
    )
    expected_binary_hash = (
        sha256_file(expected_binary) if expected_binary.is_file() else None
    )
    require(
        binary.get("sha256") == expected_binary_hash,
        "compressor_binary_hash",
        expected_binary_hash,
        binary.get("sha256"),
    )
    require(
        binary.get("post_sha256") == expected_binary_hash
        and binary.get("unchanged") is True,
        "compressor_binary_unchanged",
        expected_binary_hash,
        {
            "post_sha256": binary.get("post_sha256"),
            "unchanged": binary.get("unchanged"),
        },
    )
    require(
        binary.get("argv")
        == [str(expected_binary), "--filter", "compressor-matrix"],
        "compressor_child_argv",
        [str(expected_binary), "--filter", "compressor-matrix"],
        binary.get("argv"),
    )
    require(
        bool(binary.get("cwd"))
        and Path(binary["cwd"]).resolve()
        == Path(manifest["source_root"]).resolve(),
        "compressor_child_cwd",
        manifest["source_root"],
        binary.get("cwd"),
    )
    require(
        binary.get("exit_code") == 1,
        "compressor_expected_child_exit",
        1,
        binary.get("exit_code"),
    )

    frozen_observer = next(
        (
            item
            for item in manifest["qualification_harness"]["files"]
            if Path(item["path"]).name
            == "runtime_compressor_characterization.py"
        ),
        None,
    )
    observer = report.get("observer", {})
    expected_observer_path = (
        (run_root / frozen_observer["path"]).resolve()
        if frozen_observer
        else None
    )
    actual_observer_path = observer.get("path")
    require(
        expected_observer_path is not None
        and bool(actual_observer_path)
        and Path(actual_observer_path).resolve() == expected_observer_path,
        "compressor_observer_path",
        str(expected_observer_path),
        actual_observer_path,
    )
    require(
        frozen_observer is not None
        and observer.get("sha256") == frozen_observer["sha256"],
        "compressor_observer_hash",
        frozen_observer["sha256"] if frozen_observer else None,
        observer.get("sha256"),
    )
    require(
        isinstance(observer.get("duration_seconds"), (int, float))
        and not isinstance(observer.get("duration_seconds"), bool)
        and math.isfinite(float(observer["duration_seconds"]))
        and observer["duration_seconds"] > 0,
        "compressor_duration",
        "> 0 finite",
        observer.get("duration_seconds"),
    )

    expected_raw_paths = {
        "raw-stdout": (
            run_root
            / "perf"
            / "compressor-characterization.stdout.log"
        ).resolve(),
        "raw-stderr": (
            run_root
            / "perf"
            / "compressor-characterization.stderr.log"
        ).resolve(),
    }
    report_raw = report.get("raw_evidence", {})
    for role, report_key in (
        ("raw-stdout", "stdout"),
        ("raw-stderr", "stderr"),
    ):
        descriptors = artifacts_by_role.get(role, [])
        expected_path = expected_raw_paths[role]
        if len(descriptors) == 1:
            actual_path = (run_root / descriptors[0]["path"]).resolve()
            require(
                actual_path == expected_path,
                f"compressor_{role}_path",
                str(expected_path),
                str(actual_path),
            )
        raw = report_raw.get(report_key, {})
        require(
            bool(raw.get("path"))
            and Path(raw["path"]).resolve() == expected_path,
            f"compressor_report_{report_key}_path",
            str(expected_path),
            raw.get("path"),
        )
        expected_hash = (
            sha256_file(expected_path) if expected_path.is_file() else None
        )
        expected_size = (
            expected_path.stat().st_size
            if expected_path.is_file()
            else None
        )
        require(
            raw.get("sha256") == expected_hash,
            f"compressor_report_{report_key}_hash",
            expected_hash,
            raw.get("sha256"),
        )
        require(
            raw.get("size") == expected_size,
            f"compressor_report_{report_key}_size",
            expected_size,
            raw.get("size"),
        )
    require(
        expected_raw_paths["raw-stderr"].is_file()
        and expected_raw_paths["raw-stderr"].stat().st_size == 0,
        "compressor_raw_stderr_empty",
        0,
        (
            expected_raw_paths["raw-stderr"].stat().st_size
            if expected_raw_paths["raw-stderr"].is_file()
            else None
        ),
    )

    result = report.get("result", {})
    require(
        result.get("measured_rows") == 35,
        "compressor_measured_rows",
        35,
        result.get("measured_rows"),
    )
    require(
        isinstance(result.get("deadline_misses"), int)
        and not isinstance(result.get("deadline_misses"), bool)
        and result["deadline_misses"] >= 0,
        "compressor_deadline_misses",
        "non-negative integer diagnostic",
        result.get("deadline_misses"),
    )
    require(
        isinstance(result.get("absolute_budget_failures"), list),
        "compressor_absolute_budget_findings",
        "array",
        result.get("absolute_budget_failures"),
    )
    fraction = result.get("max_callback_deadline_fraction")
    require(
        isinstance(fraction, (int, float))
        and not isinstance(fraction, bool)
        and math.isfinite(float(fraction))
        and fraction >= 0,
        "compressor_max_callback_fraction",
        "non-negative finite",
        fraction,
    )
    require(
        result.get("full_runtime_gate_status")
        == "unverified-full-matrix-and-external-evidence-required",
        "compressor_full_gate_status",
        "unverified-full-matrix-and-external-evidence-required",
        result.get("full_runtime_gate_status"),
    )
    expected_boundary = {
        "characterization_complete": True,
        "finite_cost_regression_certified": False,
        "hard_realtime_certified": False,
        "glitch_free_certified": False,
        "full_runtime_gate_closed": False,
    }
    require(
        report.get("claim_boundary") == expected_boundary,
        "compressor_claim_boundary",
        expected_boundary,
        report.get("claim_boundary"),
    )
    return violations


def validate_compressor_paired_evidence(
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    expected = {
        "perf-compressor-voice-self": {
            "scenario": "voice-self-64",
            "mask": "00000001",
        },
        "perf-compressor-bus-routed": {
            "scenario": "bus-routed-16x4",
            "mask": "00000010",
        },
    }
    violations = []
    observed_masks = 0

    def require(
        kind: str,
        condition: bool,
        rule: str,
        expected_value: Any,
        actual: Any,
    ) -> None:
        if not condition:
            violations.append(
                {
                    "kind": kind,
                    "rule": rule,
                    "expected": expected_value,
                    "actual": actual,
                }
            )

    def require_contract(
        kind: str,
        row: dict[str, Any],
        scoped_applicable: bool,
        scoped_passed: bool,
        miss_count: int,
        context: str,
    ) -> None:
        values = {
            "performance_contract_vintage": 2,
            "performance_gate_scope": "finite-cost-regression-only",
            "scoped_cost_gate_applicable": scoped_applicable,
            "scoped_cost_gate_passed": scoped_passed,
            "deadline_misses_diagnostic_only": True,
            "deadline_miss_hard_fail_applicable": False,
            "deadline_reliability_claimed": False,
            "deadline_observation_status": (
                "clear" if miss_count == 0 else "unclassified"
            ),
            "scoped_gate_meaning": (
                "finite-cost-regression-only-not-hard-realtime-"
                "or-glitch-free"
            ),
        }
        for field, value in values.items():
            require(
                kind,
                row.get(field) == value,
                f"{context}_{field}",
                value,
                row.get(field),
            )

    for kind, contract in expected.items():
        record = next(
            (item for item in records if item["kind"] == kind),
            None,
        )
        if record is None:
            continue
        stderr_descriptor = record.get("stderr", {})
        require(
            kind,
            stderr_descriptor.get("size") == 0,
            "paired_stderr_empty",
            0,
            stderr_descriptor.get("size"),
        )
        stdout_path = run_root / record.get("stdout", {}).get("path", "")
        try:
            rows = [
                json.loads(line)
                for line in stdout_path.read_text(
                    encoding="utf-8"
                ).splitlines()
                if line.strip()
            ]
        except (json.JSONDecodeError, OSError, UnicodeDecodeError) as error:
            require(
                kind,
                False,
                "paired_stdout_json",
                "complete UTF-8 JSON lines",
                str(error),
            )
            continue
        require(
            kind,
            all(isinstance(row, dict) for row in rows),
            "paired_json_objects",
            True,
            False,
        )
        if not all(isinstance(row, dict) for row in rows):
            continue
        require(
            kind,
            all(
                row.get("benchmark") == "compressor-matrix-paired"
                for row in rows
            ),
            "paired_benchmark",
            "compressor-matrix-paired",
            sorted(
                {
                    row.get("benchmark")
                    for row in rows
                    if isinstance(row, dict)
                },
                key=str,
            ),
        )
        require(
            kind,
            all(finite_json_numbers(row) for row in rows),
            "paired_finite_json",
            True,
            False,
        )
        statuses: dict[str, list[dict[str, Any]]] = {}
        for row in rows:
            statuses.setdefault(str(row.get("status")), []).append(row)
        expected_counts = {
            "matrix": 1,
            "session": 20,
            "measured": 5,
            "aggregate": 1,
            "complete": 1,
        }
        require(
            kind,
            {name: len(items) for name, items in statuses.items()}
            == expected_counts,
            "paired_status_counts",
            expected_counts,
            {name: len(items) for name, items in statuses.items()},
        )
        if any(
            len(statuses.get(name, [])) != count
            for name, count in expected_counts.items()
        ):
            continue

        matrix = statuses["matrix"][0]
        require_contract(
            kind,
            matrix,
            False,
            False,
            0,
            "paired_matrix",
        )
        matrix_expected = {
            "order": "B-F-F-B",
            "pairs": 5,
            "warmup_blocks_per_session": 2000,
            "measured_blocks_per_session": 10000,
            "process_pinned": True,
            "priority_set": True,
            "canonical_counts": True,
            "expected_scoped_scenario_worker_mask": contract["mask"],
            "expected_scoped_rows": 1,
        }
        for field, value in matrix_expected.items():
            require(
                kind,
                matrix.get(field) == value,
                f"paired_matrix_{field}",
                value,
                matrix.get(field),
            )

        expected_sessions = {
            (pair, role)
            for pair in range(1, 6)
            for role in ("B1", "F1", "F2", "B2")
        }
        observed_sessions = []
        deadline_misses_by_pair = {
            pair: 0 for pair in range(1, 6)
        }
        for row in statuses["session"]:
            key = (row.get("pair"), row.get("role"))
            observed_sessions.append(key)
            require(
                kind,
                isinstance(row.get("pair"), int)
                and not isinstance(row.get("pair"), bool),
                "paired_session_pair_type",
                "integer",
                row.get("pair"),
            )
            require(
                kind,
                isinstance(row.get("role"), str),
                "paired_session_role_type",
                "string",
                row.get("role"),
            )
            fixed = {
                "scenario": contract["scenario"],
                "workers": 0,
                "sample_rate": 48000,
                "block_frames": 256,
                "channels": 8,
                "warmup_blocks": 2000,
                "measured_blocks": 10000,
                "process_pinned": True,
                "priority_set": True,
                "callback_thread_priority_gate_passed": True,
                "output_finite": True,
                "nonfinite_output_sample_count": 0,
                "arena_overflow_count": 0,
                "dynamic_route_refusal_count": 0,
                "dynamic_plan_retire_overflow_count": 0,
                "setup_valid": True,
                "setup_error": "none",
            }
            for field, value in fixed.items():
                require(
                    kind,
                    row.get(field) == value,
                    f"paired_session_{field}",
                    value,
                    row.get(field),
                )
            session_misses = row.get("deadline_misses")
            require(
                kind,
                isinstance(session_misses, int)
                and not isinstance(session_misses, bool)
                and session_misses >= 0,
                "paired_session_deadline_misses",
                "non-negative integer diagnostic",
                session_misses,
            )
            if (
                isinstance(session_misses, int)
                and not isinstance(session_misses, bool)
                and session_misses >= 0
                and row.get("pair") in deadline_misses_by_pair
            ):
                deadline_misses_by_pair[row["pair"]] += session_misses
                require_contract(
                    kind,
                    row,
                    False,
                    False,
                    session_misses,
                    "paired_session",
                )
            for field in ("median_us", "p99_us", "max_us"):
                value = row.get(field)
                require(
                    kind,
                    isinstance(value, (int, float))
                    and not isinstance(value, bool)
                    and math.isfinite(float(value))
                    and value >= 0,
                    f"paired_session_{field}",
                    "non-negative finite",
                    value,
                )
        require(
            kind,
            len(observed_sessions) == len(set(observed_sessions))
            and set(observed_sessions) == expected_sessions,
            "paired_session_coverage",
            sorted(expected_sessions),
            sorted(observed_sessions, key=str),
        )

        measured_pairs = []
        for row in statuses["measured"]:
            measured_pairs.append(row.get("pair"))
            require(
                kind,
                isinstance(row.get("pair"), int)
                and not isinstance(row.get("pair"), bool),
                "paired_measured_pair_type",
                "integer",
                row.get("pair"),
            )
            fixed = {
                "scenario": contract["scenario"],
                "workers": 0,
                "order": "B-F-F-B",
                "sample_rate": 48000,
                "block_frames": 256,
                "channels": 8,
                "process_pinned": True,
                "priority_set": True,
                "callback_thread_priority_gate_passed": True,
                "warmup_blocks_per_session": 2000,
                "measured_blocks_per_session": 10000,
                "matched_delta_blocks": 10000,
                "paired_delta_within_budget": True,
                "quartet_absolute_p99_below_deadline": True,
                "all_sessions_output_finite": True,
                "nonfinite_output_sample_count": 0,
                "scoped_gate_applicable": True,
                "aggregate_delta_conditions_met": True,
                "aggregate_delta_gate_passed": True,
                "scoped_gate_passed": True,
                "scoped_gate_status": "passed",
                "hashes_stable": True,
                "same_init_config": True,
                "setup_phase_aligned": True,
                "setup_valid": True,
            }
            for field, value in fixed.items():
                require(
                    kind,
                    row.get(field) == value,
                    f"paired_measured_{field}",
                    value,
                    row.get(field),
                )
            pair_value = row.get("pair")
            pair_misses = deadline_misses_by_pair.get(pair_value)
            require(
                kind,
                isinstance(row.get("quartet_zero_deadline_misses"), bool)
                and row["quartet_zero_deadline_misses"]
                == (pair_misses == 0),
                "paired_measured_deadline_diagnostic",
                pair_misses == 0,
                row.get("quartet_zero_deadline_misses"),
            )
            component_miss_fields = (
                "feature_first_absolute_deadline_misses",
                "feature_second_absolute_deadline_misses",
                "baseline_first_absolute_deadline_misses",
                "baseline_second_absolute_deadline_misses",
            )
            component_misses = []
            for field in component_miss_fields:
                value = row.get(field)
                require(
                    kind,
                    isinstance(value, int)
                    and not isinstance(value, bool)
                    and value >= 0,
                    f"paired_measured_{field}",
                    "non-negative integer diagnostic",
                    value,
                )
                if (
                    isinstance(value, int)
                    and not isinstance(value, bool)
                    and value >= 0
                ):
                    component_misses.append(value)
            if pair_misses is not None:
                require(
                    kind,
                    len(component_misses) == 4
                    and sum(component_misses) == pair_misses,
                    "paired_measured_deadline_sum",
                    pair_misses,
                    (
                        sum(component_misses)
                        if len(component_misses) == 4
                        else component_misses
                    ),
                )
                require_contract(
                    kind,
                    row,
                    True,
                    True,
                    pair_misses,
                    "paired_measured",
                )
            for field in (
                "paired_delta_median_us",
                "paired_delta_p99_us",
                "paired_delta_max_us",
                "paired_delta_budget_us",
                "feature_absolute_p99_us",
                "baseline_absolute_p99_us",
            ):
                value = row.get(field)
                require(
                    kind,
                    isinstance(value, (int, float))
                    and not isinstance(value, bool)
                    and math.isfinite(float(value))
                    and value >= 0,
                    f"paired_measured_{field}",
                    "non-negative finite",
                    value,
                )
        require(
            kind,
            sorted(measured_pairs) == [1, 2, 3, 4, 5],
            "paired_measured_coverage",
            [1, 2, 3, 4, 5],
            sorted(measured_pairs, key=str),
        )

        aggregate = statuses["aggregate"][0]
        total_deadline_misses = sum(deadline_misses_by_pair.values())
        require_contract(
            kind,
            aggregate,
            True,
            True,
            total_deadline_misses,
            "paired_aggregate",
        )
        aggregate_expected = {
            "scenario": contract["scenario"],
            "workers": 0,
            "pairs": 5,
            "all_sessions_output_finite": True,
            "nonfinite_output_sample_count": 0,
            "process_pinned": True,
            "priority_set": True,
            "scoped_gate_applicable": True,
            "setup_valid": True,
            "scoped_gate_passed": True,
            "scoped_gate_status": "passed",
        }
        for field, value in aggregate_expected.items():
            require(
                kind,
                aggregate.get(field) == value,
                f"paired_aggregate_{field}",
                value,
                aggregate.get(field),
            )
        aggregate_max_delta = aggregate.get("max_pair_delta_p99_us")
        expected_budget = (
            1066.666
            if contract["scenario"] == "voice-self-64"
            else 1333.333
        )
        require(
            kind,
            isinstance(aggregate_max_delta, (int, float))
            and not isinstance(aggregate_max_delta, bool)
            and math.isfinite(float(aggregate_max_delta))
            and 0 <= aggregate_max_delta <= expected_budget,
            "paired_aggregate_max_delta",
            f"0..{expected_budget}",
            aggregate_max_delta,
        )

        complete = statuses["complete"][0]
        require_contract(
            kind,
            complete,
            True,
            True,
            total_deadline_misses,
            "paired_complete",
        )
        complete_expected = {
            "setup_valid": True,
            "process_pinned": True,
            "priority_set": True,
            "canonical_counts": True,
            "expected_scoped_scenario_worker_mask": contract["mask"],
            "observed_scoped_scenario_worker_mask": contract["mask"],
            "expected_scoped_rows": 1,
            "observed_scoped_rows": 1,
            "scoped_coverage_complete": True,
            "all_sessions_output_finite": True,
            "nonfinite_output_sample_count": 0,
            "required_paired_scenario_worker_mask": "00000011",
            "required_paired_coverage_complete": False,
            "scoped_gate_applicable": True,
            "scoped_gate_passed": True,
            "scoped_gate_status": "passed",
            "overall_runtime_gate_passed": False,
            "overall_runtime_gate_status": (
                "unverified-full-matrix-and-external-evidence-required"
            ),
        }
        for field, value in complete_expected.items():
            require(
                kind,
                complete.get(field) == value,
                f"paired_complete_{field}",
                value,
                complete.get(field),
            )
        mask_text = complete.get(
            "observed_scoped_scenario_worker_mask"
        )
        try:
            observed_masks |= int(mask_text, 16)
        except (TypeError, ValueError):
            require(
                kind,
                False,
                "paired_mask_parse",
                contract["mask"],
                mask_text,
            )

    present = {
        record["kind"]
        for record in records
        if record["kind"] in expected
    }
    if present == set(expected):
        require(
            "perf-compressor-paired-union",
            observed_masks == 0x11,
            "paired_union_coverage",
            "00000011",
            f"{observed_masks:08x}",
        )
    return violations


def validate_memory_evidence(
    manifest: dict[str, Any],
    run_root: Path,
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    entry = next(
        (
            item
            for item in manifest["evidence_policy"]["entries"]
            if item["kind"] == "memory-baseline"
        ),
        None,
    )
    if entry is None or entry["execution_kind"] != "subprocess":
        return []
    record = next(
        (
            item
            for item in records
            if item["kind"] == "memory-baseline"
        ),
        None,
    )
    if record is None:
        return []
    artifacts_by_role: dict[str, list[dict[str, Any]]] = {}
    for item in record.get("artifacts", []):
        artifacts_by_role.setdefault(item.get("role", ""), []).append(item)
    reports = artifacts_by_role.get("report", [])
    role_violations = []
    for role in ("report", "raw-stdout", "raw-stderr"):
        if len(artifacts_by_role.get(role, [])) != 1:
            role_violations.append(
                {
                    "kind": "memory-baseline",
                    "rule": f"memory_{role}_artifact",
                    "expected": "exactly one",
                    "actual": len(artifacts_by_role.get(role, [])),
                }
            )
    if len(reports) != 1:
        return role_violations
    expected_report_path = (
        run_root / "memory" / "memory-baseline.json"
    ).resolve()
    report_path = (run_root / reports[0]["path"]).resolve()
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as error:
        return [
            {
                "kind": "memory-baseline",
                "rule": "memory_report_json",
                "expected": "valid JSON",
                "actual": str(error),
            }
        ]

    violations = role_violations

    def require(condition: bool, rule: str, expected: Any, actual: Any) -> None:
        if not condition:
            violations.append(
                {
                    "kind": "memory-baseline",
                    "rule": rule,
                    "expected": expected,
                    "actual": actual,
                }
            )

    require(
        report_path == expected_report_path,
        "memory_report_path",
        str(expected_report_path),
        str(report_path),
    )
    require(
        report.get("commit_sha") == manifest["commit_sha"],
        "memory_commit",
        manifest["commit_sha"],
        report.get("commit_sha"),
    )
    expected_binary = (
        run_root
        / "build"
        / "vs"
        / "bin"
        / "Release"
        / "harken_benchmarks.exe"
    ).resolve()
    binary = report.get("binary", {})
    actual_binary_path = binary.get("path")
    require(
        bool(actual_binary_path)
        and Path(actual_binary_path).resolve() == expected_binary,
        "memory_binary_path",
        str(expected_binary),
        actual_binary_path,
    )
    require(
        expected_binary.is_file()
        and binary.get("sha256") == sha256_file(expected_binary),
        "memory_binary_hash",
        sha256_file(expected_binary) if expected_binary.is_file() else None,
        binary.get("sha256"),
    )
    require(
        expected_binary.is_file()
        and binary.get("post_sha256") == sha256_file(expected_binary)
        and binary.get("unchanged") is True,
        "memory_binary_unchanged",
        sha256_file(expected_binary) if expected_binary.is_file() else None,
        {
            "post_sha256": binary.get("post_sha256"),
            "unchanged": binary.get("unchanged"),
        },
    )
    expected_memory_argv = [
        str(expected_binary),
        "--filter",
        "phase1",
    ]
    require(
        binary.get("argv") == expected_memory_argv,
        "memory_binary_argv",
        expected_memory_argv,
        binary.get("argv"),
    )
    require(
        bool(binary.get("cwd"))
        and Path(binary["cwd"]).resolve() == expected_binary.parent,
        "memory_binary_cwd",
        str(expected_binary.parent),
        binary.get("cwd"),
    )
    require(
        binary.get("exit_code") == 0,
        "memory_binary_exit",
        0,
        binary.get("exit_code"),
    )

    frozen_observer = next(
        (
            item
            for item in manifest["qualification_harness"]["files"]
            if Path(item["path"]).name == "runtime_memory_observer.py"
        ),
        None,
    )
    observer = report.get("observer", {})
    expected_observer_path = (
        (run_root / frozen_observer["path"]).resolve()
        if frozen_observer
        else None
    )
    actual_observer_path = observer.get("path")
    require(
        expected_observer_path is not None
        and bool(actual_observer_path)
        and Path(actual_observer_path).resolve() == expected_observer_path,
        "memory_observer_path",
        str(expected_observer_path),
        actual_observer_path,
    )
    require(
        frozen_observer is not None
        and observer.get("sha256") == frozen_observer["sha256"],
        "memory_observer_hash",
        frozen_observer["sha256"] if frozen_observer else None,
        observer.get("sha256"),
    )
    require(
        isinstance(observer.get("sample_count"), int)
        and observer.get("sample_count", 0) > 0,
        "memory_sample_count",
        "> 0",
        observer.get("sample_count"),
    )
    expected_raw_paths = {
        "raw-stdout": (
            run_root / "memory" / "observer.stdout.log"
        ).resolve(),
        "raw-stderr": (
            run_root / "memory" / "observer.stderr.log"
        ).resolve(),
    }
    report_raw = report.get("raw_evidence", {})
    for role, report_key in (
        ("raw-stdout", "stdout"),
        ("raw-stderr", "stderr"),
    ):
        descriptors = artifacts_by_role.get(role, [])
        expected_path = expected_raw_paths[role]
        if len(descriptors) == 1:
            actual_path = (run_root / descriptors[0]["path"]).resolve()
            require(
                actual_path == expected_path,
                f"memory_{role}_path",
                str(expected_path),
                str(actual_path),
            )
        raw = report_raw.get(report_key, {})
        require(
            bool(raw.get("path"))
            and Path(raw["path"]).resolve() == expected_path,
            f"memory_report_{report_key}_path",
            str(expected_path),
            raw.get("path"),
        )
        expected_hash = (
            sha256_file(expected_path) if expected_path.is_file() else None
        )
        expected_size = (
            expected_path.stat().st_size
            if expected_path.is_file()
            else None
        )
        require(
            raw.get("sha256") == expected_hash,
            f"memory_report_{report_key}_hash",
            expected_hash,
            raw.get("sha256"),
        )
        require(
            raw.get("size") == expected_size,
            f"memory_report_{report_key}_size",
            expected_size,
            raw.get("size"),
        )
    require(
        expected_raw_paths["raw-stderr"].is_file()
        and expected_raw_paths["raw-stderr"].stat().st_size == 0,
        "memory_raw_stderr_empty",
        0,
        (
            expected_raw_paths["raw-stderr"].stat().st_size
            if expected_raw_paths["raw-stderr"].is_file()
            else None
        ),
    )

    config = report.get("config", {})
    expected_config = {
        "backend": "NullBackend",
        "sample_rate": 48000,
        "block_frames": 256,
        "layouts": ["7.1", "stereo"],
        "scenario": "phase1",
    }
    for key, expected in expected_config.items():
        require(
            config.get(key) == expected,
            f"memory_config_{key}",
            expected,
            config.get(key),
        )
    for section in ("process_bytes", "working_set_bytes"):
        values = report.get(section, {})
        for key in ("initial_observation", "peak", "after_exit"):
            value = values.get(key)
            require(
                isinstance(value, int) and value >= 0,
                f"memory_{section}_{key}",
                "non-negative integer",
                value,
            )
        if isinstance(values.get("peak"), int) and isinstance(
            values.get("initial_observation"),
            int,
        ):
            require(
                values["peak"] >= values["initial_observation"],
                f"memory_{section}_peak",
                ">= initial_observation",
                values["peak"],
            )
    arena = report.get("arena", {})
    for key in ("high_water_bytes", "overflow_count"):
        value = arena.get(key)
        require(
            isinstance(value, int) and value >= 0,
            f"memory_arena_{key}",
            "non-negative integer",
            value,
        )
    require(
        isinstance(report.get("capacities"), dict)
        and bool(report.get("capacities")),
        "memory_capacities",
        "non-empty object",
        report.get("capacities"),
    )
    static_allocations = report.get("static_allocations")
    require(
        isinstance(static_allocations, list)
        and bool(static_allocations),
        "memory_static_allocations",
        "non-empty array",
        static_allocations,
    )
    if isinstance(static_allocations, list):
        categories = {
            item.get("name")
            for item in static_allocations
            if isinstance(item, dict)
        }
        require(
            REQUIRED_STATIC_ALLOCATION_CATEGORIES.issubset(categories),
            "memory_static_allocation_coverage",
            sorted(REQUIRED_STATIC_ALLOCATION_CATEGORIES),
            sorted(item for item in categories if item is not None),
        )
        for item in static_allocations:
            if not isinstance(item, dict):
                require(
                    False,
                    "memory_static_allocation_entry",
                    "object",
                    item,
                )
                continue
            byte_count = item.get("bytes")
            require(
                byte_count is None
                or (
                    isinstance(byte_count, int)
                    and byte_count >= 0
                ),
                "memory_static_allocation_bytes",
                "null or non-negative integer",
                byte_count,
            )
            require(
                isinstance(item.get("status"), str)
                and bool(item.get("status")),
                "memory_static_allocation_status",
                "non-empty string",
                item.get("status"),
            )
            require(
                isinstance(item.get("source"), str)
                and bool(item.get("source")),
                "memory_static_allocation_source",
                "non-empty string",
                item.get("source"),
            )
    require(
        isinstance(report.get("limitations"), list)
        and bool(report.get("limitations")),
        "memory_limitations",
        "non-empty array",
        report.get("limitations"),
    )
    return violations


def finalize(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    run_root = require_under(
        Path(args.run_root),
        repo_root / ".temp" / "runtime-qualification",
    )
    manifest = load_manifest(run_root)
    if manifest["state"] == "finalized":
        raise RuntimeError("finalized evidence is immutable")
    verify_harness_identity(manifest, run_root)
    verify_qualification_contract(manifest, run_root)
    verify_source_index(
        Path(manifest["source_root"]),
        run_root,
        manifest["source_identity"]["index"],
    )
    records = read_command_records(run_root / "commands.jsonl")
    integrity_violations = validate_record_integrity(
        run_root,
        records,
    )
    present_kinds = {record["kind"] for record in records}
    required_kinds = sorted(
        item["kind"] for item in manifest["evidence_policy"]["entries"]
    )
    policy_violations = validate_evidence_policy(
        manifest,
        run_root,
        records,
    )
    policy_violations.extend(
        validate_ctest_coverage(run_root, records)
    )
    policy_violations.extend(
        validate_record_scope(manifest, run_root, records)
    )
    policy_violations.extend(
        validate_compressor_evidence(manifest, run_root, records)
    )
    policy_violations.extend(
        validate_compressor_paired_evidence(run_root, records)
    )
    policy_violations.extend(
        validate_memory_evidence(manifest, run_root, records)
    )
    missing_kinds = sorted(
        {
            item["kind"]
            for item in manifest["evidence_policy"]["entries"]
            if not any(
                record["kind"] == item["kind"] for record in records
            )
        }
    )
    failed = [
        {
            "sequence": record["sequence"],
            "kind": record["kind"],
            "exit_code": record["exit_code"],
        }
        for record in records
        if record["exit_code"] != 0
    ]
    invalid_policy = [
        item
        for item in policy_violations
        if item["rule"] not in ("artifact", "count")
        or (
            item["rule"] == "count"
            and int(item.get("actual", 0)) > 0
        )
    ]
    if failed or invalid_policy or integrity_violations:
        qualification_status = "failed"
    elif policy_violations or not records:
        qualification_status = "incomplete"
    else:
        qualification_status = "passed"
    summary = {
        "schema_version": SCHEMA_VERSION,
        "claim_id": manifest["claim_id"],
        "run_id": manifest["run_id"],
        "qualification_status": qualification_status,
        "required_kinds": required_kinds,
        "present_kinds": sorted(present_kinds),
        "missing_kinds": missing_kinds,
        "failed_commands": failed,
        "policy_violations": policy_violations,
        "integrity_violations": integrity_violations,
        "command_count": len(records),
    }
    (run_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    artifacts = []
    for path in iter_artifacts(run_root):
        artifacts.append(
            {
                "path": str(path.relative_to(run_root)),
                "sha256": sha256_file(path),
                "size": path.stat().st_size,
            }
        )
    manifest["timestamp_end_utc"] = utc_now()
    manifest["artifacts"] = artifacts
    manifest["qualification_status"] = qualification_status
    manifest["state"] = "finalized"
    write_manifest(run_root, manifest)
    print(run_root / "manifest.json")
    return 0 if qualification_status == "passed" else 1


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument(
        "--repo-root", default=str(default_repo_root())
    )
    subparsers = result.add_subparsers(dest="subcommand", required=True)

    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--expected-commit", required=True)
    prepare_parser.add_argument("--run-id")
    prepare_parser.add_argument("--run-root")
    prepare_parser.set_defaults(func=prepare)

    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--run-root", required=True)
    run_parser.add_argument("--kind", required=True)
    run_parser.add_argument("--cwd")
    run_parser.add_argument(
        "--env",
        action="append",
        default=[],
    )
    run_parser.add_argument(
        "--artifact",
        action="append",
        default=[],
    )
    run_parser.add_argument("command", nargs=argparse.REMAINDER)
    run_parser.set_defaults(func=execute)

    external_parser = subparsers.add_parser("record-external")
    external_parser.add_argument("--run-root", required=True)
    external_parser.add_argument("--kind", required=True)
    external_parser.add_argument("--cwd")
    external_parser.add_argument(
        "--status",
        required=True,
        choices=("success", "failure"),
    )
    external_parser.add_argument("--started-utc", required=True)
    external_parser.add_argument("--ended-utc", required=True)
    external_parser.add_argument(
        "--artifact",
        action="append",
        default=[],
    )
    external_parser.add_argument(
        "--metadata",
        action="append",
        default=[],
    )
    external_parser.set_defaults(func=record_external)

    verify_parser = subparsers.add_parser("verify-configure")
    verify_parser.add_argument("--run-root", required=True)
    verify_parser.add_argument("--build-dir", required=True)
    verify_parser.set_defaults(func=verify_configure)

    finalize_parser = subparsers.add_parser("finalize")
    finalize_parser.add_argument("--run-root", required=True)
    finalize_parser.set_defaults(func=finalize)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        return int(args.func(args))
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as error:
        print(f"runtime qualification error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
