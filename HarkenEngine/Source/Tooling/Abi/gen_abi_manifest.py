#!/usr/bin/env python3
"""Generate the machine-readable Harken public ABI contract.

The wire table remains the single hand-maintained schema. This generator adds
two guarantees that the existing native/C# pins cannot provide alone:

* completeness: public header symbols cannot be silently omitted from the
  table;
* provenance: one canonical JSON document binds layouts, values, functions,
  wire opcodes, Trace vocabulary, vintage rules, and generated-file hashes.

The manifest describes the declared public Vn_* export set. Incidental C++
symbols exported by a particular linker configuration are diagnostics, not
public ABI.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import operator
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


SCRIPT_PATH = Path(__file__).resolve()
DEFAULT_REPO_ROOT = SCRIPT_PATH.parents[4]
DEFAULT_OUTPUT = (
    SCRIPT_PATH.parent / "generated" / "harken_abi_manifest.g.json"
)

INTEGER_BINOPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.BitOr: operator.or_,
    ast.BitAnd: operator.and_,
    ast.BitXor: operator.xor,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.Mult: operator.mul,
}
INTEGER_UNARYOPS = {
    ast.UAdd: operator.pos,
    ast.USub: operator.neg,
    ast.Invert: operator.invert,
}


class ManifestError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def canonical_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def display_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            indent=2,
        )
        + "\n"
    ).encode("utf-8")


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError("duplicate JSON key: %s" % key)
        result[key] = value
    return result


def load_json_strict(path: Path) -> dict[str, Any]:
    value = json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=reject_duplicate_keys,
    )
    if not isinstance(value, dict):
        raise ManifestError("top-level JSON value must be an object")
    return value


def strip_integer_suffixes(expression: str) -> str:
    return re.sub(
        r"(?i)\b(0x[0-9a-f]+|[0-9]+)(?:ull|llu|ul|lu|ll|u|l)\b",
        r"\1",
        expression,
    )


def evaluate_integer(expression: str, names: dict[str, int] | None = None) -> int:
    cleaned = strip_integer_suffixes(expression.strip())
    node = ast.parse(cleaned, mode="eval")
    known = names or {}

    def visit(item: ast.AST) -> int:
        if isinstance(item, ast.Expression):
            return visit(item.body)
        if isinstance(item, ast.Constant) and isinstance(item.value, int):
            return item.value
        if isinstance(item, ast.Name) and item.id in known:
            return known[item.id]
        if isinstance(item, ast.BinOp) and type(item.op) in INTEGER_BINOPS:
            return INTEGER_BINOPS[type(item.op)](
                visit(item.left),
                visit(item.right),
            )
        if isinstance(item, ast.UnaryOp) and type(item.op) in INTEGER_UNARYOPS:
            return INTEGER_UNARYOPS[type(item.op)](visit(item.operand))
        raise ManifestError(
            "unsupported integer expression: %s" % expression
        )

    return int(visit(node))


def remove_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", source)


def normalize_ctype(value: str) -> str:
    normalized = " ".join(value.strip().split())
    normalized = re.sub(r"\s*\*\s*", "*", normalized)
    return normalized


def split_top_level(value: str, delimiter: str) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(value):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth < 0:
                raise ManifestError("unbalanced declaration: %s" % value)
        elif char == delimiter and depth == 0:
            parts.append(value[start:index].strip())
            start = index + 1
    if depth != 0:
        raise ManifestError("unbalanced declaration: %s" % value)
    parts.append(value[start:].strip())
    return [part for part in parts if part]


def parse_parameter_list(value: str) -> list[dict[str, str]]:
    if value.strip() == "void":
        return []
    params = []
    for declaration in split_top_level(value, ","):
        match = re.match(
            r"^(.*\S)\s+([A-Za-z_][A-Za-z0-9_]*)$",
            declaration.strip(),
        )
        if match is None:
            match = re.match(
                r"^(.*\*)\s*([A-Za-z_][A-Za-z0-9_]*)$",
                declaration.strip(),
            )
        if match is None:
            raise ManifestError(
                "unsupported function parameter: %s" % declaration
            )
        params.append({
            "name": match.group(2),
            "ctype": normalize_ctype(match.group(1)),
        })
    return params


def extract_struct_bodies(header: str) -> dict[str, str]:
    clean = remove_comments(header)
    result: dict[str, str] = {}
    patterns = (
        (
            re.compile(
                r"\btypedef\s+struct\s+(Vn[A-Za-z0-9_]+)\s*\{"
            ),
            True,
        ),
        (
            re.compile(
                r"(?<!typedef\s)\bstruct\s+(Vn[A-Za-z0-9_]+)\s*\{"
            ),
            False,
        ),
    )
    for pattern, requires_alias in patterns:
        for match in pattern.finditer(clean):
            name = match.group(1)
            depth = 1
            cursor = match.end()
            while cursor < len(clean) and depth:
                if clean[cursor] == "{":
                    depth += 1
                elif clean[cursor] == "}":
                    depth -= 1
                cursor += 1
            if depth != 0:
                raise ManifestError("unterminated public struct: %s" % name)
            suffix = clean[cursor:]
            if requires_alias:
                alias = re.match(r"\s*(Vn[A-Za-z0-9_]+)\s*;", suffix)
                if alias is None or alias.group(1) != name:
                    raise ManifestError(
                        "public struct alias mismatch: %s" % name
                    )
            elif re.match(r"\s*;", suffix) is None:
                raise ManifestError(
                    "public struct terminator mismatch: %s" % name
                )
            body = clean[match.end():cursor - 1]
            result[name] = re.sub(
                r"(?ms)^[ \t]*#ifdef[ \t]+__cplusplus[ \t]*\r?\n"
                r".*?^[ \t]*#endif[^\n]*(?:\n|$)", "", body)
    return result


def extract_struct_declarations(body: str, struct_name: str) -> list[str]:
    declarations: list[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(body):
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                raise ManifestError(
                    "unbalanced declaration in %s" % struct_name
                )
        elif char == ";" and depth == 0:
            declarations.append(body[start:index].strip())
            start = index + 1
    if body[start:].strip():
        raise ManifestError("unterminated field in %s" % struct_name)
    return declarations


def extract_struct_fields(body: str, struct_name: str) -> list[str]:
    fields: list[str] = []
    for declaration in extract_struct_declarations(body, struct_name):
        if declaration.startswith("union"):
            suffix = declaration[declaration.rfind("}") + 1:].strip()
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", suffix):
                raise ManifestError(
                    "unsupported union field in %s: %s"
                    % (struct_name, declaration)
                )
            fields.append(suffix)
            continue
        declarators: list[str] = []
        start = 0
        depth = 0
        for index, char in enumerate(declaration):
            if char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
            elif char == "," and depth == 0:
                declarators.append(declaration[start:index].strip())
                start = index + 1
        declarators.append(declaration[start:].strip())

        for declarator in declarators:
            match = re.search(
                r"([A-Za-z_][A-Za-z0-9_]*)\s*"
                r"(?:\[[^\]]+\])?\s*$",
                declarator,
            )
            if match is None:
                raise ManifestError(
                    "unsupported field declaration in %s: %s"
                    % (struct_name, declaration)
                )
            fields.append(match.group(1))
    return fields


PRIMITIVE_SCALARS = {
    "char": "u8",
    "uint8_t": "u8",
    "uint16_t": "u16",
    "int32_t": "i32",
    "uint32_t": "u32",
    "int64_t": "i64",
    "uint64_t": "u64",
    "float": "f32",
    "double": "f64",
}


def parse_scalar_typedefs(clean: str) -> dict[str, str]:
    typedefs: dict[str, str] = {}
    for match in re.finditer(
        r"\btypedef\s+(int32_t|uint32_t|int64_t|uint64_t|float|double)\s+"
        r"(Vn[A-Za-z0-9_]+)\s*;",
        clean,
    ):
        typedefs[match.group(2)] = PRIMITIVE_SCALARS[match.group(1)]
    return typedefs


def parse_callbacks(clean: str) -> dict[str, Any]:
    callbacks: dict[str, Any] = {}
    pattern = re.compile(
        r"\btypedef\s+([A-Za-z_][A-Za-z0-9_ \t]*?)\s*"
        r"\(\s*\*\s*(Vn[A-Za-z0-9_]+)\s*\)\s*"
        r"\((.*?)\)\s*;",
        flags=re.DOTALL,
    )
    for match in pattern.finditer(clean):
        name = match.group(2)
        if name in callbacks:
            raise ManifestError("duplicate callback typedef: %s" % name)
        callbacks[name] = {
            "returnType": normalize_ctype(match.group(1)),
            "callingConvention": "cdecl",
            "params": parse_parameter_list(match.group(3)),
        }
    return callbacks


def parse_defines(clean: str) -> dict[str, int]:
    joined = re.sub(r"\\\r?\n", "", clean)
    defines: dict[str, int] = {}
    seen_lines = re.findall(
        r"(?m)^[ \t]*#define[ \t]+VN_[^\r\n]*$",
        joined,
    )
    for line in seen_lines:
        match = re.match(
            r"^[ \t]*#define[ \t]+(VN_[A-Z0-9_]+)"
            r"(\([^)\r\n]*\))?[ \t]+(.+?)\s*$",
            line,
        )
        if match is None:
            raise ManifestError("unsupported public macro: %s" % line.strip())
        if match.group(2) is not None:
            if match.group(1) == "VN_OBJECT_ID":
                continue
            raise ManifestError(
                "function-like public macro is not supported: %s"
                % match.group(1)
            )
        name = match.group(1)
        if name in defines:
            raise ManifestError("duplicate public macro: %s" % name)
        defines[name] = parse_define_value(match.group(3))
    return defines


def parse_enum_values(clean: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for enum_match in re.finditer(
        r"\benum(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*\{(.*?)\}\s*;",
        clean,
        flags=re.DOTALL,
    ):
        for item in split_top_level(enum_match.group(1), ","):
            match = re.fullmatch(
                r"(VN_[A-Z0-9_]+)\s*=\s*(.+)",
                item.strip(),
                flags=re.DOTALL,
            )
            if match is None:
                raise ManifestError(
                    "public enum members require explicit VN_* values: %s"
                    % item.strip()
                )
            name = match.group(1)
            if name in values:
                raise ManifestError("duplicate public enum value: %s" % name)
            values[name] = evaluate_integer(match.group(2), values)
    return values


def validate_public_typedef_consumption(
    clean: str,
    scalar_typedefs: dict[str, str],
    callbacks: dict[str, Any],
    struct_names: set[str],
) -> None:
    anonymous_struct = re.search(r"\btypedef\s+struct\s*\{", clean)
    if anonymous_struct is not None:
        raise ManifestError(
            "anonymous public struct typedefs are not supported"
        )

    candidates = set(re.findall(
        r"\btypedef\s+[^;{}]+?\s+(Vn[A-Za-z0-9_]+)\s*;",
        clean,
    ))
    consumed = set(scalar_typedefs) | set(callbacks) | struct_names
    unsupported = sorted(candidates - consumed)
    if unsupported:
        raise ManifestError(
            "unsupported public typedef declarations: %s" % unsupported
        )


def canonical_field_shape(
    ctype: str,
    array_expression: str | None,
    struct_names: set[str],
    scalar_typedefs: dict[str, str],
    callback_names: set[str],
    integer_names: dict[str, int],
    struct_typedefs: dict[str, str],
) -> dict[str, Any]:
    normalized = normalize_ctype(ctype)
    if "*" in normalized or normalized in callback_names:
        if array_expression is not None:
            raise ManifestError("pointer arrays are not supported: %s" % ctype)
        return {"kind": "pointer", "ctype": normalized}

    scalar = PRIMITIVE_SCALARS.get(
        normalized, scalar_typedefs.get(normalized)
    )
    element: str
    if scalar is not None:
        element = scalar
    elif normalized in struct_names or normalized in struct_typedefs:
        element = struct_typedefs.get(normalized, normalized)
    else:
        raise ManifestError("unsupported public field type: %s" % normalized)

    if array_expression is not None:
        expression = array_expression[1:-1].strip()
        return {
            "kind": "array",
            "element": element,
            "count": evaluate_integer(expression, integer_names),
        }
    if scalar is not None:
        return {"kind": "scalar", "type": scalar}
    return {"kind": "struct", "type": element}


def parse_struct_shapes(
    bodies: dict[str, str],
    scalar_typedefs: dict[str, str],
    callbacks: dict[str, Any],
    integer_names: dict[str, int],
    struct_typedefs: dict[str, str],
) -> dict[str, list[dict[str, Any]]]:
    struct_names = set(bodies)
    result: dict[str, list[dict[str, Any]]] = {}
    for struct_name, body in bodies.items():
        fields: list[dict[str, Any]] = []
        for declaration in extract_struct_declarations(body, struct_name):
            if declaration.startswith("union"):
                open_at = declaration.find("{")
                close_at = declaration.rfind("}")
                if open_at < 0 or close_at <= open_at:
                    raise ManifestError(
                        "unsupported union declaration in %s" % struct_name
                    )
                field_name = declaration[close_at + 1:].strip()
                if not re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_]*", field_name
                ):
                    raise ManifestError(
                        "unsupported union field in %s: %s"
                        % (struct_name, declaration)
                    )
                members = []
                for member in extract_struct_declarations(
                    declaration[open_at + 1:close_at],
                    struct_name + "." + field_name,
                ):
                    match = re.fullmatch(
                        r"(Vn[A-Za-z0-9_]+)\s+"
                        r"[A-Za-z_][A-Za-z0-9_]*",
                        member,
                    )
                    if match is None or match.group(1) not in struct_names:
                        raise ManifestError(
                            "unsupported union member in %s: %s"
                            % (struct_name, member)
                        )
                    members.append(match.group(1))
                fields.append({
                    "name": field_name,
                    "shape": {"kind": "union", "members": members},
                })
                continue

            declarators = split_top_level(declaration, ",")
            inherited_type: str | None = None
            for declarator in declarators:
                match = re.fullmatch(
                    r"(.*?)\b([A-Za-z_][A-Za-z0-9_]*)\s*"
                    r"(\[[^\]]+\])?",
                    declarator.strip(),
                )
                if match is None:
                    raise ManifestError(
                        "unsupported field declaration in %s: %s"
                        % (struct_name, declaration)
                    )
                ctype = match.group(1).strip()
                if ctype:
                    inherited_type = ctype
                elif inherited_type is None or "*" in inherited_type:
                    raise ManifestError(
                        "unsupported shared field type in %s: %s"
                        % (struct_name, declaration)
                    )
                fields.append({
                    "name": match.group(2),
                    "shape": canonical_field_shape(
                        inherited_type,
                        match.group(3),
                        struct_names,
                        scalar_typedefs,
                        set(callbacks),
                        integer_names,
                        struct_typedefs,
                    ),
                })
        result[struct_name] = fields
    return result


def parse_functions(clean: str) -> list[dict[str, Any]]:
    candidates = set(re.findall(r"\b(Vn_[A-Za-z0-9_]+)\s*\(", clean))
    functions: list[dict[str, Any]] = []
    pattern = re.compile(
        r"(?m)^[ \t]*([A-Za-z_][A-Za-z0-9_ \t\*]*?)[ \t]+"
        r"(Vn_[A-Za-z0-9_]+)\s*\((.*?)\)\s*;",
        flags=re.DOTALL,
    )
    for match in pattern.finditer(clean):
        functions.append({
            "name": match.group(2),
            "returnType": normalize_ctype(match.group(1)),
            "callingConvention": "cdecl",
            "params": parse_parameter_list(match.group(3)),
        })
    parsed = [function["name"] for function in functions]
    if len(parsed) != len(set(parsed)):
        raise ManifestError("duplicate public function declaration")
    unsupported = sorted(candidates - set(parsed))
    if unsupported:
        raise ManifestError(
            "unsupported public function declarations: %s" % unsupported
        )
    return functions


def parse_define_value(expression: str) -> Any:
    expression = expression.strip()
    object_id = re.fullmatch(r"VN_OBJECT_ID\(([^,]+),([^\)]+)\)", expression)
    if object_id is not None:
        return {"high": evaluate_integer(object_id.group(1)),
                "low": evaluate_integer(object_id.group(2))}
    while expression.startswith("(") and expression.endswith(")"):
        expression = expression[1:-1].strip()
    expression = re.sub(
        r"^\(\s*(?:Vn[A-Za-z0-9_]+|u?int(?:32|64)_t)\s*\)",
        "",
        expression,
    ).strip()
    while expression.startswith("(") and expression.endswith(")"):
        expression = expression[1:-1].strip()
    return evaluate_integer(expression)


def parse_header_contract(header: str) -> dict[str, Any]:
    clean = remove_comments(header)
    defines = parse_defines(clean)
    enum_values = parse_enum_values(clean)
    scalar_typedefs = parse_scalar_typedefs(clean)
    callbacks = parse_callbacks(clean)
    bodies = extract_struct_bodies(header)
    struct_typedefs = dict((alias, backing) for backing, alias in re.findall(
        r"\btypedef\s+(Vn[A-Za-z0-9_]+)\s+(Vn[A-Za-z0-9_]+)\s*;", clean)
        if backing in bodies)
    validate_public_typedef_consumption(
        clean, scalar_typedefs, callbacks, set(bodies) | set(struct_typedefs)
    )
    return {
        "functions": parse_functions(clean),
        "defines": defines,
        "enum_values": enum_values,
        "scalar_typedefs": scalar_typedefs,
        "struct_typedefs": struct_typedefs,
        "structs": parse_struct_shapes(
            bodies,
            scalar_typedefs,
            callbacks,
            {**{key: value for key, value in defines.items() if isinstance(value, int)}, **enum_values},
            struct_typedefs,
        ),
        "callbacks": callbacks,
    }


def completeness_field_shape(spec: Any) -> dict[str, Any]:
    if isinstance(spec, str):
        return {"kind": "scalar", "type": spec}
    if "array" in spec:
        return {
            "kind": "array",
            "element": spec["array"][0],
            "count": spec["array"][1],
        }
    if "struct" in spec:
        return {"kind": "struct", "type": spec["struct"]}
    if "union" in spec:
        return {"kind": "union", "members": list(spec["union"])}
    if "ptr" in spec:
        if "ctype" not in spec:
            raise ManifestError("pointer field is missing ctype metadata")
        return {
            "kind": "pointer",
            "ctype": normalize_ctype(spec["ctype"]),
        }
    raise ManifestError("unknown field shape: %r" % spec)


def table_contract(table: dict[str, Any]) -> dict[str, Any]:
    enum_values = dict(table["api"]["results"]["values"])
    for enum in table["api"]["enums"].values():
        enum_values.update(enum["values"])
    callbacks = {
        name: {
            "returnType": normalize_ctype(
                callback.get("returnType", "void")
            ),
            "callingConvention": callback.get(
                "callingConvention", "cdecl"
            ),
            "params": [
                {
                    "name": parameter["name"],
                    "ctype": normalize_ctype(parameter["ctype"]),
                }
                for parameter in callback.get("params", [])
            ],
        }
        for name, callback in table["api"]["callbacks"].items()
        if not name.startswith("_")
    }
    return {
        "functions": [
            {
                "name": function["name"],
                "returnType": normalize_ctype(
                    function.get("returnType", "VnResult")
                ),
                "callingConvention": function.get(
                    "callingConvention",
                    table["abi"]["callingConvention"],
                ),
                "params": [
                    {
                        "name": parameter["name"],
                        "ctype": normalize_ctype(parameter["ctype"]),
                    }
                    for parameter in function.get("params", [])
                ],
            }
            for function in table["functions"]
            if function.get("public", True)
        ],
        "defines": {
            name: definition["value"]
            for name, definition in table["api"]["defines"].items()
        },
        "enum_values": enum_values,
        "scalar_typedefs": table["api"]["scalarTypedefs"],
        "struct_typedefs": table["api"].get("structTypedefs", {}),
        "structs": {
            name: [
                {
                    "name": field[0],
                    "shape": completeness_field_shape(field[1]),
                }
                for field in definition["fields"]
            ]
            for name, definition in table["types"].items()
        },
        "callbacks": callbacks,
    }


def compare_completeness(
    header_contract: dict[str, Any],
    declared_contract: dict[str, Any],
) -> list[str]:
    def callable_signature(value: dict[str, Any]) -> tuple[Any, ...]:
        return (
            value["returnType"],
            value["callingConvention"],
            tuple(parameter["ctype"] for parameter in value["params"]),
        )

    issues: list[str] = []
    header_functions = {
        function["name"]: function
        for function in header_contract["functions"]
    }
    table_functions = {
        function["name"]: function
        for function in declared_contract["functions"]
    }
    function_header_only = sorted(
        set(header_functions) - set(table_functions)
    )
    function_table_only = sorted(
        set(table_functions) - set(header_functions)
    )
    function_mismatched = sorted(
        name for name in set(header_functions) & set(table_functions)
        if callable_signature(header_functions[name])
        != callable_signature(table_functions[name])
    )
    if (
        function_header_only
        or function_table_only
        or function_mismatched
    ):
        issues.append(
            "functions mismatch: header_only=%s table_only=%s signatures=%s"
            % (
                function_header_only,
                function_table_only,
                function_mismatched,
            )
        )

    for category in ("callbacks", "scalar_typedefs", "struct_typedefs"):
        header_values = header_contract[category]
        table_values = declared_contract[category]
        header_only = sorted(set(header_values) - set(table_values))
        table_only = sorted(set(table_values) - set(header_values))
        if category == "callbacks":
            mismatched = sorted(
                name for name in set(header_values) & set(table_values)
                if callable_signature(header_values[name])
                != callable_signature(table_values[name])
            )
        else:
            mismatched = sorted(
                name for name in set(header_values) & set(table_values)
                if header_values[name] != table_values[name]
            )
        if header_only or table_only or mismatched:
            issues.append(
                "%s mismatch: header_only=%s table_only=%s values=%s"
                % (
                    category,
                    header_only,
                    table_only,
                    mismatched,
                )
            )
    for category in ("defines", "enum_values"):
        header_values = header_contract[category]
        table_values = declared_contract[category]
        header_only = sorted(set(header_values) - set(table_values))
        table_only = sorted(set(table_values) - set(header_values))
        mismatched = sorted(
            name for name in set(header_values) & set(table_values)
            if header_values[name] != table_values[name]
        )
        if header_only or table_only or mismatched:
            issues.append(
                "%s mismatch: header_only=%s table_only=%s values=%s"
                % (category, header_only, table_only, mismatched)
            )

    header_structs = header_contract["structs"]
    table_structs = declared_contract["structs"]
    header_only = sorted(set(header_structs) - set(table_structs))
    table_only = sorted(set(table_structs) - set(header_structs))
    if header_only or table_only:
        issues.append(
            "struct mismatch: header_only=%s table_only=%s"
            % (header_only, table_only)
        )
    for name in sorted(set(header_structs) & set(table_structs)):
        if header_structs[name] != table_structs[name]:
            issues.append(
                "field mismatch for %s: header=%s table=%s"
                % (name, header_structs[name], table_structs[name])
            )
    return issues


def import_layout_module(repo_root: Path):
    wire_dir = (
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Tooling"
        / "Remote"
        / "wire"
    )
    sys.path.insert(0, str(wire_dir))
    try:
        import gen_wire
    finally:
        sys.path.pop(0)
    return gen_wire


def validate_table_metadata(table: dict[str, Any]) -> None:
    abi = table.get("abi")
    expected = {
        "schemaVersion": 1,
        "target": "windows-msvc-x86_64",
        "dataModel": "LLP64",
        "pointerBytes": 8,
        "byteOrder": "little",
        "callingConvention": "cdecl",
        "publicExportPrefix": "Vn_",
        "publicExportPolicy": "required-set",
        "defaultStructSizePolicy": "exact-current",
    }
    if not isinstance(abi, dict):
        raise ManifestError("abi metadata is required")
    expected_keys = set(expected) | {
        "structSizePolicies",
        "nonStandaloneStructSizePolicies",
        "stridePolicies",
    }
    if set(abi) != expected_keys:
        raise ManifestError(
            "unsupported abi metadata keys: missing=%s extra=%s"
            % (
                sorted(expected_keys - set(abi)),
                sorted(set(abi) - expected_keys),
            )
        )
    for key, value in expected.items():
        if abi.get(key) != value:
            raise ManifestError(
                "unsupported abi metadata %s=%r; expected %r"
                % (key, abi.get(key), value)
            )

    function_names: set[str] = set()
    opcodes: set[int] = set()
    for function in table["functions"]:
        name = function["name"]
        if name in function_names:
            raise ManifestError("duplicate function: %s" % name)
        function_names.add(name)
        public = function.get("public", True)
        if not isinstance(public, bool):
            raise ManifestError("function public flag must be boolean: %s" % name)
        if public and not name.startswith(abi["publicExportPrefix"]):
            raise ManifestError("invalid public export name: %s" % name)
        if "opcode" in function:
            opcode = function["opcode"]
            if (
                not isinstance(opcode, int)
                or isinstance(opcode, bool)
                or opcode <= 0
                or opcode > 0xFFFF
            ):
                raise ManifestError(
                    "remote opcode is outside uint16: %r" % opcode
                )
            if opcode in opcodes:
                raise ManifestError(
                    "duplicate remote opcode: %d" % opcode
                )
            opcodes.add(opcode)

    public_constants: set[str] = set()
    constant_groups = [
        table["api"]["results"]["values"],
        table["api"]["defines"],
        *(
            enum["values"]
            for enum in table["api"]["enums"].values()
        ),
    ]
    for group in constant_groups:
        overlap = public_constants & set(group)
        if overlap:
            raise ManifestError(
                "duplicate public constants: %s" % sorted(overlap)
            )
        public_constants.update(group)

    for type_name, definition in table["types"].items():
        field_names = [field[0] for field in definition["fields"]]
        if len(field_names) != len(set(field_names)):
            raise ManifestError(
                "duplicate fields in %s" % type_name
            )

    scalar_typedefs = table["api"]["scalarTypedefs"]
    for name, backing in table["api"].get("structTypedefs", {}).items():
        if name in scalar_typedefs or backing not in table["types"]:
            raise ManifestError("invalid struct typedef %s=%s" % (name, backing))
    allowed_scalars = {"i32", "u32", "i64", "u64", "f32"}
    for name, backing in scalar_typedefs.items():
        if backing not in allowed_scalars:
            raise ManifestError(
                "unsupported scalar typedef backing %s=%s"
                % (name, backing)
            )
    if scalar_typedefs.get("VnResult") != table["api"]["results"]["backing"]:
        raise ManifestError("VnResult backing disagrees with results")
    for name, enum in table["api"]["enums"].items():
        if enum["backing"] not in allowed_scalars:
            raise ManifestError(
                "unsupported enum backing %s=%s"
                % (name, enum["backing"])
            )
        if (
            name in scalar_typedefs
            and scalar_typedefs[name] != enum["backing"]
        ):
            raise ManifestError(
                "enum backing disagrees with scalar typedef: %s" % name
            )
    for name, callback in table["api"]["callbacks"].items():
        if name.startswith("_"):
            continue
        if callback.get("callingConvention") != abi["callingConvention"]:
            raise ManifestError(
                "callback calling convention mismatch: %s" % name
            )
        for parameter in callback.get("params", []):
            if "ctype" not in parameter:
                raise ManifestError(
                    "callback parameter is missing ctype: %s.%s"
                    % (name, parameter.get("name"))
                )

    expected_missing_fields = {
        "VnConfig": "engine-defaults",
        "VnSnapshot": "caller-tail-untouched",
        "VnPlayDesc": "documented-play-defaults",
        "VnAttenuationDesc": "legacy-proximity-frequency-and-disabled-highpass",
    }
    if set(abi["structSizePolicies"]) != set(expected_missing_fields):
        raise ManifestError(
            "unexpected explicit struct-size policy set: %s"
            % sorted(abi["structSizePolicies"])
        )
    allowed_struct_modes = {"exact-discrete-prefix"}
    for type_name, policy in abi["structSizePolicies"].items():
        expected_policy_keys = {
            "mode",
            "boundaryFields",
            "missingFields",
            "unknownSize",
        }
        if set(policy) != expected_policy_keys:
            raise ManifestError(
                "unsupported struct-size policy keys for %s: %s"
                % (type_name, sorted(set(policy) - expected_policy_keys))
            )
        if policy.get("mode") not in allowed_struct_modes:
            raise ManifestError(
                "unsupported struct-size policy for %s" % type_name
            )
        boundaries = policy.get("boundaryFields")
        if (
            not isinstance(boundaries, list)
            or not boundaries
            or any(not isinstance(field, str) for field in boundaries)
            or len(boundaries) != len(set(boundaries))
        ):
            raise ManifestError(
                "invalid vintage boundary fields for %s" % type_name
            )
        if policy.get("missingFields") != expected_missing_fields[type_name]:
            raise ManifestError(
                "unsupported missing-field semantics for %s: %r"
                % (type_name, policy.get("missingFields"))
            )
        if policy.get("unknownSize") != "reject":
            raise ManifestError(
                "struct-size policy must reject unknown sizes: %s"
                % type_name
            )

    expected_non_standalone = {
        "VnEffectDesc": {
            "mode": "polymorphic-base",
            "concreteTypes": [
                "VnFeedbackDelayDesc",
                "VnThreeBandToneDesc",
                "VnGainDesc",
                "VnLimiterDesc",
                "VnCompressorDesc",
            ],
        }
    }
    if abi["nonStandaloneStructSizePolicies"] != expected_non_standalone:
        raise ManifestError(
            "unsupported non-standalone struct-size policies"
        )

    expected_stride_policies = {}
    if abi["stridePolicies"] != expected_stride_policies:
        raise ManifestError("unsupported stride policies")
    for name, policy in abi["stridePolicies"].items():
        expected_stride_keys = {
            "elementType",
            "acceptedValues",
            "legacyBoundaryField",
            "zeroMeaning",
            "unknownValue",
        }
        if set(policy) != expected_stride_keys:
            raise ManifestError(
                "unsupported stride policy keys for %s: %s"
                % (name, sorted(set(policy) - expected_stride_keys))
            )
        if policy.get("unknownValue") != "reject":
            raise ManifestError(
                "stride policy must reject unknown values: %s" % name
            )


def field_footprint(
    spec: Any,
    layouts: dict[str, Any],
    scalar_sizes: dict[str, int],
    pointer_bytes: int,
) -> tuple[int, int]:
    if isinstance(spec, str):
        size = scalar_sizes[spec]
        return size, size
    if "array" in spec:
        element, count = spec["array"]
        if isinstance(element, str) and element in scalar_sizes:
            size = scalar_sizes[element]
            align = size
        else:
            size = layouts[element].size
            align = layouts[element].align
        return size * count, align
    if "struct" in spec:
        layout = layouts[spec["struct"]]
        return layout.size, layout.align
    if "union" in spec:
        members = [layouts[name] for name in spec["union"]]
        align = max(member.align for member in members)
        size = max(member.size for member in members)
        size = (size + align - 1) // align * align
        return size, align
    if "ptr" in spec:
        return pointer_bytes, pointer_bytes
    raise ManifestError("unknown field shape: %r" % spec)


def normalized_field_shape(spec: Any) -> Any:
    if isinstance(spec, str):
        return {"kind": "scalar", "type": spec}
    if "array" in spec:
        return {
            "kind": "array",
            "element": spec["array"][0],
            "count": spec["array"][1],
        }
    if "struct" in spec:
        return {"kind": "struct", "type": spec["struct"]}
    if "union" in spec:
        return {"kind": "union", "members": list(spec["union"])}
    if "ptr" in spec:
        return {
            "kind": "pointer",
            "ctype": normalize_ctype(spec["ctype"]),
            "storage": spec["ptr"],
        }
    raise ManifestError("unknown field shape: %r" % spec)


def build_types(
    table: dict[str, Any],
    layouts: dict[str, Any],
    scalar_sizes: dict[str, int],
    pointer_bytes: int,
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, definition in table["types"].items():
        layout = layouts[name]
        offsets = dict(layout.fields)
        fields = []
        for field_name, spec in definition["fields"]:
            size, align = field_footprint(
                spec, layouts, scalar_sizes, pointer_bytes
            )
            fields.append({
                "name": field_name,
                "offset": offsets[field_name],
                "size": size,
                "alignment": align,
                "shape": normalized_field_shape(spec),
            })
        result[name] = {
            "size": layout.size,
            "alignment": layout.align,
            "fields": fields,
        }
    return result


def build_vintage_policies(
    table: dict[str, Any],
    layouts: dict[str, Any],
) -> dict[str, Any]:
    abi = table.get("abi")
    if abi is None:
        return {
            "status": "unavailable-in-source",
            "struct_size": {},
            "stride": {},
        }
    explicit = abi["structSizePolicies"]
    non_standalone = abi["nonStandaloneStructSizePolicies"]
    struct_size_types = {
        type_name
        for type_name, definition in table["types"].items()
        if definition["fields"]
        and definition["fields"][0][0] == "structSize"
    }
    invalid_classifications = sorted(
        (set(explicit) | set(non_standalone)) - struct_size_types
    )
    if invalid_classifications:
        raise ManifestError(
            "struct-size policies target types without leading structSize: %s"
            % invalid_classifications
        )
    overlapping = sorted(set(explicit) & set(non_standalone))
    if overlapping:
        raise ManifestError(
            "struct-size types have conflicting policies: %s"
            % overlapping
        )

    policies: dict[str, Any] = {}
    for type_name in sorted(struct_size_types):
        if type_name in non_standalone:
            policy = non_standalone[type_name]
            policies[type_name] = {
                **policy,
                "source": "explicit-non-standalone",
                "currentSize": layouts[type_name].size,
            }
            continue
        if type_name not in explicit:
            policies[type_name] = {
                "mode": abi["defaultStructSizePolicy"],
                "source": "default",
                "acceptedSizes": [layouts[type_name].size],
                "currentSize": layouts[type_name].size,
                "unknownSize": "reject",
            }
            continue
        policy = explicit[type_name]
        if type_name not in layouts:
            raise ManifestError("unknown vintage type: %s" % type_name)
        offsets = dict(layouts[type_name].fields)
        boundaries = []
        for field_name in policy["boundaryFields"]:
            if field_name not in offsets:
                raise ManifestError(
                    "unknown vintage boundary %s.%s"
                    % (type_name, field_name)
                )
            boundaries.append({
                "field": field_name,
                "size": offsets[field_name],
            })
        accepted = [
            *[item["size"] for item in boundaries],
            layouts[type_name].size,
        ]
        if accepted != sorted(set(accepted)):
            raise ManifestError(
                "vintage sizes are not strictly increasing: %s"
                % type_name
            )
        policies[type_name] = {
            **policy,
            "source": "explicit",
            "boundaries": boundaries,
            "acceptedSizes": accepted,
            "currentSize": layouts[type_name].size,
        }

    strides = json.loads(json.dumps(abi.get("stridePolicies", {})))
    for policy_name, policy in strides.items():
        element = policy["elementType"]
        current_size = layouts[element].size
        if policy["acceptedValues"] != [0, current_size]:
            raise ManifestError(
                "%s accepted strides %r must be [0, sizeof(%s)=%d]"
                % (
                    policy_name,
                    policy["acceptedValues"],
                    element,
                    current_size,
                )
            )
        boundary_field = policy["legacyBoundaryField"]
        element_offsets = dict(layouts[element].fields)
        if boundary_field not in element_offsets:
            raise ManifestError(
                "%s has unknown legacy boundary field %s"
                % (policy_name, boundary_field)
            )
        legacy_size = element_offsets[boundary_field]
        zero_meaning = "legacy-%d-byte-child" % legacy_size
        if policy.get("zeroMeaning") != zero_meaning:
            raise ManifestError(
                "%s zeroMeaning %r does not match resolved %r"
                % (policy_name, policy.get("zeroMeaning"), zero_meaning)
            )
        policy["resolvedLegacyStride"] = legacy_size
        owner_type, owner_field = policy_name.split(".", 1)
        owner_fields = {
            field[0] for field in table["types"][owner_type]["fields"]
        }
        if owner_field not in owner_fields:
            raise ManifestError(
                "unknown stride field: %s" % policy_name
            )
    return {
        "status": "declared-and-layout-resolved",
        "struct_size": policies,
        "stride": strides,
    }


def parse_trace_contract(trace_header: str) -> dict[str, Any]:
    clean = remove_comments(trace_header)
    constants: dict[str, int] = {}
    pattern = re.compile(
        r"\b(?:inline\s+)?constexpr\s+"
        r"(?:std::)?(?:u?int(?:16|32|64)_t|char)\s+"
        r"(kTrace[A-Za-z0-9_]+)\s*=\s*([^;]+);"
    )
    for match in pattern.finditer(clean):
        name = match.group(1)
        expression = match.group(2).strip()
        if expression.startswith("{"):
            continue
        constants[name] = evaluate_integer(expression, constants)

    call_enum = re.search(
        r"\benum\s*:\s*std::uint16_t\s*\{(.*?)\};",
        clean,
        flags=re.DOTALL,
    )
    if call_enum is None:
        raise ManifestError("Trace call-id enum was not parsed")

    call_ids: dict[str, int] = {}
    for item in split_top_level(call_enum.group(1), ","):
        match = re.fullmatch(
            r"(kTraceCall[A-Za-z0-9_]+)\s*=\s*(.+)",
            item.strip(),
            flags=re.DOTALL,
        )
        if match is None:
            raise ManifestError(
                "Trace call ids require explicit values: %s"
                % item.strip()
            )
        if match.group(1) in call_ids:
            raise ManifestError(
                "duplicate Trace call id: %s" % match.group(1)
            )
        call_ids[match.group(1)] = evaluate_integer(
            match.group(2),
            {**constants, **call_ids},
        )
    if "kTraceFormatVersion" not in constants:
        raise ManifestError("Trace format version was not parsed")
    return {
        "formatVersion": constants["kTraceFormatVersion"],
        "acceptance": "exact-current-version",
        "constants": constants,
        "callIds": call_ids,
    }


def generated_artifact_paths(repo_root: Path) -> list[Path]:
    relative = [
        "HarkenSDK/Unity/Harken/Runtime/Generated/HarkenTypes.g.cs",
        "HarkenSDK/Unity/Harken/Runtime/Generated/HarkenNative.g.cs",
        "HarkenSDK/Unity/Harken/Runtime/Generated/HarkenAbi.g.cs",
        "HarkenEngine/Source/Tooling/Binding/generated/"
        "harken_binding_abi_pin.g.h",
        "HarkenEngine/Tool/Remote/generated/harken_wire_ids.h",
        "HarkenEngine/Tool/Remote/generated/"
        "harken_wire_layout_asserts.h",
        "HarkenEngine/Tool/Remote/generated/"
        "harken_wire_desc_meta.h",
        "HarkenEngine/Tool/Remote/generated/"
        "harken_wire_dispatch.inl",
        "HarkenEngine/Tool/Remote/harken_wire.py",
    ]
    return [repo_root / item for item in relative]


def build_manifest(repo_root: Path) -> dict[str, Any]:
    table_path = (
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Tooling"
        / "Remote"
        / "wire"
        / "harken_wire_table.json"
    )
    header_path = (
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Runtime"
        / "api"
        / "include"
        / "harken"
        / "Harken.h"
    )
    trace_path = (
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Runtime"
        / "api"
        / "src"
        / "TraceFormat.h"
    )
    generator_paths = (
        SCRIPT_PATH,
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Tooling"
        / "Binding"
        / "gen_binding.py",
        repo_root
        / "HarkenEngine"
        / "Source"
        / "Tooling"
        / "Remote"
        / "wire"
        / "gen_wire.py",
    )
    table = load_json_strict(table_path)
    identity_header_path = header_path.with_name("ObjectId.h")
    header = (identity_header_path.read_text(encoding="utf-8") + "\n" +
              header_path.read_text(encoding="utf-8"))
    validate_table_metadata(table)

    issues = compare_completeness(
        parse_header_contract(header),
        table_contract(table),
    )
    if issues:
        raise ManifestError("; ".join(issues))

    gen_wire = import_layout_module(repo_root)
    layouts = gen_wire.compute_layouts(table["types"])
    types = build_types(
        table,
        layouts,
        gen_wire.SCALAR_SIZES,
        table["abi"]["pointerBytes"],
    )
    vintages = build_vintage_policies(table, layouts)

    native_functions = []
    remote_operations = []
    for function in table["functions"]:
        parameters = [
            {
                "name": parameter["name"],
                "ctype": parameter["ctype"],
            }
            for parameter in function.get("params", [])
        ]
        if function.get("public", True):
            native_functions.append({
                "name": function["name"],
                "returnType": function.get("returnType", "VnResult"),
                "callingConvention": function.get(
                    "callingConvention",
                    table["abi"]["callingConvention"],
                ),
                "parameters": parameters,
            })
        if "opcode" in function:
            remote_operations.append({
                "name": function["name"],
                "opcode": function["opcode"],
                "policy": function.get("policy"),
                "parameters": [
                {
                    "name": parameter["name"],
                    "wire": parameter["wire"],
                    **({
                        "type": parameter["type"],
                    } if "type" in parameter else {}),
                }
                for parameter in function.get("params", [])
                ],
            })

    native_abi = {
        "platform": {
            key: table["abi"][key]
            for key in (
                "target",
                "dataModel",
                "pointerBytes",
                "byteOrder",
                "callingConvention",
                "publicExportPrefix",
                "publicExportPolicy",
            )
        },
        "types": types,
        "scalarTypedefs": table["api"]["scalarTypedefs"],
        "structTypedefs": table["api"].get("structTypedefs", {}),
        "results": table["api"]["results"],
        "enums": table["api"]["enums"],
        "defines": table["api"]["defines"],
        "callbacks": {
            name: value
            for name, value in table["api"]["callbacks"].items()
            if not name.startswith("_")
        },
        "functions": native_functions,
        "publicExports": sorted(
            function["name"] for function in table["functions"]
            if function.get("public", True)
        ),
        "vintages": vintages,
    }
    remote_wire = {
        "protocol": table["protocol"],
        "operations": remote_operations,
    }
    trace_format = parse_trace_contract(
            trace_path.read_text(encoding="utf-8")
    )
    contract = {
        "nativeAbi": native_abi,
        "remoteWire": remote_wire,
        "traceFormat": trace_format,
    }
    section_hashes = {
        "nativeAbiSha256": sha256_bytes(canonical_bytes(native_abi)),
        "remoteWireSha256": sha256_bytes(canonical_bytes(remote_wire)),
        "traceFormatSha256": sha256_bytes(canonical_bytes(trace_format)),
    }
    source_files = {
        str(path.relative_to(repo_root)).replace("\\", "/"): {
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
        for path in (
            table_path,
            header_path,
            identity_header_path,
            trace_path,
            *generator_paths,
        )
    }
    generated_files = {}
    for path in generated_artifact_paths(repo_root):
        if not path.is_file():
            raise ManifestError("generated artifact missing: %s" % path)
        generated_files[
            str(path.relative_to(repo_root)).replace("\\", "/")
        ] = {
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    return {
        "schemaVersion": 1,
        "kind": "harken-contract-suite-manifest",
        "contractSha256": sha256_bytes(canonical_bytes(contract)),
        "sectionSha256": section_hashes,
        "contract": contract,
        "sourceFiles": source_files,
        "generatedFiles": generated_files,
    }


def parse_dumpbin_exports(output: str) -> list[str]:
    exports: list[str] = []
    pattern = re.compile(
        r"^\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+"
        r"[0-9A-Fa-f]+\s+(\S+)(?:\s|$)"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            exports.append(match.group(1))
    return sorted(set(exports))


def audit_binary(
    manifest: dict[str, Any],
    binary: Path,
    dumpbin: Path,
) -> dict[str, Any]:
    completed = subprocess.run(
        [str(dumpbin), "/nologo", "/exports", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise ManifestError(
            "dumpbin failed with %d: %s"
            % (completed.returncode, completed.stderr.strip())
        )
    exports = parse_dumpbin_exports(completed.stdout)
    native_abi = manifest["contract"]["nativeAbi"]
    expected = set(native_abi["publicExports"])
    prefix = native_abi["platform"]["publicExportPrefix"]
    observed_public = {
        name for name in exports if name.startswith(prefix)
    }
    missing = sorted(expected - observed_public)
    extra_public = sorted(observed_public - expected)
    report = {
        "schemaVersion": 1,
        "kind": "harken-public-export-audit",
        "binary": {
            "path": str(binary.resolve()),
            "sha256": sha256_file(binary),
            "size": binary.stat().st_size,
        },
        "dumpbin": {
            "path": str(dumpbin.resolve()),
            "sha256": sha256_file(dumpbin),
        },
        "requiredPublicExportCount": len(expected),
        "observedPublicExportCount": len(observed_public),
        "observedNamedExportCount": len(exports),
        "incidentalNonPublicExportCount": len(exports - observed_public)
        if isinstance(exports, set) else len(set(exports) - observed_public),
        "missingPublicExports": missing,
        "extraPublicExports": extra_public,
        "passed": not missing and not extra_public,
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=DEFAULT_REPO_ROOT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--audit-binary", type=Path)
    parser.add_argument("--dumpbin", type=Path)
    parser.add_argument("--audit-report", type=Path)
    args = parser.parse_args()

    try:
        manifest = build_manifest(args.repo_root.resolve())
        output_bytes = display_bytes(manifest)
        output_path = args.output.resolve()
        if args.check:
            if not output_path.is_file():
                raise ManifestError(
                    "manifest does not exist: %s" % output_path
                )
            if output_path.read_bytes() != output_bytes:
                raise ManifestError(
                    "manifest is stale: %s" % output_path
                )
            print("ok: %s" % output_path)
        else:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(output_bytes)
            print("wrote: %s" % output_path)

        if args.audit_binary is not None:
            if args.dumpbin is None or args.audit_report is None:
                raise ManifestError(
                    "--audit-binary requires --dumpbin and --audit-report"
                )
            report = audit_binary(
                manifest,
                args.audit_binary.resolve(),
                args.dumpbin.resolve(),
            )
            report_path = args.audit_report.resolve()
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_bytes(display_bytes(report))
            print("wrote: %s" % report_path)
            if not report["passed"]:
                raise ManifestError(
                    "public export audit failed: missing=%s extra=%s"
                    % (
                        report["missingPublicExports"],
                        report["extraPublicExports"],
                    )
                )
    except (OSError, ValueError, KeyError, ManifestError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
