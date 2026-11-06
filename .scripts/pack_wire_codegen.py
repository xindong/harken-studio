from __future__ import annotations

import argparse
import copy
import json
import math
import re
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = Path("HarkenSchema/Pack/current/PackManifestSchema.json")
BINDINGS = Path(".scripts/pack-wire-bindings.json")
OUTPUTS = {
    "engine": Path("HarkenEngine/Source/Runtime/media/include/harken/media/generated/harken_pack_wire.g.h"),
    "studio": Path("HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/generated/harken_pack_wire.g.h"),
    "engine_constants": Path("HarkenEngine/Source/Runtime/media/include/harken/media/generated/harken_pack_wire_constants.g.h"),
    "studio_constants": Path("HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/generated/harken_pack_wire_constants.g.h"),
}
NAMESPACES = {"engine": "harken", "studio": "harkenstudiobuild"}
SCALAR_BYTES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "i32": 4, "f32": 4, "id": 16}
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
CPP_TYPE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*\Z")
BYTE_ACCESS = re.compile(r"\b(?:StoreLE\w*|LoadLE\w*|StoreObjectId|LoadObjectId|Append(?:U8|U16|U32|U64|F32|ObjectId))\s*\(")
BODY_BYTE_ACCESS = re.compile(r"(?:\.|->)\s*(?:U8|U16|U32|U64|F32|ObjectId)\s*\(")
SOURCE_SCANS = {
    'HarkenEngine/Source/Runtime/media/include/harken/media/PackFormat.h': None,
    'HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/PackFormat.h': None,
    'HarkenEngine/Source/Runtime/media/include/harken/media/PackFormatV2.h': None,
    'HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/PackFormatV2.h': None,
    'HarkenEngine/Source/Runtime/media/include/harken/media/CanonicalBusSnapshot.h': None,
    'HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/BusSnapshot.h': None,
    'HarkenEngine/Source/Runtime/media/include/harken/media/PackVariableNameCatalog.h': None,
    'HarkenEngine/Source/Runtime/media/src/PackLogicImage.cpp': None,
    'HarkenStudio/Source/Tooling/PackBuild/src/PackImage.cpp': None,
    'HarkenEngine/Source/Runtime/media/src/PackTable.cpp': None,
    'HarkenEngine/Source/Runtime/api/src/HarkenApi.cpp': None,
    'HarkenEngine/Source/Runtime/core/src/PackComposition.cpp': None,
    'HarkenStudio/Source/Tooling/Harkenizer/Core/CanonicalPackSetExport.cpp': None,
    'HarkenStudio/Source/Tooling/Harkenizer/ResourceCompiler/ResourceCompiler.cpp': None,
    'HarkenStudio/Source/Tooling/Harkenizer/ResourceCompiler/Cache.h': None,
    'HarkenStudio/Source/RuntimeBridge/PackagingBridge.cpp': None,
    'HarkenEngine/Source/Runtime/core/src/CurrentWorldCodec.cpp': None,
    'HarkenStudio/Source/Tooling/PackBuild/src/WorldCodec.cpp': None,
}

SOURCE_EXEMPTIONS = {
    'HarkenEngine/Source/Runtime/media/include/harken/media/PackFormat.h': {
        'StoreLE16': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreLE32': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreLE64': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE16': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE32': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE64': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreObjectId': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadObjectId': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
    },
    'HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/PackFormat.h': {
        'StoreLE16': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreLE32': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreLE64': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE16': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE32': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadLE64': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'StoreObjectId': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
        'LoadObjectId': 'Shared scalar little-endian/ObjectId primitive; it describes a scalar representation rather than a Pack record.',
    },
    'HarkenEngine/Source/Runtime/media/include/harken/media/PackFormatV2.h': {
        'StoreLEF32': 'Shared scalar float/bit conversion primitive used by generated codecs.',
        'LoadLEF32': 'Shared scalar float/bit conversion primitive used by generated codecs.',
    },
    'HarkenStudio/Source/Tooling/PackBuild/include/harken_studio_build/PackFormatV2.h': {
        'StoreLEF32': 'Shared scalar float/bit conversion primitive used by generated codecs.',
        'LoadLEF32': 'Shared scalar float/bit conversion primitive used by generated codecs.',
    },
    'HarkenEngine/Source/Runtime/core/src/PackComposition.cpp': {
        'ProviderClosureFingerprint': 'Hash-only (pack stable ID, content revision) canonicalization; these 16-byte scratch rows are never emitted into a Pack payload.',
    },
    'HarkenStudio/Source/Tooling/Harkenizer/ResourceCompiler/ResourceCompiler.cpp': {
        'BuildStreamBytes': 'Independent .stream writer, explicitly excluded from the .bytes contract. Includes stream chunk checksums and PCM channel planes.',
    },
    'HarkenEngine/Source/Runtime/core/src/CurrentWorldCodec.cpp': {
        'AppendU32': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendU64': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendF32': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendObjectId': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'EncodeSignalEndpoint': 'V20 Trace/preview signal endpoint extension. Canonical Pack body admission accepts shipping V18 only.',
        'DecodeSignalEndpoint': 'V20 Trace/preview signal endpoint extension. Canonical Pack body admission accepts shipping V18 only.',
        'PackBodyWriteTrace::Segment': 'V19 Trace-only segment marker and bounds extension; output is null for shipping V18.',
        'EncodeCurrentWorldBodyV20': 'V20 Trace writer appends signal tables after generated shipping body; never used by canonical .bytes export.',
    },
    'HarkenStudio/Source/Tooling/PackBuild/src/WorldCodec.cpp': {
        'AppendU32': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendU64': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendF32': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'AppendObjectId': 'Scalar primitive retained exclusively for V19/V20 Trace or preview extensions after generated shipping-body traversal. Do not exempt callers generically.',
        'EncodeSignalEndpoint': 'V20 Trace/preview signal endpoint extension. Canonical Pack body admission accepts shipping V18 only.',
        'DecodeSignalEndpoint': 'V20 Trace/preview signal endpoint extension. Canonical Pack body admission accepts shipping V18 only.',
        'EncodeCurrentWorldPreviewBody': 'V20 preview writer appends signal tables; shipping-vintage branch immediately delegates to the generated shipping writer.',
    },
}

