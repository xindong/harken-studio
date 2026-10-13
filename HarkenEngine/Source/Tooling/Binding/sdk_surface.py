#!/usr/bin/env python3
"""Inventory the SDK source surface without changing the native ABI or wire policy.

Every observed declaration needs an explicit policy entry. The inventory command
only reports observations: it never assigns audiences or updates approved policy.
Both sides of conditional compilation are inspected, including Player-only APIs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from tree_sitter import Language, Parser
import tree_sitter_c_sharp
import tree_sitter_cpp


ROOT = Path(__file__).resolve().parents[4]
POLICY = Path(__file__).with_name("sdk_api_surface.json")
AUDIENCES = {"gameplay", "host-integration", "tooling", "internal"}
DISPOSITIONS = {"supported", "move-internal", "compatibility", "tombstone"}
CS_TYPES = {"class_declaration", "struct_declaration", "interface_declaration",
            "enum_declaration", "record_declaration"}
CS_MEMBERS = {"method_declaration", "constructor_declaration", "destructor_declaration",
              "property_declaration", "indexer_declaration", "field_declaration",
              "event_declaration", "event_field_declaration", "operator_declaration",
              "conversion_operator_declaration", "delegate_declaration"}
CPP_TYPES = {"class_specifier", "struct_specifier", "union_specifier", "enum_specifier"}
CPP_HEADERS = {".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".inc"}
CONDITIONALS = {"preproc_if", "preproc_ifdef", "preproc_elif", "preproc_else"}
UE_MACROS = {"UCLASS", "USTRUCT", "UENUM", "UPROPERTY", "UFUNCTION", "UMETA",
             "UPARAM", "GENERATED_BODY", "GENERATED_UCLASS_BODY", "UE_DEPRECATED"}
PARSERS = {"managed": Parser(Language(tree_sitter_c_sharp.language())),
           "unreal": Parser(Language(tree_sitter_cpp.language()))}


class SurfaceError(ValueError):
    pass


def load_json(path: Path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise SurfaceError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique)


def text(node, source: bytes) -> str:
    return source[node.start_byte:node.end_byte].decode("utf-8")


def walk(node):
    yield node
    for child in node.named_children:
        yield from walk(child)


def tokens(node, source: bytes, *, exclude=()) -> list[str]:
    if node.type == "comment" or node.id in exclude:
        return []
    if not node.children:
        return [text(node, source)]
    return [part for child in node.children
            for part in tokens(child, source, exclude=exclude)]


def signature(node, source: bytes, *, is_type=False) -> str:
    excluded = set()
    if is_type:
        body = node.child_by_field_name("body")
        if body:
            excluded.add(body.id)
    else:
        for child in walk(node):
            if child.type in {"block", "compound_statement", "arrow_expression_clause",
                              "constructor_initializer"}:
                excluded.add(child.id)
    return " ".join(tokens(node, source, exclude=excluded)).strip().rstrip(" ;")


def conditional_header(node, source):
    condition = node.child_by_field_name("condition") or node.child_by_field_name("name")
    return text(node.children[0], source) + (" " + " ".join(tokens(condition, source)) if condition else "")


def conditional_context(node, source):
    context = []
    parent = node.parent
    while parent:
        if parent.type in CONDITIONALS:
            context.append(conditional_header(parent, source))
        parent = parent.parent
    return list(reversed(context))


def conditional_signature(declaration, conditions):
    if conditions:
        return declaration + " [conditions:" + json.dumps(conditions, ensure_ascii=False) + "]"
    return declaration


def preprocessor_context(root, source, label, language, assembly="", anchors=()):
    """Preserve local macro/include state transitions without evaluating them."""
    def state_directive(node):
        return (node.type.startswith("preproc_") and node.type not in CONDITIONALS and
                node.children and text(node.children[0], source).startswith("#"))

    def relevant_nodes(node, in_body=False):
        in_body = in_body or node.type in {"block", "compound_statement", "arrow_expression_clause",
                                          "constructor_initializer"}
        # A body-only branch cannot alter declarations. Definitions, includes and
        # pragmas can outlive the body, so preserve their enclosing branch chain.
        if in_body and node.type in CONDITIONALS and not any(state_directive(n) for n in walk(node)):
            return
        yield node
        for child in node.named_children:
            yield from relevant_nodes(child, in_body)

    directives = {}
    for node in relevant_nodes(root):
        if node.type in CONDITIONALS:
            directives[node.start_byte] = conditional_header(node, source)
        elif state_directive(node):
            # Read the logical directive line, never a following declaration or
            # body. Continuation lines belong to macro definitions.
            end = source.find(b"\n", node.start_byte)
            while end != -1 and source[node.start_byte:end].rstrip(b"\r").endswith(b"\\"):
                end = source.find(b"\n", end + 1)
            if end == -1:
                end = len(source)
            directives[node.start_byte] = source[node.start_byte:end].decode("utf-8").replace("\r\n", "\n").strip()
        for child in node.children:
            if child.type in {"#endif", "#endregion"}:
                directives[child.start_byte] = text(child, source)
    ordered = [directives[pos] for pos in sorted(directives)]
    if not ordered:
        return {}
    # Relative position matters: moving a type outside an existing pack/nullable
    # region changes its contract without reordering the directives themselves.
    # Offsets are sorting aids only, never part of the saved identity.
    events = [(pos, 0, value) for pos, value in directives.items()]
    events.extend((pos, 1, "declaration: " + key) for pos, key in anchors)
    sequence = [value for pos, kind, value in sorted(events)]
    digest = hashlib.sha256(json.dumps(sequence, ensure_ascii=False).encode("utf-8")).hexdigest()
    identity = "/".join(filter(None, (assembly, label)))
    return {f"context:{language}:{identity}::preprocessor-order:{digest}": {
        "language": language, "owner": label, "kind": "binding", "file": label,
        "assembly": assembly, "line": 1, "ordered_directives": ordered,
        "compilation_context": sequence}}


def declaration_order(node, source: bytes) -> list[str]:
    """Freeze enum numbering and field layout without including method bodies.

    Private fields also affect public type layout. Keep conditional branches in
    lexical order; this is a conservative source contract, not an ABI calculator.
    Nested types own their separate order records.
    """
    body = node.child_by_field_name("body")
    ordered = []

    def collect(child):
        if child.type in {"field_declaration", "event_field_declaration", "property_declaration",
                          "enum_member_declaration", "enumerator"}:
            ordered.append(signature(child, source))
        elif child.type == "access_specifier":
            ordered.append("access: " + text(child, source))
        elif child.type in CONDITIONALS:
            ordered.append(conditional_header(child, source))
            for nested in child.named_children:
                collect(nested)
            ordered.append("end: " + child.type)
        elif child.type in {"preproc_define", "preproc_undef", "preproc_def", "preproc_function_def"}:
            ordered.append(signature(child, source))

    if body:
        for child in body.named_children:
            collect(child)
    return ordered


def ordered_signature(declaration, ordered):
    if ordered:
        digest = hashlib.sha256(json.dumps(ordered, ensure_ascii=False).encode("utf-8")).hexdigest()
        return f"{declaration} [ordered-members:{digest}]"
    return declaration


def parse(source: bytes, language: str, label: str):
    tree = PARSERS[language].parse(source)
    if tree.root_node.has_error:
        def declaration_error(node):
            # Some valid unsafe C# expressions exceed the grammar. A bounded
            # error wholly inside a method body cannot alter its declaration.
            parent = node.parent
            while parent:
                if parent.type in {"block", "compound_statement"}:
                    return bool(re.search(rb'[{}]|\b(public|protected|class|struct|interface)\b',
                                          source[node.start_byte:node.end_byte]))
                parent = parent.parent
            return True
        problems = [n for n in walk(tree.root_node)
                    if (n.type == "ERROR" or n.is_missing) and declaration_error(n)]
        if not problems:
            return tree
        detail = ", ".join(f"{n.start_point.row + 1}:{n.start_point.column + 1} {text(n, source)[:90]!r}"
                           for n in problems[:5])
        raise SurfaceError(f"Cannot fully parse {label}: {detail}")
    return tree


def managed_surface(source: bytes, label: str, assembly="") -> dict[str, dict]:
    return managed_surfaces([(source, label, assembly)])


def managed_surfaces(sources) -> dict[str, dict]:
    # Accessibility belongs to the merged partial type, within one assembly.
    # Nested partial types may need another pass after their parent is exposed.
    parsed = [(source, label, assembly, parse(source, "managed", label))
              for source, label, assembly in sources]
    partial_types = set()
    while True:
        before = set(partial_types)
        result = {}
        for source, label, assembly, tree in parsed:
            result.update(_managed_surface(source, label, assembly, tree, partial_types))
        if before == partial_types:
            return result


def _managed_surface(source, label, assembly, tree, partial_types):
    root = tree.root_node
    result, anchors = {}, []

    def add(node, owner, kind, sig=None):
        declaration = signature(node, source, is_type=kind == "type") if sig is None else sig
        ordered = declaration_order(node, source) if kind == "type" else []
        declaration = ordered_signature(declaration, ordered)
        conditions = conditional_context(node, source)
        declaration = conditional_signature(declaration, conditions)
        identity = "/".join(filter(None, (assembly, label, owner)))
        key = f"managed:{identity}::{declaration}"
        anchors.append((node.start_byte, key))
        result[key] = {"language": "managed", "owner": owner, "kind": kind,
                       "declaration": declaration, "file": label,
                       "assembly": assembly, "line": node.start_point.row + 1,
                       "ordered_members": ordered, "conditions": conditions}

    def visit(node, scope="", visible=True, interface=False):
        if node.type == "compilation_unit":
            for child in node.named_children:
                if child.type == "file_scoped_namespace_declaration":
                    scope = text(child.child_by_field_name("name"), source)
                else:
                    visit(child, scope, visible)
            return
        if node.type in {"namespace_declaration", "file_scoped_namespace_declaration"}:
            name = text(node.child_by_field_name("name"), source)
            for child in node.named_children:
                if child.type != "identifier":
                    visit(child, ".".join(filter(None, (scope, name))), visible)
            return
        if node.type in {"using_directive", "global_attribute", "preproc_define", "preproc_undef"}:
            add(node, scope or "<global>", "binding")
            return
        if node.type in CS_TYPES or node.type == "delegate_declaration":
            modifiers = {text(c, source) for c in node.named_children if c.type == "modifier"}
            name_node = node.child_by_field_name("name")
            if not name_node:
                raise SurfaceError(f"Unnamed managed type in {label}")
            name = text(name_node, source)
            parameters = next((c for c in node.named_children if c.type == "type_parameter_list"), None)
            if parameters:
                name += "`" + str(sum(c.type == "type_parameter" for c in parameters.named_children))
            owner = ".".join(filter(None, (scope, name)))
            implicit_partial = ("partial" in modifiers and
                                not modifiers & {"public", "protected", "internal", "private", "file"} and
                                (assembly, owner) in partial_types)
            exposed = visible and not modifiers & {"private", "file"} and (
                bool(modifiers & {"public", "protected"}) or implicit_partial or
                (interface and "internal" not in modifiers))
            if exposed and "partial" in modifiers:
                partial_types.add((assembly, owner))
            if exposed:
                add(node, owner, "type")
            body = node.child_by_field_name("body")
            if body:
                for child in body.named_children:
                    visit(child, owner, exposed, node.type == "interface_declaration")
            return
        if node.type in CS_MEMBERS:
            modifiers = {text(c, source) for c in node.named_children if c.type == "modifier"}
            if visible and "private" not in modifiers and (
                    bool(modifiers & {"public", "protected"}) or (interface and "internal" not in modifiers)):
                add(node, scope, "member")
            return
        if node.type == "enum_member_declaration":
            if visible:
                add(node, scope, "member")
            return
        # Namespace/declaration containers and preprocessor branches are transparent.
        if node.type not in {"block", "arrow_expression_clause", "attribute_list"}:
            for child in node.named_children:
                visit(child, scope, visible, interface)

    visit(root)
    result.update(preprocessor_context(root, source, label, "managed", assembly, anchors))
    return result


def mask_ue_macros(source: bytes):
    """Strip only known decoration macros, preserving offsets for diagnostics."""
    value = bytearray(source)
    decorations = []
    pattern = rb'\b(?:' + b'|'.join(x.encode() for x in sorted(UE_MACROS)) + rb')\s*\('
    lexical = re.sub(rb'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                     lambda m: b' ' * len(m.group()), source, flags=re.DOTALL)
    for match in re.finditer(pattern, lexical):
        depth, index, quote = 1, match.end(), None
        while index < len(source) and depth:
            char = source[index]
            if quote:
                if char == 92:
                    index += 2
                    continue
                if char == quote:
                    quote = None
            elif char in (34, 39):
                quote = char
            elif char == 40:
                depth += 1
            elif char == 41:
                depth -= 1
            index += 1
        if depth:
            raise SurfaceError("Unterminated Unreal decoration macro")
        decorations.append((match.start(), index, source[match.start():index].decode("utf-8")))
        for pos in range(match.start(), index):
            if value[pos] not in (10, 13):
                value[pos] = 32
    for match in re.finditer(rb'\bHARKEN[A-Z_]*_API\b', lexical):
        decorations.append((match.start(), match.end(), match.group().decode("ascii")))
        value[match.start():match.end()] = b' ' * (match.end() - match.start())
    return bytes(value), decorations


def unreal_surface(source: bytes, label: str) -> dict[str, dict]:
    masked, decorations = mask_ue_macros(source)
    tree = parse(masked, "unreal", label)
    root = tree.root_node
    result, anchors = {}, []
    # Comments are trivia when attaching prefix or trailing reflection metadata.
    trivia = bytearray(masked)
    for node in walk(root):
        if node.type == "comment":
            trivia[node.start_byte:node.end_byte] = b" " * (node.end_byte - node.start_byte)
    consumed = set()

    def metadata_for(node):
        prefix = node.start_byte
        while prefix and chr(trivia[prefix - 1]).isspace():
            prefix -= 1
        body = node.child_by_field_name("body")
        end = body.start_byte if body else node.end_byte
        if node.type == "enumerator":
            while end < len(trivia) and chr(trivia[end]).isspace():
                end += 1
        matched = [(start, raw) for start, stop, raw in decorations
                   if start >= prefix and stop <= end and
                   not raw.startswith(("GENERATED_BODY(", "GENERATED_UCLASS_BODY("))]
        consumed.update(start for start, raw in matched)
        return [re.sub(r"\s+", " ", raw).strip() for start, raw in sorted(matched)]

    def add(node, owner, kind, metadata=None):
        declaration = signature(node, masked, is_type=kind == "type")
        ordered = declaration_order(node, masked) if kind == "type" else []
        declaration = ordered_signature(declaration, ordered)
        conditions = conditional_context(node, masked)
        declaration = conditional_signature(declaration, conditions)
        metadata = metadata_for(node) if metadata is None else metadata
        if metadata:
            declaration = " ".join(metadata) + " " + declaration
        key = f"unreal:{label}/{owner}::{declaration}"
        anchors.append((node.start_byte, key))
        result[key] = {
            "language": "unreal", "owner": owner, "kind": kind,
            "declaration": declaration, "file": label, "line": node.start_point.row + 1,
            "ordered_members": ordered, "conditions": conditions}

    def visit(node, scope="", visible=True):
        if node.type == "template_declaration":
            raise SurfaceError(f"New C++ template needs explicit surface support: {label}")
        if node.type == "namespace_definition":
            name = node.child_by_field_name("name")
            owner = "::".join(filter(None, (scope, text(name, masked) if name else "<anonymous>")))
            for child in node.child_by_field_name("body").named_children:
                visit(child, owner, visible and name is not None)
            return
        if node.type in CPP_TYPES:
            name = node.child_by_field_name("name")
            if not name:
                raise SurfaceError(f"Unnamed public C++ type in {label}")
            owner = "::".join(filter(None, (scope, text(name, masked))))
            metadata = metadata_for(node)
            if visible or any(re.match(r"U(CLASS|STRUCT|ENUM)\s*\(", m) for m in metadata):
                add(node, owner, "type", metadata)
            body = node.child_by_field_name("body")
            if body:
                def members(children, states):
                    for child in children:
                        if child.type == "access_specifier":
                            states = {text(child, masked) in {"public", "protected"}}
                        elif child.type in {"preproc_if", "preproc_ifdef", "preproc_elif"}:
                            alternate = child.child_by_field_name("alternative")
                            branch = [c for c in child.named_children if c != alternate]
                            left = members(branch, set(states))
                            right = members([alternate], set(states)) if alternate else states
                            states = left | right
                        elif child.type == "preproc_else":
                            states = members(child.named_children, set(states))
                        else:
                            visit(child, owner, visible and any(states))
                    return states
                members(body.named_children, {node.type != "class_specifier"})
            return
        if node.type in {"field_declaration", "declaration", "function_definition",
                          "alias_declaration", "type_definition", "enumerator", "using_declaration"}:
            nested = [c for c in node.named_children if c.type in CPP_TYPES]
            if nested:
                for child in nested:
                    visit(child, scope, visible)
            else:
                metadata = metadata_for(node)
                # Reflection exposes private properties/functions independently
                # of their native C++ accessibility (including Blueprint access).
                if visible or any(re.match(r"U(PROPERTY|FUNCTION)\s*\(", m) for m in metadata):
                    add(node, scope, "member", metadata)
            return
        if node.type == "preproc_include":
            add(node, label, "binding")
            return
        if node.type in {"preproc_def", "preproc_function_def"}:
            if visible:
                add(node, label, "macro")
            return
        if node.type not in {"compound_statement", "comment"}:
            for child in node.named_children:
                visit(child, scope, visible)

    visit(root)
    for start, stop, raw in decorations:
        if start not in consumed and not re.match(r"GENERATED_(?:UCLASS_)?BODY\s*\(", raw):
            raise SurfaceError(f"Unattached Unreal decoration in {label}: {raw}")
    result.update(preprocessor_context(root, source, label, "unreal", anchors=anchors))
    return result


def inventory(root: Path) -> dict[str, dict]:
    result = {}
    table = load_json(root / "HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json")
    for fn in table["functions"]:
        name = fn["name"]
        if f"native:{name}" in result:
            raise SurfaceError(f"Duplicate native function: {name}")
        result[f"native:{name}"] = {"language": "native", "owner": name, "kind": "function",
                                   "native_public": fn.get("public", True),
                                   "file": "HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json"}
    sdk = root / "HarkenSDK/Unity/Harken"
    assemblies = {}
    sources = []
    for folder in (sdk / "Runtime", sdk / "Editor"):
        if list(folder.rglob("*.asmref")):
            raise SurfaceError("New asmref requires explicit assembly-resolution support")
        for path in folder.rglob("*.asmdef"):
            if path.parent in assemblies:
                raise SurfaceError(f"Multiple assembly definitions: {path.parent}")
            assemblies[path.parent] = load_json(path)["name"]
    for folder in (sdk / "Runtime", sdk / "Editor"):
        for path in sorted(folder.rglob("*.cs")):
            label = path.relative_to(root).as_posix()
            result[f"source:{label}"] = {"language": "managed", "owner": label,
                                         "kind": "binding", "file": label}
            assembly = next((assemblies[p] for p in path.parents if p in assemblies), None)
            if not assembly:
                raise SurfaceError(f"SDK source has no assembly definition: {label}")
            sources.append((path.read_bytes(), label, assembly))
        for path in sorted(folder.rglob("*.asmdef")):
            label = path.relative_to(root).as_posix()
            definition = json.dumps(load_json(path), sort_keys=True, separators=(",", ":"))
            result[f"assembly:{label}::{definition}"] = {
                "language": "managed", "owner": label, "kind": "binding", "file": label}
    result.update(managed_surfaces(sources))
    folder = root / "HarkenSDK/Unreal/Harken/Source/HarkenRuntime/Public"
    for path in sorted(p for p in folder.rglob("*") if p.is_file()):
        label = path.relative_to(root).as_posix()
        if path.suffix.lower() not in CPP_HEADERS:
            raise SurfaceError(f"Unmodelled Unreal public file needs explicit surface support: {label}")
        result[f"source:{label}"] = {"language": "unreal", "owner": label,
                                     "kind": "binding", "file": label}
        result.update(unreal_surface(path.read_bytes(), label))
    return dict(sorted(result.items()))


def validate(policy, observed) -> list[str]:
    errors = []
    if policy.get("schema_version") != 1:
        return ["Unsupported SDK surface policy version"]
    entries = {}
    for name, group in policy.get("groups", {}).items():
        if group.get("audience") not in AUDIENCES or group.get("disposition") not in DISPOSITIONS:
            errors.append(f"Invalid audience/disposition in {name}")
        if not group.get("rationale") or not group.get("consumer_contract"):
            errors.append(f"Missing rationale/consumer contract in {name}")
        if group.get("disposition") == "compatibility" and not group.get("retirement_condition"):
            errors.append(f"Missing retirement condition in {name}")
        for key in group.get("entries", []):
            if key in entries:
                errors.append(f"Multiply classified: {key}")
            entries[key] = group
    for key in sorted(observed.keys() - entries.keys()):
        errors.append(f"Unclassified SDK declaration: {key}")
    for key in sorted(entries.keys() - observed.keys()):
        errors.append(f"Removed or changed SDK declaration: {key}")
    for key in observed.keys() & entries.keys():
        if observed[key].get("native_public") is False and entries[key].get("disposition") != "tombstone":
            errors.append(f"Retired native entry must remain a tombstone: {key}")
        if observed[key].get("native_public") is True and entries[key].get("disposition") == "tombstone":
            errors.append(f"Native tombstone unexpectedly published: {key}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--policy", type=Path, default=POLICY)
    parser.add_argument("--inventory", type=Path, help="Write observations only; never approve APIs")
    args = parser.parse_args()
    try:
        observed = inventory(args.root)
        if args.inventory:
            args.inventory.parent.mkdir(parents=True, exist_ok=True)
            args.inventory.write_text(json.dumps(observed, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(f"Observed {len(observed)} SDK declarations; no audiences assigned.")
            return 0
        errors = validate(load_json(args.policy), observed)
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        print(f"Classified SDK surface verified: {len(observed)} declarations.")
        return 0
    except (SurfaceError, OSError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
