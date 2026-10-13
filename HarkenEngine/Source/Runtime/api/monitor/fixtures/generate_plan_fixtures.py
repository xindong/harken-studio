#!/usr/bin/env python3
"""Emit checked-in Monitor plan/registry/lease contract fixtures.

Container = little-endian u64 recordCount followed by packed records.
No Session magic, chunk header, CRC, or commit footer.
`--check` never rewrites artifacts; any SHA / bytes / table drift fails.
Zero ABI: payloads reuse M0-2 structs only. Expected JSON is the contract.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
OUT = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor" / "plan"
TABLE = Path(__file__).resolve().parent / "plan_state_table.json"
MANIFEST = OUT / "manifest.json"
HARKEN_H = REPO / "HarkenEngine" / "Source" / "Runtime" / "api" / "include" / "harken" / "Harken.h"


def packed_bytes_from_header() -> int:
    text = HARKEN_H.read_text(encoding="utf-8")
    match = re.search(
        r"#define\s+VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES\s+(\d+)u", text
    )
    if not match:
        raise SystemExit("missing VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES")
    return int(match.group(1))


PACKED = packed_bytes_from_header()
if PACKED != 316:
    raise SystemExit("packed envelope drifted from 316: %d" % PACKED)
KIND_TIME_SERIES = 6
KIND_DROP = 13
KIND_PLAN_ACTIVATED = 15
KIND_TAP_BINDING = 16
KIND_REGISTRY = 21
FLAG_REQUIRED = 1
STREAM_TIME_SERIES = 1 << 2
STREAM_EVENT = 1 << 1
TIMESERIES_PAYLOAD_BYTES = 32
DROP_PAYLOAD_BYTES = 56
EVENT_PAYLOAD_BYTES = 48
REASON_STOP = 5
REASON_APPLY = 12


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


def pack_header(
    *,
    kind: int,
    stream_kind: int,
    payload_bytes: int,
    source_sequence: int,
    capture_plan_generation: int,
    execute_frame: int,
    validity: int,
    reason: int,
    stable_id: int = 0,
    instance_generation: int = 0,
    instance_handle: int = 0,
) -> bytes:
    record_bytes = aligned_record_bytes(PACKED, payload_bytes)
    parts = [
        u32(kind),
        u32(1),
        u32(FLAG_REQUIRED),
        u32(stream_kind),
        u64(record_bytes),
        u64(1),  # streamId
        u64(1),  # producerId
        u64(source_sequence),
        u64(0),  # deliverySequence
        u64(1),  # engineMonitorInstanceId
        u64(1),  # monitorSessionEpoch
        u64(1),  # worldGeneration
        u64(capture_plan_generation),
        u64(1),  # renderPlanGeneration
        u64(1),  # transportEpoch
        u64(1),  # deviceEpoch
        u64(1),  # clockAnchorGeneration
        u64(1),  # measurementSegmentGeneration
        u64(execute_frame),
        u64(0),  # graphFrame
        u64(0),  # devicePresentationEstimate
        u32(0),  # deviceUncertaintyFrames
        u32(0),  # entityKind
        u64(0),  # programSemanticFingerprint
        u64(0),  # runtimeLineageFingerprint
        u64(0),  # optionalSymbolFingerprint
        stable_id.to_bytes(16, "big"),
        u64(instance_handle),
        u64(instance_generation),
        u64(0),  # actionInstanceHandle
        u64(0),  # playableInstanceHandle
        u64(0),  # parentActionInstanceHandle
        u64(0),  # traceCorrelationId
        u32(reason),
        u32(validity),
        u64(payload_bytes),
        u32(0),  # telemetry.reserved0
        u64(0),  # telemetry.arrangingInstance
        bytes(16),
        u32(0),  # telemetry.loopIteration
        u32(0),  # telemetry.occurrenceOrdinal
        u64(0),  # telemetry.logicCommitSequence
        u64(0),  # telemetry.executeFrame
    ]
    blob = b"".join(parts)
    if len(blob) != PACKED:
        raise SystemExit("packed header size %d != %d" % (len(blob), PACKED))
    return blob, record_bytes


def pad_record(header: bytes, payload: bytes, record_bytes: int) -> bytes:
    blob = header + payload
    if len(blob) > record_bytes:
        raise SystemExit("payload overflow")
    if len(blob) < record_bytes:
        blob += b"\x00" * (record_bytes - len(blob))
    return blob


def pack_time_series(seq: int, plan_gen: int, frame: int, validity: int, value_bits: int) -> bytes:
    header, record_bytes = pack_header(
        kind=KIND_TIME_SERIES,
        stream_kind=STREAM_TIME_SERIES,
        payload_bytes=TIMESERIES_PAYLOAD_BYTES,
        source_sequence=seq,
        capture_plan_generation=plan_gen,
        execute_frame=frame,
        validity=validity,
        reason=0,
    )
    payload = b"".join(
        [
            u32(TIMESERIES_PAYLOAD_BYTES),
            u32(1),  # metricId
            u32(60),
            u32(1),
            u32(1),
            u32(1),
            u32(value_bits),
            u32(0),
        ]
    )
    if len(payload) != TIMESERIES_PAYLOAD_BYTES:
        raise SystemExit("timeseries payload %d" % len(payload))
    return pad_record(header, payload, record_bytes)


def pack_drop(seq: int, plan_gen: int, first_seq: int, last_seq: int) -> bytes:
    header, record_bytes = pack_header(
        kind=KIND_DROP,
        stream_kind=STREAM_EVENT,
        payload_bytes=DROP_PAYLOAD_BYTES,
        source_sequence=seq,
        capture_plan_generation=plan_gen,
        execute_frame=seq,
        validity=0,
        reason=0,
    )
    payload = b"".join(
        [
            u32(DROP_PAYLOAD_BYTES),
            u32(1),  # SOURCE_PRODUCER
            u64(1),
            u64(first_seq),
            u64(last_seq),
            u64(last_seq - first_seq + 1),
            u64((last_seq - first_seq + 1) * 32),
            u32(0),
            u32(0),
        ]
    )
    if len(payload) != DROP_PAYLOAD_BYTES:
        raise SystemExit("drop payload %d" % len(payload))
    return pad_record(header, payload, record_bytes)


def pack_plan_activated(seq: int, plan_gen: int, prev_gen: int, reason: int, frame: int) -> bytes:
    header, record_bytes = pack_header(
        kind=KIND_PLAN_ACTIVATED,
        stream_kind=STREAM_EVENT,
        payload_bytes=EVENT_PAYLOAD_BYTES,
        source_sequence=seq,
        capture_plan_generation=plan_gen,
        execute_frame=frame,
        validity=0,
        reason=reason,
        instance_generation=prev_gen,
        instance_handle=plan_gen,
    )
    payload = b"".join(
        [
            u32(EVENT_PAYLOAD_BYTES),
            u32(12),  # EVENT_PLAN
            u32(reason),
            u32(0),
            u64(0),
            u64(0),
            u64(0),
            u64(0),
        ]
    )
    if len(payload) != EVENT_PAYLOAD_BYTES:
        raise SystemExit("plan payload %d" % len(payload))
    return pad_record(header, payload, record_bytes)


def pack_registry(seq: int, descriptor_id: int, generation: int, fingerprint: int) -> bytes:
    header, record_bytes = pack_header(
        kind=KIND_REGISTRY,
        stream_kind=STREAM_EVENT,
        payload_bytes=EVENT_PAYLOAD_BYTES,
        source_sequence=seq,
        capture_plan_generation=1,
        execute_frame=seq,
        validity=0,
        reason=2,  # REGISTRY_TAP
        stable_id=descriptor_id,
        instance_generation=generation,
        instance_handle=fingerprint,
    )
    payload = b"".join(
        [
            u32(EVENT_PAYLOAD_BYTES),
            u32(12),
            u32(2),
            u32(0),
            u64(0),
            u64(0),
            u64(0),
            u64(0),
        ]
    )
    return pad_record(header, payload, record_bytes)


def pack_tap_binding(seq: int, descriptor_id: int, generation: int, selector: int) -> bytes:
    header, record_bytes = pack_header(
        kind=KIND_TAP_BINDING,
        stream_kind=STREAM_EVENT,
        payload_bytes=EVENT_PAYLOAD_BYTES,
        source_sequence=seq,
        capture_plan_generation=1,
        execute_frame=seq,
        validity=0,
        reason=selector,
        stable_id=descriptor_id,
        instance_generation=generation,
    )
    payload = b"".join(
        [
            u32(EVENT_PAYLOAD_BYTES),
            u32(12),
            u32(selector),
            u32(0),
            u64(0),
            u64(0),
            u64(0),
            u64(0),
        ]
    )
    return pad_record(header, payload, record_bytes)


def container(records: list[bytes]) -> bytes:
    return struct.pack("<Q", len(records)) + b"".join(records)


def dump_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def expected_layers(table: dict) -> dict:
    layers = []
    for item in table["layers"]:
        view = {k: v for k, v in item.items() if k != "record"}
        layers.append(view)
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "layers": layers,
    }


def layer_records(table: dict) -> list[bytes]:
    records = []
    for item in table["layers"]:
        rec = item["record"]
        if rec["kind"] == "time-series":
            records.append(
                pack_time_series(
                    rec["sourceSequence"],
                    rec["capturePlanGeneration"],
                    rec["executeFrame"],
                    rec["validityFlags"],
                    rec["valueBits"],
                )
            )
        elif rec["kind"] == "drop":
            records.append(
                pack_drop(
                    rec["sourceSequence"],
                    rec["capturePlanGeneration"],
                    rec["firstSourceSequence"],
                    rec["lastSourceSequence"],
                )
            )
        else:
            raise SystemExit("unknown layer record kind %s" % rec["kind"])
    return records


def expected_registry(table: dict) -> dict:
    seq = next(item for item in table["sequences"] if item["id"] == "registry-revision")
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "steps": seq["steps"],
    }


def expected_lease(table: dict) -> dict:
    seq = next(item for item in table["sequences"] if item["id"] == "lease-lifecycle")
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "steps": seq["steps"],
    }


def expected_stop(table: dict) -> dict:
    ids = [
        "stop-active-to-empty",
        "stop-already-empty",
        "stop-stale-lease",
        "stop-slots-full",
    ]
    sequences = [item for item in table["sequences"] if item["id"] in ids]
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "sequences": sequences,
    }


def expected_apply(table: dict) -> dict:
    ids = ["apply-budget-fail", "apply-unsupported-fail", "apply-no-lease"]
    sequences = [item for item in table["sequences"] if item["id"] in ids]
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "sequences": sequences,
    }


def expected_capacity(table: dict) -> dict:
    return {
        "schemaVersion": 1,
        "producingPackage": "m0-3",
        "isProvisional": False,
        "contractVerified": True,
        "contractDefault": table["capacity"]["contractDefault"],
        "hard": table["capacity"]["hard"],
        "resolvedSample": table["capacity"]["resolvedSample"],
        "errorContract": {
            "sessionCapacity": "VN_MONITOR_SESSION_CAPACITY",
            "planSlotCapacity": "VN_MONITOR_PLAN_CAPACITY",
            "budgetExceeded": "VN_MONITOR_BUDGET_EXCEEDED",
            "unsupported": "VN_MONITOR_UNSUPPORTED",
            "staleLease": "VN_MONITOR_STALE_LEASE",
        },
    }


def artifacts(table: dict) -> dict[str, bytes | str]:
    files: dict[str, bytes | str] = {}
    files["registry.bin"] = container(
        [
            pack_registry(1, 11, 1, 111),
            pack_registry(2, 11, 2, 222),
            pack_tap_binding(3, 11, 1, 0),
            pack_tap_binding(4, 11, 2, 1),
        ]
    )
    files["layers.bin"] = container(layer_records(table))
    files["lease.bin"] = container(
        [
            pack_plan_activated(1, 1, 0, REASON_APPLY, 1),
            pack_plan_activated(2, 1, 0, REASON_APPLY, 1),
        ]
    )
    files["stop.bin"] = container(
        [
            pack_plan_activated(1, 11, 10, REASON_STOP, 20),
        ]
    )
    files["apply_fail.bin"] = container(
        [
            pack_plan_activated(1, 10, 9, REASON_APPLY, 10),
        ]
    )
    files["registry.expected.json"] = json.dumps(expected_registry(table), indent=2) + "\n"
    files["layers.expected.json"] = json.dumps(expected_layers(table), indent=2) + "\n"
    files["lease.expected.json"] = json.dumps(expected_lease(table), indent=2) + "\n"
    files["stop.expected.json"] = json.dumps(expected_stop(table), indent=2) + "\n"
    files["apply_fail.expected.json"] = json.dumps(expected_apply(table), indent=2) + "\n"
    files["capacity.expected.json"] = json.dumps(expected_capacity(table), indent=2) + "\n"
    files["state_table.json"] = json.dumps(table, indent=2) + "\n"
    return files


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def as_bytes(value: bytes | str) -> bytes:
    if isinstance(value, bytes):
        return value
    return value.encode("utf-8")


def write_manifest(files: dict[str, bytes | str]) -> str:
    entries = []
    for name in sorted(files):
        digest = sha256_bytes(as_bytes(files[name]))
        role = "record-stream" if name.endswith(".bin") else "expected-view"
        if name == "state_table.json":
            role = "state-table"
        entries.append(
            {
                "path": name,
                "sha256": digest,
                "isProvisional": False,
                "producingPackage": "m0-3",
                "contractVersion": 1,
                "formatVersion": 1,
                "fixtureRole": role,
            }
        )
    payload = {
        "schemaVersion": 1,
        "isProvisional": False,
        "producingPackage": "m0-3",
        "contractVersion": 1,
        "files": entries,
    }
    return json.dumps(payload, indent=2) + "\n"


def check_artifacts(files: dict[str, bytes | str]) -> None:
    if not MANIFEST.is_file():
        raise SystemExit("checked-in plan manifest missing")
    man = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if man.get("schemaVersion") != 1:
        raise SystemExit("manifest schemaVersion")
    if man.get("isProvisional"):
        raise SystemExit("provisional fixture")
    if man.get("producingPackage") != "m0-3":
        raise SystemExit("producingPackage")
    if man.get("contractVersion") != 1:
        raise SystemExit("manifest contractVersion")
    listed = man.get("files") or []
    listed_paths = [item["path"] for item in listed]
    if listed_paths != sorted(files):
        raise SystemExit("manifest path set drift")
    for item in listed:
        path = OUT / item["path"]
        if not path.is_file():
            raise SystemExit("missing %s" % item["path"])
        digest = sha256_bytes(path.read_bytes())
        if digest != item.get("sha256"):
            raise SystemExit("sha mismatch %s" % item["path"])
        if item.get("isProvisional"):
            raise SystemExit("provisional file %s" % item["path"])
        if item.get("producingPackage") != "m0-3":
            raise SystemExit("producingPackage %s" % item["path"])
        if item.get("contractVersion") != 1:
            raise SystemExit("contractVersion %s" % item["path"])
        if item.get("formatVersion") != 1:
            raise SystemExit("formatVersion %s" % item["path"])
        role = item.get("fixtureRole")
        if item["path"].endswith(".bin"):
            expected_role = "record-stream"
        elif item["path"] == "state_table.json":
            expected_role = "state-table"
        else:
            expected_role = "expected-view"
        if role != expected_role:
            raise SystemExit("fixtureRole %s" % item["path"])
        expected = as_bytes(files[item["path"]])
        if path.read_bytes() != expected:
            raise SystemExit("bytes drift %s" % item["path"])
    on_disk = {p.name for p in OUT.iterdir() if p.is_file() and p.name != "manifest.json"}
    extra = on_disk - set(files)
    missing = set(files) - on_disk
    if extra or missing:
        raise SystemExit("manifest/files mismatch extra=%s missing=%s" % (extra, missing))
    table_on_disk = json.loads((OUT / "state_table.json").read_text(encoding="utf-8"))
    source_table = json.loads(TABLE.read_text(encoding="utf-8"))
    if table_on_disk != source_table:
        raise SystemExit("state table drift vs source")
    print("ok:", OUT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    table = json.loads(TABLE.read_text(encoding="utf-8"))
    files = artifacts(table)
    if args.check:
        check_artifacts(files)
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    for name, payload in files.items():
        (OUT / name).write_bytes(as_bytes(payload))
    MANIFEST.write_text(write_manifest(files), encoding="utf-8")
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
