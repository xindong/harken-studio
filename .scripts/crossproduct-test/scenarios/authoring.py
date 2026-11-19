import json
import math
import struct
import wave
from pathlib import Path

SAMPLE_RATE = 48000


def write_tone(path: Path, frequency: float, seconds: float, amplitude: float = 0.3) -> Path:
    frames = int(SAMPLE_RATE * seconds)
    with wave.open(str(path), "wb") as stream:
        stream.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
        stream.writeframes(b"".join(
            struct.pack("<h", int(32767 * amplitude * math.sin(2 * math.pi * frequency * i / SAMPLE_RATE)))
            for i in range(frames)))
    return path


def inspector_fields(studio, project: Path, schema_id: str) -> dict:
    schema = studio.call(project, "get_inspector_schema", {"schema_id": schema_id})["schema"]
    fields = {}

    def walk(node):
        if isinstance(node, dict):
            if "field" in node:
                fields.setdefault(node["field"], node)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(schema)
    return fields


def option(fields: dict, field: str, text: str):
    for entry in fields[field].get("options", []):
        if entry["text"] == text:
            return entry["value"]
    raise LookupError(f"Studio no longer offers {field}={text!r}; offered: {fields[field].get('options')}")


def export_all(studio, project: Path, output: Path) -> Path:
    platform = studio.call(project, "list_project_platforms")["platforms"][0]["id"]
    result = studio.call(project, "export_pack", {"output_path": str(output), "platform_id": platform,
                                                  "all": True, "force": True})
    if not result.get("success"):
        raise AssertionError(f"export failed: {json.dumps(result)[:2000]}")
    directories = {path.parent for path in output.rglob("Init.bytes")}
    if len(directories) != 1:
        raise AssertionError(f"expected one exported platform directory, found {sorted(map(str, directories))}")
    return directories.pop()


def exported_actions(directory: Path) -> dict:
    actions = {}
    for metadata in directory.glob("*.json"):
        if metadata.stem == "Init":
            continue
        document = json.loads(metadata.read_text(encoding="utf-8"))
        actions[document["name"]] = {"id": document["guid"], "pack": str(metadata.with_suffix(".bytes"))}
    return actions