SOURCE_BLOCK_EXEMPTIONS = {
    'HarkenEngine/Source/Runtime/core/src/CurrentWorldCodec.cpp': {
        "worldTraceVintage == kCurrentWorldBodyVintageV20": "Trace/preview V20 signal extension; Pack accepts only shipping V18.",
        "state.bodyVintage == kCurrentWorldBodyVintageV19": "Trace V19 segment extension; Pack accepts only shipping V18.",
    },
    'HarkenStudio/Source/Tooling/PackBuild/src/WorldCodec.cpp': {
        "worldTraceVintage == kCurrentWorldBodyVintageV20": "Trace/preview V20 signal extension; Pack accepts only shipping V18.",
    },
}

CONTENT_REVISION_SOURCE = "HarkenStudio/Source/Tooling/Harkenizer/Core/CanonicalPackSetExport.cpp"


def pascal(name: str) -> str:
    return name[0].upper() + name[1:]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def length_value(value: object, constants: dict) -> int:
    if isinstance(value, str):
        require(value in constants, f"unknown length constant: {value}")
        value = constants[value]
    require(integer(value) and value > 0, f"invalid fixed length: {value!r}")
    return value


def element_spec(field: dict) -> dict:
    element = field["of"]
    return {"type": element} if isinstance(element, str) else element


def _validate_fixed_layout(layout: dict) -> None:
    require(isinstance(layout, dict), "binaryLayout must be an object")
    require(set(layout) <= {"byteOrder", "constants", "kinds", "retiredKinds", "records"}, "unknown binaryLayout property")
    require(layout.get("byteOrder") == "little", "binaryLayout.byteOrder must be little")
    constants = layout.get("constants", {})
    records = layout.get("records")
    require(isinstance(constants, dict), "binaryLayout.constants must be an object")
    require(isinstance(records, dict) and bool(records), "binaryLayout.records must be a nonempty object")
    for name, value in constants.items():
        require(isinstance(name, str) and bool(IDENTIFIER.fullmatch(name)), f"invalid constant name: {name!r}")
        require(integer(value) and 0 <= value <= 0xFFFFFFFFFFFFFFFF, f"invalid constant value: {name}")

    references = {name: [] for name in records}

    def validate_type(field: dict, context: str, owner: str) -> None:
        kind = field.get("type")
        require(isinstance(kind, str), f"{context}: missing field type")
        if kind in SCALAR_BYTES:
            require("length" not in field and "record" not in field and "of" not in field, f"{context}: scalar has composite properties")
        elif kind == "bytes":
            require("record" not in field and "of" not in field, f"{context}: bytes has composite properties")
            length_value(field.get("length"), constants)
        elif kind == "record":
            reference = field.get("record")
            require(isinstance(reference, str) and reference in records, f"{context}: unknown record reference {reference!r}")
            require("length" not in field and "of" not in field, f"{context}: record has array properties")
            references[owner].append(reference)
        elif kind == "array":
            require("record" not in field, f"{context}: use of for array elements")
            length_value(field.get("length"), constants)
            require("of" in field, f"{context}: missing array element type")
            element = element_spec(field)
            require(isinstance(element, dict), f"{context}: invalid array element")
            require(set(element) <= {"type", "record"}, f"{context}: invalid array element properties")
            require(isinstance(element.get("type"), str) and (element["type"] in SCALAR_BYTES or element["type"] == "record"), f"{context}: invalid array element type")
            validate_type(element, context + "[]", owner)
        else:
            raise ValueError(f"{context}: unsupported field type {kind!r}")

    for name, record in records.items():
        require(isinstance(name, str) and bool(IDENTIFIER.fullmatch(name)), f"invalid record name: {name!r}")
        require(isinstance(record, dict) and set(record) <= {"mode", "fields"}, f"{name}: invalid record properties")
        require(record.get("mode") == "fixed", f"{name}: only fixed records are supported")
        fields = record.get("fields")
        require(isinstance(fields, list) and bool(fields), f"{name}: fields must be a nonempty array")
        seen = {}
        for field in fields:
            require(isinstance(field, dict), f"{name}: field must be an object")
            require(set(field) <= {"name", "type", "length", "record", "of", "reserved", "const", "live"}, f"{name}: unknown field property")
            field_name = field.get("name")
            context = f"{name}.{field_name}"
            require(isinstance(field_name, str) and bool(IDENTIFIER.fullmatch(field_name)), f"{name}: invalid field name")
            require(field_name not in seen, f"{context}: duplicate field name")
            validate_type(field, context, name)
            require("reserved" not in field or field["reserved"] is True, f"{context}: reserved must be true")
            require(not (field.get("reserved") and "const" in field), f"{context}: reserved and const are exclusive")
            if "live" in field:
                live = field["live"]
                require(field["type"] == "array" and not field.get("reserved") and "const" not in field, f"{context}: live requires a bound array")
                if isinstance(live, dict):
                    require(set(live) == {"count", "limit", "subtract"}, f"{context}: invalid derived live count")
                    require(integer(live["limit"]) and live["limit"] > 0, f"{context}: live limit must be positive")
                    require(integer(live["subtract"]) and 0 <= live["subtract"] <= live["limit"], f"{context}: live subtract must be between zero and the limit")
                    require(live["limit"] - live["subtract"] <= length_value(field["length"], constants), f"{context}: live count exceeds fixed length")
                    count_name = live["count"]
                else:
                    count_name = live
                require(isinstance(count_name, str) and count_name in seen, f"{context}: live must name an earlier count field")
                count = seen[count_name]
                require(count["type"] in {"u8", "u16", "u32", "u64"} and not count.get("reserved") and "const" not in count, f"{context}: live must name an unsigned bound count")
            if "const" in field:
                value = field["const"]
                kind = field["type"]
                if kind == "bytes":
                    require(isinstance(value, str) and value.isascii(), f"{context}: bytes const must be ASCII text")
                    require(len(value) == length_value(field["length"], constants), f"{context}: bytes const length mismatch")
                elif kind == "f32":
                    require(isinstance(value, (int, float)) and not isinstance(value, bool) and (not isinstance(value, float) or math.isfinite(value)), f"{context}: invalid float const")
                    try:
                        struct.pack("<f", value)
                    except (OverflowError, struct.error):
                        raise ValueError(f"{context}: float const is outside f32") from None
                else:
                    require(kind in {"u8", "u16", "u32", "u64", "i32"}, f"{context}: const requires a scalar or bytes")
                    minimum = -(1 << 31) if kind == "i32" else 0
                    maximum = (1 << (SCALAR_BYTES[kind] * 8 - (kind == "i32"))) - 1
                    require(integer(value) and minimum <= value <= maximum, f"{context}: const is outside {kind}")
            seen[field_name] = field
        require(len({pascal(field) for field in seen}) == len(seen), f"{name}: field names collide in generated identifiers")

    active = set()
    done = set()

    def visit(name: str) -> None:
        require(name not in active, f"recursive record reference: {name}")
        if name in done:
            return
        active.add(name)
        for reference in references[name]:
            visit(reference)
        active.remove(name)
        done.add(name)

    for name in records:
        visit(name)
    require(len({pascal(name) for name in records}) == len(records), "record names collide in generated identifiers")
    require(len({pascal(name) for name in constants}) == len(constants), "constant names collide in generated identifiers")
    names = set()
    ids = set()
    for retired, key in ((False, "kinds"), (True, "retiredKinds")):
        kinds = layout.get(key, [])
        require(isinstance(kinds, list), f"binaryLayout.{key} must be an array")
        for kind in kinds:
            expected = {"name", "id"} if retired else {"name", "id", "vintage", "record"}
            require(isinstance(kind, dict) and set(kind) == expected, f"invalid {key} entry")
            name = kind["name"]
            require(isinstance(name, str) and bool(IDENTIFIER.fullmatch(name)), f"invalid kind name: {name!r}")
            require(pascal(name) not in names, f"duplicate kind name: {name}")
            require(integer(kind["id"]) and 0 < kind["id"] <= 0xFFFFFFFF and kind["id"] not in ids, f"invalid or duplicate kind id: {name}")
            if not retired:
                require(integer(kind["vintage"]) and 0 <= kind["vintage"] <= 0xFFFFFFFF, f"invalid kind vintage: {name}")
                require(isinstance(kind["record"], str) and kind["record"] in records, f"unknown kind record: {name}")
            names.add(pascal(name))
            ids.add(kind["id"])
    symbols = ["kPack" + pascal(name) for name in constants]
    for name, record in records.items():
        symbols.append("kPack" + pascal(name) + "Bytes")
        symbols.extend("kPack" + pascal(name) + pascal(field["name"]) + "Offset" for field in record["fields"])
        symbols.extend("kPack" + pascal(name) + pascal(field["name"]) for field in record["fields"]
                       if record["mode"] == "fixed" and field["type"] == "bytes" and "const" in field)
    for kind in layout.get("kinds", []) + layout.get("retiredKinds", []):
        symbols.append("kPackEntryKind" + pascal(kind["name"]))
        if "vintage" in kind:
            symbols.append("kPack" + pascal(kind["name"]) + "Vintage")
    require(len(symbols) == len(set(symbols)), "names collide in generated constants")


