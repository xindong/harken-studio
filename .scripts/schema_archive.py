#!/usr/bin/env python3

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator


REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA_ROOT = REPO_ROOT / "HarkenSchema"
PROJECT_SCHEMA_NAME = "ProjectSerializedSchema.json"
PACK_SCHEMA_NAME = "PackManifestSchema.json"
TREE_SCHEMA_NAMES = {"Editor": PROJECT_SCHEMA_NAME, "Pack": PACK_SCHEMA_NAME}
GENERATED_PROJECT_HEADER = "ProjectSchemaGenerated.h"
GENERATED_PACK_HEADER = "PackSchemaGenerated.h"
GENERATED_VALIDATOR_HEADER = "ProjectSchemaValidator.generated.h"
GENERATED_VALIDATOR_SOURCE = "ProjectSchemaValidator.generated.cpp"
GENERATED_BUILD_HEADER = "SchemaBuildGenerated.h"
RULE_METHODS = {
    "uniqueAcrossProject": "uniqueAcrossProject",
    "documentIdMirrorsRoot": "documentIdMirrorsRoot",
    "projectIdUniform": "projectIdUniform",
    "referencesResolve": "referencesResolve",
    "acyclic": "acyclic",
    "singleton": "singleton",
    "schemaVersionUniform": "schemaVersionUniform",
}
REFERENCE_KINDS = {"guid", "object", "parent"}
SUPPORTED_RULES = {
    "uniqueAcrossProject",
    "documentIdMirrorsRoot",
    "projectIdUniform",
    "referencesResolve",
    "acyclic",
    "singleton",
    "schemaVersionUniform",
}
SUPPORTED_SCHEMA_KEYWORDS = {
    "$ref",
    "$schema",
    "additionalProperties",
    "const",
    "deprecated",
    "enum",
    "format",
    "if",
    "items",
    "maxItems",
    "maxLength",
    "maximum",
    "minItems",
    "minProperties",
    "minLength",
    "minimum",
    "pattern",
    "oneOf",
    "properties",
    "propertyNames",
    "required",
    "then",
    "type",
}


def canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def snapshot_hash(manifest: dict[str, Any]) -> str:
    payload = {key: value for key, value in manifest.items() if key != "snapshotHash"}
    return digest(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    )


def checked_name(value: Any, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or "/" in value
        or "\\" in value
        or value in {".", ".."}
    ):
        raise ValueError(f"invalid {label}")
    return value


def validate_local_refs(value: Any, defs: dict[str, Any]) -> None:
    if isinstance(value, dict):
        reference = value.get("$ref")
        if reference is not None:
            if not isinstance(reference, str) or not reference.startswith("#/$defs/"):
                raise ValueError(f"unsupported schema reference: {reference}")
            name = reference[len("#/$defs/") :].replace("~1", "/").replace("~0", "~")
            if "/" in name or name not in defs:
                raise ValueError(f"unresolved schema reference: {reference}")
        for child in value.values():
            validate_local_refs(child, defs)
    elif isinstance(value, list):
        for child in value:
            validate_local_refs(child, defs)


def validate_schema_language(schema: Any, path: str) -> None:
    if not isinstance(schema, dict):
        raise ValueError(f"{path} must be a schema object")
    unknown = set(schema) - SUPPORTED_SCHEMA_KEYWORDS
    if unknown:
        raise ValueError(f"{path} uses unsupported schema keywords: {sorted(unknown)}")
    schema_type = schema.get("type")
    supported_types = {
        "array",
        "boolean",
        "integer",
        "null",
        "number",
        "object",
        "string",
    }
    if isinstance(schema_type, list):
        if (
            not schema_type
            or len(schema_type) != len(set(schema_type))
            or any(item not in supported_types for item in schema_type)
        ):
            raise ValueError(f"{path} has unsupported types")
    elif schema_type is not None and schema_type not in supported_types:
        raise ValueError(f"{path} has an unsupported type")
    if "oneOf" in schema:
        alternatives = schema["oneOf"]
        if not isinstance(alternatives, list) or not alternatives:
            raise ValueError(f"{path}.oneOf must contain schemas")
        for index, child in enumerate(alternatives):
            validate_schema_language(child, f"{path}.oneOf[{index}]")
    properties = schema.get("properties", {})
    if not isinstance(properties, dict):
        raise ValueError(f"{path}.properties must be an object")
    for name, child in properties.items():
        if not isinstance(name, str):
            raise ValueError(f"{path}.properties contains an invalid name")
        validate_schema_language(child, f"{path}.properties.{name}")
    additional = schema.get("additionalProperties")
    if additional is not None and not isinstance(additional, bool):
        validate_schema_language(additional, f"{path}.additionalProperties")
    if "items" in schema:
        validate_schema_language(schema["items"], f"{path}.items")
    if "propertyNames" in schema:
        validate_schema_language(schema["propertyNames"], f"{path}.propertyNames")
    if ("if" in schema) != ("then" in schema):
        raise ValueError(f"{path} requires both if and then")
    for keyword in ("if", "then"):
        if keyword in schema:
            validate_schema_language(schema[keyword], f"{path}.{keyword}")
    required = schema.get("required", [])
    if not isinstance(required, list) or any(not isinstance(item, str) for item in required):
        raise ValueError(f"{path}.required must contain strings")
    enum = schema.get("enum")
    if enum is not None and (not isinstance(enum, list) or not enum):
        raise ValueError(f"{path}.enum must be a non-empty array")
    if schema.get("format") not in {None, "uuid"}:
        raise ValueError(f"{path} has an unsupported format")
    if "deprecated" in schema and not isinstance(schema["deprecated"], bool):
        raise ValueError(f"{path}.deprecated must be a boolean")
    for keyword in ("minLength", "maxLength", "minItems", "maxItems", "minProperties"):
        value = schema.get(keyword)
        if value is not None and (not isinstance(value, int) or value < 0):
            raise ValueError(f"{path}.{keyword} must be a non-negative integer")
    for keyword in ("minimum", "maximum"):
        value = schema.get(keyword)
        if value is not None and (not isinstance(value, (int, float)) or isinstance(value, bool)):
            raise ValueError(f"{path}.{keyword} must be numeric")
    if "pattern" in schema and not isinstance(schema["pattern"], str):
        raise ValueError(f"{path}.pattern must be a string")


