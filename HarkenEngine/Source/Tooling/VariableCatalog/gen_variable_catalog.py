#!/usr/bin/env python3
"""Generate Native Variable catalog constants from VariableContractArtifactV1 JSON.

Reads HarkenEngine/Source/Tooling/VariableCatalog/variable_catalog.json (the
EncodeVariableContractArtifactJsonV1 shape) and emits:

  HarkenEngine/Source/Runtime/api/include/harken/generated/HarkenVariables.g.h
  HarkenIntegration/Unity/Harken/Runtime/Generated/HarkenVariables.g.cs

The generated files are checked in so the ordinary build stays Python-free.

    py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py
    py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py --check
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
import uuid


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))
JSON_PATH = os.path.join(SCRIPT_DIR, "variable_catalog.json")
OUT_PATH = os.path.join(
    REPO_ROOT,
    "HarkenEngine",
    "Source",
    "Runtime",
    "api",
    "include",
    "harken",
    "generated",
    "HarkenVariables.g.h",
)
CS_OUT_PATH = os.path.join(
    REPO_ROOT,
    "HarkenIntegration",
    "Unity",
    "Harken",
    "Runtime",
    "Generated",
    "HarkenVariables.g.cs",
)
CODE_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
RESERVED_SYMBOLS = frozenset((
    "abstract as base bool break byte case catch char checked class const continue "
    "decimal default delegate do double else enum event explicit extern false finally "
    "fixed float for foreach goto if implicit in int interface internal is lock long "
    "namespace new null object operator out override params private protected public "
    "readonly ref return sbyte sealed short sizeof stackalloc static string struct "
    "switch this throw true try typeof uint ulong unchecked unsafe ushort using "
    "virtual void volatile while alignas alignof and and_eq asm auto bitand bitor "
    "char8_t char16_t char32_t compl concept consteval constexpr constinit const_cast "
    "co_await co_return co_yield decltype delete dynamic_cast export friend inline "
    "mutable noexcept not not_eq nullptr or or_eq register reinterpret_cast requires "
    "signed static_assert static_cast template thread_local typedef typeid typename "
    "union unsigned wchar_t xor xor_eq HarkenActions HarkenVariables "
    "ExecutionCatalogFingerprint CatalogFingerprint"
).split())


def symbol_name(name: str, prefix: str) -> str:
    return prefix + name if (name in RESERVED_SYMBOLS or name.startswith(prefix)
                            or (prefix == "V_" and name.startswith("Label_"))) else name

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
EXECUTION_DOMAIN = b"HARKEN-VARIABLE-EXECUTION-V1\0"
CATALOG_DOMAIN = b"HARKEN-VARIABLE-CATALOG-V1\0"
EXECUTION_CATALOG_DOMAIN = b"HARKEN-VARIABLE-EXECUTION-CATALOG-V1\0"
SDK_CATALOG_DOMAIN = b"HARKEN-VARIABLE-SDK-CATALOG-V1\0"


class Hasher:
    def __init__(self, domain: bytes) -> None:
        self._hash = FNV_OFFSET
        self.add_bytes(domain)

    def add_bytes(self, data: bytes) -> None:
        h = self._hash
        for byte in data:
            h ^= byte
            h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
        self._hash = h

    def add_u8(self, value: int) -> None:
        self.add_bytes(bytes([value & 0xFF]))

    def add_u16(self, value: int) -> None:
        value &= 0xFFFF
        self.add_bytes(bytes([value & 0xFF, (value >> 8) & 0xFF]))

    def add_u32(self, value: int) -> None:
        value &= 0xFFFFFFFF
        self.add_bytes(bytes([
            value & 0xFF,
            (value >> 8) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 24) & 0xFF,
        ]))

    def add_u64(self, value: int) -> None:
        value &= 0xFFFFFFFFFFFFFFFF
        self.add_bytes(bytes([
            value & 0xFF,
            (value >> 8) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 24) & 0xFF,
            (value >> 32) & 0xFF,
            (value >> 40) & 0xFF,
            (value >> 48) & 0xFF,
            (value >> 56) & 0xFF,
        ]))

    def add_object_id(self, value: str) -> None:
        self.add_bytes(uuid.UUID(value).bytes)

    def add_len_bytes(self, text: str) -> None:
        encoded = text.encode("utf-8")
        self.add_u32(len(encoded))
        if encoded:
            self.add_bytes(encoded)

    def finish(self) -> int:
        return 1 if self._hash == 0 else self._hash


def bits_to_f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def canonical_f32_bits(value: float) -> int | None:
    if not math.isfinite(value):
        return None
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    if value == 0.0 and (bits >> 31) == 1:
        return None
    return bits


def execution_fingerprint(variable: dict) -> int:
    variable_id = uuid.UUID(variable["variableId"])
    kind = int(variable["kind"])
    scope = int(variable["scopePolicy"])
    writers = int(variable["writers"])
    if variable_id.int == 0 or kind > 1 or scope != 1 or writers < 1 or writers > 3:
        raise SystemExit("invalid execution identity for variable %s" % variable_id)
    min_bits = int(variable["rangeMinBits"])
    max_bits = int(variable["rangeMaxBits"])
    default_bits = int(variable["defaultBits"])
    range_min = bits_to_f32(min_bits)
    range_max = bits_to_f32(max_bits)
    default_value = bits_to_f32(default_bits)
    if (
        canonical_f32_bits(range_min) != min_bits
        or canonical_f32_bits(range_max) != max_bits
        or canonical_f32_bits(default_value) != default_bits
        or range_min < 0
        or range_min > range_max
        or default_value < range_min
        or default_value > range_max
    ):
        raise SystemExit("non-canonical range for variable %s" % variable_id)

    labels = list(variable.get("labels") or [])
    anchors = sorted(int(label["anchor"]) for label in labels)
    if kind == 1:
        anchors = []
    else:
        bound = 16777216
        for index, anchor in enumerate(anchors):
            if anchor < -bound or anchor > bound:
                raise SystemExit("anchor out of exact-int range on %s" % variable_id)
            if float(anchor) < range_min or float(anchor) > range_max:
                raise SystemExit("anchor outside range on %s" % variable_id)
            if index != 0 and anchor <= anchors[index - 1]:
                raise SystemExit("unsorted or duplicate anchors on %s" % variable_id)

    hasher = Hasher(EXECUTION_DOMAIN)
    hasher.add_u8(1)
    hasher.add_bytes(variable_id.bytes)
    hasher.add_u8(kind)
    hasher.add_u8(scope)
    hasher.add_u8(writers)
    hasher.add_u32(min_bits)
    hasher.add_u32(max_bits)
    hasher.add_u32(default_bits)
    hasher.add_u32(len(anchors))
    for anchor in anchors:
        hasher.add_u32(anchor & 0xFFFFFFFF)
    return hasher.finish()


def catalog_fingerprint(variable: dict) -> int:
    hasher = Hasher(CATALOG_DOMAIN)
    hasher.add_object_id(variable["variableId"])
    hasher.add_len_bytes(str(variable.get("codeName") or ""))
    hasher.add_len_bytes(str(variable.get("displayName") or ""))
    hasher.add_len_bytes(str(variable.get("description") or ""))
    hasher.add_len_bytes(str(variable.get("unit") or ""))
    hasher.add_u32(int(variable.get("precision") or 0) & 0xFFFFFFFF)
    hasher.add_u8(int(variable.get("displayScale") or 0))
    hasher.add_u8(int(variable.get("deprecated") or 0))
    hasher.add_u16(0)
    hasher.add_object_id(variable.get("replacementId") or str(uuid.UUID(int=0)))
    labels = list(variable.get("labels") or [])
    hasher.add_u32(len(labels))
    for label in labels:
        hasher.add_u32(int(label["anchor"]) & 0xFFFFFFFF)
        hasher.add_object_id(label["labelId"])
        hasher.add_len_bytes(str(label.get("name") or ""))
    hasher.add_u64(int(variable.get("ownerInitPackId") or 0))
    return hasher.finish()


def load_variables(path: str) -> list[dict]:
    with open(path, "r", encoding="utf-8") as handle:
        artifact = json.load(handle)
    return load_variables_from_artifact(artifact)


def label_symbol(variable: dict, label: dict) -> str:
    name = re.sub(r"[^A-Za-z0-9]+", "_", label.get("name") or "Unnamed").strip("_") or "Unnamed"
    return "Label_%s_%s_%s" % (variable["codeName"], name, uuid.UUID(label["labelId"]).hex)


def catalog_hashes(variables: list[dict]) -> tuple[int, int]:
    execution = Hasher(EXECUTION_CATALOG_DOMAIN)
    sdk = Hasher(SDK_CATALOG_DOMAIN)
    for variable in variables:
        execution.add_object_id(variable["variableId"])
        execution.add_u64(variable["_executionFingerprint"])
        sdk.add_u64(variable["_catalogFingerprint"])
    return execution.finish(), sdk.finish()


def emit(variables: list[dict], execution_catalog: int, sdk_catalog: int) -> str:
    lines = [
        "// generated, do not edit.",
        "// Generated by gen_variable_catalog.py from",
        "// HarkenEngine/Source/Tooling/VariableCatalog/variable_catalog.json.",
        "// Regenerate: py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py",
        "#ifndef HARKEN_API_GENERATED_HARKEN_VARIABLES_G_H",
        "#define HARKEN_API_GENERATED_HARKEN_VARIABLES_G_H",
        "",
        "#include <cstdint>",
        "#include <harken/Harken.h>",
        "",
        "namespace harken {",
        "namespace generated {",
        "",
        "inline constexpr std::uint64_t kVariableExecutionCatalogFingerprint = %sull;"
        % execution_catalog,
        "inline constexpr std::uint64_t kVariableCatalogFingerprint = %sull;"
        % sdk_catalog,
    ]
    for variable in variables:
        value = uuid.UUID(variable["variableId"]).int
        lines.append(
            "inline constexpr VnVariableId kVariable_%s = {0x%016xull, 0x%016xull};"
            % (symbol_name(variable["codeName"], "V_"), value >> 64, value & 0xFFFFFFFFFFFFFFFF)
        )
        for label in variable.get("labels") or []:
            identity = uuid.UUID(label["labelId"]).int
            lines.append("inline constexpr VnObjectId k%s = {0x%016xull, 0x%016xull};"
                         % (label_symbol(variable, label), identity >> 64, identity & 0xFFFFFFFFFFFFFFFF))
    lines.extend([
        "",
        "} // namespace generated",
        "} // namespace harken",
        "",
        "#endif",
        "",
    ])
    return "\n".join(lines)


def emit_header_comment(source: str, regenerate: str) -> list[str]:
    def one_line(text: str) -> str:
        return "".join(" " if ord(ch) < 32 else ch for ch in text.replace("\\", "/"))
    return [
        "// generated, do not edit.",
        "// Generated by gen_variable_catalog.py from",
        "// %s." % one_line(source),
        "// Regenerate: %s" % one_line(regenerate),
    ]


def emit_cs(variables: list[dict], execution_catalog: int, sdk_catalog: int,
            source: str = "HarkenEngine/Source/Tooling/VariableCatalog/variable_catalog.json") -> str:
    lines = emit_header_comment(
        source,
        "py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py",
    ) + [
        "namespace Harken.Generated",
        "{",
        "    public static class HarkenVariables",
        "    {",
        "        public const ulong ExecutionCatalogFingerprint = %sUL;"
        % execution_catalog,
        "        public const ulong CatalogFingerprint = %sUL;" % sdk_catalog,
    ]
    for variable in variables:
        value = uuid.UUID(variable["variableId"]).int
        lines.append(
            "        public static readonly VnObjectId %s = new VnObjectId(0x%016xUL, 0x%016xUL);"
            % (symbol_name(variable["codeName"], "V_"), value >> 64, value & 0xFFFFFFFFFFFFFFFF)
        )
        for label in variable.get("labels") or []:
            identity = uuid.UUID(label["labelId"]).int
            lines.append("        public static readonly VnObjectId %s = new VnObjectId(0x%016xUL, 0x%016xUL);"
                         % (label_symbol(variable, label), identity >> 64, identity & 0xFFFFFFFFFFFFFFFF))
    lines.extend([
        "    }",
        "}",
        "",
    ])
    return "\n".join(lines)


def load_variables_from_artifact(artifact: dict) -> list[dict]:
    if not isinstance(artifact, dict) or not isinstance(artifact.get("variables"), list):
        raise SystemExit("VariableContractArtifactV1 requires a variables array")
    variables = list(artifact["variables"])
    variables.sort(key=lambda row: uuid.UUID(row["variableId"]).int)
    seen_ids = set()
    seen_names = set()
    for variable in variables:
        variable_id = uuid.UUID(variable["variableId"])
        if variable_id in seen_ids:
            raise SystemExit("duplicate variableId %s" % variable_id)
        seen_ids.add(variable_id)
        code_name = str(variable.get("codeName") or "")
        if not CODE_NAME_RE.fullmatch(code_name) or len(code_name) > 128:
            raise SystemExit("codeName %r is not [A-Za-z][A-Za-z0-9_]*" % code_name)
        if code_name in seen_names:
            raise SystemExit("duplicate codeName %s" % code_name)
        seen_names.add(code_name)
        labels = list(variable.get("labels") or [])
        labels.sort(key=lambda label: int(label["anchor"]))
        identities, anchors, names = set(), set(), set()
        for label in labels:
            if type(label["anchor"]) is not int:
                raise SystemExit("label anchor must be an integer on variable %s" % variable_id)
            identity = uuid.UUID(label["labelId"])
            anchor = int(label["anchor"])
            name = str(label.get("name") or "")
            if (identity.int == 0 or identity in identities or anchor in anchors
                    or anchor < 0 or anchor > 16777216
                    or anchor < bits_to_f32(int(variable["rangeMinBits"]))
                    or anchor > bits_to_f32(int(variable["rangeMaxBits"]))
                    or name != name.strip() or "\0" in name or (name and name in names)):
                raise SystemExit("invalid label on variable %s" % variable_id)
            identities.add(identity)
            anchors.add(anchor)
            if name:
                names.add(name)
        variable["labels"] = labels
        variable["_executionFingerprint"] = execution_fingerprint(variable)
        variable["_catalogFingerprint"] = catalog_fingerprint(variable)
    return variables


def emit_actions(actions: list[dict], language: str) -> str:
    if language == "cs":
        lines = ["namespace Harken.Generated", "{", "    public static class HarkenActions", "    {"]
    else:
        lines = ['#pragma once', '#include <harken/Harken.h>', '', 'namespace harken::actions {']
    for action in actions:
        name = symbol_name(action["name"], "A_")
        value = uuid.UUID(action["actionStableId"]).int
        if language == "cs":
            lines.append("        public static readonly VnObjectId %s = new VnObjectId(0x%016xUL, 0x%016xUL);" %
                         (name, value >> 64, value & 0xFFFFFFFFFFFFFFFF))
        else:
            lines.append("inline constexpr ::VnObjectId %s{0x%016xull, 0x%016xull};" %
                         (name, value >> 64, value & 0xFFFFFFFFFFFFFFFF))
    lines.extend(["    }", "}", ""] if language == "cs" else ["}", ""])
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="gen_variable_catalog.py",
        description=(
            "Generate or verify Native/Unity Action and Variable GUID symbols."
        ),
        allow_abbrev=False,
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the checked-in products match the artifact without rewriting them",
    )
    parser.add_argument(
        "--json",
        help="VariableContractArtifactV1 JSON (defaults to the repository sample)",
    )
    parser.add_argument(
        "--publication",
        help="retired legacy manifest input; companion symbol generation is unavailable",
    )
    parser.add_argument(
        "--csharp",
        help="write only the Unity C# catalog to this path",
    )
    parser.add_argument("--action-csharp", help="write Action C# symbols to this path")
    parser.add_argument("--native", help="write Native Variable symbols to this path")
    parser.add_argument("--action-native", help="write Native Action symbols to this path")
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args(sys.argv[1:])
    if args.publication:
        raise SystemExit("Legacy manifest symbol generation is retired. Re-export with the current Studio. "
                         "Companion symbol generation is unavailable.")
    source = args.json or JSON_PATH
    variables = load_variables(source)
    execution_catalog, sdk_catalog = catalog_hashes(variables)
    try:
        relative_source = os.path.relpath(source, REPO_ROOT) if os.path.isabs(source) else source
    except ValueError:
        relative_source = source
    selected = any((args.csharp, args.action_csharp, args.native, args.action_native))
    actions = []
    candidates = [
        (args.native, OUT_PATH, emit(variables, execution_catalog, sdk_catalog)),
        (args.csharp, CS_OUT_PATH, emit_cs(variables, execution_catalog, sdk_catalog, relative_source)),
        (args.action_native, OUT_PATH.replace("Variables", "Actions"), emit_actions(actions, "native")),
        (args.action_csharp, CS_OUT_PATH.replace("Variables", "Actions"), emit_actions(actions, "cs")),
    ]
    outputs = [(os.path.abspath(path or default), content) for path, default, content in candidates
               if path or not selected]
    if args.check:
        mismatched = False
        for path, content in outputs:
            try:
                with open(path, "r", encoding="utf-8", newline="") as handle:
                    existing = handle.read()
            except OSError:
                existing = None
            if existing != content:
                print("MISMATCH: %s" % path)
                mismatched = True
            else:
                print("ok: %s" % path)
        return 1 if mismatched else 0
    for path, content in outputs:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
        print("wrote: %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