def _fixed_record_layouts(layout: dict) -> dict[str, dict]:
    _validate_fixed_layout(layout)
    records = layout["records"]
    constants = layout.get("constants", {})
    result = {}

    def size_of(field: dict) -> int:
        kind = field["type"]
        if kind in SCALAR_BYTES:
            return SCALAR_BYTES[kind]
        if kind == "bytes":
            return length_value(field["length"], constants)
        if kind == "record":
            return record_size(field["record"])
        return length_value(field["length"], constants) * size_of(element_spec(field))

    def record_size(name: str) -> int:
        if name not in result:
            offset = 0
            fields = []
            for field in records[name]["fields"]:
                size = size_of(field)
                fields.append((field, offset, size))
                offset += size
                require(offset <= 0xFFFFFFFFFFFFFFFF, f"record is too large: {name}")
            result[name] = {"bytes": offset, "fields": fields}
        return result[name]["bytes"]

    for name in sorted(records):
        record_size(name)
    return result


def _validate_fixed_bindings(layout: dict, bindings: dict) -> None:
    require(isinstance(bindings, dict) and set(bindings) <= {"records", "members", "setters"}, "invalid bindings properties")
    records = bindings.get("records")
    require(isinstance(records, dict) and set(records) == set(layout["records"]), "bindings.records must match the contract records")
    for name, targets in records.items():
        require(isinstance(targets, dict) and set(targets) == set(NAMESPACES), f"{name}: bindings must name engine and studio types")
        for target, value in targets.items():
            require(isinstance(value, str) and bool(CPP_TYPE.fullmatch(value)), f"{name}.{target}: invalid bound type")
    members = bindings.get("members", {})
    require(isinstance(members, dict), "bindings.members must be an object")
    fields = {f"{name}.{field['name']}": field for name, record in layout["records"].items() for field in record["fields"]}
    setters = bindings.get("setters", [])
    require(isinstance(setters, list) and all(isinstance(name, str) for name in setters), "bindings.setters must be an array of field names")
    require(len(set(setters)) == len(setters), "duplicate bound setter")
    for name in setters:
        require(name in fields and fields[name]["type"] in {"u8", "u16", "u32", "u64", "i32", "f32"} and not fields[name].get("reserved") and "const" not in fields[name], f"invalid bound setter: {name}")
    for name, value in members.items():
        require(name in fields and not fields[name].get("reserved") and "const" not in fields[name], f"invalid bound member: {name}")
        if isinstance(value, dict):
            require(set(value) == set(NAMESPACES), f"{name}: members must name engine and studio")
            values = value.values()
        else:
            values = [value]
        for member in values:
            require(isinstance(member, str) and (member == "" or bool(re.fullmatch(r"[A-Za-z_]\w*(?:\[\d+\])?(?:\.[A-Za-z_]\w*(?:\[\d+\])?)*", member))), f"{name}: invalid member expression")


def member_name(bindings: dict, record: str, field: str, target: str) -> str:
    value = bindings.get("members", {}).get(f"{record}.{field}", field)
    return value[target] if isinstance(value, dict) else value


def member_expression(bindings: dict, record: str, field: str, target: str, writing: bool) -> str:
    member = member_name(bindings, record, field, target)
    if not member:
        return "row" if writing else "(*out)"
    return ("row." if writing else "out->") + member


def scalar_write(kind: str, address: str, value: str) -> str:
    if kind == "id":
        return f"StoreObjectId({address}, {value});"
    if kind == "f32":
        return f"StoreLEF32({address}, static_cast<float>({value}));"
    if kind == "i32":
        return f"StoreLE32({address}, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>({value})));"
    if kind == "u8":
        return f"*({address}) = static_cast<std::uint8_t>({value});"
    bits = kind[1:]
    return f"StoreLE{bits}({address}, static_cast<std::uint{bits}_t>({value}));"