def validate_document_type(item: dict[str, Any]) -> None:
    checked_name(item.get("type"), "document type")
    patterns = item.get("pathPatterns")
    if (
        not isinstance(patterns, list)
        or not patterns
        or any(not isinstance(pattern, str) or not pattern for pattern in patterns)
    ):
        raise ValueError(f"{item.get('type')} needs pathPatterns")
    if not isinstance(item.get("required"), bool):
        raise ValueError(f"{item.get('type')} needs a required flag")
    if not isinstance(item.get("schema"), dict):
        raise ValueError(f"{item.get('type')} needs a complete document schema")


def validate_project_source(source: dict[str, Any]) -> None:
    allowed = {
        "$defs", "$id", "$schema", "projectSchemaVersion",
        "x-harken-documentEnvelope", "x-harken-documentTypes",
        "x-harken-projectRules", "x-harken-extensionPolicy",
    }
    if not isinstance(source, dict) or set(source) - allowed:
        raise ValueError("project contract must be a complete ProjectSerializedSchema")
    version = source.get("projectSchemaVersion")
    if not isinstance(version, int) or isinstance(version, bool) or version < 1:
        raise ValueError("project contract needs a positive projectSchemaVersion")
    if source.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise ValueError("project contract needs JSON Schema draft 2020-12")
    documents = source.get("x-harken-documentTypes")
    defs = source.get("$defs")
    rules = source.get("x-harken-projectRules")
    if not isinstance(documents, list) or len(documents) != 6:
        raise ValueError("project schema must define exactly six document types")
    if not isinstance(defs, dict) or not isinstance(rules, list):
        raise ValueError("project schema needs $defs and x-harken-projectRules")
    for name, schema in defs.items():
        checked_name(name, "shared schema definition")
        validate_schema_language(schema, f"$defs.{name}")
        validate_local_refs(schema, defs)
    seen_types: set[str] = set()
    seen_patterns: set[str] = set()
    for item in documents:
        if not isinstance(item, dict):
            raise ValueError("documentTypes entries must be objects")
        validate_document_type(item)
        document_type = item["type"]
        if document_type in seen_types:
            raise ValueError(f"duplicate document type: {document_type}")
        seen_types.add(document_type)
        for pattern in item["pathPatterns"]:
            if pattern in seen_patterns:
                raise ValueError(f"overlapping document path pattern: {pattern}")
            seen_patterns.add(pattern)
        validate_local_refs(item["schema"], defs)
        validate_schema_language(item["schema"], f"{document_type}.schema")
        header = item["schema"].get("properties", {}).get("harkenDocument", {})
        properties = header.get("properties", {})
        if properties.get("schemaVersion", {}).get("const") != source["projectSchemaVersion"]:
            raise ValueError(f"{document_type} header has a different schema version")
        if properties.get("type", {}).get("const") != document_type:
            raise ValueError(f"{document_type} header has a different document type")
    expected = {
        "project.root",
        "project.layer",
        "project.bus",
        "project.action",
        "project.variable",
        "project.attenuation",
    }
    if seen_types != expected:
        raise ValueError("project schema document type set is incomplete")
    seen_rules: set[str] = set()
    for rule in rules:
        if not isinstance(rule, dict) or rule.get("rule") not in SUPPORTED_RULES:
            raise ValueError(f"unsupported project rule: {rule}")
        rule_name = rule["rule"]
        if rule_name in seen_rules:
            raise ValueError(f"duplicate project rule: {rule_name}")
        seen_rules.add(rule_name)
        allowed_fields = {
            "uniqueAcrossProject": {"rule", "fields"},
            "documentIdMirrorsRoot": {"rule", "appliesTo"},
            "projectIdUniform": {"rule"},
            "referencesResolve": {"rule", "references"},
            "acyclic": {"rule", "reference"},
            "singleton": {"rule", "documentType", "where", "message"},
            "schemaVersionUniform": {"rule"},
        }[rule_name]
        if set(rule) != allowed_fields:
            raise ValueError(f"invalid fields for project rule {rule_name}")
        if rule_name == "uniqueAcrossProject":
            fields = rule["fields"]
            if (
                not isinstance(fields, list)
                or not fields
                or len(set(fields)) != len(fields)
                or any(
                    field
                    not in {
                        "guid",
                        "runtimeStableId",
                        "documentId",
                        "folderMetadataPath",
                    }
                    for field in fields
                )
            ):
                raise ValueError("uniqueAcrossProject has invalid fields")
        elif rule_name == "documentIdMirrorsRoot":
            applies_to = rule["appliesTo"]
            if (
                not isinstance(applies_to, list)
                or not applies_to
                or len(set(applies_to)) != len(applies_to)
                or any(item not in seen_types for item in applies_to)
            ):
                raise ValueError("documentIdMirrorsRoot has invalid appliesTo")
        elif rule_name == "referencesResolve":
            references = rule["references"]
            if not isinstance(references, list) or not references:
                raise ValueError("referencesResolve needs references")
            seen_references: set[tuple[str, ...]] = set()
            for reference in references:
                if not isinstance(reference, dict):
                    raise ValueError("referencesResolve entries must be objects")
                allowed = {
                    "kind",
                    "path",
                    "targetTypes",
                    "allowNull",
                    "selectorField",
                    "targetTypesBySelector",
                }
                if set(reference) - allowed:
                    raise ValueError("referencesResolve entry has invalid fields")
                path = reference.get("path")
                if (
                    not isinstance(path, list)
                    or not path
                    or any(not isinstance(segment, str) or not segment for segment in path)
                    or tuple(path) in seen_references
                ):
                    raise ValueError("referencesResolve entry has an invalid path")
                seen_references.add(tuple(path))
                if reference.get("kind") not in REFERENCE_KINDS:
                    raise ValueError("referencesResolve entry has an invalid kind")
                target_types = reference.get("targetTypes", [])
                if (
                    not isinstance(target_types, list)
                    or any(not isinstance(item, str) or not item for item in target_types)
                ):
                    raise ValueError("referencesResolve entry has invalid targetTypes")
                if not isinstance(reference.get("allowNull", False), bool):
                    raise ValueError("referencesResolve entry has invalid allowNull")
                selector = reference.get("selectorField", "")
                mappings = reference.get("targetTypesBySelector", {})
                if not isinstance(selector, str) or not isinstance(mappings, dict):
                    raise ValueError("referencesResolve entry has an invalid selector")
                if bool(selector) != bool(mappings):
                    raise ValueError("referencesResolve selector must have a type map")
                for value, types in mappings.items():
                    if (
                        not isinstance(value, str)
                        or not isinstance(types, list)
                        or not types
                        or any(not isinstance(item, str) or not item for item in types)
                    ):
                        raise ValueError("referencesResolve selector has invalid target types")
        elif rule_name == "acyclic":
            reference = rule["reference"]
            if not isinstance(reference, list) or not reference or any(
                not isinstance(segment, str) or not segment for segment in reference
            ):
                raise ValueError("acyclic has an invalid reference path")
        elif rule_name == "singleton":
            where = rule["where"]
            if (
                rule["documentType"] not in seen_types
                or not isinstance(where, dict)
                or not isinstance(rule["message"], str)
                or not rule["message"]
            ):
                raise ValueError("singleton has an invalid document type or predicate")
            if set(where) != {"path", "equals"}:
                raise ValueError("singleton has an invalid predicate")
            if (
                not isinstance(where["path"], list)
                or not where["path"]
                or any(not isinstance(segment, str) or not segment for segment in where["path"])
                or not isinstance(where["equals"], str)
            ):
                raise ValueError("singleton has an invalid predicate")
    envelope = source.get("x-harken-documentEnvelope")
    if envelope != "harkenDocument-v1":
        raise ValueError(f"unsupported document envelope: {envelope}")


