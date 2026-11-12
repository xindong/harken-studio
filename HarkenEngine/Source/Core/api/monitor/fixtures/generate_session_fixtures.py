#!/usr/bin/env python3
"""Emit checked-in Monitor Session file fixtures.

`--check` never rewrites. Packed chunk header is 32 bytes, commit footer 24.
Header CRC covers the first 28 packed bytes (everything except headerCrc32c).
CRC32C matches the M0-2 harness (Castagnoli reflected).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
OUT = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor" / "session"
RECORD = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor" / "legal_event.bin"
MANIFEST = OUT / "manifest.json"

FILE_MAGIC = 0x30314E4F4D4E56
COMMIT_MAGIC = 0x3031544D434E56
HEADER_PACKED = 64
CHUNK_HEADER = 32
FOOTER = 24
CHUNK_EVENTS = 7
FLAG_REQUIRED = 1
FLAG_OPTIONAL = 2
POLY = 0x82F63B78


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def u64(v: int) -> bytes:
    return struct.pack("<Q", v)


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = 0 - (crc & 1)
            crc = (crc >> 1) ^ (POLY & mask)
    return crc ^ 0xFFFFFFFF


def file_header(*, fingerprint: int, instance: int) -> bytes:
    parts = [
        u32(1),
        u32(0),
        u32(0),
        u64(FILE_MAGIC),
        u64(HEADER_PACKED),
        u64(1),
        u64(instance),
        u64(0),
        u64(fingerprint),
        u32(0),
    ]
    blob = bytearray(b"".join(parts))
    if len(blob) != HEADER_PACKED:
        raise SystemExit("file header %d" % len(blob))
    digest = crc32c(bytes(blob))
    blob[60:64] = u32(digest)
    return bytes(blob)


def chunk_and_footer(*, kind: int, flags: int, sequence: int, payload: bytes) -> bytes:
    header = bytearray(
        b"".join(
            [
                u16(kind),
                u16(flags),
                u16(1),
                u16(CHUNK_HEADER),
                u64(len(payload)),
                u64(sequence),
                u32(crc32c(payload)),
                u32(0),
            ]
        )
    )
    if len(header) != CHUNK_HEADER:
        raise SystemExit("chunk header %d" % len(header))
    header[28:32] = u32(crc32c(bytes(header[:28])))
    footer = b"".join([u64(COMMIT_MAGIC), u64(sequence), u64(len(payload))])
    if len(footer) != FOOTER:
        raise SystemExit("footer %d" % len(footer))
    return bytes(header) + payload + footer


def record_payload() -> bytes:
    blob = RECORD.read_bytes()
    if len(blob) < 8:
        raise SystemExit("legal_event.bin too small")
    count = struct.unpack_from("<Q", blob, 0)[0]
    if count != 1:
        raise SystemExit("legal_event.bin count")
    return blob[8:]


def legal_file() -> bytes:
    payload = record_payload()
    return file_header(fingerprint=11, instance=1) + chunk_and_footer(
        kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=1, payload=payload
    )


def truncated_file() -> bytes:
    return legal_file()[:-FOOTER]


def recovered_tail_file() -> bytes:
    payload = record_payload()
    first = bytearray(
        chunk_and_footer(kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=1, payload=payload)
    )
    first[CHUNK_HEADER] ^= 0xFF
    second = chunk_and_footer(kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=2, payload=payload)
    return file_header(fingerprint=11, instance=1) + bytes(first) + second


def optional_unknown_file() -> bytes:
    payload = record_payload()
    unknown = chunk_and_footer(kind=200, flags=FLAG_OPTIONAL, sequence=1, payload=b"\x11\x22\x33\x44")
    events = chunk_and_footer(kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=2, payload=payload)
    return file_header(fingerprint=11, instance=1) + unknown + events


def incomparable_file() -> bytes:
    payload = record_payload()
    return file_header(fingerprint=99, instance=2) + chunk_and_footer(
        kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=1, payload=payload
    )


def missing_file() -> bytes:
    return file_header(fingerprint=11, instance=1)


def trailing_junk_file() -> bytes:
    return legal_file() + b"\xaa" * 8


def required_decode_fail_file() -> bytes:
    return file_header(fingerprint=11, instance=1) + chunk_and_footer(
        kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=1, payload=b"\x01\x02\x03\x04\x05\x06\x07\x08"
    )


def nine_commits_file() -> bytes:
    payload = record_payload()
    blob = bytearray(file_header(fingerprint=11, instance=1))
    for sequence in range(1, 10):
        blob.extend(
            chunk_and_footer(
                kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=sequence, payload=payload
            )
        )
    return bytes(blob)


def optional_crc_fail_file() -> bytes:
    payload = record_payload()
    unknown = bytearray(
        chunk_and_footer(kind=200, flags=FLAG_OPTIONAL, sequence=1, payload=b"\x11\x22\x33\x44")
    )
    unknown[CHUNK_HEADER] ^= 0xFF
    events = chunk_and_footer(kind=CHUNK_EVENTS, flags=FLAG_REQUIRED, sequence=2, payload=payload)
    return file_header(fingerprint=11, instance=1) + bytes(unknown) + events


def artifacts() -> dict[str, bytes | str]:
    files: dict[str, bytes | str] = {
        "legal.bin": legal_file(),
        "truncated.bin": truncated_file(),
        "recovered_tail.bin": recovered_tail_file(),
        "optional_unknown.bin": optional_unknown_file(),
        "incomparable.bin": incomparable_file(),
        "missing.bin": missing_file(),
        "trailing_junk.bin": trailing_junk_file(),
        "required_decode_fail.bin": required_decode_fail_file(),
        "nine_commits.bin": nine_commits_file(),
        "optional_crc_fail.bin": optional_crc_fail_file(),
    }
    expected = {
        "schemaVersion": 1,
        "producingPackage": "m0-4",
        "isProvisional": False,
        "contractVerified": True,
        "compare": {
            "legal_vs_legal": "VN_MONITOR_COMPARE_COMPARABLE",
            "legal_vs_incomparable": "VN_MONITOR_COMPARE_INCOMPARABLE",
            "legal_vs_missing": "VN_MONITOR_COMPARE_MISSING_DATA",
            "legal_vs_optional": "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE",
            "legal_vs_trailing": "VN_MONITOR_COMPARE_MISSING_DATA",
            "legal_vs_recovered": "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE",
            "legal_vs_nine": "VN_MONITOR_COMPARE_COMPARABLE",
            "legal_vs_optional_crc": "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE",
        },
    }
    files["compare.expected.json"] = json.dumps(expected, indent=2) + "\n"
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
        role = "session-file" if name.endswith(".bin") else "expected-view"
        entries.append(
            {
                "path": name,
                "sha256": sha256_bytes(as_bytes(files[name])),
                "isProvisional": False,
                "producingPackage": "m0-4",
                "contractVersion": 1,
                "formatVersion": 1,
                "fixtureRole": role,
            }
        )
    payload = {
        "schemaVersion": 1,
        "isProvisional": False,
        "producingPackage": "m0-4",
        "contractVersion": 1,
        "files": entries,
    }
    return json.dumps(payload, indent=2) + "\n"


def check_artifacts(files: dict[str, bytes | str]) -> None:
    if not MANIFEST.is_file():
        raise SystemExit("checked-in session manifest missing")
    man = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if man.get("schemaVersion") != 1 or man.get("isProvisional"):
        raise SystemExit("manifest schema")
    if man.get("producingPackage") != "m0-4" or man.get("contractVersion") != 1:
        raise SystemExit("manifest package")
    listed = man.get("files") or []
    if [item["path"] for item in listed] != sorted(files):
        raise SystemExit("manifest path set drift")
    for item in listed:
        path = OUT / item["path"]
        if not path.is_file():
            raise SystemExit("missing %s" % item["path"])
        raw = path.read_bytes()
        if sha256_bytes(raw) != item.get("sha256"):
            raise SystemExit("sha mismatch %s" % item["path"])
        if raw != as_bytes(files[item["path"]]):
            raise SystemExit("bytes drift %s" % item["path"])
        if item.get("producingPackage") != "m0-4":
            raise SystemExit("producingPackage %s" % item["path"])
        if item.get("contractVersion") != 1 or item.get("formatVersion") != 1:
            raise SystemExit("version %s" % item["path"])
        expected_role = "session-file" if item["path"].endswith(".bin") else "expected-view"
        if item.get("fixtureRole") != expected_role:
            raise SystemExit("fixtureRole %s" % item["path"])
        if item.get("isProvisional"):
            raise SystemExit("provisional %s" % item["path"])
    on_disk = {p.name for p in OUT.iterdir() if p.is_file() and p.name != "manifest.json"}
    extra = on_disk - set(files)
    missing = set(files) - on_disk
    if extra or missing:
        raise SystemExit("manifest/files mismatch extra=%s missing=%s" % (extra, missing))
    print("ok:", OUT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    files = artifacts()
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