def scalar_read(kind: str, address: str) -> str:
    if kind == "id":
        return f"LoadObjectId({address})"
    if kind == "f32":
        return f"LoadLEF32({address})"
    if kind == "i32":
        return f"std::bit_cast<std::int32_t>(LoadLE32({address}))"
    if kind == "u8":
        return f"*({address})"
    return f"LoadLE{kind[1:]}({address})"


def const_value(field: dict) -> tuple[str, str]:
    kind = field["type"]
    value = field["const"]
    if kind == "f32":
        return "u32", f"0x{struct.unpack('<I', struct.pack('<f', value))[0]:08x}u"
    if kind == "i32":
        return kind, f"({value}LL)"
    return kind, f"{value}ULL"


def render_constants_header(layout: dict, target: str) -> str:
    validate_layout(layout)
    require(target in NAMESPACES, f"unknown generation target: {target}")
    lines = [
        "// Generated by .scripts/pack_wire_codegen.py. Do not edit.",
        "#pragma once", "", "#include <cstdint>",
        "", f"namespace {NAMESPACES[target]} {{", "",
    ]
    for name, value in sorted(layout.get("constants", {}).items()):
        lines.append(f"inline constexpr std::uint64_t kPack{pascal(name)} = {value}ULL;")
    lines += ["", "}", ""]
    return "\n".join(lines)


def _render_fixed_header(layout: dict, bindings: dict, target: str, kind_layout: dict) -> str:
    layouts = _fixed_record_layouts(layout)
    _validate_fixed_bindings(layout, bindings)
    require(target in NAMESPACES, f"unknown generation target: {target}")
    constants = layout.get("constants", {})
    lines = [
        "// Generated by .scripts/pack_wire_codegen.py. Do not edit.",
        "#pragma once", "", "#include <algorithm>", "#include <bit>", "#include <cstdint>",
        "#include <cstring>", "#include <iterator>", "#include <limits>", "#include <type_traits>",
        "", '#include "harken_pack_wire_constants.g.h"',
        "", f"namespace {NAMESPACES[target]} {{", "",
    ]
    kinds = sorted(kind_layout.get("kinds", []) + kind_layout.get("retiredKinds", []), key=lambda kind: kind["id"])
    for kind in kinds:
        name = pascal(kind["name"])
        lines.append(f"inline constexpr std::uint32_t kPackEntryKind{name} = {kind['id']}u;")
        if "vintage" in kind:
            lines.append(f"inline constexpr std::uint32_t kPack{name}Vintage = {kind['vintage']}u;")
    if kinds:
        lines.append("")
    lines += [
        "[[nodiscard]] inline bool PackEntryKindSupported(std::uint32_t kind) noexcept {",
        "    switch (kind) {",
    ]
    for kind in sorted(kind_layout.get("kinds", []), key=lambda kind: kind["id"]):
        lines.append(f"    case kPackEntryKind{pascal(kind['name'])}: return true;")
    lines += ["    default: return false;", "    }", "}", ""]
    lines += [
        "[[nodiscard]] inline constexpr std::uint32_t PackEntryVintage(std::uint32_t kind) noexcept {",
        "    switch (kind) {",
    ]
    for kind in sorted(kind_layout.get("kinds", []), key=lambda kind: kind["id"]):
        name = pascal(kind["name"])
        lines.append(f"    case kPackEntryKind{name}: return kPack{name}Vintage;")
    lines += ["    default: return 0;", "    }", "}", ""]
    vintage_parameter = " vintage" if kind_layout.get("kinds") else ""
    lines += [
        f"[[nodiscard]] inline bool VintageLegalForKind(std::uint32_t kind, std::uint32_t{vintage_parameter}) noexcept {{",
        "    switch (kind) {",
    ]
    for kind in sorted(kind_layout.get("kinds", []), key=lambda kind: kind["id"]):
        name = pascal(kind["name"])
        lines += [f"    case kPackEntryKind{name}: return vintage == kPack{name}Vintage;"]
    lines += ["    default: return false;", "    }", "}", ""]

    def fixed_value(field: dict, address: str, size: int, writing: bool, indent: str) -> list[str]:
        if field.get("reserved"):
            if writing:
                return [f"{indent}std::memset({address}, 0, {size});"]
            return [f"{indent}for (std::uint64_t i = 0; i < {size}; ++i) {{", f"{indent}    if (({address})[i] != 0) return false;", f"{indent}}}"]
        if field["type"] == "bytes":
            encoded = ", ".join(f"0x{value:02x}" for value in field["const"].encode("ascii"))
            result = [f"{indent}{{", f"{indent}    constexpr std::uint8_t value[] = {{{encoded}}};"]
            if writing:
                result.append(f"{indent}    std::memcpy({address}, value, {size});")
            else:
                result.append(f"{indent}    if (std::memcmp({address}, value, {size}) != 0) return false;")
            return result + [f"{indent}}}"]
        kind, value = const_value(field)
        if writing:
            return [indent + scalar_write(kind, address, value)]
        return [f"{indent}if ({scalar_read(kind, address)} != {value}) return false;"]

    def bound_value(field: dict, member: str, address: str, size: int, writing: bool, indent: str) -> list[str]:
        kind = field["type"]
        if kind in SCALAR_BYTES:
            if writing:
                return [indent + scalar_write(kind, address, member)]
            return [f"{indent}{member} = static_cast<std::remove_cvref_t<decltype({member})>>({scalar_read(kind, address)});"]
        if kind == "bytes":
            args = f"{address}, std::data({member})" if writing else f"std::data({member}), {address}"
            return [f"{indent}std::memcpy({args}, {size});"]
        name = pascal(field["record"])
        if writing:
            return [f"{indent}SerializePack{name}({member}, {address});"]
        return [f"{indent}if (!ParsePack{name}({address}, kPack{name}Bytes, &{member})) return false;"]

    for record, info in layouts.items():
        name = pascal(record)
        row_type = bindings["records"][record][target]
        lines.append(f"inline constexpr std::uint64_t kPack{name}Bytes = {info['bytes']}ULL;")
        for field, offset, _ in info["fields"]:
            lines.append(f"inline constexpr std::uint64_t kPack{name}{pascal(field['name'])}Offset = {offset}ULL;")
            if field["type"] == "bytes" and "const" in field:
                values = ", ".join(f"{value}u" for value in field["const"].encode("ascii"))
                lines.append(f"inline constexpr std::uint8_t kPack{name}{pascal(field['name'])}[] = {{{values}}};")
        lines.append("")
        for writing in (True, False):
            has_members = any(not field.get("reserved") and "const" not in field for field, _, _ in info["fields"])
            if writing:
                row_parameter = " row" if has_members else ""
                lines.append(f"inline void SerializePack{name}(const {row_type}&{row_parameter}, std::uint8_t* out) noexcept {{")
            else:
                out_parameter = " out" if has_members else ""
                lines += [f"[[nodiscard]] inline bool ParsePack{name}(const std::uint8_t* in, std::uint64_t size, {row_type}*{out_parameter}) noexcept {{", f"    if (size != kPack{name}Bytes) return false;"]
            for field, offset, size in info["fields"]:
                address = f"{'out' if writing else 'in'} + {offset}"
                if field.get("reserved") or "const" in field:
                    lines += fixed_value(field, address, size, writing, "    ")
                    continue
                member = member_expression(bindings, record, field["name"], target, writing)
                if field["type"] in {"array", "bytes"}:
                    count = length_value(field["length"], constants)
                    lines.append(f"    static_assert(std::size(std::remove_cvref_t<decltype({member})>{{}}) == {count});")
                if field["type"] != "array":
                    lines += bound_value(field, member, address, size, writing, "    ")
                    continue
                count = length_value(field["length"], constants)
                element = element_spec(field)
                stride = size // count
                live = field.get("live")
                if live:
                    count_field = live["count"] if isinstance(live, dict) else live
                    live_member = member_expression(bindings, record, count_field, target, writing)
                    if isinstance(live, dict):
                        live_count = field["name"] + "LiveCount"
                        lines.append(f"    const auto {live_count} = std::min<std::uint64_t>({live_member}, {live['limit']});")
                        condition = f"{live_count} > {live['subtract']} && i < {live_count} - {live['subtract']}"
                    else:
                        condition = f"i < {live_member}"
                lines.append(f"    for (std::uint64_t i = 0; i < {count}; ++i) {{")
                element_address = address + f" + i * {stride}"
                if live:
                    lines.append(f"        if ({condition}) {{")
                lines += bound_value(element, member + "[i]", element_address, stride, writing, "            " if live else "        ")
                if live:
                    lines.append("        } else {")
                    if writing:
                        lines.append(f"            std::memset({element_address}, 0, {stride});")
                    else:
                        lines += [f"            for (std::uint64_t byte = 0; byte < {stride}; ++byte) {{", f"                if (({element_address})[byte] != 0) return false;", "            }", f"            {member}[i] = {{}};"]
                    lines.append("        }")
                lines.append("    }")
            if not writing:
                lines.append("    return true;")
            lines += ["}", ""]
        for field, offset, _ in info["fields"]:
            if f"{record}.{field['name']}" not in bindings.get("setters", []):
                continue
            kind = field["type"]
            value_type = "float" if kind == "f32" else ("std::int32_t" if kind == "i32" else f"std::uint{kind[1:]}_t")
            lines += [
                f"inline void StorePack{name}{pascal(field['name'])}(std::uint8_t* out, {value_type} value) noexcept {{",
                "    " + scalar_write(kind, f"out + {offset}", "value"),
                "}", "",
            ]
    return "\n".join(lines + ["}", ""])


