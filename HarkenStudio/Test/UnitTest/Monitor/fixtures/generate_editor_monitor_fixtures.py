from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
PACKED = 316
KIND_EVENT = 5
KIND_UNKNOWN = 99
FLAG_REQUIRED = 1
FLAG_OPTIONAL = 2
STREAM_EVENT = 1 << 1
VALID_ARMED_NUMERIC = 3
EVENT_CLASS_START = 4
EVENT_PAYLOAD_BYTES = 48


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
    kind: int = KIND_EVENT,
    version: int = 1,
    flags: int = FLAG_REQUIRED,
    stream_kind: int = STREAM_EVENT,
    payload_bytes: int = EVENT_PAYLOAD_BYTES,
    source_sequence: int = 1,
    delivery_sequence: int = 0,
    record_bytes: int | None = None,
) -> bytes:
    if record_bytes is None:
        record_bytes = aligned_record_bytes(PACKED, payload_bytes)
    parts = [
        u32(kind),
        u32(version),
        u32(flags),
        u32(stream_kind),
        u64(record_bytes),
        u64(1),  # streamId
        u64(1),  # producerId
        u64(source_sequence),
        u64(delivery_sequence),
        u64(1),  # engineMonitorInstanceId
        u64(1),  # monitorSessionEpoch
        u64(1),  # worldGeneration
        u64(1),  # capturePlanGeneration
        u64(1),  # renderPlanGeneration
        u64(1),  # transportEpoch
        u64(1),  # deviceEpoch
        u64(1),  # clockAnchorGeneration
        u64(1),  # measurementSegmentGeneration
        u64(1),  # executeFrame
        u64(0),  # graphFrame
        u64(0),  # devicePresentationEstimate
        u32(0),  # deviceUncertaintyFrames
        u32(0),  # entityKind
        u64(0),  # programSemanticFingerprint
        u64(0),  # runtimeLineageFingerprint
        u64(0),  # optionalSymbolFingerprint
        bytes(16),  # stableId
        u64(0),  # instanceHandle
        u64(0),  # instanceGeneration
        u64(0),  # actionInstanceHandle
        u64(0),  # playableInstanceHandle
        u64(0),  # parentActionInstanceHandle
        u64(0),  # traceCorrelationId
        u32(0),  # reason
        u32(VALID_ARMED_NUMERIC),
        u64(payload_bytes),
        u32(0),  # telemetry.reserved0
        u64(0),  # telemetry.arrangingInstance
        bytes(16),  # telemetry.clipId
        u32(0),  # telemetry.loopIteration
        u32(0),  # telemetry.occurrenceOrdinal
        u64(0),  # telemetry.logicCommitSequence
        u64(0),  # telemetry.executeFrame
    ]
    blob = b"".join(parts)
    if len(blob) != PACKED:
        raise SystemExit("packed size %d != %d" % (len(blob), PACKED))
    return blob


def with_payload(header: bytes, payload: bytes, record_bytes: int) -> bytes:
    blob = header + payload
    if len(blob) > record_bytes:
        raise SystemExit("payload overflow")
    if len(blob) < record_bytes:
        blob += b"\x00" * (record_bytes - len(blob))
    return blob


def event_payload() -> bytes:
    return b"".join(
        [
            u32(EVENT_PAYLOAD_BYTES),
            u32(EVENT_CLASS_START),
            u32(0),
            u32(0),
            u64(0),
            u64(0),
            u64(0),
            u64(0),
        ]
    )


def legal_packed() -> bytes:
    payload = event_payload()
    record_bytes = aligned_record_bytes(PACKED, len(payload))
    return with_payload(pack_header(payload_bytes=len(payload), record_bytes=record_bytes), payload, record_bytes)


def fixture_container(records: list[bytes], count: int | None = None) -> bytes:
    if count is None:
        count = len(records)
    return struct.pack("<Q", count) + b"".join(records)


def native_layout_record() -> bytes:
    # Intentionally not LE packed: host-endian memcpy of a fake in-memory header prefix.
    # First 4 bytes look like a native structSize, not recordKind.
    fake = bytearray(aligned_record_bytes(PACKED, 0))
    struct.pack_into("@I", fake, 0, 4096)
    return bytes(fake)


def write_bin(path: Path, data: bytes) -> str:
    path.write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    return digest


def main() -> None:
    out = ROOT / "HarkenStudio" / "Test" / "UnitTest" / "Monitor" / "fixtures"
    out.mkdir(parents=True, exist_ok=True)

    legal = legal_packed()
    optional_bytes = aligned_record_bytes(PACKED, 0)
    optional = with_payload(
        pack_header(kind=KIND_UNKNOWN, flags=FLAG_OPTIONAL, payload_bytes=0, record_bytes=optional_bytes),
        b"",
        optional_bytes,
    )
    required_unknown = with_payload(
        pack_header(kind=KIND_UNKNOWN, flags=FLAG_REQUIRED, payload_bytes=0, record_bytes=optional_bytes),
        b"",
        optional_bytes,
    )
    short_header = legal[:80]
    truncated = legal[:200]
    count_mismatch = fixture_container([legal], count=2)
    native = native_layout_record()
    be_count = struct.pack(">Q", 1) + legal  # wrong endian count prefix for helper

    files = {
        "optional_unknown.bin": fixture_container([optional]),
        "required_unknown.bin": fixture_container([required_unknown]),
        "short_header.bin": fixture_container([short_header], count=1),
        "truncated_record.bin": fixture_container([truncated], count=1),
        "legal_then_required_unknown.bin": fixture_container([legal, required_unknown]),
        "count_bytes_mismatch.bin": count_mismatch,
        "native_layout.bin": fixture_container([native]),
        "be_count_prefix.bin": be_count,
        "empty_batch.bin": struct.pack("<Q", 0),
    }

    manifest_files = []
    for name, blob in files.items():
        digest = write_bin(out / name, blob)
        expected = "ok"
        if name in {
            "required_unknown.bin",
            "short_header.bin",
            "truncated_record.bin",
            "legal_then_required_unknown.bin",
            "count_bytes_mismatch.bin",
            "native_layout.bin",
            "be_count_prefix.bin",
        }:
            expected = "incompatible"
        elif name == "optional_unknown.bin":
            expected = "ok-optional-skip"
        elif name == "empty_batch.bin":
            expected = "ok-empty"
        manifest_files.append(
            {
                "path": name,
                "sha256": digest,
                "isProvisional": False,
                "kind": name.replace(".bin", ""),
                "expected": expected,
            }
        )

    manifest = {
        "schemaVersion": 1,
        "isProvisional": False,
        "files": manifest_files,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("wrote", len(files), "fixtures to", out)


if __name__ == "__main__":
    main()
