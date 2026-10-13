#!/usr/bin/env python3
"""Emit checked-in Monitor record fixtures. Do not treat live output as gold.

Container = little-endian u64 recordCount followed by packed records.
No Session magic, chunk header, CRC, or commit footer.
`--check` never rewrites artifacts; any SHA / bytes / include drift fails.
Default write upserts `lifecycle/legal_event.bin` and its manifest entry.
It never drops extra manifest paths (M2-2 lineage stays).
Root `legal_event.bin` remains the M0-2 decoder fixture.
`--check` inventories only this record manifest's owned paths.
plan/, remote/, and session/ keep their own manifests.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
OUT = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor"
MANIFEST = OUT / "manifest.json"
DECODER_H = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor_fixture_decode.h"
DECODER_CC = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor_fixture_decode.cpp"

PACKED = 316
KIND_EVENT = 5
FLAG_REQUIRED = 1
STREAM_EVENT = 1 << 1
VALID_ARMED_NUMERIC = 3
EVENT_PAYLOAD_BYTES = 48  # sizeof(VnMonitorEventPayload)

ENTITY_PLAYABLE = 2
ENTITY_ACTION = 3
ENTITY_CONTAINER = 7
ENTITY_QUEUE = 8
ENTITY_TRANSITION = 9

EVENT_ACCEPT = 2
EVENT_REJECT = 3
EVENT_START = 4
EVENT_STOP = 5
EVENT_RESULT = 6
EVENT_QUEUE = 8

REASON_NONE = 0
REASON_NATURAL = 1
REASON_STOP_REQUESTED = 2
REASON_WORLD_REPLACED = 5


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def u64(v: int) -> bytes:
    return struct.pack("<Q", v)


def aligned_record_bytes(header_bytes: int, payload_bytes: int = 0) -> int:
    total = header_bytes + payload_bytes
    remainder = total & 7
    if remainder:
        total += 8 - remainder
    return total


def pack_event_record(
    *,
    producer_id: int,
    source_sequence: int,
    session_epoch: int,
    world_generation: int,
    execute_frame: int,
    entity_kind: int,
    instance_handle: int,
    instance_generation: int,
    action_instance_handle: int,
    playable_instance_handle: int,
    reason: int,
    validity_flags: int,
    event_class: int,
    engine_monitor_instance_id: int = 0,
    capture_plan_generation: int = 0,
    render_plan_generation: int = 0,
    transport_epoch: int = 0,
    device_epoch: int = 0,
    clock_anchor_generation: int = 0,
    measurement_segment_generation: int = 0,
    stable_id: int = 0,
) -> bytes:
    record_bytes = aligned_record_bytes(PACKED, EVENT_PAYLOAD_BYTES)
    parts = [
        u32(KIND_EVENT),
        u32(1),
        u32(FLAG_REQUIRED),
        u32(STREAM_EVENT),
        u64(record_bytes),
        u64(1),  # streamId
        u64(producer_id),
        u64(source_sequence),
        u64(0),  # deliverySequence
        u64(engine_monitor_instance_id),
        u64(session_epoch),
        u64(world_generation),
        u64(capture_plan_generation),
        u64(render_plan_generation),
        u64(transport_epoch),
        u64(device_epoch),
        u64(clock_anchor_generation),
        u64(measurement_segment_generation),
        u64(execute_frame),
        u64(0),  # graphFrame
        u64(0),  # devicePresentationEstimate
        u32(0),  # deviceUncertaintyFrames
        u32(entity_kind),
        u64(0),  # programSemanticFingerprint
        u64(0),  # runtimeLineageFingerprint
        u64(0),  # optionalSymbolFingerprint
        struct.pack(">QQ", 0x3DA91A64B9474560 if stable_id else 0, stable_id),
        u64(instance_handle),
        u64(instance_generation),
        u64(action_instance_handle),
        u64(playable_instance_handle),
        u64(0),  # parentActionInstanceHandle
        u64(0),  # traceCorrelationId
        u32(reason),
        u32(validity_flags),
        u64(EVENT_PAYLOAD_BYTES),
        u32(0),  # telemetry.reserved0
        u64(0),  # telemetry.arrangingInstance
        bytes(16),  # telemetry.clipId
        u32(0),  # telemetry.loopIteration
        u32(0),  # telemetry.occurrenceOrdinal
        u64(0),  # telemetry.logicCommitSequence
        u64(0),  # telemetry.executeFrame
        u32(EVENT_PAYLOAD_BYTES),  # VnMonitorEventPayload.structSize
        u32(event_class),
        u32(reason),
        u32(0),  # reserved0
        u64(playable_instance_handle),
        u64(action_instance_handle),
        u64(0),  # voiceInstanceHandle
        u64(0),  # parentActionInstanceHandle
    ]
    blob = b"".join(parts)
    packed_with_payload = PACKED + EVENT_PAYLOAD_BYTES
    if len(blob) != packed_with_payload:
        raise SystemExit("packed size %d != %d" % (len(blob), packed_with_payload))
    if record_bytes > packed_with_payload:
        blob += b"\x00" * (record_bytes - packed_with_payload)
    if len(blob) != record_bytes:
        raise SystemExit("record size %d != %d" % (len(blob), record_bytes))
    return blob


def pack_event_header() -> bytes:
    return pack_event_record(
        producer_id=1,
        source_sequence=1,
        session_epoch=1,
        world_generation=1,
        execute_frame=1,
        entity_kind=0,
        instance_handle=0,
        instance_generation=0,
        action_instance_handle=0,
        playable_instance_handle=0,
        reason=0,
        validity_flags=VALID_ARMED_NUMERIC,
        event_class=EVENT_START,
        engine_monitor_instance_id=1,
        capture_plan_generation=1,
        render_plan_generation=1,
        transport_epoch=1,
        device_epoch=1,
        clock_anchor_generation=1,
        measurement_segment_generation=1,
    )


def container_bytes() -> bytes:
    return struct.pack("<Q", 1) + pack_event_header()


def lifecycle_event_specs() -> list[dict]:
    # Conversion table samples. Playable/Container/Transition reason=0 is the
    # published producer value (ObserveMonitorLifecycle writes 0). Action STOP/
    # REJECT=StopRequested(2), RESULT=Natural(1). Queue REJECT=WorldReplaced(5).
    # CONTAINER RESULT/STOP are the same Playable mapping with Container kind.
    rows = [
        (ENTITY_PLAYABLE, EVENT_START, REASON_NONE),
        (ENTITY_PLAYABLE, EVENT_RESULT, REASON_NONE),
        (ENTITY_PLAYABLE, EVENT_STOP, REASON_NONE),
        (ENTITY_PLAYABLE, EVENT_REJECT, REASON_NONE),
        (ENTITY_CONTAINER, EVENT_START, REASON_NONE),
        (ENTITY_CONTAINER, EVENT_RESULT, REASON_NONE),
        (ENTITY_CONTAINER, EVENT_STOP, REASON_NONE),
        (ENTITY_CONTAINER, EVENT_REJECT, REASON_NONE),
        (ENTITY_ACTION, EVENT_ACCEPT, REASON_NONE, {"world": 7, "action_is_self": True}),
        (ENTITY_ACTION, EVENT_STOP, REASON_STOP_REQUESTED, {"world": 7, "action_is_self": True}),
        (ENTITY_ACTION, EVENT_RESULT, REASON_NATURAL, {"world": 7, "action_is_self": True}),
        (ENTITY_ACTION, EVENT_REJECT, REASON_STOP_REQUESTED, {"world": 7, "action_is_self": True}),
        (ENTITY_QUEUE, EVENT_QUEUE, REASON_NONE, {"world": 7, "playable": 11}),
        (ENTITY_QUEUE, EVENT_REJECT, REASON_WORLD_REPLACED, {"world": 7, "playable": 11}),
        (ENTITY_TRANSITION, EVENT_START, REASON_NONE),
        (ENTITY_TRANSITION, EVENT_STOP, REASON_NONE),
        (ENTITY_TRANSITION, EVENT_REJECT, REASON_NONE),
    ]
    specs = []
    for index, row in enumerate(rows, start=1):
        entity, event_class, reason = row[:3]
        extra = row[3] if len(row) == 4 else {}
        handle = 0x1000 + index
        playable = extra.get("playable", handle)
        action = handle if extra.get("action_is_self") else extra.get("action", 0)
        specs.append(
            {
                "entity": entity,
                "event_class": event_class,
                "reason": reason,
                "world": extra.get("world", 0),
                "seq": index,
                "frame": index,
                "handle": handle,
                "generation": 1,
                "stable": 0x2000 + index,
                "action": action,
                "playable": playable,
            }
        )
    return specs


def pack_lifecycle_event(spec: dict) -> bytes:
    return pack_event_record(
        producer_id=3,
        source_sequence=spec["seq"],
        session_epoch=0,
        world_generation=spec["world"],
        execute_frame=spec["frame"],
        entity_kind=spec["entity"],
        instance_handle=spec["handle"],
        instance_generation=spec["generation"],
        action_instance_handle=spec["action"],
        playable_instance_handle=spec["playable"],
        reason=spec["reason"],
        validity_flags=0,
        event_class=spec["event_class"],
        stable_id=spec["stable"],
    )


def lifecycle_container_bytes() -> bytes:
    records = [pack_lifecycle_event(spec) for spec in lifecycle_event_specs()]
    return struct.pack("<Q", len(records)) + b"".join(records)


def file_sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def lifecycle_entries(lifecycle_digest: str, *, out: Path | None = None) -> list[dict]:
    root = OUT if out is None else out
    files = [
        (root / "lifecycle" / "legal_event.bin", "legal-event", lifecycle_digest),
        (root / "lifecycle" / "gap_variable_control.json", "gap-variable-control", None),
        (root / "lifecycle" / "gap_queue_cursor.json", "gap-queue-cursor", None),
        (
            root / "lifecycle" / "gap_playable_world_generation.json",
            "gap-playable-world-generation",
            None,
        ),
    ]
    entries = []
    for path, kind, digest in files:
        if kind != "legal-event" and not path.is_file():
            raise SystemExit("checked-in fixture missing: %s" % path)
        relative = path.relative_to(root).as_posix()
        sha = digest if digest is not None else file_sha(path)
        entries.append(
            {
                "path": relative,
                "sha256": sha,
                "isProvisional": False,
                "kind": kind,
                "producerPackage": "m2-1",
                "schemaVersion": 1,
            }
        )
    return entries


def load_manifest() -> dict:
    if not MANIFEST.is_file():
        return {"schemaVersion": 1, "isProvisional": False, "files": []}
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def upsert_manifest_files(files: list, entries: list[dict]) -> list:
    by_path = {}
    for item in files:
        path = item.get("path")
        if path:
            by_path[path] = dict(item)
    for entry in entries:
        existing = by_path.get(entry["path"], {})
        merged = dict(existing)
        merged.update(entry)
        by_path[entry["path"]] = merged
    out = []
    seen = set()
    for item in files:
        path = item.get("path")
        if not path or path in seen:
            continue
        out.append(by_path[path])
        seen.add(path)
    for entry in entries:
        if entry["path"] not in seen:
            out.append(by_path[entry["path"]])
            seen.add(entry["path"])
    return out


def write_manifest(m0_digest: str, lifecycle_digest: str) -> str:
    man = load_manifest()
    man["schemaVersion"] = 1
    man["isProvisional"] = False
    entries = [
        {
            "path": "legal_event.bin",
            "sha256": m0_digest,
            "isProvisional": False,
            "kind": "legal-event",
        }
    ] + lifecycle_entries(lifecycle_digest)
    man["files"] = upsert_manifest_files(man.get("files") or [], entries)
    return json.dumps(man, indent=2) + "\n"


def check_decoder_includes() -> None:
    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
    allowed = {
        DECODER_H: {"harken/Harken.h", "stddef.h", "stdint.h"},
        DECODER_CC: {"monitor_fixture_decode.h", "cstring"},
    }
    forbidden_needles = (
        "Runtime/api/src",
        "harken/internal",
        "core/include",
        "harken_remote/",
    )
    for path, allow in allowed.items():
        text = path.read_text(encoding="utf-8")
        for needle in forbidden_needles:
            if needle in text:
                raise SystemExit("decoder private include: %s in %s" % (needle, path.name))
        found = []
        for line in text.splitlines():
            match = include_re.match(line)
            if match:
                found.append(match.group(1).replace("\\", "/"))
        extra = [name for name in found if name not in allow]
        if extra:
            raise SystemExit("decoder include not allowed in %s: %s" % (path.name, extra))
        missing = allow - set(found)
        if missing:
            raise SystemExit("decoder include missing in %s: %s" % (path.name, sorted(missing)))


def manifest_relative_path(raw: str) -> str:
    text = str(raw or "").replace("\\", "/").strip()
    if not text or text == "manifest.json":
        raise SystemExit("manifest path")
    if text.startswith("/") or text.startswith("//"):
        raise SystemExit("manifest path")
    parts = [part for part in text.split("/") if part not in ("", ".")]
    if not parts or any(part == ".." for part in parts):
        raise SystemExit("manifest path")
    if any(":" in part for part in parts):
        raise SystemExit("manifest path")
    return "/".join(parts)


def owned_disk_paths(root: Path, listed: set[str]) -> set[str]:
    owned = set()
    for relative in listed:
        path = root.joinpath(*relative.split("/"))
        try:
            path.resolve().relative_to(root.resolve())
        except ValueError as exc:
            raise SystemExit("manifest path escapes fixture root: %s" % relative) from exc
        if path.is_file():
            owned.add(relative)
        elif path.exists():
            raise SystemExit("owned path is not a file: %s" % relative)
    return owned


def extra_owned_disk_paths(root: Path, listed: set[str]) -> set[str]:
    extra = set()
    if root.is_dir():
        for child in root.iterdir():
            if not child.is_file() or child.name == "manifest.json":
                continue
            if child.name not in listed:
                extra.add(child.name)
    owned_dirs = {relative.split("/", 1)[0] for relative in listed if "/" in relative}
    for dirname in owned_dirs:
        directory = root / dirname
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if relative not in listed:
                extra.add(relative)
    return extra


def classify_owned_inventory(root: Path, listed: set[str]) -> tuple[set[str], set[str], set[str]]:
    present = owned_disk_paths(root, listed)
    missing = listed - present
    extra = extra_owned_disk_paths(root, listed)
    return present, missing, extra


def check_artifacts(m0_payload: bytes, m0_digest: str, lifecycle_payload: bytes,
                    lifecycle_digest: str, *, out: Path | None = None,
                    manifest: Path | None = None) -> None:
    root = OUT if out is None else out
    manifest_path = MANIFEST if manifest is None else manifest
    artifact = root / "legal_event.bin"
    lifecycle_artifact = root / "lifecycle" / "legal_event.bin"
    if not artifact.is_file() or not manifest_path.is_file():
        raise SystemExit("checked-in fixture missing")
    if artifact.read_bytes() != m0_payload:
        raise SystemExit("fixture drift: legal_event.bin")
    if not lifecycle_artifact.is_file():
        raise SystemExit("checked-in fixture missing: lifecycle/legal_event.bin")
    if lifecycle_artifact.read_bytes() != lifecycle_payload:
        raise SystemExit("fixture drift: lifecycle/legal_event.bin")
    man = json.loads(manifest_path.read_text(encoding="utf-8"))
    if man.get("schemaVersion") != 1:
        raise SystemExit("manifest schemaVersion")
    if man.get("isProvisional"):
        raise SystemExit("provisional fixture")
    files = man.get("files") or []
    by_path = {}
    for item in files:
        relative = manifest_relative_path(item.get("path"))
        if relative in by_path:
            raise SystemExit("duplicate manifest path: %s" % relative)
        by_path[relative] = item
    legal = by_path.get("legal_event.bin")
    if legal is None:
        raise SystemExit("manifest path")
    if legal.get("sha256") != m0_digest:
        raise SystemExit("manifest sha mismatch")
    if legal.get("isProvisional"):
        raise SystemExit("provisional fixture")
    if legal.get("producerPackage") == "m2-1":
        raise SystemExit("root legal_event.bin is m0-2, not m2-1")
    expected = lifecycle_entries(lifecycle_digest, out=root)
    for entry in expected:
        listed_item = by_path.get(entry["path"])
        if listed_item is None:
            raise SystemExit("manifest missing %s" % entry["path"])
        if listed_item.get("sha256") != entry["sha256"]:
            raise SystemExit("manifest sha mismatch: %s" % entry["path"])
        if listed_item.get("isProvisional"):
            raise SystemExit("provisional fixture")
        if listed_item.get("producerPackage") != "m2-1":
            raise SystemExit("producerPackage: %s" % entry["path"])
        if listed_item.get("schemaVersion") != 1:
            raise SystemExit("schemaVersion: %s" % entry["path"])
    listed = set(by_path)
    present, missing, extra = classify_owned_inventory(root, listed)
    if extra or missing:
        raise SystemExit(
            "manifest/files mismatch extra=%s missing=%s present=%s"
            % (sorted(extra), sorted(missing), sorted(present))
        )
    for relative in sorted(present):
        item = by_path[relative]
        expected_sha = item.get("sha256")
        if not expected_sha:
            raise SystemExit("manifest sha missing: %s" % relative)
        actual_sha = file_sha(root.joinpath(*relative.split("/")))
        if actual_sha != expected_sha:
            raise SystemExit("manifest sha mismatch: %s" % relative)
    if out is None:
        check_decoder_includes()
    print("ok:", artifact)
    print("ok:", lifecycle_artifact)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    m0_payload = container_bytes()
    m0_digest = hashlib.sha256(m0_payload).hexdigest()
    lifecycle_payload = lifecycle_container_bytes()
    lifecycle_digest = hashlib.sha256(lifecycle_payload).hexdigest()
    if args.check:
        check_artifacts(m0_payload, m0_digest, lifecycle_payload, lifecycle_digest)
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "lifecycle").mkdir(parents=True, exist_ok=True)
    (OUT / "legal_event.bin").write_bytes(m0_payload)
    (OUT / "lifecycle" / "legal_event.bin").write_bytes(lifecycle_payload)
    MANIFEST.write_text(write_manifest(m0_digest, lifecycle_digest), encoding="utf-8")
    print("wrote", OUT / "legal_event.bin")
    print("wrote", OUT / "lifecycle" / "legal_event.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