def split_layout(layout: dict) -> tuple[dict, dict]:
    require(isinstance(layout, dict), "binaryLayout must be an object")
    require(isinstance(layout.get("records"), dict), "binaryLayout.records must be an object")
    require(all(isinstance(record, dict) for record in layout["records"].values()), "records must be objects")
    fixed = copy.deepcopy(layout)
    streams = {name: record for name, record in layout["records"].items() if record.get("mode") == "stream"}
    fixed["records"] = {name: record for name, record in fixed["records"].items() if name not in streams}
    fixed["kinds"] = [kind for kind in fixed.get("kinds", []) if not isinstance(kind, dict) or kind.get("record") not in streams]
    return fixed, streams


def controls(record: dict) -> set[str]:
    result = set()
    for field in record["fields"]:
        if "count" in field:
            count = field["count"]
            result.add(count["count"] if isinstance(count, dict) else count)
        if "when" in field:
            result.add(field["when"]["field"])
        result.update(field.get("position", []))
    if "length" in record:
        result.update(record["length"].values())
    return result


def known_fields(layout: dict, name: str) -> dict[str, str]:
    record = layout["records"][name]
    names = {}
    if "context" in record:
        names.update(known_fields(layout, record["context"]))
    for field in record["fields"]:
        if field["type"] == "record":
            names.update(known_fields(layout, field["record"]))
        elif field["type"] != "rows" and not field.get("reserved") and "const" not in field:
            names[field["name"]] = field["type"]
    return names


