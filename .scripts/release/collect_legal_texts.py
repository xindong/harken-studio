#!/usr/bin/env python3
"""Assemble the Legal attribution directory bundled into release artifacts."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LEGAL_SOURCE_DIRS = {
    "studio": REPO_ROOT / "HarkenStudio" / "Resources" / "Legal",
    "hub": REPO_ROOT / "HarkenHub" / "Resources" / "Legal",
}

COMMON_FILES = (
    (REPO_ROOT / "LICENSE", "LICENSE.txt"),
    (REPO_ROOT / "NOTICE", "NOTICE.txt"),
    (REPO_ROOT / "USER_TERMS.md", "USER_TERMS.md"),
    (REPO_ROOT / "USER_TERMS.zh-CN.md", "USER_TERMS.zh-CN.md"),
    (REPO_ROOT / "OPEN_SOURCE_RELEASE_NOTICE.txt", "OPEN_SOURCE_RELEASE_NOTICE.txt"),
    (REPO_ROOT / "OPEN_SOURCE_RELEASE_NOTICE.zh-CN.txt", "OPEN_SOURCE_RELEASE_NOTICE.zh-CN.txt"),
)

THIRD_PARTY_FILES = {
    "studio": (
        (LEGAL_SOURCE_DIRS["studio"] / "Qt6-THIRD-PARTY-NOTICES.txt", "Qt6-THIRD-PARTY-NOTICES.txt"),
        (LEGAL_SOURCE_DIRS["studio"] / "Qt6-SOURCES.json", "Qt6-SOURCES.json"),
        (LEGAL_SOURCE_DIRS["studio"] / "LGPL-3.0.txt", "Qt6-LGPL-3.0.txt"),
        (LEGAL_SOURCE_DIRS["studio"] / "GPL-3.0.txt", "Qt6-GPL-3.0.txt"),
        (LEGAL_SOURCE_DIRS["studio"] / "QWindowKit-LICENSE.txt", "QWindowKit-LICENSE.txt"),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "QtADS" / "LICENSE",
            "QtADS-LICENSE.txt",
        ),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "QtADS" / "gnu-lgpl-v2.1.md",
            "QtADS-LGPL-2.1.txt",
        ),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "libogg" / "COPYING",
            "libogg-COPYING.txt",
        ),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "libvorbis" / "COPYING",
            "libvorbis-COPYING.txt",
        ),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "r8brain-free-src" / "LICENSE",
            "r8brain-LICENSE.txt",
        ),
        (LEGAL_SOURCE_DIRS["studio"] / "OouraFFT-NOTICE.txt", "OouraFFT-NOTICE.txt"),
        (
            REPO_ROOT / "HarkenStudio" / "ThirdParty" / "lmdb" / "LICENSE",
            "LMDB-LICENSE.txt",
        ),
    ),
    "hub": (
        (LEGAL_SOURCE_DIRS["hub"] / "Qt6-THIRD-PARTY-NOTICES.txt", "Qt6-THIRD-PARTY-NOTICES.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "Qt6-SOURCES.json", "Qt6-SOURCES.json"),
        (LEGAL_SOURCE_DIRS["hub"] / "LGPL-3.0.txt", "Qt6-LGPL-3.0.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "GPL-3.0.txt", "Qt6-GPL-3.0.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "QWindowKit-LICENSE.txt", "QWindowKit-LICENSE.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "minizip-ng-LICENSE.txt", "minizip-ng-LICENSE.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "ed25519-LICENSE.txt", "ed25519-LICENSE.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "zlib-LICENSE.txt", "zlib-LICENSE.txt"),
        (LEGAL_SOURCE_DIRS["hub"] / "Heroicons-LICENSE.txt", "Heroicons-LICENSE.txt"),
    ),
}


def assemble(product: str, output_dir: Path, qt_version: str) -> list[str]:
    recorded_version = json.loads(
        (LEGAL_SOURCE_DIRS[product] / "Qt6-SOURCES.json").read_text(encoding="utf-8")
    )["qt_version"]
    if qt_version != recorded_version:
        raise SystemExit(
            f"error: Qt {qt_version} does not match the reviewed notices for Qt "
            f"{recorded_version}; update {LEGAL_SOURCE_DIRS[product]}"
        )
    entries = COMMON_FILES + THIRD_PARTY_FILES[product]
    missing = [str(source) for source, _ in entries if not source.is_file()]
    if missing:
        raise SystemExit(
            "error: missing legal source files:\n" + "\n".join(missing)
        )
    if output_dir.exists():
        shutil.rmtree(output_dir)
    third_party_dir = output_dir / "third-party"
    third_party_dir.mkdir(parents=True)
    written = []
    for source, name in COMMON_FILES:
        shutil.copyfile(source, output_dir / name)
        written.append(name)
    for source, name in THIRD_PARTY_FILES[product]:
        shutil.copyfile(source, third_party_dir / name)
        written.append(f"third-party/{name}")
    return written


def verify_files(entries: tuple[tuple[Path, str], ...], directory: Path) -> None:
    for source, name in entries:
        destination = directory / name
        if not destination.is_file() or destination.is_symlink():
            raise SystemExit(f"error: required legal file is missing: {destination}")
        if not source.is_file() or not source.stat().st_size:
            raise SystemExit(f"error: legal source is missing or empty: {source}")
        if destination.read_text(encoding="utf-8") != source.read_text(encoding="utf-8"):
            raise SystemExit(f"error: legal file does not match this checkout: {destination}")


def verify(product: str, directory: Path) -> None:
    verify_files(COMMON_FILES, directory)
    verify_files(THIRD_PARTY_FILES[product], directory / "third-party")


def verify_sdk(directory: Path) -> None:
    source = REPO_ROOT / "HarkenSDK" / "Unity" / "Harken"
    verify_files(tuple((source / name, name) for name in ("LICENSE.md", "NOTICE.md")),
                 directory / "Unity" / "Harken")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--product", choices=sorted(THIRD_PARTY_FILES))
    target.add_argument("--sdk-root", help="Verify licenses in a staged Unity SDK bundle")
    parser.add_argument("--output")
    parser.add_argument("--qt-version", help="Version reported by the deployed Qt kit's qmake")
    parser.add_argument("--verify", action="store_true", help="Verify an existing Legal directory")
    args = parser.parse_args(argv)
    if args.product and not args.output:
        parser.error("--product requires --output")
    if args.product and not args.verify and not args.qt_version:
        parser.error("assembling legal texts requires --qt-version")
    if args.sdk_root and (args.output or args.qt_version or args.verify):
        parser.error("--sdk-root cannot be combined with desktop packaging options")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.sdk_root:
        verify_sdk(Path(args.sdk_root).resolve())
    elif args.verify:
        verify(args.product, Path(args.output).resolve())
    else:
        for name in assemble(args.product, Path(args.output).resolve(), args.qt_version):
            print(name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
