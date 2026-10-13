#!/usr/bin/env python3
"""Build and verify the fail-closed Runtime callback-reachable call graph."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as et
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1
CLASSIFICATION_SCHEMA_VERSION = 1
EXPECTED_CLANG_BINDING = "19.1.7"
EXPECTED_LIBCLANG = "19.1.5"
EXPECTED_MSVC_TOOLS = "14.42.34433"
EXPECTED_MSVC_COMPILER = "19.42.34444.0"
EXPECTED_WINDOWS_SDK = "10.0.26100.0"
EXPECTED_GENERATOR = "Visual Studio 17 2022"
EXPECTED_PLATFORM = "x64"
EXPECTED_TOOLSET = "v143,version=14.42.34433"
REQUIRED_PROBE_CACHE = {
    "HARKEN_RT_PROBE_DEVICE_CALLBACK": "OFF",
    "HARKEN_RT_PROBE_MIX_PUBLICATION": "OFF",
    "HARKEN_RT_PROBE_WORKER_BARRIER": "OFF",
}
QUALIFICATION_TARGET = "harken_core_rt_qualification"
QUALIFICATION_CMAKE = "HarkenEngine/CMakeLists.txt"
QUALIFICATION_PROJECT_RELATIVE = (
    "HarkenEngine/harken_core_rt_qualification.vcxproj"
)
REQUIRED_CLASSIFICATION_FIELDS = {
    "allocation",
    "blocking",
    "io",
    "exception",
    "loopBound",
    "configurationBound",
    "threadQos",
    "evidenceOwner",
    "cost",
}
CLASSIFICATION_ENUMS = {
    "allocation": {
        "none",
        "bounded-block-arena",
        "external-no-allocation",
        "forbidden-system-allocation",
        "unknown-blocker",
    },
    "blocking": {
        "none",
        "lock-free-retry",
        "coordinator-yield-spin",
        "worker-epoch-wait",
        "external-nonblocking",
        "scheduler-wait",
        "scheduler-yield",
        "forbidden-lock",
        "unknown-blocker",
    },
    "io": {
        "none",
        "clock-read",
        "external-no-io",
        "forbidden-file-io",
        "unknown-blocker",
    },
    "exception": {
        "no-direct-throw",
        "caught-at-boundary",
        "may-throw-contained",
        "external-noexcept-contract",
        "unknown-blocker",
    },
    "loopBound": {
        "no-loop",
        "fixed-or-configuration-bounded",
        "concurrent-producer-unbounded",
        "worker-lifecycle-dependent",
        "consumer-contention-unbounded",
        "external-leaf",
        "unknown-blocker",
    },
    "configurationBound": {
        "release-runtime-configuration",
        "command-capacity-insufficient",
        "render-worker-count",
        "armed-meter-variables",
        "max-voices-and-parking",
        "external-leaf",
    },
    "threadQos": {
        "callback-or-render-worker",
        "render-worker-unproven",
        "os-api",
        "external-leaf",
    },
    "evidenceOwner": {
        "m2-wp0-ast",
        "m2-runtime-architecture",
        "m2-platform-qualification",
    },
    "cost": {
        "constant",
        "linear-configuration-bounded",
        "scheduler-dependent",
        "crt-basic-math",
        "crt-transcendental-open",
        "unknown-blocker",
    },
}
FORBIDDEN_CLASSIFICATIONS = {
    ("allocation", "forbidden-system-allocation"),
    ("blocking", "forbidden-lock"),
    ("io", "forbidden-file-io"),
}
FUNCTION_KINDS = {
    "CONSTRUCTOR",
    "CONVERSION_FUNCTION",
    "CXX_METHOD",
    "DESTRUCTOR",
    "FUNCTION_DECL",
    "FUNCTION_TEMPLATE",
}
CALL_KINDS = {"CALL_EXPR"}
RUNTIME_SOURCE_PREFIX = "HarkenEngine/Source/Runtime/"
ROOT_SPECS = (
    {
        "qualifiedName": "harken::DeviceBackend::Impl::DataCallback",
        "source": "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp",
    },
    {
        "qualifiedName": "harken::MixerLoop::RenderBlock",
        "source": "HarkenEngine/Source/Runtime/render/src/MixerLoop.cpp",
    },
    {
        "qualifiedName": "harken::RenderWorkerPool::Run",
        "source": "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp",
    },
    {
        "qualifiedName": "harken::RenderWorkerPool::WorkerMain",
        "source": "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp",
    },
    {
        "qualifiedName": "harken::RenderWorkerPool::RunBatch",
        "source": "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp",
    },
    {
        "qualifiedName": "harken::VoicePool::CascadeThunk",
        "source": "HarkenEngine/Source/Runtime/render/src/VoicePool.cpp",
    },
)
AVRT_SYSTEM_FIELDS = {
    "set": {
        "field": "AvSetMmThreadCharacteristicsA",
        "target": "AvSetMmThreadCharacteristicsA",
        "fieldLine": 7372,
    },
    "revert": {
        "field": "AvRevertMmThreadcharacteristics",
        "target": "AvRevertMmThreadCharacteristics",
        "fieldLine": 7373,
    },
}
AVRT_BINDINGS = (
    (
        "join-set",
        "harken::DeviceBackend::Impl::JoinWorkerDomain",
        "setCharacteristics",
        "set",
        192,
        21,
    ),
    (
        "join-revert",
        "harken::DeviceBackend::Impl::JoinWorkerDomain",
        "revertCharacteristics",
        "revert",
        204,
        24,
    ),
    (
        "leave-revert",
        "harken::DeviceBackend::Impl::LeaveWorkerDomain",
        "revertCharacteristics",
        "revert",
        258,
        9,
    ),
)
KNOWN_BLOCKERS = {
    "RT-B001": "command-drain termination",
    "RT-B002": "worker priority/barrier",
    "RT-B003": "meter overflow merge",
    "RT-B004": "publication capacity cost",
    "RT-B006": "callback-reachable CRT transcendental cost",
    "RT-B007": "unknown external leaf semantics or cost",
    "RT-B008": "unproven callback loop bound",
}
REQUIRED_DYNAMIC_BLOCKER_FIELDS = {
    "source",
    "line",
    "column",
    "kind",
    "member",
    "expression",
    "blocker",
    "evidence",
}
REQUIRED_COMPILER_IDENTITY_FIELDS = {
    "id",
    "usr",
    "mangled",
    "qualifiedName",
    "source",
    "line",
    "column",
}


class ClosureError(RuntimeError):
    pass


@dataclass
class Node:
    id: str
    usr: str
    mangled: str
    qualified_name: str
    display_name: str
    source: str
    line: int
    column: int
    body_sha256: str
    scope: str
    features: dict[str, Any] = field(default_factory=dict)

    def report(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "usr": self.usr,
            "mangled": self.mangled,
            "qualifiedName": self.qualified_name,
            "displayName": self.display_name,
            "source": self.source,
            "line": self.line,
            "column": self.column,
            "bodySha256": self.body_sha256,
            "scope": self.scope,
            "features": self.features,
        }


@dataclass(frozen=True)
class Edge:
    caller: str
    callee: str
    kind: str
    source: str
    line: int
    column: int
    overlay: str | None = None

    def report(self) -> dict[str, Any]:
        result = {
            "caller": self.caller,
            "callee": self.callee,
            "kind": self.kind,
            "site": {
                "source": self.source,
                "line": self.line,
                "column": self.column,
            },
        }
        if self.overlay is not None:
            result["overlay"] = self.overlay
        return result


@dataclass(frozen=True)
class DynamicSite:
    caller: str
    kind: str
    expression: str
    member: str
    source: str
    line: int
    column: int

    @property
    def key(self) -> str:
        return (
            f"{self.source}:{self.line}:{self.column}:"
            f"{self.kind}:{self.member}:{self.expression}"
        )

    def report(self) -> dict[str, Any]:
        return {
            "key": self.key,
            "caller": self.caller,
            "kind": self.kind,
            "expression": self.expression,
            "member": self.member,
            "source": self.source,
            "line": self.line,
            "column": self.column,
        }


@dataclass(frozen=True)
class UnresolvedCallResolution:
    kind: str
    targets: tuple[Node, ...]
    member: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def json_digest(value: Any) -> str:
    data = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")
    return hashlib.sha256(data).hexdigest()


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ClosureError(f"cannot read JSON {path}: {error}") from error


def write_json_atomic(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="\n",
            dir=path.parent,
            prefix=path.name + ".",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary_name = stream.name
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except OSError as error:
        if temporary_name:
            Path(temporary_name).unlink(missing_ok=True)
        raise ClosureError(f"cannot write {path}: {error}") from error


def parse_cmake_cache(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise ClosureError(f"missing CMake cache: {path}")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        values[declaration.split(":", 1)[0]] = value
    return values


def xml_local(element: et.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def unique_xml_text(
    root: et.Element, name: str, *, condition_contains: str | None = None
) -> str:
    values: set[str] = set()
    for element in root.iter():
        if xml_local(element) != name or not element.text:
            continue
        if condition_contains is not None:
            parent_condition = element.attrib.get("Condition", "")
            if parent_condition and condition_contains not in parent_condition:
                continue
        value = element.text.strip()
        if value and not value.startswith("%("):
            values.add(value)
    if len(values) != 1:
        raise ClosureError(
            f"expected one {name} value in generated project, found "
            f"{sorted(values)!r}"
        )
    return next(iter(values))


def reject_per_file_compile_metadata(item: et.Element) -> None:
    metadata = sorted({xml_local(child) for child in item})
    if metadata:
        raise ClosureError(
            "per-file ClCompile metadata is unsupported by the source "
            f"analyzer: {metadata!r}"
        )


def project_release_configuration(
    project: Path, repo_root: Path
) -> tuple[list[Path], list[Path], list[str], dict[str, str]]:
    if not project.is_file():
        raise ClosureError(f"missing generated project: {project}")
    root = et.parse(project).getroot()
    toolset = unique_xml_text(root, "PlatformToolset")
    sdk = unique_xml_text(root, "WindowsTargetPlatformVersion")
    if toolset != "v143":
        raise ClosureError(f"generated project toolset drift: {toolset}")
    if sdk != EXPECTED_WINDOWS_SDK:
        raise ClosureError(f"generated project Windows SDK drift: {sdk}")

    release_group: et.Element | None = None
    for group in root.iter():
        if xml_local(group) != "ItemDefinitionGroup":
            continue
        if "Release|x64" in group.attrib.get("Condition", ""):
            release_group = group
            break
    if release_group is None:
        raise ClosureError("generated project has no Release|x64 definition")
    compile_group = next(
        (
            child
            for child in release_group
            if xml_local(child) == "ClCompile"
        ),
        None,
    )
    if compile_group is None:
        raise ClosureError("Release|x64 has no ClCompile definition")
    values = {
        xml_local(child): (child.text or "").strip()
        for child in compile_group
    }
    expected = {
        "LanguageStandard": "stdcpp20",
        "RuntimeLibrary": "MultiThreadedDLL",
        "ExceptionHandling": "Sync",
    }
    for key, expected_value in expected.items():
        if values.get(key) != expected_value:
            raise ClosureError(
                f"Release compile setting drift for {key}: "
                f"{values.get(key)!r}"
            )
    definitions = [
        item
        for item in values.get("PreprocessorDefinitions", "").split(";")
        if item and not item.startswith("%(")
    ]
    required_definitions = {"WIN32", "_WINDOWS", "NDEBUG"}
    if not required_definitions.issubset(definitions):
        raise ClosureError(
            "Release preprocessor definitions are incomplete: "
            + ", ".join(definitions)
        )
    includes = [
        Path(item).resolve(strict=False)
        for item in values.get("AdditionalIncludeDirectories", "").split(";")
        if item and not item.startswith("%(")
    ]
    if not includes:
        raise ClosureError("Release include path list is empty")
    for include in includes:
        if not include.is_dir():
            raise ClosureError(f"Release include path does not exist: {include}")

    translation_units: list[Path] = []
    for item in root.iter():
        if xml_local(item) != "ClCompile":
            continue
        include = item.attrib.get("Include")
        if include:
            reject_per_file_compile_metadata(item)
            path = Path(include).resolve(strict=False)
            try:
                relative = path.relative_to(repo_root).as_posix()
            except ValueError as error:
                raise ClosureError(
                    f"Runtime translation unit is outside repository: {path}"
                ) from error
            if not relative.startswith(RUNTIME_SOURCE_PREFIX):
                raise ClosureError(
                    f"non-Runtime translation unit in harken_core: {relative}"
                )
            if not path.is_file():
                raise ClosureError(f"missing Runtime translation unit: {path}")
            translation_units.append(path)
    if not translation_units:
        raise ClosureError("generated project has no Runtime translation units")
    normalized = sorted(set(translation_units), key=lambda path: str(path).casefold())
    return normalized, includes, definitions, {
        "languageStandard": values["LanguageStandard"],
        "runtimeLibrary": values["RuntimeLibrary"],
        "exceptionHandling": values["ExceptionHandling"],
        "platformToolset": toolset,
        "windowsTargetPlatformVersion": sdk,
    }


def cmake_list_body(text: str, variable: str) -> list[str]:
    pattern = re.compile(
        rf"(?ms)^[ \t]*set\([ \t]*{re.escape(variable)}[ \t]*\n"
        rf"(?P<body>.*?)^[ \t]*\)[ \t]*$"
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ClosureError(
            f"expected one multiline CMake set({variable}), "
            f"found {len(matches)}"
        )
    tokens: list[str] = []
    for line in matches[0].group("body").splitlines():
        content = line.split("#", 1)[0].strip()
        if not content:
            continue
        parts = content.split()
        if len(parts) != 1:
            raise ClosureError(
                f"unsupported CMake source expression for {variable}: "
                f"{content!r}"
            )
        tokens.append(parts[0])
    if not tokens:
        raise ClosureError(f"CMake source list {variable} is empty")
    return tokens


def expand_runtime_source_token(token: str, repo_root: Path) -> Path:
    prefix = "${HARKEN_RUNTIME_DIR}/"
    if not token.startswith(prefix):
        raise ClosureError(
            f"unsupported Runtime source token in CMake authority: {token}"
        )
    relative = token[len(prefix):]
    if not relative.endswith(".cpp"):
        raise ClosureError(f"Runtime source is not C++: {token}")
    path = (
        repo_root / "HarkenEngine/Source/Runtime" / relative
    ).resolve(strict=False)
    if not path.is_file():
        raise ClosureError(f"CMake-owned Runtime source is missing: {path}")
    return path


def qualification_source_inventory(
    cmake_path: Path,
    cache: dict[str, str],
    repo_root: Path,
) -> tuple[list[Path], dict[str, Any]]:
    if not cmake_path.is_file():
        raise ClosureError(f"missing CMake source authority: {cmake_path}")
    text = cmake_path.read_text(encoding="utf-8", errors="strict")
    base_tokens = cmake_list_body(text, "HARKEN_RUNTIME_SOURCES")
    base_sources = [
        expand_runtime_source_token(token, repo_root)
        for token in base_tokens
    ]

    qualification_init = re.compile(
        r"set\(\s*_harken_rt_qualification_sources\s+"
        r"\$\{HARKEN_RUNTIME_SOURCES\}\s*\)"
    )
    if len(qualification_init.findall(text)) != 1:
        raise ClosureError(
            "qualification source ownership no longer initializes from "
            "HARKEN_RUNTIME_SOURCES"
        )
    conditional_pattern = re.compile(
        r"(?ms)if\(\s*HARKEN_RT_PROBE_DEFINITIONS\s*\)\s*"
        r"list\(\s*APPEND\s+_harken_rt_qualification_sources\s*"
        r"(?P<body>.*?)\)\s*endif\(\)"
    )
    conditional_matches = list(conditional_pattern.finditer(text))
    if len(conditional_matches) != 1:
        raise ClosureError(
            "qualification probe-source condition is missing or ambiguous"
        )
    conditional_tokens = [
        part
        for line in conditional_matches[0].group("body").splitlines()
        for part in line.split("#", 1)[0].split()
    ]
    if not conditional_tokens:
        raise ClosureError("qualification conditional source list is empty")
    conditional_sources = [
        expand_runtime_source_token(token, repo_root)
        for token in conditional_tokens
    ]
    target_pattern = re.compile(
        r"add_library\(\s*harken_core_rt_qualification\s+STATIC\s*"
        r"\$\{_harken_rt_qualification_sources\}\s*\)"
    )
    if len(target_pattern.findall(text)) != 1:
        raise ClosureError(
            "qualification target no longer consumes its owned source list"
        )

    probes_enabled = any(
        cache.get(key) == "ON" for key in REQUIRED_PROBE_CACHE
    )
    expected = [
        *base_sources,
        *(conditional_sources if probes_enabled else []),
    ]
    if len(set(expected)) != len(expected):
        raise ClosureError("CMake qualification source inventory has duplicates")
    return sorted(expected, key=lambda path: str(path).casefold()), {
        "authority": str(cmake_path),
        "authoritySha256": sha256_file(cmake_path),
        "baseSources": [
            path.relative_to(repo_root).as_posix()
            for path in base_sources
        ],
        "conditionalProbeSources": [
            path.relative_to(repo_root).as_posix()
            for path in conditional_sources
        ],
        "probesEnabled": probes_enabled,
        "expectedSources": [
            path.relative_to(repo_root).as_posix()
            for path in sorted(
                expected, key=lambda path: str(path).casefold()
            )
        ],
    }


def validate_tu_inventory(
    expected: Iterable[Path],
    actual: Iterable[Path],
    target: str = QUALIFICATION_TARGET,
) -> None:
    expected_set = {
        str(path.resolve(strict=False)).casefold() for path in expected
    }
    actual_set = {
        str(path.resolve(strict=False)).casefold() for path in actual
    }
    missing = sorted(expected_set - actual_set)
    unexpected = sorted(actual_set - expected_set)
    if missing or unexpected:
        raise ClosureError(
            f"{target} translation-unit inventory drift; "
            f"missing={missing!r}, unexpected={unexpected!r}"
        )


def parse_cmake_set(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    pattern = re.compile(r'^set\(([A-Za-z0-9_]+)\s+"?([^")]*)"?\)$')
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        match = pattern.match(line.strip())
        if match:
            values[match.group(1)] = match.group(2)
    return values


def find_single(root: Path, pattern: str) -> Path:
    matches = sorted(root.glob(pattern))
    if len(matches) != 1:
        raise ClosureError(
            f"expected one {pattern} under {root}, found {len(matches)}"
        )
    return matches[0]


def command_output(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise ClosureError(
            f"command failed ({result.returncode}): "
            f"{subprocess.list2cmdline(command)}\n{result.stdout}\n{result.stderr}"
        )
    return result.stdout


def validate_cache_contract(
    cache: dict[str, str], repo_root: Path
) -> dict[str, str]:
    required_cache = {
        "CMAKE_HOME_DIRECTORY": str(repo_root).replace("\\", "/"),
        "CMAKE_GENERATOR": EXPECTED_GENERATOR,
        "CMAKE_GENERATOR_PLATFORM": EXPECTED_PLATFORM,
        "CMAKE_GENERATOR_TOOLSET": EXPECTED_TOOLSET,
        "CMAKE_SYSTEM_VERSION": EXPECTED_WINDOWS_SDK,
        "HARKEN_BUILD_RT_QUALIFICATION": "ON",
        **REQUIRED_PROBE_CACHE,
    }
    for key, expected in required_cache.items():
        actual = cache.get(key)
        if key == "CMAKE_HOME_DIRECTORY" and actual is not None:
            if Path(actual).resolve(strict=False) == repo_root:
                continue
        if actual != expected:
            raise ClosureError(
                f"toolchain drift for {key}: {actual!r} != {expected!r}"
            )
    return required_cache


def toolchain_configuration(
    build: Path,
    repo_root: Path,
    libclang: Path,
    clang_executable: Path,
) -> tuple[dict[str, Any], list[Path], list[Path], list[str], dict[str, str]]:
    cache_path = build / "CMakeCache.txt"
    project = build / QUALIFICATION_PROJECT_RELATIVE
    cache = parse_cmake_cache(cache_path)
    required_cache = validate_cache_contract(cache, repo_root)
    expected_sources, inventory = qualification_source_inventory(
        repo_root / QUALIFICATION_CMAKE, cache, repo_root
    )

    compiler_file = find_single(
        build / "CMakeFiles", "*/CMakeCXXCompiler.cmake"
    )
    compiler_values = parse_cmake_set(compiler_file)
    compiler = Path(
        compiler_values.get("CMAKE_CXX_COMPILER", "")
    ).resolve(strict=False)
    expected_compiler = Path(
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/"
        f"MSVC/{EXPECTED_MSVC_TOOLS}/bin/Hostx64/x64/cl.exe"
    ).resolve(strict=False)
    if compiler != expected_compiler or not compiler.is_file():
        raise ClosureError(f"MSVC compiler path drift: {compiler}")
    if compiler_values.get("CMAKE_CXX_COMPILER_VERSION") != EXPECTED_MSVC_COMPILER:
        raise ClosureError(
            "MSVC compiler version drift: "
            f"{compiler_values.get('CMAKE_CXX_COMPILER_VERSION')!r}"
        )

    if not libclang.is_file() or not clang_executable.is_file():
        raise ClosureError("pinned Clang installation is incomplete")
    clang_version = command_output([str(clang_executable), "--version"])
    if f"clang version {EXPECTED_LIBCLANG}" not in clang_version:
        raise ClosureError(f"Clang version drift:\n{clang_version}")
    try:
        binding_version = importlib.metadata.version("clang")
    except importlib.metadata.PackageNotFoundError as error:
        raise ClosureError(
            f"clang Python binding {EXPECTED_CLANG_BINDING} is not installed"
        ) from error
    if binding_version != EXPECTED_CLANG_BINDING:
        raise ClosureError(
            f"clang Python binding drift: {binding_version} != "
            f"{EXPECTED_CLANG_BINDING}"
        )

    translation_units, project_includes, definitions, release = (
        project_release_configuration(project, repo_root)
    )
    validate_tu_inventory(expected_sources, translation_units)
    msvc_include = (
        expected_compiler.parents[3] / "include"
    ).resolve(strict=False)
    sdk_root = Path(
        f"C:/Program Files (x86)/Windows Kits/10/Include/{EXPECTED_WINDOWS_SDK}"
    )
    system_includes = [
        msvc_include,
        *(sdk_root / name for name in ("ucrt", "shared", "um", "winrt", "cppwinrt")),
    ]
    for include in system_includes:
        if not include.is_dir():
            raise ClosureError(f"pinned system include is missing: {include}")

    record = {
        "cmakeCache": {
            "path": str(cache_path),
            "sha256": sha256_file(cache_path),
            "values": required_cache,
        },
        "generatedProject": {
            "path": str(project),
            "sha256": sha256_file(project),
            **release,
        },
        "translationUnitInventory": inventory,
        "msvc": {
            "toolsVersion": EXPECTED_MSVC_TOOLS,
            "compilerVersion": EXPECTED_MSVC_COMPILER,
            "compiler": {
                "path": str(compiler),
                "sha256": sha256_file(compiler),
            },
            "compilerDefinition": {
                "path": str(compiler_file),
                "sha256": sha256_file(compiler_file),
            },
        },
        "windowsSdk": {
            "version": EXPECTED_WINDOWS_SDK,
            "includeRoots": [str(path) for path in system_includes[1:]],
        },
        "clang": {
            "version": EXPECTED_LIBCLANG,
            "executable": {
                "path": str(clang_executable),
                "sha256": sha256_file(clang_executable),
            },
            "library": {
                "path": str(libclang),
                "sha256": sha256_file(libclang),
            },
            "pythonBindingVersion": binding_version,
            "pythonExecutable": sys.executable,
            "pythonVersion": sys.version,
        },
    }
    return record, translation_units, project_includes, system_includes, definitions


def clang_arguments(
    project_includes: Iterable[Path],
    system_includes: Iterable[Path],
    definitions: Iterable[str],
) -> list[str]:
    args = [
        "-x",
        "c++",
        "-std=c++20",
        "-fms-compatibility",
        "-fms-extensions",
        "-fms-compatibility-version=19.42",
        "-fexceptions",
        "-fcxx-exceptions",
        "-D_CRT_USE_BUILTIN_OFFSETOF",
        "-Wno-deprecated-anon-enum-enum-conversion",
        "-Wno-microsoft-enum-value",
        "-Wno-pragma-pack",
    ]
    for definition in definitions:
        normalized = definition.replace('\\"', '"')
        args.append("-D" + normalized)
    for include in project_includes:
        args.append("-I" + str(include))
    for include in system_includes:
        args.extend(["-isystem", str(include)])
    return args


def relative_source(path: Any, repo_root: Path) -> str:
    if path is None:
        return "<unknown>"
    resolved = Path(str(path)).resolve(strict=False)
    try:
        return resolved.relative_to(repo_root).as_posix()
    except ValueError:
        return resolved.as_posix()


def qualified_name(cursor: Any) -> str:
    components: list[str] = []
    parent = cursor.semantic_parent
    while parent is not None and parent.kind.name != "TRANSLATION_UNIT":
        if parent.spelling:
            components.append(parent.spelling)
        parent = parent.semantic_parent
    components.reverse()
    if cursor.spelling:
        components.append(cursor.spelling)
    return "::".join(components)


def normalized_token_digest(cursor: Any) -> str:
    tokens = [token.spelling for token in cursor.get_tokens()]
    return hashlib.sha256("\x1f".join(tokens).encode("utf-8")).hexdigest()


def stable_node_id(
    usr: str, mangled: str, source: str, qualified: str
) -> str:
    identity = {
        "usr": usr,
        "mangled": mangled,
        "source": source,
        "qualifiedName": qualified,
    }
    return "fn:" + json_digest(identity)[:24]


def cursor_raii_declarations(
    cursor: Any, repo_root: Path
) -> list[dict[str, Any]]:
    declarations: list[dict[str, Any]] = []
    seen: set[str] = set()
    for declaration in cursor.walk_preorder():
        if declaration.kind.name != "VAR_DECL":
            continue
        candidates: list[tuple[Any, Any]] = []
        for descendant in declaration.walk_preorder():
            if descendant.kind.name not in {
                "CALL_EXPR",
                "CXX_CONSTRUCT_EXPR",
            }:
                continue
            target = referenced_function(descendant)
            if target is None:
                continue
            target_name = qualified_name(target)
            if "::" not in target_name:
                continue
            owner, method = target_name.rsplit("::", 1)
            if method == owner.rsplit("::", 1)[-1]:
                candidates.append((descendant, target))
        if not candidates:
            continue
        type_spelling = (
            declaration.type.get_canonical().spelling
            or declaration.type.spelling
        )
        for construct, target in candidates:
            constructor = qualified_name(target)
            owner = constructor.rsplit("::", 1)[0]
            class_name = owner.rsplit("::", 1)[-1]
            if (
                class_name not in type_spelling
                and owner not in type_spelling
            ):
                continue
            record = {
                "name": declaration.spelling,
                "type": type_spelling,
                "source": relative_source(
                    declaration.location.file, repo_root
                ),
                "line": declaration.location.line,
                "column": declaration.location.column,
                "constructor": constructor,
                "constructorSite": {
                    "line": construct.location.line,
                    "column": construct.location.column,
                },
            }
            identity = json_digest(record)
            if identity not in seen:
                seen.add(identity)
                declarations.append(record)
            break
    return sorted(
        declarations,
        key=lambda item: (
            item["source"],
            item["line"],
            item["column"],
            item["constructor"],
        ),
    )


def cursor_node(cursor: Any, repo_root: Path) -> Node:
    source = relative_source(cursor.location.file, repo_root)
    qualified = qualified_name(cursor)
    usr = cursor.get_usr() or ""
    mangled = cursor.mangled_name or ""
    if not usr and not mangled:
        raise ClosureError(
            f"function has no compiler identity: {qualified} at "
            f"{source}:{cursor.location.line}"
        )
    node_id = stable_node_id(usr, mangled, source, qualified)
    features = {
        "hasLoop": False,
        "hasTry": False,
        "hasThrow": False,
        "hasNew": False,
        "hasDelete": False,
        "raiiDeclarations": cursor_raii_declarations(cursor, repo_root),
    }
    for child in cursor.walk_preorder():
        kind = child.kind.name
        if kind in {"FOR_STMT", "WHILE_STMT", "DO_STMT", "CXX_FOR_RANGE_STMT"}:
            features["hasLoop"] = True
        elif kind == "CXX_TRY_STMT":
            features["hasTry"] = True
        elif kind == "CXX_THROW_EXPR":
            features["hasThrow"] = True
        elif kind == "CXX_NEW_EXPR":
            features["hasNew"] = True
        elif kind == "CXX_DELETE_EXPR":
            features["hasDelete"] = True
    return Node(
        id=node_id,
        usr=usr,
        mangled=mangled,
        qualified_name=qualified,
        display_name=cursor.displayname,
        source=source,
        line=cursor.location.line,
        column=cursor.location.column,
        body_sha256=normalized_token_digest(cursor),
        scope=(
            "repository"
            if source.startswith(RUNTIME_SOURCE_PREFIX)
            else "external"
        ),
        features=features,
    )


def call_expression(cursor: Any) -> tuple[str, str]:
    tokens = [token.spelling for token in cursor.get_tokens()]
    expression = " ".join(tokens[:24])
    member = cursor.spelling or ""
    if not member:
        for child in cursor.get_children():
            if child.spelling:
                member = child.spelling
                break
    return expression, member


def overload_spelling(cursor: Any) -> str:
    names = {
        descendant.spelling
        for descendant in cursor.walk_preorder()
        if descendant.kind.name == "OVERLOADED_DECL_REF"
        and descendant.spelling
    }
    if len(names) == 1:
        return next(iter(names))
    return ""


def repository_call_spelling(cursor: Any) -> str:
    tokens = [token.spelling for token in cursor.get_tokens()]
    if "(" not in tokens:
        return ""
    index = tokens.index("(")
    if index > 0 and re.match(r"^[A-Za-z_]\w*$", tokens[index - 1]):
        return tokens[index - 1]
    return ""


def _canonical_type(cursor: Any) -> Any | None:
    type_ref = getattr(cursor, "type", None)
    if type_ref is None:
        return None
    canonical = getattr(type_ref, "get_canonical", None)
    return canonical() if canonical is not None else type_ref


def _value_type(type_ref: Any) -> Any | None:
    while type_ref is not None and getattr(
        getattr(type_ref, "kind", None), "name", ""
    ) in {"LVALUEREFERENCE", "RVALUEREFERENCE", "POINTER"}:
        pointee_getter = getattr(type_ref, "get_pointee", None)
        if pointee_getter is None:
            break
        pointee = pointee_getter()
        if pointee is None or not getattr(pointee, "spelling", ""):
            break
        type_ref = pointee
    return type_ref


def _type_owner(type_ref: Any) -> str:
    type_ref = _value_type(type_ref)
    if type_ref is None:
        return ""
    element = getattr(type_ref, "get_array_element_type", None)
    if element is not None:
        try:
            array_element = element()
        except Exception:
            array_element = None
        if array_element is not None and getattr(
            array_element, "spelling", ""
        ):
            type_ref = array_element
    declaration_getter = getattr(type_ref, "get_declaration", None)
    declaration = (
        declaration_getter() if declaration_getter is not None else None
    )
    if declaration is None or not getattr(declaration, "spelling", ""):
        return ""
    return qualified_name(declaration)


def _member_expression_from_callee(cursor: Any | None) -> Any | None:
    if cursor is None:
        return None
    member_kinds = {
        "MEMBER_REF_EXPR",
        "CXX_DEPENDENT_SCOPE_MEMBER_EXPR",
        "MEMBER_EXPR",
    }
    if cursor.kind.name in member_kinds:
        return cursor
    if cursor.kind.name not in {
        "IMPLICIT_CAST_EXPR",
        "UNEXPOSED_EXPR",
        "EXPR_WITH_CLEANUPS",
    }:
        return None
    for child in cursor.get_children():
        nested = _member_expression_from_callee(child)
        if nested is not None:
            return nested
    return None


def _member_expression(cursor: Any) -> Any | None:
    return _member_expression_from_callee(next(cursor.get_children(), None))


def _type_declaration(type_ref: Any) -> Any | None:
    type_ref = _value_type(type_ref)
    if type_ref is None:
        return None
    declaration_getter = getattr(type_ref, "get_declaration", None)
    return declaration_getter() if declaration_getter is not None else None


def _receiver_owners(receiver: Any) -> set[str]:
    direct_field_owners: set[str] = set()
    inferred_field_owners: set[str] = set()
    base_owners: set[str] = set()
    base_types: list[Any] = []
    receiver_type = _canonical_type(receiver)
    owner = _type_owner(receiver_type)
    if owner:
        base_owners.add(owner)
        base_types.append(receiver_type)
    for descendant in receiver.walk_preorder():
        referenced = getattr(descendant, "referenced", None)
        authority = referenced if referenced is not None else descendant
        authority_type = _canonical_type(authority)
        owner = _type_owner(authority_type)
        if owner:
            if getattr(getattr(referenced, "kind", None), "name", "") == (
                "FIELD_DECL"
            ):
                direct_field_owners.add(owner)
            else:
                base_owners.add(owner)
                base_types.append(authority_type)
    token_getter = getattr(receiver, "get_tokens", None)
    receiver_tokens = {
        token.spelling
        for token in (token_getter() if token_getter is not None else ())
        if re.match(r"^[A-Za-z_]\w*$", token.spelling)
    }
    for type_ref in base_types:
        declaration = _type_declaration(type_ref)
        if declaration is None:
            continue
        for child in declaration.get_children():
            if (
                child.kind.name != "FIELD_DECL"
                or child.spelling not in receiver_tokens
            ):
                continue
            field_owner = _type_owner(_canonical_type(child))
            if field_owner:
                inferred_field_owners.add(field_owner)
    return direct_field_owners or inferred_field_owners or base_owners


def resolve_unresolved_call(
    cursor: Any, nodes: dict[str, Node]
) -> UnresolvedCallResolution | None:
    member_expression = _member_expression(cursor)
    if member_expression is not None:
        member = (
            member_expression.spelling
            or repository_call_spelling(cursor)
            or cursor.spelling
            or ""
        )
        receiver = next(member_expression.get_children(), None)
        owners = _receiver_owners(receiver) if receiver is not None else set()
        candidates = tuple(
            node
            for node in nodes.values()
            if node.scope == "repository"
            and any(
                node.qualified_name == owner + "::" + member
                for owner in owners
            )
        )
        if len(candidates) == 1:
            return UnresolvedCallResolution(
                kind="ast-receiver-member",
                targets=candidates,
                member=member,
            )
        return UnresolvedCallResolution(
            kind="dynamic-member-call",
            targets=(),
            member=member,
        )
    repository_name = repository_call_spelling(cursor)
    if not repository_name:
        return None
    candidates = tuple(
        node
        for node in nodes.values()
        if node.scope == "repository"
        and node.qualified_name.rsplit("::", 1)[-1] == repository_name
    )
    if not candidates:
        return None
    return UnresolvedCallResolution(
        kind="ast-repository-overload-set",
        targets=candidates,
        member=repository_name,
    )


def synthetic_external_node(name: str) -> Node:
    usr = "external-overload:" + name
    source = "<compiler-external-overload>"
    return Node(
        id=stable_node_id(usr, "", source, name),
        usr=usr,
        mangled="",
        qualified_name=name,
        display_name=name,
        source=source,
        line=0,
        column=0,
        body_sha256=hashlib.sha256(usr.encode("utf-8")).hexdigest(),
        scope="external",
        features={
            "hasLoop": False,
            "hasTry": False,
            "hasThrow": False,
            "hasNew": False,
            "hasDelete": False,
        },
    )


def referenced_function(cursor: Any) -> Any | None:
    referenced = cursor.referenced
    if referenced is not None and referenced.kind.name in FUNCTION_KINDS:
        return referenced
    candidates: dict[tuple[str, str, str], Any] = {}
    for descendant in cursor.walk_preorder():
        if descendant is cursor:
            continue
        referenced = descendant.referenced
        if referenced is None or referenced.kind.name not in FUNCTION_KINDS:
            continue
        key = (
            referenced.get_usr() or "",
            referenced.mangled_name or "",
            referenced.displayname,
        )
        candidates[key] = referenced
    if len(candidates) == 1:
        return next(iter(candidates.values()))
    return None


def is_virtual_method_target(target: Any) -> bool:
    return (
        target.kind.name == "CXX_METHOD"
        and bool(target.is_virtual_method())
    )


def function_definition(cursor: Any) -> Any:
    definition = cursor.get_definition()
    return definition if definition is not None else cursor


def resolve_compiler_node(
    cursor: Any, nodes: dict[str, Node], repo_root: Path
) -> Node:
    candidate = cursor_node(function_definition(cursor), repo_root)
    matches = [
        node
        for node in nodes.values()
        if (
            candidate.usr
            and node.usr == candidate.usr
        )
        or (
            not candidate.usr
            and candidate.mangled
            and node.mangled == candidate.mangled
        )
    ]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        definitions = [
            node for node in matches if node.source.endswith((".cpp", ".c"))
        ]
        if len(definitions) == 1:
            return definitions[0]
        raise ClosureError(
            f"compiler identity is ambiguous for "
            f"{candidate.qualified_name}: {[node.source for node in matches]!r}"
        )
    return candidate


def walk_repository_ast(cursor: Any, repo_root: Path) -> Iterable[Any]:
    for child in cursor.get_children():
        source = relative_source(child.location.file, repo_root)
        if source != "<unknown>" and not source.startswith(
            RUNTIME_SOURCE_PREFIX
        ):
            continue
        yield child
        yield from walk_repository_ast(child, repo_root)


def parse_translation_units(
    repo_root: Path,
    translation_units: list[Path],
    args: list[str],
    libclang: Path,
    clang_executable: Path,
) -> tuple[
    dict[str, Node],
    list[Edge],
    list[DynamicSite],
    dict[str, Any],
    list[dict[str, Any]],
    list[dict[str, str]],
]:
    try:
        from clang import cindex
    except ImportError as error:
        raise ClosureError(
            f"clang Python binding {EXPECTED_CLANG_BINDING} is required"
        ) from error
    cindex.Config.set_library_file(str(libclang))
    index = cindex.Index.create()
    nodes: dict[str, Node] = {}
    cursors: dict[str, Any] = {}
    call_cursors: list[tuple[str, Any]] = []
    registry_cursor: Any | None = None
    class_cursors: dict[str, Any] = {}
    diagnostics: list[dict[str, Any]] = []
    tu_records: list[dict[str, Any]] = []
    dependency_hashes: dict[str, str] = {}

    for source in translation_units:
        command = [str(clang_executable), *args, str(source)]
        tu = index.parse(str(source), args=args)
        tu_dependencies: list[dict[str, str]] = []
        for inclusion in tu.get_includes():
            dependency = Path(str(inclusion.include)).resolve(strict=False)
            if not dependency.is_file():
                raise ClosureError(
                    f"libclang include dependency is missing: {dependency}"
                )
            dependency_path = relative_source(dependency, repo_root)
            dependency_hashes.setdefault(
                dependency_path, sha256_file(dependency)
            )
            tu_dependencies.append(
                {
                    "path": dependency_path,
                    "sha256": dependency_hashes[dependency_path],
                }
            )
        tu_dependencies = sorted(
            {json_digest(item): item for item in tu_dependencies}.values(),
            key=lambda item: item["path"].casefold(),
        )
        tu_diagnostics = []
        for diagnostic in tu.diagnostics:
            entry = {
                "severity": int(diagnostic.severity),
                "spelling": diagnostic.spelling,
                "source": relative_source(
                    diagnostic.location.file, repo_root
                ),
                "line": diagnostic.location.line,
                "column": diagnostic.location.column,
            }
            tu_diagnostics.append(entry)
            if diagnostic.severity >= cindex.Diagnostic.Warning:
                diagnostics.append(entry)
        tu_records.append(
            {
                "source": source.relative_to(repo_root).as_posix(),
                "sourceSha256": sha256_file(source),
                "arguments": args,
                "commandSha256": json_digest(command),
                "diagnostics": tu_diagnostics,
                "includeDependencyCount": len(tu_dependencies),
                "includeDependencyDigest": json_digest(tu_dependencies),
            }
        )
        for cursor in walk_repository_ast(tu.cursor, repo_root):
            if (
                cursor.kind.name in FUNCTION_KINDS
                and cursor.is_definition()
                and cursor.location.file is not None
            ):
                node = cursor_node(cursor, repo_root)
                if node.scope != "repository":
                    continue
                prior = nodes.get(node.id)
                if prior is not None and prior.body_sha256 != node.body_sha256:
                    raise ClosureError(
                        f"function body differs across TUs: "
                        f"{node.qualified_name}"
                    )
                nodes[node.id] = node
                cursors.setdefault(node.id, cursor)
            if (
                cursor.kind.name == "VAR_DECL"
                and cursor.spelling == "kEffectRegistry"
                and relative_source(cursor.location.file, repo_root).endswith(
                    "harken_effect_registry.g.h"
                )
            ):
                registry_cursor = cursor
            if (
                cursor.kind.name in {"CLASS_DECL", "STRUCT_DECL"}
                and cursor.is_definition()
                and cursor.location.file is not None
            ):
                class_name = qualified_name(cursor)
                if class_name in {
                    "harken::TimelineExecutorSink",
                    "harken::VoiceTimelineSink",
                }:
                    class_cursors.setdefault(class_name, cursor)

    reject_parse_diagnostics(diagnostics)

    for node_id, cursor in cursors.items():
        for descendant in cursor.walk_preorder():
            if descendant is cursor or descendant.kind.name not in CALL_KINDS:
                continue
            call_cursors.append((node_id, descendant))

    direct_edges: list[Edge] = []
    dynamic_sites: list[DynamicSite] = []
    external_nodes: dict[str, Node] = {}
    for caller_id, call in call_cursors:
        source = relative_source(call.location.file, repo_root)
        target = referenced_function(call)
        if target is not None:
            if is_virtual_method_target(target):
                expression, member = call_expression(call)
                dynamic_sites.append(
                    DynamicSite(
                        caller=caller_id,
                        kind="VIRTUAL_CALL_EXPR",
                        expression=expression,
                        member=member,
                        source=source,
                        line=call.location.line,
                        column=call.location.column,
                    )
                )
                continue
            target_node = resolve_compiler_node(target, nodes, repo_root)
            if target_node.scope == "repository":
                nodes.setdefault(target_node.id, target_node)
            else:
                external_nodes.setdefault(target_node.id, target_node)
            direct_edges.append(
                Edge(
                    caller=caller_id,
                    callee=target_node.id,
                    kind="ast-direct",
                    source=source,
                    line=call.location.line,
                    column=call.location.column,
                )
            )
            continue
        overloaded = overload_spelling(call)
        if overloaded and _member_expression(call) is None:
            candidates = [
                node
                for node in nodes.values()
                if node.qualified_name.rsplit("::", 1)[-1] == overloaded
            ]
            if not candidates:
                external = synthetic_external_node(overloaded)
                nodes.setdefault(external.id, external)
                candidates = [nodes[external.id]]
            for target_node in candidates:
                direct_edges.append(
                    Edge(
                        caller=caller_id,
                        callee=target_node.id,
                        kind="ast-overload-set",
                        source=source,
                        line=call.location.line,
                        column=call.location.column,
                    )
                )
            continue
        resolution = resolve_unresolved_call(call, nodes)
        if resolution is not None and resolution.targets:
            for target_node in resolution.targets:
                direct_edges.append(
                    Edge(
                        caller=caller_id,
                        callee=target_node.id,
                        kind=resolution.kind,
                        source=source,
                        line=call.location.line,
                        column=call.location.column,
                    )
                )
            continue
        if resolution is not None and resolution.kind == "dynamic-member-call":
            expression, member = call_expression(call)
            dynamic_sites.append(
                DynamicSite(
                    caller=caller_id,
                    kind=call.kind.name,
                    expression=expression,
                    member=member or resolution.member,
                    source=source,
                    line=call.location.line,
                    column=call.location.column,
                )
            )
            continue
        expression, member = call_expression(call)
        dynamic_sites.append(
            DynamicSite(
                caller=caller_id,
                kind=call.kind.name,
                expression=expression,
                member=member,
                source=source,
                line=call.location.line,
                column=call.location.column,
            )
        )
    template_groups: dict[tuple[str, int, str], list[Node]] = defaultdict(list)
    for node in nodes.values():
        if node.scope == "repository":
            template_groups[
                (
                    node.source,
                    node.line,
                    node.qualified_name,
                )
            ].append(node)
    for group in template_groups.values():
        canonical = [node for node in group if not node.mangled]
        if len(group) < 2 or len(canonical) != 1:
            continue
        for specialization in group:
            if specialization.id == canonical[0].id:
                continue
            direct_edges.append(
                Edge(
                    caller=specialization.id,
                    callee=canonical[0].id,
                    kind="ast-template-body",
                    source=specialization.source,
                    line=specialization.line,
                    column=specialization.column,
                )
            )
    nodes.update(external_nodes)
    if registry_cursor is None:
        raise ClosureError("generated kEffectRegistry AST declaration is missing")
    return nodes, direct_edges, dynamic_sites, {
        "cursor": registry_cursor,
        "cursors": cursors,
        "classes": class_cursors,
    }, tu_records, [
        {"path": path, "sha256": digest}
        for path, digest in sorted(
            dependency_hashes.items(),
            key=lambda item: item[0].casefold(),
        )
    ]


def reject_parse_diagnostics(diagnostics: list[dict[str, Any]]) -> None:
    if diagnostics:
        raise ClosureError(
            f"libclang emitted {len(diagnostics)} warning/error diagnostics: "
            + json.dumps(diagnostics[:10], sort_keys=True)
        )


def field_reference(initializer: Any) -> Any | str | None:
    declarations: list[Any] = []
    overloaded_names: set[str] = set()
    for cursor in initializer.walk_preorder():
        if cursor.kind.name == "OVERLOADED_DECL_REF" and cursor.spelling:
            overloaded_names.add(cursor.spelling)
        if (
            cursor.referenced is not None
            and cursor.referenced.kind.name == "OVERLOADED_DECL_REF"
            and cursor.referenced.spelling
        ):
            overloaded_names.add(cursor.referenced.spelling)
        if cursor.kind.name == "DECL_REF_EXPR":
            referenced = referenced_function(cursor)
            if referenced is not None:
                declarations.append(referenced)
    unique = {
        (declaration.get_usr(), declaration.mangled_name, declaration.spelling):
        declaration
        for declaration in declarations
    }
    if len(unique) == 1:
        return next(iter(unique.values()))
    if not unique and len(overloaded_names) == 1:
        return next(iter(overloaded_names))
    return None


def registry_effect_targets(
    registry_cursor: Any,
    all_nodes: dict[str, Node],
    repo_root: Path,
) -> dict[str, list[str]]:
    children = list(registry_cursor.get_children())
    top_initializer = next(
        (child for child in children if child.kind.name == "INIT_LIST_EXPR"),
        None,
    )
    if top_initializer is None:
        raise ClosureError("kEffectRegistry has no AST initializer")
    row_type = next(
        (
            child.referenced
            for child in children
            if child.kind.name == "TYPE_REF" and child.referenced is not None
        ),
        None,
    )
    if row_type is None:
        raise ClosureError("kEffectRegistry row type is unresolved")
    field_names = [
        child.spelling
        for child in row_type.get_children()
        if child.kind.name == "FIELD_DECL"
    ]
    required = ("process", "tailFrames", "setParam", "reset")
    if not set(required).issubset(field_names):
        raise ClosureError("EffectRegistryRow callback fields drifted")
    indexes = {name: field_names.index(name) for name in required}
    result: dict[str, set[str]] = {name: set() for name in required}
    process_prefixes: set[str] = set()
    rows = [
        child
        for child in top_initializer.get_children()
        if child.kind.name == "INIT_LIST_EXPR"
    ]
    if not rows:
        raise ClosureError("kEffectRegistry has no rows")
    for row in rows:
        values = list(row.get_children())
        if len(values) != len(field_names):
            raise ClosureError(
                "kEffectRegistry row/field cardinality drift: "
                f"{len(values)} != {len(field_names)}"
            )
        for field_name, index in indexes.items():
            declaration = field_reference(values[index])
            if declaration is None:
                raise ClosureError(
                    f"kEffectRegistry {field_name} target is unresolved"
                )
            if isinstance(declaration, str):
                candidates = [
                    node
                    for node in all_nodes.values()
                    if node.qualified_name == "harken::" + declaration
                    and node.source.endswith(
                        "Runtime/render/src/BuiltinEffects.cpp"
                    )
                    and (
                        field_name not in {"process", "reset"}
                        or node.display_name.startswith(
                            declaration + "(void *"
                        )
                    )
                ]
                if len(candidates) != 1:
                    raise ClosureError(
                        f"kEffectRegistry overloaded {field_name} target "
                        f"{declaration} resolved {len(candidates)} times"
                    )
                target = candidates[0]
            else:
                target = resolve_compiler_node(
                    declaration, all_nodes, repo_root
                )
                if target.id not in all_nodes:
                    all_nodes[target.id] = target
            result[field_name].add(target.id)
            if field_name == "process" and target.qualified_name.endswith("Process"):
                process_prefixes.add(target.qualified_name[:-len("Process")])

    dynamic_ids = {
        node.id
        for node in all_nodes.values()
        if any(
            node.qualified_name == prefix + "ProcessDynamic"
            for prefix in process_prefixes
        )
    }
    if not dynamic_ids:
        raise ClosureError(
            "no processDynamic target was derived from registry process targets"
        )
    result["processDynamic"] = dynamic_ids
    return {name: sorted(ids) for name, ids in sorted(result.items())}


def resolve_roots(nodes: dict[str, Node]) -> list[str]:
    roots: list[str] = []
    for spec in ROOT_SPECS:
        matches = [
            node.id
            for node in nodes.values()
            if node.qualified_name == spec["qualifiedName"]
            and node.source == spec["source"]
        ]
        if len(matches) != 1:
            raise ClosureError(
                f"root {spec['qualifiedName']} resolved {len(matches)} times"
            )
        roots.append(matches[0])
    return roots


def overlay_site_matches(site: DynamicSite, expected: dict[str, Any]) -> bool:
    return all(
        getattr(site, key) == value
        for key, value in expected.items()
        if key in {
            "source",
            "line",
            "column",
            "member",
            "kind",
            "expression",
        }
    )


def cursor_identity(cursor: Any, repo_root: Path) -> dict[str, Any]:
    return {
        "id": stable_node_id(
            cursor.get_usr() or "",
            cursor.mangled_name or "",
            relative_source(cursor.location.file, repo_root),
            qualified_name(cursor),
        ),
        "usr": cursor.get_usr() or "",
        "mangled": cursor.mangled_name or "",
        "qualifiedName": qualified_name(cursor),
        "source": relative_source(cursor.location.file, repo_root),
        "line": cursor.location.line,
        "column": cursor.location.column,
    }


def node_identity(node: Node) -> dict[str, Any]:
    return {
        "id": node.id,
        "usr": node.usr,
        "mangled": node.mangled,
        "qualifiedName": node.qualified_name,
        "source": node.source,
        "line": node.line,
        "column": node.column,
    }


def node_full_identity(node: Node) -> dict[str, Any]:
    return {
        **node_identity(node),
        "bodySha256": node.body_sha256,
    }


def resolve_full_identity_target(
    nodes: dict[str, Node], overlay_name: str, identity: Any
) -> list[str]:
    if not isinstance(identity, dict):
        raise ClosureError(f"overlay {overlay_name} authority identity is invalid")
    required = set(REQUIRED_COMPILER_IDENTITY_FIELDS) | {"bodySha256"}
    if set(identity) != required:
        raise ClosureError(
            f"overlay {overlay_name} authority identity fields differ: "
            f"{sorted(set(identity))!r}"
        )
    matches = [
        node.id for node in nodes.values()
        if node_full_identity(node) == identity
    ]
    if len(matches) != 1:
        raise ClosureError(
            f"overlay {overlay_name} authority identity resolved "
            f"{len(matches)} times"
        )
    return matches


def register_compiler_node(
    nodes: dict[str, Node], candidate: Node, authority_name: str
) -> Node:
    if candidate.scope != "repository":
        raise ClosureError(
            f"{authority_name} compiler node is not a repository node"
        )
    identity = node_full_identity(candidate)
    matches = [node for node in nodes.values() if node_full_identity(node) == identity]
    if len(matches) > 1:
        raise ClosureError(
            f"{authority_name} compiler node resolved {len(matches)} times"
        )
    if len(matches) == 1:
        return matches[0]
    prior = nodes.get(candidate.id)
    if prior is not None:
        raise ClosureError(f"{authority_name} compiler node identity collision")
    nodes[candidate.id] = candidate
    return candidate


def avrt_external_node(target: str) -> Node:
    usr = "external-avrt:" + target
    source = "<compiler-external-avrt>"
    return Node(
        id=stable_node_id(usr, "", source, target),
        usr=usr,
        mangled="",
        qualified_name=target,
        display_name=target + "()",
        source=source,
        line=0,
        column=0,
        body_sha256=hashlib.sha256(usr.encode("utf-8")).hexdigest(),
        scope="external",
        features={
            "hasLoop": False,
            "hasTry": False,
            "hasThrow": False,
            "hasNew": False,
            "hasDelete": False,
            "raiiDeclarations": [],
            "externalAuthority": "avrt-system-field",
        },
    )


def avrt_field_identity(field: Any, repo_root: Path) -> dict[str, Any]:
    field_type = field.type.get_canonical().spelling or field.type.spelling
    return {
        "usr": field.get_usr() or "",
        "qualifiedName": qualified_name(field),
        "source": relative_source(field.location.file, repo_root),
        "line": field.location.line,
        "column": field.location.column,
        "type": field_type,
    }


def avrt_system_ast_authority(
    ast_data: dict[str, Any], nodes: dict[str, Node], repo_root: Path
) -> dict[str, Any]:
    source = "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
    owner_names = {
        name: [
            node
            for node in nodes.values()
            if node.qualified_name == name and node.source == source
        ]
        for name in {
            "harken::DeviceBackend::Impl::JoinWorkerDomain",
            "harken::DeviceBackend::Impl::LeaveWorkerDomain",
        }
    }
    if any(len(matches) != 1 for matches in owner_names.values()):
        raise ClosureError("AVRT owner cardinality drift")
    families: dict[str, dict[str, Any]] = {}
    for (
        _, owner_name, variable_name, family, call_line, call_column
    ) in AVRT_BINDINGS:
        owner = owner_names[owner_name][0]
        cursor = ast_data.get("cursors", {}).get(owner.id)
        if cursor is None:
            raise ClosureError("AVRT owner cursor is missing")
        binding_matches: list[Any] = []
        for descendant in cursor.walk_preorder():
            if (
                descendant.kind.name != "VAR_DECL"
                or descendant.spelling != variable_name
            ):
                continue
            fields = [
                child.referenced
                for child in descendant.walk_preorder()
                if child.kind.name == "MEMBER_REF_EXPR"
                and child.spelling == AVRT_SYSTEM_FIELDS[family]["field"]
                and child.referenced is not None
            ]
            if len(fields) == 1:
                binding_matches.append((descendant, fields[0]))
            elif fields:
                raise ClosureError("AVRT pointer-field target cardinality drift")
        if len(binding_matches) != 1:
            raise ClosureError(
                f"AVRT {family} binding cardinality drift for {owner_name}"
            )
        variable, field = binding_matches[0]
        field_info = avrt_field_identity(field, repo_root)
        expected = AVRT_SYSTEM_FIELDS[family]
        if (
            not field_info["qualifiedName"].endswith(
                "::" + expected["field"]
            )
            or field_info["source"]
            != "HarkenEngine/ThirdParty/miniaudio/miniaudio.h"
            or field_info["line"] != expected["fieldLine"]
        ):
            raise ClosureError(f"AVRT {family} field identity drift")
        variable_usr = variable.get_usr() or ""
        if not variable_usr:
            raise ClosureError(
                f"AVRT {family} local pointer has no compiler identity"
            )
        variable_site = {
            "source": relative_source(variable.location.file, repo_root),
            "line": variable.location.line,
            "column": variable.location.column,
            "tokens": " ".join(token.spelling for token in variable.get_tokens()),
        }
        family_record = families.setdefault(family, {
            "name": family,
            "field": field_info,
            "target": node_full_identity(avrt_external_node(expected["target"])),
            "bindings": [],
            "callSites": [],
        })
        if family_record["field"] != field_info:
            raise ClosureError(f"AVRT {family} field identity is ambiguous")
        family_record["bindings"].append({
            "owner": node_full_identity(owner),
            "variable": {
                "name": variable_name,
                "usr": variable_usr,
                "type": (
                    variable.type.get_canonical().spelling
                    or variable.type.spelling
                ),
            },
            "site": variable_site,
        })
        calls = []
        for descendant in cursor.walk_preorder():
            if descendant.kind.name != "CALL_EXPR":
                continue
            references = [
                child.referenced
                for child in descendant.walk_preorder()
                if child.kind.name == "DECL_REF_EXPR"
                and child.referenced is not None
                and (child.referenced.get_usr() or "") == variable_usr
            ]
            if len(references) == 1:
                calls.append(descendant)
        if len(calls) != 1:
            raise ClosureError(
                f"AVRT {family} call cardinality drift for {owner_name}"
            )
        call = calls[0]
        if (
            call.location.line != call_line
            or call.location.column != call_column
        ):
            raise ClosureError(f"AVRT {family} call-site identity drift")
        family_record["callSites"].append({
            "source": relative_source(call.location.file, repo_root),
            "line": call.location.line,
            "column": call.location.column,
            "tokens": " ".join(token.spelling for token in call.get_tokens()),
        })
    for family, expected_count in (("set", 1), ("revert", 2)):
        record = families.get(family)
        if record is None:
            raise ClosureError(f"AVRT {family} family is missing")
        if (
            len(record["bindings"]) != expected_count
            or len(record["callSites"]) != expected_count
        ):
            raise ClosureError(f"AVRT {family} family cardinality drift")
    for family in families:
        families[family]["bindings"].sort(
            key=lambda item: item["owner"]["qualifiedName"]
        )
        families[family]["callSites"].sort(key=lambda item: item["line"])
    return {"families": [families[name] for name in ("set", "revert")]}


def derive_avrt_system_target(
    nodes: dict[str, Node], authority: dict[str, Any], family: str
) -> list[str]:
    if family not in AVRT_SYSTEM_FIELDS:
        raise ClosureError(f"AVRT family is invalid: {family}")
    families = authority.get("families")
    if not isinstance(families, list):
        raise ClosureError("AVRT family authority is invalid")
    matches = [
        entry
        for entry in families
        if isinstance(entry, dict) and entry.get("name") == family
    ]
    if len(matches) != 1:
        raise ClosureError(f"AVRT {family} family cardinality drift")
    record = matches[0]
    expected = AVRT_SYSTEM_FIELDS[family]
    field = record.get("field")
    if (
        not isinstance(field, dict)
        or not field.get("qualifiedName", "").endswith(
            "::" + expected["field"]
        )
        or field.get("source") != "HarkenEngine/ThirdParty/miniaudio/miniaudio.h"
        or field.get("line") != expected["fieldLine"]
        or not field.get("usr")
    ):
        raise ClosureError(f"AVRT {family} field identity drift")
    target_identity = record.get("target")
    target_ids = resolve_full_identity_target(
        nodes, "AVRT " + family + " target", target_identity
    )
    target = nodes[target_ids[0]]
    if (
        target.scope != "external"
        or target.qualified_name != expected["target"]
        or target.features.get("externalAuthority") != "avrt-system-field"
    ):
        raise ClosureError(f"AVRT {family} target identity drift")
    expected_count = 1 if family == "set" else 2
    bindings = record.get("bindings")
    calls = record.get("callSites")
    if not isinstance(bindings, list) or len(bindings) != expected_count:
        raise ClosureError(f"AVRT {family} binding cardinality drift")
    if not isinstance(calls, list) or len(calls) != expected_count:
        raise ClosureError(f"AVRT {family} call-site cardinality drift")
    expected_bindings = {
        "set": {
            ("harken::DeviceBackend::Impl::JoinWorkerDomain", "setCharacteristics"),
        },
        "revert": {
            ("harken::DeviceBackend::Impl::JoinWorkerDomain", "revertCharacteristics"),
            ("harken::DeviceBackend::Impl::LeaveWorkerDomain", "revertCharacteristics"),
        },
    }[family]
    observed_bindings = {
        (
            entry.get("owner", {}).get("qualifiedName"),
            entry.get("variable", {}).get("name"),
        )
        for entry in bindings if isinstance(entry, dict)
    }
    if observed_bindings != expected_bindings:
        raise ClosureError(f"AVRT {family} binding identity drift")
    expected_calls = {
        "set": [(192, 21, 'setCharacteristics ( "Games" , & taskIndex )')],
        "revert": [
            (204, 24, "revertCharacteristics ( handle )"),
            (258, 9, "revertCharacteristics ( handle )"),
        ],
    }[family]
    observed_calls = sorted(
        (entry.get("line"), entry.get("column"), entry.get("tokens"))
        for entry in calls if isinstance(entry, dict)
    )
    if observed_calls != sorted(expected_calls):
        raise ClosureError(f"AVRT {family} call-site identity drift")
    return target_ids


def realtime_worker_domain_ast_authority(
    ast_data: dict[str, Any], nodes: dict[str, Node], repo_root: Path
) -> dict[str, Any]:
    """Derive the production WorkerDomain function-pointer assignments."""
    source = "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
    factory_matches = [
        node for node in nodes.values()
        if node.qualified_name == "harken::DeviceBackend::WorkerDomain"
        and node.source == source
    ]
    if len(factory_matches) != 1:
        raise ClosureError(
            "RealtimeWorkerDomain factory cardinality "
            f"{len(factory_matches)}"
        )
    factory = factory_matches[0]
    cursor = ast_data.get("cursors", {}).get(factory.id)
    if cursor is None:
        raise ClosureError("RealtimeWorkerDomain factory cursor is missing")

    expected = {
        "join": "domain . ops . join = & Impl :: JoinWorkerDomain",
        "leave": "domain . ops . leave = & Impl :: LeaveWorkerDomain",
    }
    assignments: list[dict[str, Any]] = []
    for descendant in cursor.walk_preorder():
        if descendant.kind.name != "BINARY_OPERATOR":
            continue
        tokens = " ".join(token.spelling for token in descendant.get_tokens())
        field = next(
            (
                name
                for name, expression in expected.items()
                if tokens == expression
            ),
            None,
        )
        if field is None:
            continue
        target_cursor = next(
            (
                child.referenced
                for child in descendant.walk_preorder()
                if child.referenced is not None
                and child.referenced.kind.name in FUNCTION_KINDS
                and child.spelling in {"JoinWorkerDomain", "LeaveWorkerDomain"}
            ),
            None,
        )
        if target_cursor is None:
            raise ClosureError(
                f"RealtimeWorkerDomain {field} assignment target is missing"
            )
        target = resolve_compiler_node(target_cursor, nodes, repo_root)
        assignments.append({
            "field": field,
            "site": {
                "source": relative_source(descendant.location.file, repo_root),
                "line": descendant.location.line,
                "column": descendant.location.column,
                "tokens": tokens,
            },
            "target": node_full_identity(target),
        })
    if len(assignments) != len(expected) or {
        entry["field"] for entry in assignments
    } != set(expected):
        raise ClosureError(
            "RealtimeWorkerDomain function-pointer assignment cardinality "
            f"{len(assignments)}"
        )

    binding_expected = {
        "context": "domain . context = m_impl",
        "generation": "domain . generation = m_impl -> domainGeneration",
    }
    bindings: list[dict[str, Any]] = []
    for descendant in cursor.walk_preorder():
        if descendant.kind.name != "BINARY_OPERATOR":
            continue
        tokens = " ".join(token.spelling for token in descendant.get_tokens())
        field = next(
            (
                name
                for name, expression in binding_expected.items()
                if tokens == expression
            ),
            None,
        )
        if field is not None:
            bindings.append({
                "field": field,
                "site": {
                    "source": relative_source(descendant.location.file, repo_root),
                    "line": descendant.location.line,
                    "column": descendant.location.column,
                    "tokens": tokens,
                },
            })
    if len(bindings) != len(binding_expected) or {
        entry["field"] for entry in bindings
    } != set(binding_expected):
        raise ClosureError(
            "RealtimeWorkerDomain context/generation binding cardinality "
            f"{len(bindings)}"
        )
    return {
        "factory": node_full_identity(factory),
        "assignments": sorted(assignments, key=lambda item: item["field"]),
        "bindings": sorted(bindings, key=lambda item: item["field"]),
    }


def derive_realtime_worker_domain_target(
    nodes: dict[str, Node], authority: dict[str, Any], field: str
) -> list[str]:
    if field not in {"join", "leave"}:
        raise ClosureError(f"RealtimeWorkerDomain field is invalid: {field}")
    factory_identity = authority.get("factory")
    resolve_full_identity_target(
        nodes, "realtime-worker-domain-factory", factory_identity
    )
    if (
        not isinstance(factory_identity, dict)
        or factory_identity.get("qualifiedName")
        != "harken::DeviceBackend::WorkerDomain"
        or factory_identity.get("source")
        != "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
        or factory_identity.get("line") != 914
        or factory_identity.get("column") != 37
    ):
        raise ClosureError("RealtimeWorkerDomain factory identity drift")
    assignments = authority.get("assignments")
    if not isinstance(assignments, list) or len(assignments) != 2:
        raise ClosureError("RealtimeWorkerDomain assignment cardinality drift")
    matches = [
        entry for entry in assignments
        if isinstance(entry, dict) and entry.get("field") == field
    ]
    if len(matches) != 1:
        raise ClosureError(
            f"RealtimeWorkerDomain {field} assignment resolved {len(matches)} times"
        )
    assignment = matches[0]
    expected_assignment = {
        "join": {
            "line": 920,
            "targetName": "harken::DeviceBackend::Impl::JoinWorkerDomain",
            "tokens": "domain . ops . join = & Impl :: JoinWorkerDomain",
        },
        "leave": {
            "line": 921,
            "targetName": "harken::DeviceBackend::Impl::LeaveWorkerDomain",
            "tokens": "domain . ops . leave = & Impl :: LeaveWorkerDomain",
        },
    }[field]
    site = assignment.get("site")
    if (
        not isinstance(site, dict)
        or site.get("source")
        != "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
        or site.get("line") != expected_assignment["line"]
        or site.get("column") != 5
        or site.get("tokens") != expected_assignment["tokens"]
    ):
        raise ClosureError(
            f"RealtimeWorkerDomain {field} assignment site drift"
        )
    target_identity = assignment.get("target")
    if (
        not isinstance(target_identity, dict)
        or target_identity.get("qualifiedName")
        != expected_assignment["targetName"]
        or target_identity.get("source")
        != "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
    ):
        raise ClosureError(
            f"RealtimeWorkerDomain {field} target identity drift"
        )
    bindings = authority.get("bindings")
    if not isinstance(bindings, list) or len(bindings) != 2:
        raise ClosureError("RealtimeWorkerDomain binding cardinality drift")
    expected_bindings = {
        "context": (922, "domain . context = m_impl"),
        "generation": (923, "domain . generation = m_impl -> domainGeneration"),
    }
    for binding in bindings:
        binding_field = binding.get("field") if isinstance(binding, dict) else None
        expected_binding = expected_bindings.get(binding_field)
        binding_site = binding.get("site") if isinstance(binding, dict) else None
        if (
            expected_binding is None
            or not isinstance(binding_site, dict)
            or binding_site.get("source")
            != "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
            or binding_site.get("line") != expected_binding[0]
            or binding_site.get("column") != 5
            or binding_site.get("tokens") != expected_binding[1]
        ):
            raise ClosureError("RealtimeWorkerDomain binding site drift")
    if {
        entry.get("field") for entry in bindings if isinstance(entry, dict)
    } != set(expected_bindings):
        raise ClosureError("RealtimeWorkerDomain binding fields drifted")
    return resolve_full_identity_target(
        nodes, "realtime-worker-domain-" + field, target_identity
    )
def spin_until_ast_authority(
    ast_data: dict[str, Any], nodes: dict[str, Node], repo_root: Path
) -> dict[str, Any]:
    """Derive the single jobsDrained callable used by SpinUntil."""
    source = "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp"
    template_matches = [
        node for node in nodes.values()
        if node.qualified_name == "harken::RenderWorkerPool::SpinUntil"
        and node.source == source
        and not node.mangled
    ]
    instantiation_matches = [
        node for node in nodes.values()
        if node.qualified_name == "harken::RenderWorkerPool::SpinUntil"
        and node.source == source
        and node.mangled
    ]
    run_matches = [
        node for node in nodes.values()
        if node.qualified_name == "harken::RenderWorkerPool::Run"
        and node.source == source
    ]
    if len(template_matches) != 1 or len(instantiation_matches) != 1:
        raise ClosureError(
            "SpinUntil template/instantiation cardinality "
            f"template={len(template_matches)} "
            f"instantiation={len(instantiation_matches)}"
        )
    if len(run_matches) != 1:
        raise ClosureError(f"SpinUntil caller cardinality {len(run_matches)}")
    template = template_matches[0]
    instantiation = instantiation_matches[0]
    run = run_matches[0]
    cursors = ast_data.get("cursors", {})
    template_cursor = cursors.get(template.id)
    run_cursor = cursors.get(run.id)
    if template_cursor is None or run_cursor is None:
        raise ClosureError("SpinUntil authority cursor is missing")

    ready_calls = [
        descendant for descendant in template_cursor.walk_preorder()
        if descendant.kind.name == "CALL_EXPR"
        and descendant.location.line == 1128
        and descendant.location.column == 13
        and " ".join(token.spelling for token in descendant.get_tokens())
        == "ready ( )"
    ]
    if len(ready_calls) != 1:
        raise ClosureError(f"SpinUntil ready call cardinality {len(ready_calls)}")
    ready_parameter = ready_calls[0].referenced
    if ready_parameter is None or ready_parameter.kind.name != "PARM_DECL":
        raise ClosureError("SpinUntil ready parameter identity is missing")

    variables = [
        descendant for descendant in run_cursor.walk_preorder()
        if descendant.kind.name == "VAR_DECL"
        and descendant.spelling == "jobsDrained"
        and descendant.location.line == 1463
        and descendant.location.column == 16
    ]
    if len(variables) != 1:
        raise ClosureError(f"jobsDrained variable cardinality {len(variables)}")
    variable = variables[0]
    lambda_decl = variable.type.get_declaration()
    lambda_methods = [
        child for child in lambda_decl.get_children()
        if child.kind.name == "CXX_METHOD"
        and child.spelling == "operator()"
    ]
    if len(lambda_methods) != 1:
        raise ClosureError(
            "jobsDrained lambda operator cardinality "
            f"{len(lambda_methods)}"
        )
    lambda_target = register_compiler_node(
        nodes,
        resolve_compiler_node(lambda_methods[0], nodes, repo_root),
        "SpinUntil jobsDrained lambda",
    )

    call_sites: list[dict[str, Any]] = []
    for descendant in run_cursor.walk_preorder():
        if (
            descendant.kind.name != "CALL_EXPR"
            or descendant.spelling != "SpinUntil"
        ):
            continue
        if descendant.location.line not in {1483, 1511}:
            continue
        tokens = " ".join(token.spelling for token in descendant.get_tokens())
        arguments = [
            child for child in descendant.get_children()
            if child.kind.name == "CALL_EXPR"
            and child.referenced is not None
            and child.referenced.kind.name == "CONSTRUCTOR"
        ]
        if len(arguments) != 1:
            raise ClosureError("SpinUntil jobsDrained argument identity is missing")
        call_sites.append({
            "source": relative_source(descendant.location.file, repo_root),
            "line": descendant.location.line,
            "column": descendant.location.column,
            "tokens": tokens,
            "argument": {
                "name": "jobsDrained",
                "usr": variable.get_usr() or "",
                "source": relative_source(variable.location.file, repo_root),
                "line": variable.location.line,
                "column": variable.location.column,
            },
        })
    if len(call_sites) != 2 or {
        entry["line"] for entry in call_sites
    } != {1483, 1511}:
        raise ClosureError(
            "SpinUntil jobsDrained call-site cardinality "
            f"{len(call_sites)}"
        )
    return {
        "template": node_full_identity(template),
        "instantiation": node_full_identity(instantiation),
        "caller": node_full_identity(run),
        "readyCall": {
            "source": relative_source(ready_calls[0].location.file, repo_root),
            "line": ready_calls[0].location.line,
            "column": ready_calls[0].location.column,
            "tokens": " ".join(
                token.spelling for token in ready_calls[0].get_tokens()
            ),
            "parameter": {
                "usr": ready_parameter.get_usr() or "",
                "qualifiedName": qualified_name(ready_parameter),
                "source": relative_source(ready_parameter.location.file, repo_root),
                "line": ready_parameter.location.line,
                "column": ready_parameter.location.column,
            },
        },
        "lambdaVariable": {
            "name": variable.spelling,
            "usr": variable.get_usr() or "",
            "qualifiedName": qualified_name(variable),
            "source": relative_source(variable.location.file, repo_root),
            "line": variable.location.line,
            "column": variable.location.column,
            "type": variable.type.get_canonical().spelling,
        },
        "lambdaTarget": node_full_identity(lambda_target),
        "callSites": sorted(call_sites, key=lambda item: item["line"]),
    }


def derive_spin_until_target(
    nodes: dict[str, Node], authority: dict[str, Any]
) -> list[str]:
    identities = {
        name: authority.get(name)
        for name in ("template", "instantiation", "caller", "lambdaTarget")
    }
    for name, identity in identities.items():
        resolve_full_identity_target(nodes, "spin-until-" + name, identity)
    source = "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp"
    if (
        not isinstance(identities["template"], dict)
        or identities["template"].get("qualifiedName")
        != "harken::RenderWorkerPool::SpinUntil"
        or identities["template"].get("source") != source
        or identities["template"].get("line") != 1125
        or identities["template"].get("column") != 33
        or identities["template"].get("mangled") != ""
    ):
        raise ClosureError("SpinUntil template identity drift")
    if (
        not isinstance(identities["instantiation"], dict)
        or identities["instantiation"].get("qualifiedName")
        != "harken::RenderWorkerPool::SpinUntil"
        or identities["instantiation"].get("source") != source
        or identities["instantiation"].get("line") != 1125
        or identities["instantiation"].get("column") != 33
        or not identities["instantiation"].get("mangled")
    ):
        raise ClosureError("SpinUntil instantiation identity drift")
    if (
        not isinstance(identities["caller"], dict)
        or identities["caller"].get("qualifiedName")
        != "harken::RenderWorkerPool::Run"
        or identities["caller"].get("source") != source
    ):
        raise ClosureError("SpinUntil caller identity drift")
    lambda_variable = authority.get("lambdaVariable")
    if (
        not isinstance(lambda_variable, dict)
        or lambda_variable.get("name") != "jobsDrained"
        or lambda_variable.get("qualifiedName")
        != "harken::RenderWorkerPool::Run::jobsDrained"
        or lambda_variable.get("source") != source
        or lambda_variable.get("line") != 1463
        or lambda_variable.get("column") != 16
        or not lambda_variable.get("usr")
        or "lambda at " not in str(lambda_variable.get("type"))
        or "RenderWorkerPool.cpp:1463:30" not in str(lambda_variable.get("type"))
    ):
        raise ClosureError("jobsDrained variable identity drift")
    lambda_target = identities["lambdaTarget"]
    if (
        not isinstance(lambda_target, dict)
        or not str(lambda_target.get("qualifiedName", "")).endswith(
            "RenderWorkerPool.cpp:1463:30)::operator()"
        )
        or lambda_target.get("source") != source
        or lambda_target.get("line") != 1463
        or lambda_target.get("column") != 30
        or not lambda_target.get("usr")
        or not lambda_target.get("mangled")
    ):
        raise ClosureError("jobsDrained lambda target identity drift")
    ready_call = authority.get("readyCall")
    parameter = ready_call.get("parameter") if isinstance(ready_call, dict) else None
    if (
        not isinstance(ready_call, dict)
        or ready_call.get("source") != source
        or ready_call.get("line") != 1128
        or ready_call.get("column") != 13
        or ready_call.get("tokens") != "ready ( )"
        or not isinstance(parameter, dict)
        or parameter.get("qualifiedName")
        != "harken::RenderWorkerPool::SpinUntil::ready"
        or parameter.get("source") != source
        or parameter.get("line") != 1125
        or parameter.get("column") != 49
        or not parameter.get("usr")
    ):
        raise ClosureError("SpinUntil ready-call authority drift")
    call_sites = authority.get("callSites")
    if not isinstance(call_sites, list) or len(call_sites) != 2:
        raise ClosureError("SpinUntil call-site cardinality drift")
    expected_columns = {1483: 13, 1511: 17}
    for call_site in call_sites:
        line = call_site.get("line") if isinstance(call_site, dict) else None
        argument = call_site.get("argument") if isinstance(call_site, dict) else None
        if (
            line not in expected_columns
            or call_site.get("source") != source
            or call_site.get("column") != expected_columns[line]
            or call_site.get("tokens")
            != "SpinUntil ( jobsDrained , deadlineAt , drainYields )"
            or not isinstance(argument, dict)
            or argument.get("name") != "jobsDrained"
            or argument.get("source") != source
            or argument.get("line") != 1463
            or argument.get("column") != 16
            or argument.get("usr") != lambda_variable.get("usr")
        ):
            raise ClosureError("SpinUntil call-site authority drift")
    if {
        entry.get("line") for entry in call_sites if isinstance(entry, dict)
    } != set(expected_columns):
        raise ClosureError("SpinUntil call-site lines drifted")
    return [lambda_target["id"]]


def timeline_sink_ast_authority(
    classes: dict[str, Any],
    nodes: dict[str, Node],
    cursors: dict[str, Any],
    repo_root: Path,
) -> dict[str, Any]:
    """Derive exact TimelineExecutorSink and production construction evidence."""
    interface_name = "harken::TimelineExecutorSink"
    implementation_name = "harken::VoiceTimelineSink"
    interface_matches = [
        cursor for cursor in classes.values()
        if qualified_name(cursor) == interface_name
    ]
    implementation_matches = [
        cursor for cursor in classes.values()
        if qualified_name(cursor) == implementation_name
    ]
    if len(interface_matches) != 1 or len(implementation_matches) != 1:
        raise ClosureError(
            "TimelineExecutorSink authority class cardinality "
            f"interface={len(interface_matches)} "
            f"implementation={len(implementation_matches)}"
        )
    interface = interface_matches[0]
    implementation = implementation_matches[0]
    interface_children = list(interface.get_children())
    implementation_children = list(implementation.get_children())
    bases = [
        child for child in implementation_children
        if child.kind.name == "CXX_BASE_SPECIFIER"
        and child.referenced is not None
    ]
    if len(bases) != 1 or qualified_name(bases[0].referenced) != interface_name:
        raise ClosureError(
            "VoiceTimelineSink must have exactly one direct TimelineExecutorSink base"
        )
    is_final = any(
        child.kind.name == "CXX_FINAL_ATTR"
        for child in implementation_children
    )
    if not is_final:
        raise ClosureError("VoiceTimelineSink is not final")
    interface_methods = sorted({
        child.spelling for child in interface_children
        if child.kind.name == "CXX_METHOD" and child.is_virtual_method()
    })
    expected_methods = sorted({
        "AdmitQuantum",
        "ApplyPlan03Groups",
        "ApplyTransaction",
        "BeginQuantum",
        "BindingValid",
        "CancelBinding",
        "EndQuantum",
        "FailPlan03Deadline",
    })
    if interface_methods != expected_methods:
        raise ClosureError(
            "TimelineExecutorSink interface method set drift: "
            f"{interface_methods!r}"
        )
    methods: list[dict[str, Any]] = []
    for method_name in expected_methods:
        declarations = [
            child for child in implementation_children
            if child.kind.name == "CXX_METHOD"
            and child.spelling == method_name
        ]
        if len(declarations) != 1:
            raise ClosureError(
                f"VoiceTimelineSink override {method_name} resolved "
                f"{len(declarations)} times"
            )
        declaration = declarations[0]
        if not any(
            child.kind.name == "CXX_OVERRIDE_ATTR"
            for child in declaration.get_children()
        ):
            raise ClosureError(
                f"VoiceTimelineSink override {method_name} lacks override evidence"
            )
        matches = [
            node for node in nodes.values()
            if node.qualified_name == qualified_name(declaration)
            and node.source == relative_source(declaration.location.file, repo_root)
            and node.line == declaration.location.line
            and node.column == declaration.location.column
        ]
        if len(matches) != 1:
            raise ClosureError(
                f"VoiceTimelineSink override {method_name} target cardinality "
                f"{len(matches)}"
            )
        base_methods = [
            child for child in interface_children
            if child.kind.name == "CXX_METHOD"
            and child.spelling == method_name
        ]
        if len(base_methods) != 1:
            raise ClosureError(
                f"TimelineExecutorSink method {method_name} target cardinality "
                f"{len(base_methods)}"
            )
        methods.append({
            "name": method_name,
            "interface": cursor_identity(base_methods[0], repo_root),
            "target": node_identity(matches[0]),
            "override": True,
        })
    engine_constructors = [
        node for node in nodes.values()
        if node.qualified_name == "harken::Engine::Engine"
        and node.source == "HarkenEngine/Source/Runtime/api/src/Engine.h"
    ]
    if len(engine_constructors) != 1:
        raise ClosureError(
            "Engine production sink constructor cardinality "
            f"{len(engine_constructors)}"
        )
    engine_cursor = cursors.get(engine_constructors[0].id)
    if engine_cursor is None:
        raise ClosureError("Engine production sink constructor cursor is missing")
    sink_fields = [
        child for child in engine_cursor.semantic_parent.get_children()
        if child.kind.name == "FIELD_DECL"
        and child.spelling == "timelineSink"
        and child.type.get_canonical().spelling == "harken::VoiceTimelineSink"
    ]
    sink_constructions: list[Any] = []
    reset_calls: list[Any] = []
    for descendant in engine_cursor.walk_preorder():
        if descendant.kind.name != "CALL_EXPR":
            continue
        target = referenced_function(descendant)
        target_name = qualified_name(target) if target is not None else ""
        if target_name == "harken::VoiceTimelineSink::VoiceTimelineSink":
            sink_constructions.append(descendant)
        elif target_name == "harken::TimelineExecutor::Reset":
            tokens = [token.spelling for token in descendant.get_tokens()]
            if tokens == [
                "timelineExecutor", ".", "Reset", "(", "resolved", ".",
                "blockFrames", ",", "resolved", ".", "maxEventsPerQuantum",
                ",", "&", "timelinePool", ",", "&", "timelineArms", ",",
                "&", "timelineSink", ",", "&", "decisions", ")",
            ]:
                reset_calls.append(descendant)
    if (
        len(sink_fields) != 1
        or len(sink_constructions) != 1
        or len(reset_calls) != 1
    ):
        raise ClosureError(
            "Engine production sink ownership cardinality "
            f"fields={len(sink_fields)} "
            f"constructors={len(sink_constructions)} "
            f"resets={len(reset_calls)}"
        )
    production = {
        "constructor": node_identity(engine_constructors[0]),
        "sinkField": {
            "usr": sink_fields[0].get_usr() or "",
            "qualifiedName": qualified_name(sink_fields[0]),
            "type": sink_fields[0].type.get_canonical().spelling,
            "source": relative_source(sink_fields[0].location.file, repo_root),
            "line": sink_fields[0].location.line,
            "column": sink_fields[0].location.column,
        },
        "sinkConstruction": {
            "source": relative_source(
                sink_constructions[0].location.file, repo_root
            ),
            "line": sink_constructions[0].location.line,
            "column": sink_constructions[0].location.column,
        },
        "resetBinding": {
            "source": relative_source(reset_calls[0].location.file, repo_root),
            "line": reset_calls[0].location.line,
            "column": reset_calls[0].location.column,
            "argument": "&timelineSink",
        },
    }
    return {
        "interface": cursor_identity(interface, repo_root),
        "implementation": {
            **cursor_identity(implementation, repo_root),
            "isFinal": True,
        },
        "interfaceMethods": expected_methods,
        "methods": methods,
        "production": production,
    }


def derive_timeline_executor_sink_targets(
    nodes: dict[str, Node],
    ast_data: dict[str, Any],
    authority: dict[str, Any],
    target_method: str | None = None,
) -> list[str]:
    methods = authority.get("methods")
    if not isinstance(methods, list):
        raise ClosureError("TimelineExecutorSink authority methods are invalid")
    if target_method is None:
        raise ClosureError("TimelineExecutorSink target method is missing")
    matches = [
        method for method in methods
        if isinstance(method, dict) and method.get("name") == target_method
    ]
    if len(matches) != 1:
        raise ClosureError(
            f"TimelineExecutorSink target method {target_method} resolved "
            f"{len(matches)} times"
        )
    target = matches[0].get("target")
    if not isinstance(target, dict):
        raise ClosureError(
            f"TimelineExecutorSink target cardinality 0 for {target_method}"
        )
    candidates = [
        node.id for node in nodes.values()
        if node_identity(node) == target
    ]
    if len(candidates) != 1:
        raise ClosureError(
            f"TimelineExecutorSink target cardinality {len(candidates)} "
            f"for {target_method}"
        )
    observed = ast_data.get("timelineSinkAuthority")
    if observed is not None and observed != authority:
        raise ClosureError("TimelineExecutorSink AST authority drift")
    return candidates


def validate_digest_sink_test_authority(
    repo_root: Path, authority: dict[str, Any]
) -> None:
    required = {
        "source",
        "qualifiedName",
        "method",
        "targetKind",
        "bodySha256",
        "setDigestSinkCallCount",
        "ownerTarget",
    }
    if set(authority) != required:
        raise ClosureError(
            "TimelineDigestSink test authority fields differ: "
            f"{sorted(authority)!r}"
        )
    source = authority["source"]
    if not isinstance(source, str) or not source.startswith("HarkenEngine/Test/"):
        raise ClosureError("TimelineDigestSink test authority source is not test-only")
    path = repo_root / source
    if not path.is_file():
        raise ClosureError(f"TimelineDigestSink test authority source is missing: {source}")
    text = path.read_text(encoding="utf-8", errors="strict")
    if authority["qualifiedName"] != "(anonymous namespace)::DigestSink":
        raise ClosureError("TimelineDigestSink test authority class identity drift")
    if authority["method"] != "Fold":
        raise ClosureError("TimelineDigestSink test authority method drift")
    if authority["targetKind"] != "TimelineDigestSink::Fold":
        raise ClosureError("TimelineDigestSink test authority target kind drift")
    if authority["ownerTarget"] != "harken_runtime_tests":
        raise ClosureError("TimelineDigestSink test authority owner drift")
    if sha256_file(path) != authority["bodySha256"]:
        raise ClosureError("TimelineDigestSink test authority body digest drift")
    class_pattern = re.compile(
        r"class\s+DigestSink\s+final\s*:\s*public\s+"
        r"harken::TimelineDigestSink"
    )
    fold_pattern = re.compile(
        r"void\s+Fold\s*\(\s*const\s+"
        r"harken::TimelineDigestRecord\s*&\s*record\s*\)"
        r"\s*noexcept\s+override"
    )
    if len(class_pattern.findall(text)) != 1 or len(fold_pattern.findall(text)) != 1:
        raise ClosureError("TimelineDigestSink test authority AST shape drift")
    calls = re.findall(
        r"engine->timelineExecutor\.SetDigestSink\(\s*&digestSink\s*\)",
        text,
    )
    if len(calls) != authority["setDigestSinkCallCount"]:
        raise ClosureError("TimelineDigestSink test authority setter cardinality drift")
    cmake = repo_root / "HarkenEngine/Test/CMakeLists.txt"
    cmake_text = cmake.read_text(encoding="utf-8", errors="strict")
    if cmake_text.count("UnitTest/TimelineConvergenceTest.cpp") != 1:
        raise ClosureError("TimelineDigestSink test target source cardinality drift")
    if "add_executable(harken_runtime_tests" not in cmake_text:
        raise ClosureError("TimelineDigestSink test owner target drift")


def resolve_compiler_identity_target(
    nodes: dict[str, Node],
    overlay_name: str,
    identity: Any,
) -> list[str]:
    if not isinstance(identity, dict):
        raise ClosureError(
            f"overlay {overlay_name} compiler identity is invalid"
        )
    fields = set(identity)
    if fields != REQUIRED_COMPILER_IDENTITY_FIELDS:
        raise ClosureError(
            f"overlay {overlay_name} compiler identity fields differ: "
            f"{sorted(fields)!r}"
        )
    matches = [
        node.id
        for node in nodes.values()
        if {
            "id": node.id,
            "usr": node.usr,
            "mangled": node.mangled,
            "qualifiedName": node.qualified_name,
            "source": node.source,
            "line": node.line,
            "column": node.column,
        }
        == identity
    ]
    if len(matches) != 1:
        raise ClosureError(
            f"overlay {overlay_name} compiler identity resolved "
            f"{len(matches)} times"
        )
    return matches


def apply_overlays(
    nodes: dict[str, Node],
    dynamic_sites: list[DynamicSite],
    manifest: dict[str, Any],
    effect_targets: dict[str, list[str]],
    timeline_sink_authority: dict[str, Any] | None = None,
    realtime_worker_domain_authority: dict[str, Any] | None = None,
    avrt_system_authority: dict[str, Any] | None = None,
    spin_until_authority: dict[str, Any] | None = None,
    repo_root: Path | None = None,
) -> tuple[list[Edge], list[DynamicSite], list[dict[str, Any]]]:
    overlays = manifest.get("overlays")
    if not isinstance(overlays, list) or not overlays:
        raise ClosureError("classification manifest has no overlays")
    unmatched = set(range(len(dynamic_sites)))
    edges: list[Edge] = []
    overlay_reports: list[dict[str, Any]] = []
    for overlay in overlays:
        if not isinstance(overlay, dict):
            raise ClosureError("overlay entry is not an object")
        name = overlay.get("name")
        expected_sites = overlay.get("sites")
        derivation = overlay.get("targetDerivation")
        if not isinstance(name, str) or not isinstance(expected_sites, list):
            raise ClosureError("overlay name/sites are invalid")
        qualification_private = False
        matched_indexes: list[int] = []
        for expected in expected_sites:
            matches = [
                index
                for index in unmatched
                if overlay_site_matches(dynamic_sites[index], expected)
            ]
            if len(matches) != 1:
                raise ClosureError(
                    f"overlay {name} site resolved {len(matches)} times: "
                    f"{expected!r}"
                )
            matched_indexes.append(matches[0])
        if derivation == "voice-cascade-thunk":
            targets = [
                node.id
                for node in nodes.values()
                if node.qualified_name == "harken::VoicePool::CascadeThunk"
            ]
        elif isinstance(derivation, str) and derivation.startswith(
            "effect-registry:"
        ):
            field_name = derivation.split(":", 1)[1]
            targets = effect_targets.get(field_name, [])
        elif isinstance(derivation, str) and derivation.startswith("symbol:"):
            qualified = derivation.split(":", 1)[1]
            targets = [
                node.id
                for node in nodes.values()
                if node.qualified_name == qualified
            ]
        elif derivation == "compiler-identity":
            targets = resolve_compiler_identity_target(
                nodes,
                name,
                overlay.get("targetIdentity"),
            )
        elif derivation == "test-only-digest-sink":
            authority_report = overlay.get("authority")
            if not isinstance(authority_report, dict) or repo_root is None:
                raise ClosureError(
                    f"overlay {name} digest authority is missing"
                )
            validate_digest_sink_test_authority(repo_root, authority_report)
            targets = []
            qualification_private = True
        elif isinstance(derivation, str) and derivation.startswith(
            "realtime-worker-domain:"
        ):
            if realtime_worker_domain_authority is None:
                raise ClosureError(
                    f"overlay {name} lacks RealtimeWorkerDomain authority"
                )
            authority_report = overlay.get("authority")
            if (
                isinstance(authority_report, str)
                and authority_report == "realtime-worker-domain-production"
            ):
                authority_report = realtime_worker_domain_authority
            if (
                not isinstance(authority_report, dict)
                or authority_report != realtime_worker_domain_authority
            ):
                raise ClosureError(
                    f"overlay {name} RealtimeWorkerDomain authority drift"
                )
            targets = derive_realtime_worker_domain_target(
                nodes,
                realtime_worker_domain_authority,
                derivation.split(":", 1)[1],
            )
        elif isinstance(derivation, str) and derivation.startswith("avrt-system:"):
            if avrt_system_authority is None:
                raise ClosureError(f"overlay {name} lacks AVRT system authority")
            authority_report = overlay.get("authority")
            if (
                isinstance(authority_report, str)
                and authority_report == "avrt-system-production"
            ):
                authority_report = avrt_system_authority
            if (
                not isinstance(authority_report, dict)
                or authority_report != avrt_system_authority
            ):
                raise ClosureError(f"overlay {name} AVRT authority drift")
            targets = derive_avrt_system_target(
                nodes,
                avrt_system_authority,
                derivation.split(":", 1)[1],
            )
        elif derivation == "spin-until-jobs-drained":
            if spin_until_authority is None:
                raise ClosureError(f"overlay {name} lacks SpinUntil authority")
            authority_report = overlay.get("authority")
            if (
                isinstance(authority_report, str)
                and authority_report == "spin-until-jobs-drained-production"
            ):
                authority_report = spin_until_authority
            if (
                not isinstance(authority_report, dict)
                or authority_report != spin_until_authority
            ):
                raise ClosureError(f"overlay {name} SpinUntil authority drift")
            targets = derive_spin_until_target(nodes, spin_until_authority)
        elif isinstance(derivation, str) and derivation.startswith(
            "timeline-executor-sink:"
        ):
            if timeline_sink_authority is None:
                raise ClosureError(
                    f"overlay {name} lacks TimelineExecutorSink authority"
                )
            targets = derive_timeline_executor_sink_targets(
                nodes,
                {"timelineSinkAuthority": timeline_sink_authority},
                timeline_sink_authority,
                derivation.split(":", 1)[1],
            )
        else:
            raise ClosureError(f"overlay {name} has unknown target derivation")
        if not targets and not qualification_private:
            raise ClosureError(f"overlay {name} has no target")
        for index in matched_indexes:
            unmatched.remove(index)
            site = dynamic_sites[index]
            for target in targets:
                edges.append(
                    Edge(
                        caller=site.caller,
                        callee=target,
                        kind="overlay",
                        source=site.source,
                        line=site.line,
                        column=site.column,
                        overlay=name,
                    )
                )
        overlay_reports.append(
            {
                "name": name,
                "targetDerivation": derivation,
                "siteCount": len(matched_indexes),
                "targetCount": len(targets),
                "targets": sorted(targets),
                "status": "PASS",
            }
        )
    return edges, [dynamic_sites[index] for index in sorted(unmatched)], overlay_reports


def validate_dynamic_blockers(
    dynamic_sites: list[DynamicSite],
    blockers: Any,
) -> list[dict[str, Any]]:
    if blockers is None:
        return []
    if not isinstance(blockers, list):
        raise ClosureError("dynamicBlockers must be a list")
    unmatched = set(range(len(dynamic_sites)))
    reports: list[dict[str, Any]] = []
    for blocker in blockers:
        if not isinstance(blocker, dict):
            raise ClosureError("dynamic blocker entry is not an object")
        missing = sorted(REQUIRED_DYNAMIC_BLOCKER_FIELDS - set(blocker))
        if missing:
            raise ClosureError(
                f"dynamic blocker entry is incomplete: {missing!r}"
            )
        blocker_id = blocker["blocker"]
        if blocker_id not in KNOWN_BLOCKERS:
            raise ClosureError(
                f"dynamic blocker has invalid blocker: {blocker_id!r}"
            )
        matches = [
            index
            for index in unmatched
            if overlay_site_matches(dynamic_sites[index], blocker)
        ]
        if len(matches) != 1:
            raise ClosureError(
                "dynamic blocker site resolved "
                f"{len(matches)} times: {blocker!r}"
            )
        unmatched.remove(matches[0])
        site = dynamic_sites[matches[0]]
        reports.append(
            {
                "site": site.key,
                "blocker": blocker_id,
                "evidence": blocker["evidence"],
                "status": "OPEN",
            }
        )
    return reports


def partition_dynamic_blockers(
    unknown_sites: list[DynamicSite],
    blocker_reports: list[dict[str, Any]],
) -> tuple[list[DynamicSite], list[dict[str, Any]]]:
    blocked_keys = {entry["site"] for entry in blocker_reports}
    remaining = [
        site for site in unknown_sites if site.key not in blocked_keys
    ]
    return remaining, blocker_reports


def reachable_closure(
    roots: list[str], nodes: dict[str, Node], edges: list[Edge]
) -> tuple[set[str], list[Edge]]:
    adjacency: dict[str, list[Edge]] = defaultdict(list)
    for edge in edges:
        adjacency[edge.caller].append(edge)
    reached = set(roots)
    selected: list[Edge] = []
    queue = deque(roots)
    while queue:
        caller = queue.popleft()
        for edge in adjacency.get(caller, []):
            selected.append(edge)
            if edge.callee in nodes and edge.callee not in reached:
                reached.add(edge.callee)
                if nodes[edge.callee].scope == "repository":
                    queue.append(edge.callee)
    return reached, selected


def reject_reachable_unknown_sites(
    unknown_sites: list[DynamicSite], reached: set[str]
) -> None:
    reachable = [site for site in unknown_sites if site.caller in reached]
    if reachable:
        raise ClosureError(
            "reachable unknown call sites remain: "
            + ", ".join(site.key for site in reachable[:20])
        )



def validate_classifications(
    manifest: dict[str, Any],
    reached: set[str],
    nodes: dict[str, Node],
) -> dict[str, dict[str, Any]]:
    raw = manifest.get("classifications")
    if not isinstance(raw, dict):
        raise ClosureError("classification manifest has no classifications")
    identity_owners: dict[tuple[str, str, str, str], str] = {}
    for node_id in sorted(reached):
        node = nodes.get(node_id)
        if node is None or node.id != node_id:
            raise ClosureError(
                f"reachable compiler identity registration mismatch: {node_id}"
            )
        identity = (
            node.usr,
            node.mangled,
            node.qualified_name,
            node.source,
        )
        prior = identity_owners.setdefault(identity, node_id)
        if prior != node_id:
            raise ClosureError(
                "reachable compiler identity resolved 2 times: "
                f"{node.qualified_name} [{prior}, {node_id}]"
            )
    expected = set(raw)
    missing = sorted(reached - expected)
    stale = sorted(expected - reached)
    if missing or stale:
        raise ClosureError(
            "reachable classification drift; "
            f"missing={missing!r}, stale={stale!r}"
        )
    result: dict[str, dict[str, Any]] = {}
    for node_id in sorted(reached):
        entry = raw[node_id]
        if not isinstance(entry, dict):
            raise ClosureError(f"classification {node_id} is not an object")
        missing_fields = sorted(REQUIRED_CLASSIFICATION_FIELDS - set(entry))
        if missing_fields:
            raise ClosureError(
                f"classification {node_id} is incomplete: {missing_fields!r}"
            )
        if entry.get("bodySha256") != nodes[node_id].body_sha256:
            raise ClosureError(
                f"classification body digest drift: "
                f"{nodes[node_id].qualified_name}"
            )
        if any(
            not isinstance(entry[field], str) or not entry[field]
            for field in REQUIRED_CLASSIFICATION_FIELDS
        ):
            raise ClosureError(
                f"classification {node_id} contains empty field values"
            )
        for field, allowed in CLASSIFICATION_ENUMS.items():
            if entry[field] not in allowed:
                raise ClosureError(
                    f"classification {node_id} has invalid {field}: "
                    f"{entry[field]!r}"
                )
        blockers = entry.get("blockers", [])
        if (
            not isinstance(blockers, list)
            or any(
                not isinstance(blocker, str)
                or blocker not in KNOWN_BLOCKERS
                for blocker in blockers
            )
        ):
            raise ClosureError(
                f"classification {node_id} has invalid blockers"
            )
        for field, forbidden in FORBIDDEN_CLASSIFICATIONS:
            if entry[field] == forbidden:
                raise ClosureError(
                    f"forbidden callback-reachable classification "
                    f"{field}={forbidden}: "
                    f"{nodes[node_id].qualified_name}"
                )
        result[node_id] = entry
    return result


def external_leaf_classification(node: Node) -> dict[str, Any]:
    name = node.qualified_name
    folded = name.casefold()
    result: dict[str, Any] = {
        "allocation": "external-no-allocation",
        "blocking": "external-nonblocking",
        "io": "external-no-io",
        "exception": "external-noexcept-contract",
        "loopBound": "external-leaf",
        "configurationBound": "external-leaf",
        "threadQos": "external-leaf",
        "evidenceOwner": "m2-wp0-ast",
        "cost": "constant",
    }
    if re.search(
        r"(^|::)(operator new|operator delete|malloc|calloc|realloc|free)$",
        folded,
    ):
        result["allocation"] = "forbidden-system-allocation"
        result["cost"] = "unknown-blocker"
        return result
    if re.fullmatch(
        r"std::vector::"
        r"(assign|clear|emplace|emplace_back|erase|insert|pop_back|"
        r"push_back|reserve|resize|shrink_to_fit)",
        name,
    ) or re.fullmatch(
        r"std::unique_ptr::(reset|release|swap)",
        name,
    ):
        result["allocation"] = "forbidden-system-allocation"
        result["cost"] = "unknown-blocker"
        return result
    if re.search(
        r"(mutex|lock_guard|unique_lock|criticalsection|acquiresrwlock)",
        folded,
    ):
        result["blocking"] = "forbidden-lock"
        result["cost"] = "scheduler-dependent"
        return result
    if re.search(
        r"(^|::)(fopen|fread|fwrite|readfile|writefile|createfile[aw]?)$",
        folded,
    ):
        result["io"] = "forbidden-file-io"
        result["cost"] = "unknown-blocker"
        return result
    if name == "std::_Atomic_storage::wait":
        result.update(
            {
                "blocking": "scheduler-wait",
                "threadQos": "os-api",
                "evidenceOwner": "m2-runtime-architecture",
                "cost": "scheduler-dependent",
                "blockers": ["RT-B002"],
            }
        )
        return result
    if name == "std::this_thread::yield":
        result.update(
            {
                "blocking": "scheduler-yield",
                "threadQos": "os-api",
                "evidenceOwner": "m2-runtime-architecture",
                "cost": "scheduler-dependent",
                "blockers": ["RT-B002"],
            }
        )
        return result
    transcendental = {
        "cos",
        "exp",
        "exp2",
        "log",
        "log10",
        "log2",
        "pow",
        "sin",
        "tanh",
    }
    if name in transcendental:
        result.update(
            {
                "evidenceOwner": "m2-runtime-architecture",
                "cost": "crt-transcendental-open",
                "blockers": ["RT-B006"],
            }
        )
        return result
    if name in {"ceil", "fabs", "isfinite", "isnan", "sqrt"}:
        result["cost"] = "crt-basic-math"
        return result
    if name in {"memcpy", "memset", "std::fill"}:
        result["cost"] = "linear-configuration-bounded"
        return result
    if (
        node.source == "<compiler-external-avrt>"
        and node.features.get("externalAuthority") == "avrt-system-field"
        and name in {entry["target"] for entry in AVRT_SYSTEM_FIELDS.values()}
    ):
        result.update({
            "threadQos": "os-api",
            "evidenceOwner": "m2-platform-qualification",
        })
        return result
    exact_nonblocking = {
        "_LARGE_INTEGER::_LARGE_INTEGER",
        "_mm_getcsr",
        "_mm_setcsr",
        "max",
        "std::_Atomic_integral::fetch_add",
        "std::_Atomic_integral_facade::fetch_sub",
        "std::_Atomic_storage::load",
        "std::_Atomic_storage::notify_all",
        "std::_Atomic_storage::store",
        "std::_Vector_iterator::operator*",
        "std::_Vector_iterator::operator+",
        "std::array::operator[]",
        "std::atomic::compare_exchange_weak",
        "std::clamp",
        "std::max",
        "std::min",
        "std::numeric_limits::max",
        "std::numeric_limits::min",
        "std::operator==",
        "std::unique_ptr::get",
        "std::unique_ptr::operator[]",
        "std::vector::begin",
        "std::vector::data",
        "std::vector::end",
        "std::vector::operator[]",
        "std::vector::size",
        "std::vector::swap",
    }
    if name == "QueryPerformanceCounter" or name in exact_nonblocking:
        if name == "QueryPerformanceCounter":
            result.update(
                {
                    "io": "clock-read",
                    "threadQos": "os-api",
                    "evidenceOwner": "m2-platform-qualification",
                }
            )
        return result
    return {
        "allocation": "unknown-blocker",
        "blocking": "unknown-blocker",
        "io": "unknown-blocker",
        "exception": "unknown-blocker",
        "loopBound": "external-leaf",
        "configurationBound": "external-leaf",
        "threadQos": "external-leaf",
        "evidenceOwner": "m2-runtime-architecture",
        "cost": "unknown-blocker",
        "blockers": ["RT-B007"],
    }


def internal_node_classification(node: Node) -> dict[str, Any]:
    result: dict[str, Any] = {
        "allocation": (
            "forbidden-system-allocation"
            if node.features["hasNew"] or node.features["hasDelete"]
            else "none"
        ),
        "blocking": "none",
        "io": "none",
        "exception": (
            "caught-at-boundary"
            if node.features["hasTry"]
            else (
                "may-throw-contained"
                if node.features["hasThrow"]
                else "no-direct-throw"
            )
        ),
        "loopBound": (
            "unknown-blocker"
            if node.features["hasLoop"]
            else "no-loop"
        ),
        "configurationBound": "release-runtime-configuration",
        "threadQos": "callback-or-render-worker",
        "evidenceOwner": "m2-wp0-ast",
        "cost": (
            "unknown-blocker"
            if node.features["hasLoop"]
            else "constant"
        ),
    }
    if node.features["hasLoop"]:
        result["evidenceOwner"] = "m2-runtime-architecture"
        result["blockers"] = ["RT-B008"]
    if node.qualified_name in {
        "harken::BlockArena::Alloc",
        "harken::BlockArena::AllocFloats",
    }:
        result["allocation"] = "bounded-block-arena"
    return result


def classification_template(
    nodes: dict[str, Node],
    reached: set[str],
    overlays: list[dict[str, Any]],
    source_dependency_digest: str,
) -> dict[str, Any]:
    classifications: dict[str, Any] = {}
    for node_id in sorted(reached):
        node = nodes[node_id]
        entry = (
            external_leaf_classification(node)
            if node.scope == "external"
            else internal_node_classification(node)
        )
        classifications[node_id] = {
            "qualifiedName": node.qualified_name,
            "source": node.source,
            "bodySha256": node.body_sha256,
            **entry,
        }
        entry = classifications[node_id]
        if node.qualified_name == "harken::MixerLoop::DrainCommands":
            entry.update(
                {
                    "blocking": "none",
                    "loopBound": "concurrent-producer-unbounded",
                    "configurationBound": "command-capacity-insufficient",
                    "evidenceOwner": "m2-runtime-architecture",
                    "cost": "unknown-blocker",
                    "blockers": ["RT-B001"],
                }
            )
        elif node.qualified_name in {
            "harken::RenderWorkerPool::Run",
            "harken::RenderWorkerPool::RunBatch",
            "harken::RenderWorkerPool::WorkerMain",
        }:
            entry.update(
                {
                    "blocking": (
                        "coordinator-yield-spin"
                        if node.qualified_name.endswith("::Run")
                        else "worker-epoch-wait"
                    ),
                    "loopBound": "worker-lifecycle-dependent",
                    "configurationBound": "render-worker-count",
                    "threadQos": "render-worker-unproven",
                    "evidenceOwner": "m2-runtime-architecture",
                    "cost": "scheduler-dependent",
                    "blockers": ["RT-B002"],
                }
            )
        elif node.qualified_name in {
            "harken::MeterBlockRing::MergeOverflowLane",
            "harken::MeterBlockRing::MergeOverflowTaps",
            "harken::MeterBlockRing::PushTaps",
        }:
            entry.update(
                {
                    "blocking": "lock-free-retry",
                    "loopBound": "consumer-contention-unbounded",
                    "configurationBound": "armed-meter-variables",
                    "evidenceOwner": "m2-runtime-architecture",
                    "cost": "scheduler-dependent",
                    "blockers": ["RT-B003"],
                }
            )
        elif node.qualified_name in {
            "harken::MixerLoop::PublishSnapshot",
            "harken::VoicePool::FillSourceCursors",
            "harken::SourceCursorMirror::BeginWrite",
            "harken::SourceCursorMirror::EndWrite",
        }:
            entry.update(
                {
                    "loopBound": "fixed-or-configuration-bounded",
                    "configurationBound": "max-voices-and-parking",
                    "evidenceOwner": "m2-runtime-architecture",
                    "cost": "linear-configuration-bounded",
                    "blockers": ["RT-B004"],
                }
            )
    return {
        "schemaVersion": CLASSIFICATION_SCHEMA_VERSION,
        "description": (
            "Exact compiler-identity and body-digest classification for the "
            "Release Runtime callback closure."
        ),
        "sourceDependencyDigest": source_dependency_digest,
        "knownBlockers": KNOWN_BLOCKERS,
        "overlays": overlays,
        "classifications": classifications,
    }


def load_manifest(path: Path) -> dict[str, Any]:
    value = read_json(path)
    if not isinstance(value, dict):
        raise ClosureError("classification manifest root is not an object")
    if value.get("schemaVersion") != CLASSIFICATION_SCHEMA_VERSION:
        raise ClosureError("classification schema version drift")
    blockers = value.get("knownBlockers")
    if blockers != KNOWN_BLOCKERS:
        raise ClosureError("known blocker classification drift")
    return value


def validate_source_dependency_digest(
    manifest: dict[str, Any], observed: str
) -> None:
    if manifest.get("sourceDependencyDigest") != observed:
        raise ClosureError(
            "classification source/include dependency digest drift"
        )


def git_record(repo_root: Path) -> dict[str, Any]:
    commit = command_output(["git", "-C", str(repo_root), "rev-parse", "HEAD"]).strip()
    status = command_output(
        [
            "git",
            "-C",
            str(repo_root),
            "status",
            "--porcelain=v1",
            "--untracked-files=no",
        ]
    ).splitlines()
    return {
        "commit": commit,
        "trackedWorktreeClean": not status,
        "trackedStatus": status,
    }


def default_overlay_template(
    dynamic_sites: list[DynamicSite],
    nodes: dict[str, Node],
) -> list[dict[str, Any]]:
    groups: list[dict[str, Any]] = []
    job_sites = [
        site
        for site in dynamic_sites
        if site.source.endswith("RenderWorkerPool.cpp")
        and site.member in {"fn", "m_fn"}
    ]
    groups.append(
        {
            "name": "render-worker-jobfn-to-cascade",
            "targetDerivation": "voice-cascade-thunk",
            "sites": [
                {
                    "source": site.source,
                    "line": site.line,
                    "column": site.column,
                    "kind": site.kind,
                    "member": site.member,
                }
                for site in sorted(job_sites, key=lambda item: item.key)
            ],
        }
    )
    for member, target in (
        ("renderBlock", "harken::MixerLoop::RenderBlock"),
        ("copy", "memcpy"),
    ):
        sites = [
            site
            for site in dynamic_sites
            if site.source.endswith("Runtime/api/src/RenderSlicer.h")
            and site.member == member
        ]
        groups.append(
            {
                "name": f"render-slicer-{member}",
                "targetDerivation": "symbol:" + target,
                "sites": [
                    {
                        "source": site.source,
                        "line": site.line,
                        "column": site.column,
                        "kind": site.kind,
                        "member": site.member,
                    }
                    for site in sorted(sites, key=lambda item: item.key)
                ],
            }
        )
    for member, field_name in (
        ("process", "process"),
        ("processDynamic", "processDynamic"),
        ("tailFrames", "tailFrames"),
        ("setParam", "setParam"),
        ("reset", "reset"),
    ):
        sites = [site for site in dynamic_sites if site.member == member]
        if sites:
            groups.append(
                {
                    "name": f"insert-effect-{member}",
                    "targetDerivation": f"effect-registry:{field_name}",
                    "sites": [
                        {
                            "source": site.source,
                            "line": site.line,
                            "column": site.column,
                            "kind": site.kind,
                            "member": site.member,
                        }
                        for site in sorted(sites, key=lambda item: item.key)
                    ],
                }
            )
    raii = {
        "audio-thread-scope-callback": (
            "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp",
            "harken::DeviceBackend::Impl::DataCallback",
            "harken::memguard::AudioThreadScope::AudioThreadScope",
            "harken::memguard::AudioThreadScope::~AudioThreadScope",
        ),
        "audio-thread-scope-mix": (
            "HarkenEngine/Source/Runtime/render/src/MixerLoop.cpp",
            "harken::MixerLoop::RenderBlock",
            "harken::memguard::AudioThreadScope::AudioThreadScope",
            "harken::memguard::AudioThreadScope::~AudioThreadScope",
        ),
        "flush-denormals-mix": (
            "HarkenEngine/Source/Runtime/render/src/MixerLoop.cpp",
            "harken::MixerLoop::RenderBlock",
            "harken::floatmode::ScopedFlushDenormals::ScopedFlushDenormals",
            "harken::floatmode::ScopedFlushDenormals::~ScopedFlushDenormals",
        ),
        "audio-thread-scope-worker-batch": (
            "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp",
            "harken::RenderWorkerPool::RunBatch",
            "harken::memguard::AudioThreadScope::AudioThreadScope",
            "harken::memguard::AudioThreadScope::~AudioThreadScope",
        ),
        "flush-denormals-worker-main": (
            "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp",
            "harken::RenderWorkerPool::WorkerMain",
            "harken::floatmode::ScopedFlushDenormals::ScopedFlushDenormals",
            "harken::floatmode::ScopedFlushDenormals::~ScopedFlushDenormals",
        ),
    }
    for name, (
        source,
        caller_name,
        constructor,
        destructor,
    ) in raii.items():
        callers = [
            node
            for node in nodes.values()
            if node.source == source
            and node.qualified_name == caller_name
        ]
        if len(callers) != 1:
            raise ClosureError(
                f"RAII template {name} caller resolved {len(callers)} times"
            )
        caller = callers[0]
        declarations = [
            declaration
            for declaration in caller.features["raiiDeclarations"]
            if declaration["constructor"] == constructor
        ]
        if len(declarations) != 1:
            raise ClosureError(
                f"RAII template {name} declaration resolved "
                f"{len(declarations)} times"
            )
        binding = {
            "implicitCaller": {
                "id": caller.id,
                "qualifiedName": caller.qualified_name,
                "source": caller.source,
                "bodySha256": caller.body_sha256,
            },
            "implicitDeclaration": declarations[0],
        }
        groups.append(
            {
                "name": name + "-construct",
                "targetDerivation": "symbol:" + constructor,
                "sites": [],
                **binding,
            }
        )
        groups.append(
            {
                "name": name + "-destroy",
                "targetDerivation": "symbol:" + destructor,
                "sites": [],
                **binding,
            }
        )
    return groups


def materialize_implicit_raii_sites(
    overlays: list[dict[str, Any]],
    nodes: dict[str, Node],
    direct_edges: list[Edge],
) -> tuple[list[dict[str, Any]], list[Edge], list[dict[str, Any]]]:
    explicit: list[dict[str, Any]] = []
    edges: list[Edge] = []
    reports: list[dict[str, Any]] = []
    for overlay in overlays:
        caller_binding = overlay.get("implicitCaller")
        declaration_binding = overlay.get("implicitDeclaration")
        if caller_binding is None and declaration_binding is None:
            explicit.append(overlay)
            continue
        if not isinstance(caller_binding, dict) or not isinstance(
            declaration_binding, dict
        ):
            raise ClosureError(
                f"implicit RAII overlay {overlay['name']} binding incomplete"
            )
        targets = [
            node.id
            for node in nodes.values()
            if "symbol:" + node.qualified_name
            == overlay.get("targetDerivation")
        ]
        callers = [
            node.id
            for node in nodes.values()
            if node.id == caller_binding.get("id")
            and node.qualified_name == caller_binding.get("qualifiedName")
            and node.source == caller_binding.get("source")
            and node.body_sha256 == caller_binding.get("bodySha256")
        ]
        if len(targets) != 1 or len(callers) != 1:
            raise ClosureError(
                f"implicit RAII overlay {overlay['name']} resolved "
                f"callers={len(callers)} targets={len(targets)}"
            )
        caller = nodes[callers[0]]
        target = nodes[targets[0]]
        if declaration_binding not in caller.features["raiiDeclarations"]:
            raise ClosureError(
                f"implicit RAII overlay {overlay['name']} declaration drift"
            )
        owner, method = target.qualified_name.rsplit("::", 1)
        class_name = owner.rsplit("::", 1)[-1]
        constructor_name = owner + "::" + class_name
        if declaration_binding.get("constructor") != constructor_name:
            raise ClosureError(
                f"implicit RAII overlay {overlay['name']} constructor drift"
            )
        constructors = {
            node.id
            for node in nodes.values()
            if node.qualified_name == constructor_name
        }
        constructor_edges = [
            edge
            for edge in direct_edges
            if (
                edge.kind == "ast-direct"
                and edge.caller == caller.id
                and edge.callee in constructors
                and edge.source == declaration_binding["source"]
                and edge.line
                == declaration_binding["constructorSite"]["line"]
            )
        ]
        if len(constructors) != 1 or not constructor_edges:
            raise ClosureError(
                f"implicit RAII overlay {overlay['name']} lacks direct "
                f"constructor evidence for {constructor_name}"
            )
        is_destructor = method.startswith("~")
        if is_destructor:
            edges.append(
                Edge(
                    caller=callers[0],
                    callee=targets[0],
                    kind="overlay-implicit-raii-destructor",
                    source=declaration_binding["source"],
                    line=declaration_binding["line"],
                    column=declaration_binding["column"],
                    overlay=overlay["name"],
                )
            )
        reports.append(
            {
                "name": overlay["name"],
                "targetDerivation": "implicit-raii",
                "siteCount": 1,
                "targetCount": 1,
                "targets": [targets[0]],
                "caller": caller_binding,
                "declaration": declaration_binding,
                "evidence": (
                    "cxx-raii-destructor-from-ast-constructor"
                    if is_destructor
                    else "ast-direct-constructor"
                ),
                "status": "PASS",
            }
        )
        explicit.append(overlay)
    return explicit, edges, reports


def reject_unmodeled_implicit_destructors(
    overlays: list[dict[str, Any]],
    nodes: dict[str, Node],
    reached: set[str],
) -> None:
    repository_destructors = {
        node.qualified_name
        for node in nodes.values()
        if (
            node.scope == "repository"
            and node.qualified_name.rsplit("::", 1)[-1].startswith("~")
        )
    }
    modeled = {
        (
            overlay.get("implicitCaller", {}).get("id"),
            json_digest(overlay["implicitDeclaration"]),
            overlay.get("targetDerivation"),
        )
        for overlay in overlays
        if (
            isinstance(overlay.get("implicitCaller"), dict)
            and isinstance(overlay.get("implicitDeclaration"), dict)
        )
    }
    missing: list[str] = []
    for node_id in sorted(reached):
        node = nodes[node_id]
        if node.scope != "repository":
            continue
        for declaration in node.features["raiiDeclarations"]:
            constructor = declaration["constructor"]
            owner = constructor.rsplit("::", 1)[0]
            class_name = owner.rsplit("::", 1)[-1]
            destructor = owner + "::~" + class_name
            if destructor not in repository_destructors:
                continue
            key = (
                node.id,
                json_digest(declaration),
                "symbol:" + destructor,
            )
            if key not in modeled:
                missing.append(
                    f"{node.qualified_name}:{declaration['line']}:"
                    f"{declaration['name']}->{destructor}"
                )
    if missing:
        raise ClosureError(
            "reachable implicit destructors lack exact AST-bound overlays: "
            + ", ".join(missing)
        )


def build_report(
    repo_root: Path,
    build: Path,
    classification_path: Path,
    libclang: Path,
    clang_executable: Path,
    *,
    write_template: bool,
) -> dict[str, Any]:
    (
        toolchain,
        translation_units,
        project_includes,
        system_includes,
        definitions,
    ) = toolchain_configuration(
        build, repo_root, libclang, clang_executable
    )
    args = clang_arguments(project_includes, system_includes, definitions)
    (
        nodes,
        direct_edges,
        dynamic_sites,
        ast_data,
        tu_records,
        include_dependencies,
    ) = (
        parse_translation_units(
            repo_root,
            translation_units,
            args,
            libclang,
            clang_executable,
        )
    )
    source_dependency_digest = json_digest(
        [
            {
                "path": record["source"],
                "sha256": record["sourceSha256"],
            }
            for record in tu_records
        ]
        + include_dependencies
    )
    timeline_authority = timeline_sink_ast_authority(
        ast_data["classes"], nodes, ast_data["cursors"], repo_root
    )
    ast_data["timelineSinkAuthority"] = timeline_authority
    realtime_domain_authority = realtime_worker_domain_ast_authority(
        ast_data, nodes, repo_root
    )
    avrt_system_authority = avrt_system_ast_authority(
        ast_data, nodes, repo_root
    )
    for family in AVRT_SYSTEM_FIELDS:
        target = avrt_external_node(AVRT_SYSTEM_FIELDS[family]["target"])
        prior = nodes.get(target.id)
        if (
            prior is not None
            and node_full_identity(prior) != node_full_identity(target)
        ):
            raise ClosureError("AVRT external target identity collision")
        nodes[target.id] = target
    spin_until_authority = spin_until_ast_authority(ast_data, nodes, repo_root)
    roots = resolve_roots(nodes)
    effect_targets = registry_effect_targets(
        ast_data["cursor"], nodes, repo_root
    )

    if write_template:
        overlays = default_overlay_template(dynamic_sites, nodes)
        manifest = {"dynamicBlockers": []}
    else:
        manifest = load_manifest(classification_path)
        validate_source_dependency_digest(
            manifest, source_dependency_digest
        )
        overlays = manifest["overlays"]
    overlays, raii_edges, raii_reports = materialize_implicit_raii_sites(
        overlays, nodes, direct_edges
    )
    non_raii_manifest = {
        "overlays": [
            overlay
            for overlay in overlays
            if "implicitCaller" not in overlay
        ]
    }
    overlay_edges, unknown_sites, overlay_reports = apply_overlays(
        nodes,
        dynamic_sites,
        non_raii_manifest,
        effect_targets,
        timeline_authority,
        realtime_domain_authority,
        avrt_system_authority,
        spin_until_authority,
        repo_root,
    )
    dynamic_blocker_reports = validate_dynamic_blockers(
        unknown_sites, manifest.get("dynamicBlockers")
    )
    unknown_sites, dynamic_blocker_reports = partition_dynamic_blockers(
        unknown_sites, dynamic_blocker_reports
    )
    edges = direct_edges + overlay_edges + raii_edges
    reached, closure_edges = reachable_closure(roots, nodes, edges)
    reject_reachable_unknown_sites(unknown_sites, reached)
    reachable_dynamic_blockers = [
        entry
        for entry in dynamic_blocker_reports
        if any(
            site.key == entry["site"] and site.caller in reached
            for site in dynamic_sites
        )
    ]
    if reachable_dynamic_blockers:
        raise ClosureError(
            "reachable dynamic blockers remain: "
            + "; ".join(
                f"{entry['site']} [{entry['blocker']}]"
                for entry in reachable_dynamic_blockers
            )
        )
    reject_unmodeled_implicit_destructors(overlays, nodes, reached)

    if write_template:
        manifest = classification_template(
            nodes,
            reached,
            overlays,
            source_dependency_digest,
        )
        write_json_atomic(classification_path, manifest)
        validate_source_dependency_digest(
            manifest, source_dependency_digest
        )
    classifications = validate_classifications(manifest, reached, nodes)
    active_blockers = {
        blocker
        for entry in classifications.values()
        for blocker in entry.get("blockers", [])
    }
    reachable_nodes = [nodes[node_id] for node_id in sorted(reached)]
    closure_identity = [
        {
            "id": node.id,
            "bodySha256": node.body_sha256,
        }
        for node in reachable_nodes
    ]
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "harken-runtime-source-callback-closure",
        "analysisLevel": "source-ast",
        "verdict": "SOURCE_CLOSURE_CONSTRUCTED_WITH_OPEN_BLOCKERS",
        "safetyVerdict": "UNRESOLVED",
        "limitations": [
            "This is a libclang source-AST closure under the pinned "
            "Release compile configuration.",
            "It is not an MSVC optimizer or post-link machine-code "
            "call graph.",
        ],
        "git": git_record(repo_root),
        "analyzer": {
            "path": str(Path(__file__).resolve()),
            "sha256": sha256_file(Path(__file__).resolve()),
        },
        "toolchain": toolchain,
        "compileConfiguration": {
            "configuration": "Release",
            "arguments": args,
            "translationUnits": tu_records,
            "translationUnitCount": len(tu_records),
            "includeDependencies": include_dependencies,
            "includeDependencyCount": len(include_dependencies),
            "sourceDependencyDigest": source_dependency_digest,
        },
        "classification": {
            "path": str(classification_path),
            "sha256": sha256_file(classification_path),
            "schemaVersion": manifest["schemaVersion"],
        },
        "roots": [
            nodes[node_id].report()
            for node_id in roots
        ],
        "closure": {
            "nodeCount": len(reachable_nodes),
            "edgeCount": len(closure_edges),
            "digest": json_digest(closure_identity),
            "nodes": [
                {
                    **node.report(),
                    "classification": classifications[node.id],
                }
                for node in reachable_nodes
            ],
            "edges": [
                edge.report()
                for edge in sorted(
                    set(closure_edges),
                    key=lambda edge: (
                        edge.caller,
                        edge.callee,
                        edge.source,
                        edge.line,
                        edge.column,
                        edge.kind,
                    ),
                )
            ],
        },
        "dynamicEdges": {
            "effectRegistryTargets": effect_targets,
            "authorities": {
                "realtimeWorkerDomain": realtime_domain_authority,
                "avrtSystem": avrt_system_authority,
                "spinUntilJobsDrained": spin_until_authority,
                "timelineExecutorSink": timeline_authority,
            },
            "overlays": overlay_reports + raii_reports,
            "unknownCalls": [],
            "status": "PASS",
        },
        "knownBlockers": [
            {
                "id": blocker_id,
                "name": name,
                "status": (
                    "OPEN"
                    if blocker_id in active_blockers
                    else "NOT_DETECTED"
                ),
            }
            for blocker_id, name in KNOWN_BLOCKERS.items()
        ],
    }


def failure_report(error: Exception) -> dict[str, Any]:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "harken-runtime-source-callback-closure",
        "analysisLevel": "source-ast",
        "verdict": "FAIL",
        "error": str(error),
    }


def parse_arguments() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--build", type=Path, default=repo_root / "build-rtq-off"
    )
    parser.add_argument(
        "--classification",
        type=Path,
        default=repo_root
        / ".scripts"
        / "runtime_callback_closure_classification.json",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo_root
        / ".temp"
        / "runtime-callback-closure"
        / "report.json",
    )
    parser.add_argument(
        "--libclang",
        type=Path,
        default=Path(
            "C:/Program Files/Microsoft Visual Studio/2022/Community/"
            "VC/Tools/Llvm/x64/bin/libclang.dll"
        ),
    )
    parser.add_argument(
        "--clang",
        type=Path,
        default=Path(
            "C:/Program Files/Microsoft Visual Studio/2022/Community/"
            "VC/Tools/Llvm/x64/bin/clang.exe"
        ),
    )
    parser.add_argument(
        "--write-classification-template",
        action="store_true",
        help="authoring mode: replace the exact closure classification",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    output = arguments.output.resolve(strict=False)
    try:
        report = build_report(
            arguments.repo_root.resolve(strict=False),
            arguments.build.resolve(strict=False),
            arguments.classification.resolve(strict=False),
            arguments.libclang.resolve(strict=False),
            arguments.clang.resolve(strict=False),
            write_template=arguments.write_classification_template,
        )
    except (ClosureError, OSError, ValueError) as error:
        write_json_atomic(output, failure_report(error))
        print(f"Runtime callback closure failed: {error}", file=sys.stderr)
        return 1
    write_json_atomic(output, report)
    print(
        f"Runtime callback closure {report['verdict']}: "
        f"{report['closure']['nodeCount']} nodes, "
        f"{report['closure']['edgeCount']} edges"
    )
    print(f"wrote: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