def validate_layout(layout: dict) -> None:
    fixed, streams = split_layout(layout)
    _validate_fixed_layout(fixed)
    for name, record in streams.items():
        require(bool(IDENTIFIER.fullmatch(name)), f"invalid stream record name: {name}")
        require(set(record) <= {"mode", "fields", "context", "length"}, f"{name}: invalid stream properties")
        if "context" in record:
            require(isinstance(record["context"], str) and record["context"] in fixed["records"], f"{name}: context must name a fixed header")
        require(isinstance(record["fields"], list) and bool(record["fields"]), f"{name}: empty stream")
        names = set()
        for field in record["fields"]:
            require(isinstance(field, dict) and set(field) <= {"name", "type", "record", "count", "when", "position"}, f"{name}: invalid stream field")
            field_name = field.get("name")
            require(isinstance(field_name, str) and bool(IDENTIFIER.fullmatch(field_name)) and pascal(field_name) not in names, f"{name}: invalid or duplicate stream field name")
            names.add(pascal(field_name))
            require(field.get("type") in {"record", "rows"}, f"{name}.{field_name}: stream fields must be records or rows")
            require(isinstance(field.get("record"), str) and field["record"] in layout["records"], f"{name}.{field_name}: unknown record")
            require((field["type"] == "rows") == ("count" in field), f"{name}.{field_name}: rows require count")
            if "count" in field:
                count = field["count"]
                if isinstance(count, dict):
                    require(set(count) == {"count", "subtract"} and integer(count["subtract"]) and count["subtract"] >= 0, f"{name}.{field_name}: invalid count projection")
                    count = count["count"]
                require(isinstance(count, str) and bool(IDENTIFIER.fullmatch(count)), f"{name}.{field_name}: invalid count reference")
            if "when" in field:
                when = field["when"]
                require(isinstance(when, dict) and set(when) == {"field", "equals"}, f"{name}.{field_name}: invalid when")
                require(isinstance(when["field"], str) and bool(IDENTIFIER.fullmatch(when["field"])) and integer(when["equals"]) and when["equals"] >= 0, f"{name}.{field_name}: invalid discriminator")
            if "position" in field:
                position = field["position"]
                require(isinstance(position, list) and bool(position) and all(isinstance(value, str) and bool(IDENTIFIER.fullmatch(value)) for value in position), f"{name}.{field_name}: position must name header offsets")
        positions = [value for field in record["fields"] for value in field.get("position", [])]
        if positions or "length" in record:
            require(len(positions) == len(set(positions)), f"{name}: duplicate position target")
            require(all(field["record"] in fixed["records"] and "when" not in field for field in record["fields"]), f"{name}: positions require unconditional fixed record tables")
            counts = {field["count"]["count"] if isinstance(field["count"], dict) else field["count"] for field in record["fields"] if "count" in field}
            require(not (set(positions) & counts), f"{name}: position cannot replace a row count")
            if "length" in record:
                length = record["length"]
                require(isinstance(length, dict) and set(length) == {"field", "from"}, f"{name}: invalid stream length")
                require(isinstance(length["field"], str) and bool(IDENTIFIER.fullmatch(length["field"])) and length["field"] not in set(positions) | counts, f"{name}: length must name a separate unsigned field")
                require(isinstance(length["from"], str) and length["from"] in positions, f"{name}: length origin must name a position target")
    visiting, done = set(), set()
    def visit(name: str) -> None:
        require(name not in visiting, f"recursive stream: {name}")
        if name in done or name not in streams:
            return
        visiting.add(name)
        for field in streams[name]["fields"]:
            visit(field["record"])
        visiting.remove(name)
        done.add(name)
    for name in streams:
        visit(name)
    for name, record in streams.items():
        available = known_fields(layout, record["context"]) if "context" in record else {}
        for field in record["fields"]:
            for control in controls({"fields": [field]}):
                require(control in available, f"{name}.{field['name']}: control {control} is not provided by an earlier header")
                require(available[control] in {"u8", "u16", "u32", "u64"}, f"{name}.{field['name']}: control must be an unsigned scalar")
            if field["type"] == "record":
                available.update(known_fields(layout, field["record"]))
        if "length" in record:
            length_field = record["length"]["field"]
            require(available.get(length_field) in {"u8", "u16", "u32", "u64"}, f"{name}: length must be provided by an unsigned header field")
    probe = copy.deepcopy(layout)
    for name in streams:
        probe["records"][name] = {"mode": "fixed", "fields": [{"name": "placeholder", "type": "u8"}]}
    _validate_fixed_layout(probe)


def fixed_bindings(bindings: dict, fixed: dict) -> dict:
    return {
        "records": {name: value for name, value in bindings["records"].items() if name in fixed["records"]},
        "members": {name: value for name, value in bindings.get("members", {}).items() if name.split(".")[0] in fixed["records"]},
        "setters": [name for name in bindings.get("setters", []) if name.split(".")[0] in fixed["records"]],
    }


def validate_bindings(layout: dict, bindings: dict) -> None:
    require(isinstance(bindings, dict) and set(bindings) <= {"records", "members", "setters"}, "invalid bindings properties")
    require(isinstance(bindings.get("records"), dict) and isinstance(bindings.get("members", {}), dict), "invalid records or members bindings")
    for name, mapped in bindings.get("members", {}).items():
        require(isinstance(name, str) and "." in name and name.split(".")[0] in layout["records"], f"unknown binding owner: {name}")
        if isinstance(mapped, dict):
            require(set(mapped) == set(NAMESPACES), f"{name}: member binding must name both targets")
    for name in bindings.get("setters", []):
        require(isinstance(name, str) and "." in name and name.split(".")[0] in layout["records"] and layout["records"][name.split(".")[0]]["mode"] == "fixed", f"invalid setter owner: {name}")
    fixed, streams = split_layout(layout)
    _validate_fixed_bindings(fixed, fixed_bindings(bindings, fixed))
    require(set(bindings["records"]) == set(layout["records"]), "stream bindings must cover all records")
    for name, record in streams.items():
        targets = bindings["records"][name]
        require(set(targets) == set(NAMESPACES), f"{name}: missing target binding")
        for target in targets.values():
            require(isinstance(target, str) and bool(CPP_TYPE.fullmatch(target)), f"{name}: invalid bound type")
        names = controls(record)
        for key, value in bindings.get("members", {}).items():
            owner, member = key.split(".", 1)
            if owner != name:
                continue
            require(member in names, f"{key}: only stream controls have member bindings")
            values = value.values() if isinstance(value, dict) else [value]
            for mapped in values:
                require(isinstance(mapped, str) and bool(re.fullmatch(r"[A-Za-z_]\w*(?:\[\d+\])?(?:\.[A-Za-z_]\w*(?:\[\d+\])?)*", mapped)), f"{key}: invalid control binding")


