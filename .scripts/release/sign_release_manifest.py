#!/usr/bin/env python3
"""Generate Ed25519 manifest signing keys, and sign or verify Harken release manifests."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

PRIVATE_KEY_NAME = "manifest_signing.private"
PUBLIC_KEY_NAME = "manifest_signing.public"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_key_hex(path: Path, expected_bytes: int) -> bytes:
    text = path.read_text(encoding="ascii").strip()
    raw = bytes.fromhex(text)
    if len(raw) != expected_bytes:
        raise SystemExit(f"error: {path} does not hold {expected_bytes} key bytes")
    return raw


def generate_keypair(args: argparse.Namespace) -> dict:
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    private_path = output_dir / PRIVATE_KEY_NAME
    public_path = output_dir / PUBLIC_KEY_NAME
    if private_path.exists() or public_path.exists():
        raise SystemExit(f"error: key files already exist in {output_dir}")

    private_key = Ed25519PrivateKey.generate()
    private_raw = private_key.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    public_raw = private_key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    fd = os.open(str(private_path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as handle:
        handle.write(private_raw.hex() + "\n")
    public_path.write_text(public_raw.hex() + "\n", encoding="ascii")
    return {
        "privateKey": str(private_path),
        "publicKey": str(public_path),
        "publicKeyHex": public_raw.hex(),
    }


def build_payload(args: argparse.Namespace) -> dict:
    package_path = Path(args.package).resolve()
    if not package_path.is_file():
        raise SystemExit(f"error: package not found: {package_path}")
    published_at = datetime.now(timezone.utc)
    expires_at = published_at + timedelta(days=args.expires_days)
    return {
        "product": args.product,
        "version": args.version,
        "buildId": args.build_id,
        "channel": args.channel,
        "platform": args.platform,
        "architecture": args.architecture,
        "packageUrl": args.package_url,
        "packageSize": package_path.stat().st_size,
        "sha256": sha256_file(package_path),
        "releaseSequence": args.release_sequence,
        "publishedAt": published_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "expiresAt": expires_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


def sign_manifest(args: argparse.Namespace) -> dict:
    private_raw = read_key_hex(Path(args.private_key), 32)
    private_key = Ed25519PrivateKey.from_private_bytes(private_raw)

    payload_bytes = json.dumps(
        build_payload(args), indent=2, sort_keys=True
    ).encode("utf-8")
    signature = private_key.sign(payload_bytes)
    envelope = {
        "algorithm": "ed25519",
        "payloadBase64": base64.b64encode(payload_bytes).decode("ascii"),
        "signatureBase64": base64.b64encode(signature).decode("ascii"),
    }

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(envelope, indent=2) + "\n", encoding="ascii")
    return {"manifest": str(output_path), "payloadBytes": len(payload_bytes)}


def verify_manifest(args: argparse.Namespace) -> dict:
    public_raw = read_key_hex(Path(args.public_key), 32)
    public_key = Ed25519PublicKey.from_public_bytes(public_raw)

    envelope = json.loads(Path(args.manifest).read_text(encoding="ascii"))
    if envelope.get("algorithm") != "ed25519":
        raise SystemExit("error: unsupported manifest signature algorithm")
    payload_bytes = base64.b64decode(envelope["payloadBase64"], validate=True)
    signature = base64.b64decode(envelope["signatureBase64"], validate=True)
    try:
        public_key.verify(signature, payload_bytes)
    except InvalidSignature:
        raise SystemExit("error: manifest signature verification failed")
    return json.loads(payload_bytes)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    generate = commands.add_parser("generate-keypair")
    generate.add_argument("--output-dir", required=True)

    sign = commands.add_parser("sign")
    sign.add_argument("--private-key", required=True)
    sign.add_argument("--package", required=True)
    sign.add_argument("--output", required=True)
    sign.add_argument("--product", default="HarkenStudio")
    sign.add_argument("--version", required=True)
    sign.add_argument("--build-id", required=True)
    sign.add_argument("--channel", default="stable")
    sign.add_argument("--platform", default="windows")
    sign.add_argument("--architecture", default="x64")
    sign.add_argument("--package-url", required=True)
    sign.add_argument("--release-sequence", type=int, required=True)
    sign.add_argument("--expires-days", type=int, default=14)

    verify = commands.add_parser("verify")
    verify.add_argument("--public-key", required=True)
    verify.add_argument("--manifest", required=True)

    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.command == "generate-keypair":
        result = generate_keypair(args)
    elif args.command == "sign":
        result = sign_manifest(args)
    else:
        result = verify_manifest(args)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
