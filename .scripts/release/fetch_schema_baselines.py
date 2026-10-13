from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import zipfile
from pathlib import Path, PureWindowsPath
from typing import Any
from urllib.parse import urlsplit
from urllib.request import urlopen


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMS = {"windows-x64", "macos-x64", "macos-arm64"}


def validate_url(value: Any) -> str:
    if not isinstance(value, str):
        raise ValueError("baseline URL must use HTTPS")
    parsed = urlsplit(value)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
        or any(character.isspace() for character in value)
    ):
        raise ValueError("baseline URL must use HTTPS without credentials or a fragment")
    return value


def selected_entries(value: Any, platform: str) -> list[dict[str, str]]:
    if platform not in PLATFORMS:
        raise ValueError(f"unsupported baseline platform: {platform}")
    if not isinstance(value, list):
        raise ValueError("schema baselines must be a JSON array")
    selected: list[dict[str, str]] = []
    identities: set[tuple[str, str]] = set()
    for entry in value:
        if not isinstance(entry, dict) or set(entry) != {"tag", "platform", "url", "sha256"}:
            raise ValueError("each schema baseline needs tag, platform, url and sha256")
        tag = entry["tag"]
        if not isinstance(tag, str) or not tag or any(character.isspace() for character in tag):
            raise ValueError("baseline tag must be a nonempty string without whitespace")
        if not isinstance(entry["platform"], str) or entry["platform"] not in PLATFORMS:
            raise ValueError("baseline entry has an unsupported platform")
        validate_url(entry["url"])
        checksum = entry["sha256"]
        if not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", checksum):
            raise ValueError("baseline sha256 must contain exactly 64 hexadecimal digits")
        identity = (tag, entry["platform"])
        if identity in identities:
            raise ValueError(f"duplicate baseline tag for {entry['platform']}: {tag}")
        identities.add(identity)
        if entry["platform"] == platform:
            selected.append({**entry, "sha256": checksum.lower()})
    return sorted(selected, key=lambda entry: entry["tag"])


def extract_package(archive_path: Path, target: Path) -> None:
    target.mkdir(parents=True)
    seen: set[str] = set()
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            name = member.orig_filename
            normalized = name[:-1] if name.endswith("/") else name
            parts = normalized.split("/")
            if (
                not normalized
                or "\\" in name
                or "\0" in name
                or ":" in name
                or any(part in {"", ".", ".."} or part.endswith((" ", "."))
                       or PureWindowsPath(part).is_reserved() for part in parts)
            ):
                raise ValueError(f"unsafe ZIP member path: {name!r}")
            identity = normalized.casefold()
            if identity in seen:
                raise ValueError(f"duplicate ZIP member path: {name!r}")
            seen.add(identity)
            mode = member.external_attr >> 16
            file_type = stat.S_IFMT(mode)
            if file_type not in {0, stat.S_IFREG, stat.S_IFDIR}:
                raise ValueError(f"ZIP member is a symlink or special file: {name!r}")
            destination = target.joinpath(*parts)
            if not destination.resolve().is_relative_to(target.resolve()):
                raise ValueError(f"ZIP member escapes its extraction directory: {name!r}")
            if member.is_dir() or file_type == stat.S_IFDIR:
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, destination.open("wb") as output:
                shutil.copyfileobj(source, output)
            permissions = mode & 0o777
            if member.create_system == 3 and permissions:
                os.chmod(destination, permissions)


def fetch_baselines(value: Any, platform: str, output_dir: Path) -> dict[str, str]:
    entries = selected_entries(value, platform)
    output_dir = output_dir.resolve()
    temp_root = (REPO_ROOT / ".temp").resolve()
    if output_dir == temp_root or not output_dir.is_relative_to(temp_root):
        raise ValueError("baseline output must be a named directory under the repository .temp")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    readers: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="schema-baselines-", dir=output_dir.parent) as directory:
        staging = Path(directory)
        candidate = staging / "readers"
        candidate.mkdir()
        for index, entry in enumerate(entries):
            archive_path = staging / f"package-{index}.zip"
            checksum = hashlib.sha256()
            with urlopen(entry["url"], timeout=60) as response, archive_path.open("wb") as output:
                validate_url(response.geturl())
                while chunk := response.read(1024 * 1024):
                    checksum.update(chunk)
                    output.write(chunk)
            if checksum.hexdigest() != entry["sha256"]:
                raise ValueError(f"baseline artifact SHA-256 mismatch: {entry['tag']} {platform}")
            name = f"reader-{index:04d}"
            extract_package(archive_path, candidate / name)
            readers[entry["tag"]] = str(output_dir / name)
        (candidate / "readers.json").write_text(
            json.dumps(readers, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8", newline="\n",
        )
        if output_dir.exists():
            shutil.rmtree(output_dir)
        os.replace(candidate, output_dir)
    return readers


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-json-env", default="HARKEN_SCHEMA_BASELINES")
    parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        value = json.loads(os.environ.get(args.input_json_env, "[]"))
        readers = fetch_baselines(value, args.platform, args.output_dir)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"error: cannot fetch Schema baselines: {error}", file=sys.stderr)
        return 1
    print(json.dumps(readers, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