def render_header(layout: dict, bindings: dict, target: str) -> str:
    validate_layout(layout)
    validate_bindings(layout, bindings)
    fixed, streams = split_layout(layout)
    header = _render_fixed_header(fixed, fixed_bindings(bindings, fixed), target, layout)
    lines = header.rstrip().splitlines()[:-1]
    for record in _fixed_record_layouts(fixed):
        name = pascal(record)
        row_type = bindings["records"][record][target]
        lines += [
            "template <class Buffer>",
            f"[[nodiscard]] bool AppendPack{name}(Buffer& buffer, const {row_type}& row) noexcept {{",
            f"    static_assert(noexcept(buffer.Take(kPack{name}Bytes)));",
            f"    auto* bytes = buffer.Take(kPack{name}Bytes);",
            "    if (!bytes) return false;",
            f"    SerializePack{name}(row, bytes);",
            "    return true;", "}", "",
            "template <class Buffer>",
            f"[[nodiscard]] bool ReadPack{name}(Buffer& buffer, {row_type}& row) noexcept {{",
            f"    static_assert(noexcept(buffer.Take(kPack{name}Bytes)));",
            f"    const auto* bytes = buffer.Take(kPack{name}Bytes);",
            f"    return bytes && ParsePack{name}(bytes, kPack{name}Bytes, &row);",
            "}", "",
            f"[[nodiscard]] inline bool MeasurePack{name}(const {row_type}&, std::uint64_t& size) noexcept {{",
            f"    if (size > UINT64_MAX - kPack{name}Bytes) return false;",
            f"    size += kPack{name}Bytes;", "    return true;", "}", "",
        ]
    ordered = []
    def order(name: str) -> None:
        if name in ordered:
            return
        for field in streams[name]["fields"]:
            if field["record"] in streams:
                order(field["record"])
        ordered.append(name)
    for name in sorted(streams):
        order(name)
    extents = {name: value["bytes"] for name, value in _fixed_record_layouts(fixed).items()}
    minimums = dict(extents)
    for record in ordered:
        fields = streams[record]["fields"]
        minimums[record] = sum(minimums[field["record"]] for field in fields
                              if field["type"] == "record" and "when" not in field)
        if all(field["type"] == "record" and "when" not in field and field["record"] in extents for field in fields):
            extents[record] = sum(extents[field["record"]] for field in fields)
            lines.append(f"inline constexpr std::uint64_t kPack{pascal(record)}Bytes = {extents[record]}ULL;")
    lines.append("")
    for record in ordered:
        name = pascal(record)
        definition = streams[record]
        positions = [value for field in definition["fields"] for value in field.get("position", [])]
        if positions:
            row_type = bindings["records"][record][target]
            wire_types = known_fields(layout, record)
            lines += [f"[[nodiscard]] inline bool LayoutPack{name}({row_type}& value, std::uint64_t& size) noexcept {{", "    size = 0;"]
            for field in definition["fields"]:
                for position in field.get("position", []):
                    member = member_name(bindings, record, position, target)
                    maximum = (1 << (SCALAR_BYTES[wire_types[position]] * 8)) - 1
                    if maximum < (1 << 64) - 1:
                        lines.append(f"    if (size > {maximum}ULL) return false;")
                    lines += [f"    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.{member})>>::max()) return false;", f"    value.{member} = static_cast<std::remove_cvref_t<decltype(value.{member})>>(size);"]
                extent = extents[field["record"]]
                if field["type"] == "rows":
                    count = field["count"]
                    count_name = count["count"] if isinstance(count, dict) else count
                    member = member_name(bindings, record, count_name, target)
                    expression = f"static_cast<std::uint64_t>(value.{member})"
                    if isinstance(count, dict) and count["subtract"]:
                        expression = f"value.{member} > {count['subtract']} ? {expression} - {count['subtract']} : 0"
                    lines += [f"    const std::uint64_t {field['name']}Count = {expression};", f"    if ({field['name']}Count > (UINT64_MAX - size) / {extent}ULL) return false;", f"    size += {field['name']}Count * {extent}ULL;"]
                else:
                    lines += [f"    if (size > UINT64_MAX - {extent}ULL) return false;", f"    size += {extent}ULL;"]
            metadata = list(positions)
            if "length" in definition:
                length = definition["length"]
                member = member_name(bindings, record, length["field"], target)
                origin = member_name(bindings, record, length["from"], target)
                lines.append(f"    const auto length = size - value.{origin};")
                maximum = (1 << (SCALAR_BYTES[wire_types[length["field"]]] * 8)) - 1
                if maximum < (1 << 64) - 1:
                    lines.append(f"    if (length > {maximum}ULL) return false;")
                lines += [f"    if (length > std::numeric_limits<std::remove_cvref_t<decltype(value.{member})>>::max()) return false;", f"    value.{member} = static_cast<std::remove_cvref_t<decltype(value.{member})>>(length);"]
                metadata.append(length["field"])
            lines += ["    return true;", "}", "", f"[[nodiscard]] inline bool ValidatePack{name}Layout(const {row_type}& value, std::uint64_t size) noexcept {{", "    auto expected = value;", "    std::uint64_t expectedSize = 0;", f"    if (!LayoutPack{name}(expected, expectedSize) || expectedSize != size) return false;"]
            comparisons = [f"value.{member_name(bindings, record, field, target)} == expected.{member_name(bindings, record, field, target)}" for field in metadata]
            lines += ["    return " + " &&\n           ".join(comparisons) + ";", "}", ""]
        for direction in ("Append", "Read", "Measure"):
            measure = direction == "Measure"
            lines += ["template <class Visitor>" if measure else "template <class Buffer, class Visitor>", f"[[nodiscard]] bool MeasurePack{name}(Visitor& visitor, std::uint64_t& size) noexcept {{" if measure else f"[[nodiscard]] bool {direction}Pack{name}(Buffer& buffer, Visitor& visitor) noexcept {{"]
            if controls(definition):
                lines += ["    static_assert(noexcept(visitor.Value()));", "    auto& value = visitor.Value();", f"    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, {bindings['records'][record][target]}>);"]
            if direction == "Read" and positions:
                lines += ["    static_assert(noexcept(buffer.Remaining()));", "    const auto initialRemaining = buffer.Remaining();"]
            for field in definition["fields"]:
                label = pascal(field["name"])
                codec = pascal(field["record"])
                indent = "    "
                if "when" in field:
                    when = field["when"]
                    member = member_name(bindings, record, when["field"], target)
                    lines.append(f"    if (static_cast<std::uint64_t>(value.{member}) == {when['equals']}) {{")
                    indent += "    "
                if field["type"] == "rows":
                    count = field["count"]
                    subtract = count.get("subtract", 0) if isinstance(count, dict) else 0
                    count_name = count["count"] if isinstance(count, dict) else count
                    member = member_name(bindings, record, count_name, target)
                    count_var = field["name"] + "Count"
                    expression = f"static_cast<std::uint64_t>(value.{member})"
                    if subtract:
                        expression = f"value.{member} > {subtract} ? {expression} - {subtract} : 0"
                    if measure and field["record"] in extents:
                        size = extents[field["record"]]
                        lines += [f"{indent}const std::uint64_t {count_var} = {expression};", f"{indent}if ({count_var} > (UINT64_MAX - size) / {size}ULL) return false;", f"{indent}size += {count_var} * {size}ULL;"]
                        if "when" in field:
                            lines.append("    }")
                        continue
                    args = "row, size" if measure else "buffer, row"
                    lines += [f"{indent}const std::uint64_t {count_var} = {expression};"]
                    if direction == "Read" and minimums[field["record"]]:
                        minimum = minimums[field["record"]]
                        lines += [f"{indent}static_assert(noexcept(buffer.Remaining()));", f"{indent}if ({count_var} > buffer.Remaining() / {minimum}ULL) return false;"]
                    lines += [f"{indent}static_assert(noexcept(visitor.Prepare{label}({count_var})));", f"{indent}if (!visitor.Prepare{label}({count_var})) return false;"]
                    lines += [f"{indent}for (std::uint64_t i = 0; i < {count_var}; ++i) {{", f"{indent}    static_assert(noexcept(visitor.{label}(i)));", f"{indent}    auto&& row = visitor.{label}(i);", f"{indent}    if (!{direction}Pack{codec}({args})) return false;", f"{indent}}}"]
                else:
                    if measure and field["record"] in extents:
                        size = extents[field["record"]]
                        lines += [f"{indent}if (size > UINT64_MAX - {size}ULL) return false;", f"{indent}size += {size}ULL;"]
                        if "when" in field:
                            lines.append("    }")
                        continue
                    args = "row, size" if measure else "buffer, row"
                    lines += [f"{indent}{{", f"{indent}    static_assert(noexcept(visitor.{label}()));", f"{indent}    auto&& row = visitor.{label}();", f"{indent}    if (!{direction}Pack{codec}({args})) return false;", f"{indent}}}"]
                if "when" in field:
                    lines.append("    }")
            if not measure:
                if direction == "Read" and positions:
                    lines.append(f"    if (!ValidatePack{name}Layout(value, initialRemaining - buffer.Remaining())) return false;")
                lines += ["    static_assert(noexcept(visitor.Finish()));", "    return visitor.Finish();"]
            else:
                lines.append("    return true;")
            lines += ["}", ""]
    return "\n".join(lines + ["}", ""])