def validate_pack_source(source: dict[str, Any]) -> None:
    if not isinstance(source, dict):
        raise ValueError("Pack contract must be a JSON object")
    metadata = source.get("x-harken-format")
    if not isinstance(metadata, dict) or metadata.get("formatId") != "generated.pack-manifest":
        raise ValueError("Pack contract must describe generated.pack-manifest")
    version = metadata.get("formatVersion")
    if not isinstance(version, int) or isinstance(version, bool) or version < 1:
        raise ValueError("Pack contract needs a positive formatVersion")
    if metadata.get("validationMode") != "generated-json":
        raise ValueError("Pack contract must use generated-json validation")
    if metadata.get("versionLocator") != "json:$.schema_version":
        raise ValueError("Pack contract must locate schema_version")
    if metadata.get("rebuildPolicy") != "rebuild":
        raise ValueError("Pack contract must be rebuildable")
    for field in ("pathPatterns", "readers", "writers", "validators"):
        items = metadata.get(field)
        if not isinstance(items, list) or not items or any(
            not isinstance(item, str) or not item for item in items
        ):
            raise ValueError(f"Pack contract needs {field}")
    schema = {key: value for key, value in source.items() if key != "x-harken-format"}
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema" or schema.get("type") != "object":
        raise ValueError("Pack contract needs a JSON object schema")
    if schema.get("properties", {}).get("schema_version", {}).get("const") != version:
        raise ValueError("Pack contract schema_version does not match formatVersion")
    validate_schema_language(schema, "Pack")
    validate_local_refs(schema, schema.get("$defs", {}))


def validate_tree_manifest(tree: str, manifest: Any) -> None:
    if tree not in TREE_SCHEMA_NAMES:
        raise ValueError(f"unknown schema tree: {tree}")
    if not isinstance(manifest, dict) or set(manifest) != {
        "manifestVersion", "tip", "published", "supportedHistorical"
    }:
        raise ValueError(f"{tree} manifest needs manifestVersion, tip, published and supportedHistorical")
    if type(manifest["manifestVersion"]) is not int or manifest["manifestVersion"] != 1:
        raise ValueError(f"{tree} manifestVersion must be 1")
    if manifest["tip"] != "current":
        raise ValueError(f"{tree} manifest tip must be current")
    for field in ("published", "supportedHistorical"):
        versions = manifest[field]
        if (
            not isinstance(versions, list)
            or any(type(version) is not int or version < 1 for version in versions)
            or versions != sorted(set(versions))
        ):
            raise ValueError(f"{tree} {field} must be a sorted list of unique positive integers")
    if not manifest["published"]:
        raise ValueError(f"{tree} published must not be empty")
    historical = set(manifest["published"][:-1])
    if not set(manifest["supportedHistorical"]).issubset(historical):
        raise ValueError(f"{tree} supportedHistorical must select published historical generations")


def source_version(tree: str, source: dict[str, Any]) -> int:
    if tree == "Editor":
        return source["projectSchemaVersion"]
    if tree == "Pack":
        return source["x-harken-format"]["formatVersion"]
    raise ValueError(f"unknown schema tree: {tree}")


def validate_tree_source(tree: str, source: dict[str, Any]) -> None:
    if tree == "Editor":
        validate_project_source(source)
    elif tree == "Pack":
        validate_pack_source(source)
    else:
        raise ValueError(f"unknown schema tree: {tree}")


def materialize_source(tree: str, source: dict[str, Any], version: int) -> dict[str, Any]:
    if type(version) is not int or version < 1:
        raise ValueError("materialized schema version must be a positive integer")
    if not isinstance(source, dict):
        raise ValueError(f"{tree} current contract must be a JSON object")
    result = copy.deepcopy(source)
    if tree == "Editor":
        if "projectSchemaVersion" in result or result.get("$id") != "harken://schema/Editor/current":
            raise ValueError("Editor current must use its current identity without a product integer")
        result["projectSchemaVersion"] = version
        result["$id"] = f"harken://schema/Editor/{version}"
        documents = result.get("x-harken-documentTypes")
        if not isinstance(documents, list):
            raise ValueError("Editor current needs complete document schemas")
        for item in documents:
            try:
                slot = item["schema"]["properties"]["harkenDocument"]["properties"]["schemaVersion"]
            except (KeyError, TypeError) as error:
                raise ValueError("Editor current needs a schemaVersion field in each document") from error
            if slot != {"type": "integer"}:
                raise ValueError("Editor current schemaVersion must be an integer without a fixed value")
            slot["const"] = version
    elif tree == "Pack":
        metadata = result.get("x-harken-format")
        if not isinstance(metadata, dict) or "formatVersion" in metadata:
            raise ValueError("Pack current must omit the formatVersion product integer")
        slot = result.get("properties", {}).get("schema_version")
        if slot != {"type": "integer"}:
            raise ValueError("Pack current schema_version must be an integer without a fixed value")
        metadata["formatVersion"] = version
        slot["const"] = version
    else:
        raise ValueError(f"unknown schema tree: {tree}")
    validate_tree_source(tree, result)
    return result


