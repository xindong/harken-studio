#!/usr/bin/env python3
"""Fail if the public API probe compile unit pulls a Runtime private header."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
PROBE = REPO / "HarkenEngine" / "Test" / "UnitTest" / "MonitorPublicApiProbe.c"
CMAKE = REPO / "HarkenEngine" / "Test" / "CMakeLists.txt"
PROBE_ALLOWED = {
    "<harken/Harken.h>",
    '"monitor_fixture_decode.h"',
    '"monitor_session_codec.h"',
    "<stdint.h>",
    "<stdio.h>",
    "<stdlib.h>",
    "<string.h>",
}
HARNESS_ALLOWED = {
    "<harken/Harken.h>",
    '"monitor_fixture_decode.h"',
    '"monitor_session_codec.h"',
    "<stdint.h>",
    "<stddef.h>",
    "<cstring>",
    "<vector>",
}
PRIVATE_MARKERS = (
    "HarkenEngine/Source/Runtime/",
    "api/src/",
    "core/include/",
    "core/src/",
    "render/include/",
    "render/src/",
    "HarkenEngine/Source/Runtime/api/src",
)


def cmake_probe_sources() -> list[Path]:
    text = CMAKE.read_text(encoding="utf-8")
    start = text.find("add_executable(harken_monitor_public_api_probe")
    if start < 0:
        raise SystemExit("public API probe target missing")
    block = text[start : text.find(")", start)]
    sources: list[Path] = []
    unit = REPO / "HarkenEngine" / "Test"
    for line in block.splitlines():
        line = line.strip().rstrip(")")
        if line.startswith("UnitTest/"):
            sources.append(unit / line)
    return sources


def includes_of(path: Path) -> list[str]:
    found: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("#include"):
            found.append(line[len("#include") :].strip())
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.parse_args()
    errors: list[str] = []
    if not PROBE.is_file() or not CMAKE.is_file():
        print("error: private-header missing probe", file=sys.stderr)
        return 1
    sources = cmake_probe_sources()
    headers: list[Path] = []
    for src in sources:
        if src.suffix in {".c", ".cpp"}:
            sibling_h = src.with_suffix(".h")
            if sibling_h.is_file():
                headers.append(sibling_h)
    for path in [PROBE, *sources, *headers]:
        if not path.is_file():
            errors.append("missing %s" % path)
            continue
        for include in includes_of(path):
            if include.startswith("<harken/") and include != "<harken/Harken.h>":
                errors.append("%s includes extra public header %s" % (path.name, include))
            if any(marker in include for marker in PRIVATE_MARKERS):
                errors.append("%s private-header %s" % (path.name, include))
            if path == PROBE and include not in PROBE_ALLOWED:
                errors.append("%s unexpected include %s" % (path.name, include))
            elif path != PROBE and include not in HARNESS_ALLOWED:
                errors.append("%s unexpected include %s" % (path.name, include))
    if errors:
        for item in errors:
            print("error:", item, file=sys.stderr)
        return 1
    print("ok: public API probe include tree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