def record_layouts(layout: dict) -> dict[str, dict]:
    validate_layout(layout)
    fixed, _ = split_layout(layout)
    return _fixed_record_layouts(fixed)


def source_code(text: str) -> str:
    pattern = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\[^\n]{1,10}?|[^\'\\\n])\''
    return re.sub(pattern, lambda match: "".join("\n" if char == "\n" else " " for char in match[0]), text)


def function_ranges(text: str, names: tuple[str, ...]) -> list[tuple[int, int]]:
    ranges = []
    for name in names:
        if "::" in name:
            owner, method = name.rsplit("::", 1)
            for match in re.finditer(r"\bstruct\s+" + re.escape(owner) + r"\s*\{", text):
                start, end = block_range(text, match.start(), match.end())
                ranges.extend((start + left, start + right) for left, right in function_ranges(text[start:end], (method,)))
        for match in re.finditer(r"^[ \t]*[^\n;{}()]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*(?:noexcept\s*)?\{", text, re.M):
            ranges.append(block_range(text, match.start(), match.end()))
    return ranges


def block_range(text: str, start: int, opening_end: int) -> tuple[int, int]:
    depth = 1
    end = opening_end
    while end < len(text) and depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return start, end


def source_exemptions(relative: str, text: str, errors: list[str]) -> list[tuple[int, int]]:
    ranges = []
    for symbol in SOURCE_EXEMPTIONS.get(relative, {}):
        found = function_ranges(text, (symbol,))
        if len(found) != 1:
            errors.append(f"{relative}: stale source exemption: {symbol}")
        ranges.extend(found)
    for condition in SOURCE_BLOCK_EXEMPTIONS.get(relative, {}):
        pattern = r"\bif\s*\(\s*" + re.escape(condition) + r"\s*\)\s*\{"
        found = list(re.finditer(pattern, text))
        if len(found) != 1:
            errors.append(f"{relative}: stale source exemption: {condition}")
        ranges.extend(block_range(text, match.start(), match.end()) for match in found)
    if relative == CONTENT_REVISION_SOURCE:
        start = text.find("auto contentRevision = harkenstudiobuild::Fnv1a64(")
        end = text.find("pack.contentRevision = contentRevision & INT64_MAX;", start)
        calls = list(re.finditer(r"\bharkenstudiobuild::(?:StoreLE32|StoreLE64|StoreObjectId)\(header\.data\(\)(?:\s*\+\s*(?:4|8|24))?\s*,[^;]+;", text[start:end])) if start >= 0 and end > start else []
        if len(calls) != 7:
            errors.append(f"{relative}: stale content revision hash exemption")
        ranges.extend((start + match.start(), start + match.end()) for match in calls)
    return ranges


def scan_sources(root: Path = ROOT, scopes: dict | None = None) -> list[str]:
    errors = []
    for relative, functions in (SOURCE_SCANS if scopes is None else scopes).items():
        path = root / relative
        if not path.is_file():
            errors.append(f"missing source scan target: {relative}")
            continue
        text = source_code(path.read_text(encoding="utf-8"))
        ranges = [(0, len(text))] if functions is None else function_ranges(text, functions)
        excluded = source_exemptions(relative, text, errors)
        matches = list(BYTE_ACCESS.finditer(text))
        if relative.endswith("WorldCodec.cpp"):
            matches.extend(BODY_BYTE_ACCESS.finditer(text))
        for match in matches:
            if any(start <= match.start() < end for start, end in ranges) and not any(start <= match.start() < end for start, end in excluded):
                line = text.count("\n", 0, match.start()) + 1
                errors.append(f"{relative}:{line}: handwritten Pack byte access: {match[0].strip()}")
    return errors


def generate(root: Path = ROOT, check: bool = False) -> list[str]:
    contract = json.loads((root / CONTRACT).read_text(encoding="utf-8"))
    layout = contract.get("x-harken-format", {}).get("binaryLayout")
    validate_layout(layout)
    bindings = json.loads((root / BINDINGS).read_text(encoding="utf-8"))
    outputs = {target: render_header(layout, bindings, target) for target in NAMESPACES}
    outputs.update({target + "_constants": render_constants_header(layout, target) for target in NAMESPACES})
    errors = []
    for target, relative in OUTPUTS.items():
        path = root / relative
        content = outputs[target]
        if check:
            if not path.is_file() or path.read_bytes() != content.encode("utf-8"):
                errors.append(f"generated Pack wire header is stale: {relative}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("w", encoding="utf-8", newline="") as stream:
                stream.write(content)
    if check:
        errors.extend(scan_sources(root))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        errors = generate(check=args.check)
    except (ValueError, KeyError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    for error in errors:
        print(error, file=sys.stderr)
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