def load_tree(tree: str, schema_root: Path | None = None) -> dict[str, Any]:
    if tree not in TREE_SCHEMA_NAMES:
        raise ValueError(f"unknown schema tree: {tree}")
    tree_root = (SCHEMA_ROOT if schema_root is None else Path(schema_root)) / tree
    manifest = json.loads((tree_root / "manifest.json").read_text(encoding="utf-8"))
    validate_tree_manifest(tree, manifest)
    expected = {"manifest.json", "current", *map(str, manifest["published"])}
    unexpected = {path.name for path in tree_root.iterdir()} - expected - {"README.md"}
    if unexpected:
        raise ValueError(f"{tree} contains unexpected paths: {sorted(unexpected)}")
    name = TREE_SCHEMA_NAMES[tree]
    sources: dict[str, dict[str, Any]] = {}
    for folder in ["current", *map(str, manifest["published"])]:
        source_root = tree_root / folder
        if not source_root.is_dir() or {path.name for path in source_root.iterdir()} != {name}:
            raise ValueError(f"{tree}/{folder} must contain only {name}")
        sources[folder] = json.loads((source_root / name).read_text(encoding="utf-8"))
    published: dict[int, dict[str, Any]] = {}
    for version in manifest["published"]:
        source = sources[str(version)]
        validate_tree_source(tree, source)
        if source_version(tree, source) != version:
            raise ValueError(f"{tree}/{version} schema version does not match its directory")
        published[version] = source
    version = manifest["published"][-1]
    current = materialize_source(tree, sources["current"], version)
    return {
        "manifest": manifest,
        "currentSource": sources["current"],
        "current": current,
        "currentVersion": version,
        "published": published,
        "supported": [published[item] for item in manifest["supportedHistorical"]] + [current],
    }


def read_project_sources(schema_root: Path | None = None) -> list[dict[str, Any]]:
    return load_tree("Editor", schema_root)["supported"]


def read_pack_sources(schema_root: Path | None = None) -> list[dict[str, Any]]:
    return load_tree("Pack", schema_root)["supported"]


def load_source(schema_root: Path | None = None) -> dict[str, Any]:
    return load_tree("Editor", schema_root)["current"]


def file_entries(files: dict[str, bytes]) -> list[dict[str, Any]]:
    return [
        {"path": path, "sha256": digest(data), "size": len(data)}
        for path, data in sorted(files.items())
    ]


