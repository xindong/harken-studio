#!/usr/bin/env python3
"""Emit checked-in Monitor Remote contract fixtures.

`--check` never rewrites. Packed handshake is the public
VnMonitorRemoteHandshake field order (120 bytes LE). Packed frame
counter is VnMonitorRemoteFrameCounter (16 bytes LE). Zero ABI.
The Monitor function allowlist matches the ABI manifest excluding
the dedicated Event Session APIs and keeps Vn_MonitorCompareSessions.
Handshake packing and the wire opcodes stay independent of that count.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
OUT = REPO / "HarkenEngine" / "Test" / "UnitTest" / "monitor" / "remote"
MANIFEST = OUT / "manifest.json"
HARKEN_H = REPO / "HarkenEngine" / "Source" / "Runtime" / "api" / "include" / "harken" / "Harken.h"
WIRE = REPO / "HarkenEngine" / "Source" / "Tooling" / "Remote" / "wire" / "harken_wire_table.json"
ABI = REPO / "HarkenEngine" / "Source" / "Tooling" / "Abi" / "generated" / "harken_abi_manifest.g.json"

HANDSHAKE_PACKED = 120
COUNTER_PACKED = 16
NONCE_BYTES = 32
SPKI_BYTES = 32
COUNTER_START = 1
PROFILE_DISABLED = 0
PROFILE_LOCAL_ACL = 1
PROFILE_LAN_MTLS = 2
PERM_OBSERVE_STATE = 1 << 0
PERM_OBSERVE_EVENTS = 1 << 1
PERM_OBSERVE_MEASUREMENTS = 1 << 2
PERM_CAPTURE_CONTROL = 1 << 3
PERM_RECORD = 1 << 4
PERM_RAW_AUDIO = 1 << 5
PERM_SYMBOLS = 1 << 6
ALL_PERMS = (
    PERM_OBSERVE_STATE
    | PERM_OBSERVE_EVENTS
    | PERM_OBSERVE_MEASUREMENTS
    | PERM_CAPTURE_CONTROL
    | PERM_RECORD
    | PERM_RAW_AUDIO
    | PERM_SYMBOLS
)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def u64(v: int) -> bytes:
    return struct.pack("<Q", v)


def handshake(
    *,
    profile: int,
    major: int = 1,
    minor: int = 0,
    schema: int = 11,
    build: int = 22,
    program: int = 33,
    capability: int = 44,
    requested: int = 0,
    reserved: int = 0,
    struct_size: int = HANDSHAKE_PACKED,
    client: bytes | None = None,
    server: bytes | None = None,
) -> bytes:
    client_b = client if client is not None else bytes(NONCE_BYTES)
    server_b = server if server is not None else bytes(NONCE_BYTES)
    if len(client_b) != NONCE_BYTES or len(server_b) != NONCE_BYTES:
        raise SystemExit("nonce size")
    blob = b"".join(
        [
            u32(struct_size),
            u32(profile),
            u32(major),
            u32(minor),
            u64(schema),
            u64(build),
            u64(program),
            u64(capability),
            u32(requested),
            u32(reserved),
            client_b,
            server_b,
        ]
    )
    if len(blob) != HANDSHAKE_PACKED:
        raise SystemExit("handshake packed %d" % len(blob))
    return blob


def counter(*, value: int, struct_size: int = COUNTER_PACKED, reserved: int = 0) -> bytes:
    blob = b"".join([u32(struct_size), u32(reserved), u64(value)])
    if len(blob) != COUNTER_PACKED:
        raise SystemExit("counter packed %d" % len(blob))
    return blob


REQUIRED_MONITOR_FUNCTIONS = ("Vn_MonitorCompareSessions",)
EVENT_SESSION_FUNCTIONS = (
    "Vn_MonitorOpenEventSession",
    "Vn_MonitorSealEventSession",
    "Vn_MonitorGetEventSessionStatus",
)


def parse_monitor_functions(manifest: dict) -> list[str]:
    names = []
    seen = set()
    for item in manifest.get("contract", {}).get("nativeAbi", {}).get("functions") or []:
        name = item.get("name") or ""
        if not name.startswith("Vn_Monitor"):
            continue
        if name in seen:
            raise SystemExit("duplicate Monitor ABI function %s" % name)
        seen.add(name)
        if name not in EVENT_SESSION_FUNCTIONS:
            names.append(name)
    if not names:
        raise SystemExit("Monitor ABI function list is empty")
    missing = [name for name in REQUIRED_MONITOR_FUNCTIONS if name not in seen]
    if missing:
        raise SystemExit("Monitor ABI missing required function(s): %s" % missing)
    return names


def parse_wire_opcodes(payload: dict) -> list[dict]:
    rows = []
    for item in payload.get("functions") or []:
        opcode = item.get("opcode")
        name = item.get("name")
        if opcode is None or not name:
            continue
        rows.append({"name": name, "opcode": int(opcode)})
    if not rows:
        raise SystemExit("wire opcode list is empty")
    if len({row["opcode"] for row in rows}) != len(rows):
        raise SystemExit("duplicate wire opcode")
    if len({row["name"] for row in rows}) != len(rows):
        raise SystemExit("duplicate wire function")
    return rows


def banned_monitor_defines(text: str) -> list[str]:
    start = text.find("VN_MONITOR_WRONG_THREAD")
    end = text.find("VnResult Vn_MonitorGetCapabilities")
    if start < 0 or end < 0 or end <= start:
        raise SystemExit("monitor define window missing")
    window = text[start:end]
    banned = []
    for match in re.finditer(r"#define\s+(VN_MONITOR_[A-Z0-9_]+)\s+(\S+)", window):
        name, value = match.group(1), match.group(2)
        token = (name + " " + value).upper()
        if any(word in token for word in ("ENDPOINT", "PIPE", "LISTEN", "BIND", "PORT")):
            if "SUPPORT" in name:
                continue
            banned.append(name)
    return banned


def permission_rows() -> list[dict]:
    bits = [
        ("observe_state", PERM_OBSERVE_STATE),
        ("observe_events", PERM_OBSERVE_EVENTS),
        ("observe_measurements", PERM_OBSERVE_MEASUREMENTS),
        ("capture_control", PERM_CAPTURE_CONTROL),
        ("record", PERM_RECORD),
        ("raw_audio", PERM_RAW_AUDIO),
        ("symbols", PERM_SYMBOLS),
    ]
    rows = []
    for name, bit in bits:
        rows.append(
            {
                "name": name,
                "bit": bit,
                "grantRequested": bit,
                "grantGranted": bit,
                "grantStatus": "VN_OK",
                "denyRequested": bit,
                "denyGranted": 0,
                "denyStatus": "VN_MONITOR_PERMISSION_DENIED",
            }
        )
    return rows


def artifacts() -> dict[str, bytes | str]:
    harken = HARKEN_H.read_text(encoding="utf-8")
    wire = json.loads(WIRE.read_text(encoding="utf-8"))
    abi = json.loads(ABI.read_text(encoding="utf-8"))
    functions = parse_monitor_functions(abi)
    opcodes = parse_wire_opcodes(wire)
    overlap = sorted(set(functions) & {row["name"] for row in opcodes})
    if overlap:
        raise SystemExit("allowlist overlap %s" % overlap)
    banned = banned_monitor_defines(harken)
    if banned:
        raise SystemExit("default-closed defines leaked %s" % banned)

    client = bytes(range(NONCE_BYTES))
    server = bytes(reversed(range(NONCE_BYTES)))
    files: dict[str, bytes | str] = {
        "handshake_disabled.bin": handshake(profile=PROFILE_DISABLED),
        "handshake_local_acl.bin": handshake(
            profile=PROFILE_LOCAL_ACL, requested=ALL_PERMS, client=client, server=server
        ),
        "handshake_lan_mtls.bin": handshake(
            profile=PROFILE_LAN_MTLS, requested=PERM_OBSERVE_STATE, client=client, server=server
        ),
        "handshake_unknown_profile.bin": handshake(profile=99),
        "handshake_reserved_nonzero.bin": handshake(profile=PROFILE_LOCAL_ACL, reserved=1),
        "handshake_struct_size_bad.bin": handshake(profile=PROFILE_LOCAL_ACL, struct_size=0),
        "handshake_mismatch.bin": handshake(
            profile=PROFILE_LOCAL_ACL, schema=99, build=98, program=97, capability=96
        ),
        "handshake_unknown_permission.bin": handshake(
            profile=PROFILE_LOCAL_ACL, requested=ALL_PERMS | (1 << 7), client=client, server=server
        ),
        "counter_start.bin": counter(value=COUNTER_START),
        "counter_overflow.bin": counter(value=0),
        "principal_local_acl.bin": u64(0x1111111111111111),
        "principal_lan_mtls.bin": bytes(range(SPKI_BYTES)),
    }
    for row in permission_rows():
        files["perm_grant_%s.bin" % row["name"]] = handshake(
            profile=PROFILE_LOCAL_ACL, requested=row["bit"], client=client, server=server
        )
        files["perm_deny_%s.bin" % row["name"]] = handshake(
            profile=PROFILE_LAN_MTLS, requested=0, client=client, server=server
        )
    expected = {
        "schemaVersion": 1,
        "producingPackage": "m0-5",
        "isProvisional": False,
        "contractVerified": True,
        "packedHandshakeBytes": HANDSHAKE_PACKED,
        "packedCounterBytes": COUNTER_PACKED,
        "nonceBytes": NONCE_BYTES,
        "spkiBytes": SPKI_BYTES,
        "messageCounterStart": COUNTER_START,
        "wireOpcodeCount": len(opcodes),
        "monitorFunctionCount": len(functions),
        "statuses": {
            "disabled": "VN_OK",
            "local_acl": "VN_OK",
            "lan_mtls": "VN_OK",
            "unknown_profile": "VN_MONITOR_INCOMPATIBLE",
            "reserved_nonzero": "VN_INVALID_ARG",
            "struct_size_bad": "VN_MONITOR_INCOMPATIBLE",
            "mismatch": "VN_MONITOR_INCOMPATIBLE",
            "permission_missing": "VN_MONITOR_PERMISSION_DENIED",
            "unknown_permission": "VN_INVALID_ARG",
            "disabled_mismatch": "VN_MONITOR_INCOMPATIBLE",
            "counter_overflow": "VN_MONITOR_INCOMPATIBLE",
            "trailing_bytes": "VN_MONITOR_INCOMPATIBLE",
        },
        "permissions": permission_rows(),
        "defaultClosed": {
            "bannedDefines": [],
            "endpointName": "",
            "defaultPort": 0,
            "defaultPipe": "",
        },
    }
    files["contract.expected.json"] = json.dumps(expected, indent=2) + "\n"
    files["allowlist_monitor_functions.json"] = (
        json.dumps(
            {
                "schemaVersion": 1,
                "producingPackage": "m0-5",
                "kind": "monitor-functions",
                "names": functions,
            },
            indent=2,
        )
        + "\n"
    )
    files["allowlist_wire_opcodes.json"] = (
        json.dumps(
            {
                "schemaVersion": 1,
                "producingPackage": "m0-5",
                "kind": "wire-opcodes",
                "count": len(opcodes),
                "entries": opcodes,
            },
            indent=2,
        )
        + "\n"
    )
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
        if name.endswith(".bin"):
            role = "remote-file"
        elif name.startswith("allowlist_"):
            role = "allowlist"
        else:
            role = "expected-view"
        entries.append(
            {
                "path": name,
                "sha256": sha256_bytes(as_bytes(files[name])),
                "isProvisional": False,
                "producingPackage": "m0-5",
                "contractVersion": 1,
                "formatVersion": 1,
                "fixtureRole": role,
            }
        )
    payload = {
        "schemaVersion": 1,
        "isProvisional": False,
        "producingPackage": "m0-5",
        "contractVersion": 1,
        "files": entries,
    }
    return json.dumps(payload, indent=2) + "\n"


def check_artifacts(files: dict[str, bytes | str]) -> None:
    if not MANIFEST.is_file():
        raise SystemExit("checked-in remote manifest missing")
    man = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if man.get("schemaVersion") != 1 or man.get("isProvisional"):
        raise SystemExit("manifest schema")
    if man.get("producingPackage") != "m0-5" or man.get("contractVersion") != 1:
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
        if item.get("producingPackage") != "m0-5":
            raise SystemExit("producingPackage %s" % item["path"])
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