def generated_project_header(source: dict[str, Any], versions: list[int]) -> bytes:
    models = {
        document["schema"]["properties"]["harkenDocument"]["properties"]
        .get("propertyModel", {}).get("const")
        for document in source["x-harken-documentTypes"]
    }
    if models != {None} and (len(models) != 1 or not isinstance(next(iter(models)), str)):
        raise ValueError("project document propertyModel literals must match")
    property_model = [] if models == {None} else [
        "inline constexpr char kProjectPropertyModel[] = "
        + json.dumps(next(iter(models))) + ";"
    ]
    lines = [
        "#pragma once",
        "",
        "namespace VNX::ProjectSchema {",
        "",
        f"inline constexpr int kProjectSchemaVersion = {source['projectSchemaVersion']};",
        "inline constexpr int kProjectSchemaVersions[] = {"
        + ", ".join(str(version) for version in versions) + "};",
        *property_model,
        "",
        "}",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def generated_pack_header(pack: list[dict[str, Any]]) -> bytes:
    version = pack[-1]["x-harken-format"]["formatVersion"]
    lines = [
        "#pragma once",
        "",
        "namespace VNX::PackSchema {",
        "",
        f"inline constexpr int kPackManifestSchemaVersion = {version};",
        "",
        "}",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def cpp_identifier(value: str) -> str:
    result = "".join(character if character.isalnum() else "_" for character in value)
    return result if not result[:1].isdigit() else f"v_{result}"


def cpp_qstring(value: str) -> str:
    return f"QStringLiteral({json.dumps(value, ensure_ascii=True)})"


def cpp_qstring_list(values: list[str]) -> str:
    return "QStringList{" + ", ".join(cpp_qstring(value) for value in values) + "}"


def cpp_json_value(value: Any) -> str:
    if value is None:
        return "QJsonValue(QJsonValue::Null)"
    if isinstance(value, bool):
        return f"QJsonValue({'true' if value else 'false'})"
    if isinstance(value, str):
        return f"QJsonValue({cpp_qstring(value)})"
    if isinstance(value, (int, float)):
        return f"QJsonValue({repr(value)})"
    raise ValueError(f"unsupported generated JSON literal: {value}")


class CppSchemaEmitter:
    def __init__(self) -> None:
        self.functions: list[dict[str, Any]] = []
        self.names: dict[int, str] = {}
        self.refs: dict[int, dict[str, str]] = {}
        self.counter = 0

    def add_context(
        self,
        prefix: str,
        definitions: dict[str, Any],
        roots: dict[str, dict[str, Any]],
    ) -> dict[str, str]:
        context: dict[str, Any] = {"definitions": {}}
        for name, schema in sorted(definitions.items()):
            function = f"validate{cpp_identifier(prefix)}Node{self.counter}"
            self.counter += 1
            context["definitions"][name] = function
            self._add(function, schema, context)
        result: dict[str, str] = {}
        for name, schema in roots.items():
            function = f"validate{cpp_identifier(prefix)}Node{self.counter}"
            self.counter += 1
            result[name] = function
            self._add(function, schema, context)
        return result

    def _add(self, function: str, schema: dict[str, Any], context: dict[str, Any]) -> None:
        identity = id(schema)
        if identity in self.names:
            return
        self.names[identity] = function
        self.refs[identity] = context["definitions"]
        self.functions.append((function, schema, context))
        if "$ref" in schema:
            return
        for field, child in sorted(schema.get("properties", {}).items()):
            child_function = f"validateNode{self.counter}"
            self.counter += 1
            self._add(child_function, child, context)
        for child in schema.get("oneOf", []):
            child_function = f"validateNode{self.counter}"
            self.counter += 1
            self._add(child_function, child, context)
        if isinstance(schema.get("items"), dict):
            child_function = f"validateNode{self.counter}"
            self.counter += 1
            self._add(child_function, schema["items"], context)
        if isinstance(schema.get("additionalProperties"), dict):
            child_function = f"validateNode{self.counter}"
            self.counter += 1
            self._add(child_function, schema["additionalProperties"], context)
        if "propertyNames" in schema:
            child_function = f"validateNode{self.counter}"
            self.counter += 1
            self._add(child_function, schema["propertyNames"], context)
        for keyword in ("if", "then"):
            if keyword in schema:
                child_function = f"validateNode{self.counter}"
                self.counter += 1
                self._add(child_function, schema[keyword], context)

    def _function(self, function: str, schema: dict[str, Any], context: dict[str, Any]) -> str:
        lines = [
            f"void {function}(const QJsonValue& value, ValidationContext& context, const QString& path)",
            "{",
        ]
        if "$ref" in schema:
            name = schema["$ref"].removeprefix("#/$defs/").replace("~1", "/").replace("~0", "~")
            lines.extend([f"    {context['definitions'][name]}(value, context, path);", "}"])
            return "\n".join(lines)
        if "if" in schema:
            condition = self.names[id(schema["if"])]
            consequence = self.names[id(schema["then"])]
            lines.extend([
                "    QStringList conditionErrors;",
                "    ValidationContext conditionContext(QString(), conditionErrors);",
                f"    {condition}(value, conditionContext, path);",
                "    if (conditionErrors.isEmpty())",
                f"        {consequence}(value, context, path);",
            ])
        types = schema.get("type")
        if isinstance(types, str):
            types = [types]
        if isinstance(types, list):
            values = ", ".join(
                f"JsonType::{cpp_identifier(item).capitalize()}" for item in types
            )
            lines.append(f"    if (!context.requireTypes(value, {{{values}}}, path)) return;")
        if "oneOf" in schema:
            alternatives = ", ".join(self.names[id(child)] for child in schema["oneOf"])
            lines.append(f"    context.requireOneOf(value, {{{alternatives}}}, path);")
        if "const" in schema:
            lines.append(
                f"    context.requireConst(value, {cpp_json_value(schema['const'])}, path);"
            )
        if "enum" in schema:
            allowed = ", ".join(cpp_json_value(item) for item in schema["enum"])
            lines.append(f"    context.requireEnum(value, QJsonArray{{{allowed}}}, path);")
        for keyword, method in (
            ("minimum", "requireMinimum"),
            ("maximum", "requireMaximum"),
            ("minLength", "requireMinLength"),
            ("maxLength", "requireMaxLength"),
            ("minItems", "requireMinItems"),
            ("minProperties", "requireMinProperties"),
            ("maxItems", "requireMaxItems"),
        ):
            if keyword in schema:
                lines.append(f"    context.{method}(value, {schema[keyword]}, path);")
        if schema.get("format") == "uuid":
            lines.append("    context.requireUuid(value, path);")
        if "pattern" in schema:
            lines.append(
                f"    context.requirePattern(value, {cpp_qstring(schema['pattern'])}, path);"
            )
        if isinstance(schema.get("items"), dict):
            child = self.names[id(schema["items"])]
            lines.extend(
                [
                    "    if (value.isArray()) {",
                    "        const QJsonArray array = value.toArray();",
                    "        for (qsizetype index = 0; index < array.size(); ++index)",
                    f"            {child}(array.at(index), context, path + QStringLiteral(\"[%1]\").arg(index));",
                    "    }",
                ]
            )
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        additional = schema.get("additionalProperties")
        if properties or required or additional is not None or "propertyNames" in schema:
            lines.extend(["    if (!value.isObject()) return;", "    const QJsonObject object = value.toObject();"])
            if "propertyNames" in schema:
                child = self.names[id(schema["propertyNames"])]
                lines.extend([
                    "    for (auto it = object.constBegin(); it != object.constEnd(); ++it)",
                    f"        {child}(it.key(), context, context.childPath(path, it.key()));",
                ])
            if required:
                lines.append(f"    context.requireFields(object, {cpp_qstring_list(required)}, path);")
            for field, child_schema in sorted(properties.items()):
                child = self.names[id(child_schema)]
                literal = cpp_qstring(field)
                lines.extend(
                    [
                        f"    if (object.contains({literal}))",
                        f"        {child}(object.value({literal}), context, context.childPath(path, {literal}));",
                    ]
                )
            known = cpp_qstring_list(sorted(properties))
            if additional is False:
                lines.append(f"    context.rejectUnknownFields(object, {known}, path);")
            elif isinstance(additional, dict):
                child = self.names[id(additional)]
                lines.extend(
                    [
                        f"    const QStringList knownFields = {known};",
                        "    for (auto it = object.constBegin(); it != object.constEnd(); ++it)",
                        "        if (!knownFields.contains(it.key()))",
                        f"            {child}(it.value(), context, context.childPath(path, it.key()));",
                    ]
                )
        lines.append("}")
        return "\n".join(lines)

    def render(self) -> str:
        declarations = "\n".join(
            f"void {name}(const QJsonValue&, ValidationContext&, const QString&);"
            for name, _, _ in self.functions
        )
        definitions = "\n\n".join(
            self._function(name, schema, context)
            for name, schema, context in self.functions
        )
        return f"{declarations}\n\n{definitions}"


def generated_rule_call(rule: dict[str, Any]) -> str:
    name = rule["rule"]
    if name == "uniqueAcrossProject":
        return f"executor.uniqueAcrossProject({cpp_qstring_list(rule['fields'])});"
    if name == "documentIdMirrorsRoot":
        return f"executor.documentIdMirrorsRoot({cpp_qstring_list(rule['appliesTo'])});"
    if name == "referencesResolve":
        values = []
        kinds = {"guid": "Guid", "object": "Object", "parent": "Parent"}
        for item in rule["references"]:
            mappings = item.get("targetTypesBySelector", {})
            mapping_cpp = "QHash<QString, QStringList>{" + ", ".join(
                "{" + cpp_qstring(key) + ", " + cpp_qstring_list(value) + "}"
                for key, value in sorted(mappings.items())
            ) + "}"
            values.append(
                "VNX::ProjectSchemaReferenceRule{"
                f"VNX::ProjectSchemaReferenceKind::{kinds[item['kind']]}, "
                f"{cpp_qstring_list(item['path'])}, "
                f"{cpp_qstring_list(item.get('targetTypes', []))}, "
                f"{'true' if item.get('allowNull', False) else 'false'}, "
                f"{cpp_qstring(item.get('selectorField', ''))}, {mapping_cpp}"
                "}"
            )
        return "executor.referencesResolve(QList<VNX::ProjectSchemaReferenceRule>{" + ", ".join(values) + "});"
    if name == "acyclic":
        return f"executor.acyclic({cpp_qstring_list(rule['reference'])});"
    if name == "singleton":
        where = rule["where"]
        return (
            f"executor.singleton({cpp_qstring(rule['documentType'])}, "
            f"{cpp_qstring_list(where['path'])}, {cpp_qstring(where['equals'])}, "
            f"{cpp_qstring(rule['message'])});"
        )
    return f"executor.{RULE_METHODS[name]}();"


def generated_validator_registry(
    releases: list[dict[str, Any]],
    pack: list[dict[str, Any]],
) -> tuple[bytes, bytes]:
    header = """#pragma once

#include "ProjectSchemaRules.h"

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace VNX::GeneratedSchemaRegistry {

bool supportsProjectVersion(int version);
bool supportsPackVersion(int version);
bool validateProjectDocument(int version, QStringView documentType,
                             const QJsonValue& document,
                             const QString& relativePath,
                             QStringList& errors);
bool validatePackManifest(int version,
                          const QJsonValue& document,
                          const QString& relativePath,
                          QStringList& errors);
bool validateProjectRules(int version,
                          VNX::ProjectSchemaRuleExecutor& executor,
                          QStringList& errors);

}
""".encode("ascii")
    emitter = CppSchemaEmitter()
    project_functions: dict[tuple[int, str], str] = {}
    for release in releases:
        schema = release
        roots = {
            item["type"]: item["schema"]
            for item in schema["x-harken-documentTypes"]
        }
        functions = emitter.add_context(
            f"ProjectV{release['projectSchemaVersion']}", schema["$defs"], roots
        )
        for document_type, function in functions.items():
            project_functions[(release["projectSchemaVersion"], document_type)] = function
    pack_functions: dict[int, str] = {}
    for contract in pack:
        version = contract["x-harken-format"]["formatVersion"]
        schema = {key: value for key, value in contract.items() if key != "x-harken-format"}
        functions = emitter.add_context(
            f"PackV{version}", schema.get("$defs", {}), {"root": schema},
        )
        pack_functions[version] = functions["root"]
    project_cases = "\n".join(
        f"    case {source['projectSchemaVersion']}: return true;"
        for source in releases
    )
    pack_check = " || ".join(f"version == {version}" for version in pack_functions)
    project_document_cases = []
    for release in releases:
        version = release["projectSchemaVersion"]
        checks = []
        for document_type in sorted(
            item["type"] for item in release["x-harken-documentTypes"]
        ):
            checks.extend(
                [
                    f"        if (documentType == QLatin1String({json.dumps(document_type)})) {{",
                    f"            {project_functions[(version, document_type)]}(document, context, QStringLiteral(\"$\"));",
                    "            return true;",
                    "        }",
                ]
            )
        checks.extend(
            [
                "        errors.append(QStringLiteral(\"%1 $: expected an available verified Schema, got unknown document type %2\").arg(relativePath, documentType));",
                "        return false;",
            ]
        )
        project_document_cases.append(f"    case {version}:\n" + "\n".join(checks))
    pack_document_cases = [
        f"    case {version}:\n"
        f"        {function}(document, context, QStringLiteral(\"$\"));\n"
        "        return true;"
        for version, function in pack_functions.items()
    ]
    project_rule_cases = []
    for release in releases:
        calls = [f"        {generated_rule_call(rule)}" for rule in release["x-harken-projectRules"]]
        project_rule_cases.append(
            "    case %d:\n%s\n        return true;"
            % (release["projectSchemaVersion"], "\n".join(calls))
        )
    helpers = r'''enum class JsonType
{
    Array,
    Boolean,
    Integer,
    Null,
    Number,
    Object,
    String
};

bool isInteger(const QJsonValue& value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble();
}

QString typeName(JsonType type)
{
    switch (type) {
    case JsonType::Array: return QStringLiteral("array");
    case JsonType::Boolean: return QStringLiteral("boolean");
    case JsonType::Integer: return QStringLiteral("integer");
    case JsonType::Null: return QStringLiteral("null");
    case JsonType::Number: return QStringLiteral("number");
    case JsonType::Object: return QStringLiteral("object");
    case JsonType::String: return QStringLiteral("string");
    }
    return QStringLiteral("undefined");
}

QString valueType(const QJsonValue& value)
{
    if (value.isArray()) return QStringLiteral("array");
    if (value.isBool()) return QStringLiteral("boolean");
    if (value.isDouble()) return isInteger(value) ? QStringLiteral("integer") : QStringLiteral("number");
    if (value.isNull()) return QStringLiteral("null");
    if (value.isObject()) return QStringLiteral("object");
    if (value.isString()) return QStringLiteral("string");
    return QStringLiteral("undefined");
}

QString valueText(const QJsonValue& value)
{
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
    return valueType(value);
}

bool matchesType(const QJsonValue& value, JsonType type)
{
    switch (type) {
    case JsonType::Array: return value.isArray();
    case JsonType::Boolean: return value.isBool();
    case JsonType::Integer: return isInteger(value);
    case JsonType::Null: return value.isNull();
    case JsonType::Number: return value.isDouble();
    case JsonType::Object: return value.isObject();
    case JsonType::String: return value.isString();
    }
    return false;
}

class ValidationContext
{
public:
    ValidationContext(const QString& file, QStringList& errors)
        : file_(file), errors_(errors)
    {
    }

    using Validator = void (*)(const QJsonValue&, ValidationContext&, const QString&);

    void requireOneOf(const QJsonValue& value, std::initializer_list<Validator> validators, const QString& path)
    {
        int matches = 0;
        for (const auto validator : validators) {
            QStringList errors;
            ValidationContext branch(file_, errors);
            validator(value, branch, path);
            if (errors.isEmpty()) ++matches;
        }
        if (matches != 1)
            append(path, QStringLiteral("exactly one supported document shape"), QString::number(matches));
    }

    QString childPath(const QString& path, const QString& field) const
    {
        return path + QLatin1Char('.') + field;
    }

    bool requireTypes(const QJsonValue& value,
                      std::initializer_list<JsonType> types,
                      const QString& path)
    {
        QStringList expected;
        for (JsonType type : types) {
            expected.append(typeName(type));
            if (matchesType(value, type)) return true;
        }
        append(path, expected.join(QStringLiteral(" or ")), valueType(value));
        return false;
    }

    void requireConst(const QJsonValue& value, const QJsonValue& expected,
                      const QString& path)
    {
        if (value != expected)
            append(path, QStringLiteral("the fixed value %1").arg(valueText(expected)), valueText(value));
    }

    void requireEnum(const QJsonValue& value, const QJsonArray& allowed,
                     const QString& path)
    {
        for (const QJsonValue& candidate : allowed)
            if (candidate == value) return;
        append(path, QStringLiteral("an allowed value"), valueText(value));
    }

    void requireMinimum(const QJsonValue& value, double minimum, const QString& path)
    {
        if (value.isDouble() && value.toDouble() < minimum)
            append(path, QStringLiteral("a number no smaller than %1").arg(minimum), valueText(value));
    }

    void requireMaximum(const QJsonValue& value, double maximum, const QString& path)
    {
        if (value.isDouble() && value.toDouble() > maximum)
            append(path, QStringLiteral("a number no larger than %1").arg(maximum), valueText(value));
    }

    void requireMinLength(const QJsonValue& value, qsizetype minimum, const QString& path)
    {
        if (value.isString() && value.toString().size() < minimum)
            append(path, QStringLiteral("a string of at least %1 characters").arg(minimum), QString::number(value.toString().size()));
    }

    void requireMaxLength(const QJsonValue& value, qsizetype maximum, const QString& path)
    {
        if (value.isString() && value.toString().size() > maximum)
            append(path, QStringLiteral("a string of at most %1 characters").arg(maximum), QString::number(value.toString().size()));
    }

    void requireMinItems(const QJsonValue& value, qsizetype minimum, const QString& path)
    {
        if (value.isArray() && value.toArray().size() < minimum)
            append(path, QStringLiteral("at least %1 items").arg(minimum), QString::number(value.toArray().size()));
    }

    void requireMaxItems(const QJsonValue& value, qsizetype maximum, const QString& path)
    {
        if (value.isArray() && value.toArray().size() > maximum)
            append(path, QStringLiteral("at most %1 items").arg(maximum), QString::number(value.toArray().size()));
    }

    void requireMinProperties(const QJsonValue& value, qsizetype minimum, const QString& path)
    {
        if (value.isObject() && value.toObject().size() < minimum)
            append(path, QStringLiteral("at least %1 fields").arg(minimum), QString::number(value.toObject().size()));
    }

    void requireUuid(const QJsonValue& value, const QString& path)
    {
        if (value.isString() && QUuid::fromString(value.toString()).isNull())
            append(path, QStringLiteral("a valid UUID"), value.toString());
    }

    void requirePattern(const QJsonValue& value, const QString& pattern,
                        const QString& path)
    {
        if (value.isString() && !QRegularExpression(pattern).match(value.toString()).hasMatch())
            append(path, QStringLiteral("a string matching %1").arg(pattern), value.toString());
    }

    void requireFields(const QJsonObject& object, const QStringList& fields,
                       const QString& path)
    {
        for (const QString& field : fields)
            if (!object.contains(field))
                append(childPath(path, field), QStringLiteral("a required value"), QStringLiteral("missing"));
    }

    void rejectUnknownFields(const QJsonObject& object, const QStringList& fields,
                             const QString& path)
    {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (!fields.contains(it.key()))
                append(childPath(path, it.key()), QStringLiteral("a declared field"), QStringLiteral("Unknown field"));
    }

private:
    void append(const QString& path, const QString& expected, const QString& actual)
    {
        errors_.append(QStringLiteral("%1 %2: expected %3, got %4").arg(file_, path, expected, actual));
    }

    QString file_;
    QStringList& errors_;
};'''
    source = f'''#include "ProjectSchemaValidator.generated.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QRegularExpression>
#include <QUuid>

#include <cmath>
#include <initializer_list>

namespace VNX::GeneratedSchemaRegistry {{

namespace {{

{helpers}

{emitter.render()}

}}

bool supportsProjectVersion(int version)
{{
    switch (version) {{
{project_cases}
    default: return false;
    }}
}}

bool supportsPackVersion(int version)
{{
    return {pack_check};
}}

bool validateProjectDocument(int version, QStringView documentType,
                             const QJsonValue& document,
                             const QString& relativePath,
                             QStringList& errors)
{{
    ValidationContext context(relativePath, errors);
    switch (version) {{
{chr(10).join(project_document_cases)}
    default:
        errors.append(QStringLiteral("%1 $: expected an available verified Schema, got unsupported project version").arg(relativePath));
        return false;
    }}
}}

bool validatePackManifest(int version,
                          const QJsonValue& document,
                          const QString& relativePath,
                          QStringList& errors)
{{
    ValidationContext context(relativePath, errors);
    switch (version) {{
{chr(10).join(pack_document_cases)}
    default:
        errors.append(QStringLiteral("%1 $: expected an available verified Schema, got unsupported Pack version").arg(relativePath));
        return false;
    }}
}}

bool validateProjectRules(int version,
                          VNX::ProjectSchemaRuleExecutor& executor,
                          QStringList& errors)
{{
    switch (version) {{
{chr(10).join(project_rule_cases)}
    default:
        errors.append(QStringLiteral("unsupported project Schema version"));
        return false;
    }}
}}

}}
'''.encode("ascii")
    return header, source


def build_bundle(schema_root: Path | None = None) -> dict[str, Any]:
    schema_root = SCHEMA_ROOT if schema_root is None else Path(schema_root)
    unexpected = {path.name for path in schema_root.iterdir()} - set(TREE_SCHEMA_NAMES) - {"README.md"}
    if unexpected:
        raise ValueError(f"schema root contains unexpected paths: {sorted(unexpected)}")
    trees = {tree: load_tree(tree, schema_root) for tree in TREE_SCHEMA_NAMES}
    installed: dict[str, bytes] = {}
    inputs: dict[str, bytes] = {}
    tree_versions: dict[str, Any] = {}
    for tree, loaded in trees.items():
        version = loaded["currentVersion"]
        name = TREE_SCHEMA_NAMES[tree]
        inputs[f"{tree}/manifest.json"] = canonical_bytes(loaded["manifest"])
        inputs[f"{tree}/current/{name}"] = canonical_bytes(loaded["current"])
        for published_version, source in loaded["published"].items():
            path = f"{tree}/{published_version}/{name}"
            if published_version == version:
                installed[path] = canonical_bytes(loaded["current"])
            else:
                installed[path] = canonical_bytes(source)
                inputs[path] = installed[path]
        tree_versions[tree] = {
            "currentVersion": version,
            "supportedVersions": [source_version(tree, source) for source in loaded["supported"]],
        }
    editor = trees["Editor"]
    pack = trees["Pack"]
    validator_header, validator_source = generated_validator_registry(editor["supported"], pack["supported"])
    generated = {
        GENERATED_PROJECT_HEADER: generated_project_header(
            editor["current"], tree_versions["Editor"]["supportedVersions"]
        ),
        GENERATED_PACK_HEADER: generated_pack_header(pack["supported"]),
        GENERATED_VALIDATOR_HEADER: validator_header,
        GENERATED_VALIDATOR_SOURCE: validator_source,
    }
    manifest = {
        "manifestVersion": 1,
        "trees": tree_versions,
        "files": file_entries(installed),
        "inputs": file_entries(inputs),
        "generated": file_entries(generated),
    }
    manifest["snapshotHash"] = snapshot_hash(manifest)
    installed["build.json"] = canonical_bytes(manifest)
    generated[GENERATED_BUILD_HEADER] = (
        "#pragma once\n\nnamespace VNX::SchemaBuild {\n\n"
        f'inline constexpr char kSnapshotHash[] = "{manifest["snapshotHash"]}";\n'
        "\n}\n"
    ).encode("ascii")
    return {
        "manifest": manifest,
        "installed": installed,
        "generated": generated,
        "trees": trees,
        "sourceRoot": schema_root.resolve(),
    }


def installed_files(bundle: dict[str, Any] | None = None) -> dict[str, bytes]:
    return dict((build_bundle() if bundle is None else bundle)["installed"])


def generated_files(bundle: dict[str, Any] | None = None) -> dict[str, bytes]:
    return dict((build_bundle() if bundle is None else bundle)["generated"])


def write_generated_code(target: Path, bundle: dict[str, Any]) -> None:
    target = target.resolve()
    source_roots = ((REPO_ROOT / "HarkenStudio").resolve(), SCHEMA_ROOT.resolve(), bundle["sourceRoot"])
    for source_root in source_roots:
        if target == source_root or source_root in target.parents:
            raise ValueError("generated Schema code must not be written under HarkenStudio or HarkenSchema")
    target.mkdir(parents=True, exist_ok=True)
    for name, data in generated_files(bundle).items():
        destination = target / name
        if not destination.is_file() or destination.read_bytes() != data:
            destination.write_bytes(data)


def check_generated(target: Path, bundle: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for name, expected in generated_files(bundle).items():
        path = target / name
        if not path.is_file():
            errors.append(f"missing generated schema file: {name}")
        elif path.read_bytes() != expected:
            errors.append(f"generated schema file does not match current inputs: {name}")
    return errors


@contextmanager
def publish_lock(target: Path) -> Iterator[None]:
    lock_root = Path(tempfile.gettempdir()) / "harken-schema-publish"
    lock_root.mkdir(exist_ok=True)
    identity = digest(os.fsencode(os.path.normcase(str(target))))
    with (lock_root / f"{identity}.lock").open("a+b") as handle:
        if os.name == "nt":
            import msvcrt

            if handle.seek(0, os.SEEK_END) == 0:
                handle.write(b"0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def publish_bundle(bundle: dict[str, Any], target: Path) -> None:
    target = target.resolve()
    if (
        target.name.lower() != "schema"
        or any(
            target == source_root
            or source_root in target.parents
            or target in source_root.parents
            for source_root in (SCHEMA_ROOT.resolve(), bundle["sourceRoot"])
        )
    ):
        raise ValueError("published Schema target must be a separate schema directory")
    with publish_lock(target):
        files = installed_files(bundle)
        expected_directories = {
            parent.as_posix()
            for relative in files
            for parent in Path(relative).parents
            if parent != Path(".")
        }
        target.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f".{target.name}-publish-", dir=target.parent) as directory:
            staging = Path(directory)
            for relative, data in files.items():
                prepared = staging / relative
                prepared.parent.mkdir(parents=True, exist_ok=True)
                prepared.write_bytes(data)
            for relative in sorted(files, key=lambda path: (path == "build.json", path)):
                destination = target / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not destination.is_file() or destination.read_bytes() != files[relative]:
                    os.replace(staging / relative, destination)
        for path in sorted(target.rglob("*"), key=lambda item: len(item.parts), reverse=True):
            relative = path.relative_to(target).as_posix()
            if path.is_dir() and not path.is_symlink():
                if relative not in expected_directories:
                    try:
                        path.rmdir()
                    except FileNotFoundError:
                        pass
            elif relative not in files:
                path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--schema-root", type=Path)
    parser.add_argument("--publish", type=Path)
    parser.add_argument("--generate-code", type=Path)
    parser.add_argument("--check-generated", type=Path)
    args = parser.parse_args()
    try:
        bundle = build_bundle(args.schema_root)
        if args.check_generated:
            errors = check_generated(args.check_generated, bundle)
            if errors:
                print("\n".join(errors), file=sys.stderr)
                return 1
        if args.generate_code:
            write_generated_code(args.generate_code, bundle)
        if args.publish:
            publish_bundle(bundle, args.publish)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"schema generation failed: {error}", file=sys.stderr)
        return 1
    trees = bundle["manifest"]["trees"]
    print(
        f"Editor {trees['Editor']['currentVersion']}, Pack {trees['Pack']['currentVersion']}, "
        f"snapshot {bundle['manifest']['snapshotHash']}, {len(bundle['installed'])} installed files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
