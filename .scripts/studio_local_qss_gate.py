#!/usr/bin/env python3
"""Fail when a Harken Studio file gains local QSS without a declared owner.

Studio visual ownership belongs to the application stylesheet plus semantic
widget properties. A `setStyleSheet()` call on a business widget expands the
active theme tokens into a construction-time string, so the widget keeps the
old theme after a switch unless something rebuilds or reapplies it. Every
remaining call site therefore has to belong to a file listed below.

The list is the whole configuration: an entry is either present or the gate
fails. There is no baseline, no occurrence count and no free-text justification
field, because a script can check none of those.

  py -3 .scripts/studio_local_qss_gate.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCAN_ROOT = "HarkenStudio"
EXCLUDED_PREFIXES = ("HarkenStudio/ThirdParty/", "HarkenStudio/Test/")
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})

# Files allowed to install local QSS, with the owner that justifies it.
LOCAL_QSS_OWNERS: dict[str, str] = {
    "HarkenStudio/Source/Core/QtAdsThemeAdapter.cpp": "third-party adapter",
    "HarkenStudio/Source/Core/ThemeManager.cpp": "application skin",
    "HarkenStudio/Source/Widgets/Common/CollapsibleSection.cpp": "component skin, runtime indicator color",
    "HarkenStudio/Source/Widgets/Common/LayoutDebugTint.h": "protected debug scaffolding",
    "HarkenStudio/Source/Widgets/Inspector/Drawers/ColorFieldDrawer.cpp": "runtime dynamic visual",
    "HarkenStudio/Source/Widgets/Inspector/WidgetFactory.cpp": "runtime dynamic visual",
    "HarkenStudio/Source/Widgets/TitleBar/TitleBarWidget.cpp": "unowned, tracked as known debt",
}

CALL_PATTERN = re.compile(r"\bsetStyleSheet\s*\(")
RAW_STRING_OPEN = re.compile(r'R"([^()\\\s]{0,16})\(')


def _skip_quoted(text: str, start: int) -> int:
    quote = text[start]
    index = start + 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
            continue
        if text[index] == quote:
            return index + 1
        if text[index] == "\n":
            return index
        index += 1
    return index


def strip_non_code(text: str) -> str:
    """Blank comments and literals with spaces, keeping every offset and newline.

    Offsets survive so reported line numbers match the original file, and blanked
    literals keep a quoted error message from being mistaken for a call site.
    """
    chars = list(text)
    index = 0
    size = len(text)

    def blank(start: int, end: int) -> int:
        for position in range(start, min(end, size)):
            if chars[position] != "\n":
                chars[position] = " "
        return min(end, size)

    while index < size:
        char = text[index]
        if char == "/" and text.startswith("//", index):
            end = text.find("\n", index)
            index = blank(index, size if end < 0 else end)
        elif char == "/" and text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = blank(index, size if end < 0 else end + 2)
        elif char == '"' and index and text[index - 1] == "R":
            match = RAW_STRING_OPEN.match(text, index - 1)
            if not match:
                index = blank(index, _skip_quoted(text, index))
                continue
            terminator = ")" + match.group(1) + '"'
            end = text.find(terminator, match.end())
            index = blank(index, size if end < 0 else end + len(terminator))
        elif char in "\"'":
            index = blank(index, _skip_quoted(text, index))
        else:
            index += 1
    return "".join(chars)


def call_sites(root: Path) -> dict[str, list[tuple[int, str]]]:
    """Map each relative path to its (line number, source line) local QSS calls."""
    found: dict[str, list[tuple[int, str]]] = {}
    scan_root = root / SCAN_ROOT
    for path in sorted(scan_root.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative.startswith(EXCLUDED_PREFIXES):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        code = strip_non_code(text)
        hits = [
            (code.count("\n", 0, match.start()) + 1, match.start())
            for match in CALL_PATTERN.finditer(code)
        ]
        if not hits:
            continue
        lines = text.splitlines()
        found[relative] = [
            (number, lines[number - 1].strip() if number <= len(lines) else "")
            for number, _ in hits
        ]
    return found


def report(found: dict[str, list[tuple[int, str]]], stream) -> int:
    unowned = {path: sites for path, sites in found.items() if path not in LOCAL_QSS_OWNERS}
    stale = sorted(path for path in LOCAL_QSS_OWNERS if path not in found)

    for path in sorted(unowned):
        for number, source in unowned[path]:
            print(f"{path}:{number}: unowned local QSS: {source}", file=stream)
    if unowned:
        print(
            "\nLocal QSS freezes the active theme into a construction-time snapshot.\n"
            "Prefer a semantic property plus a rule in\n"
            "HarkenStudio/Resources/styles/dark_theme.qss. If the call really owns a\n"
            "runtime-computed visual, add the file to LOCAL_QSS_OWNERS in\n"
            ".scripts/studio_local_qss_gate.py together with the owning category.",
            file=stream,
        )

    for path in stale:
        print(f"{path}: listed in LOCAL_QSS_OWNERS but installs no local QSS", file=stream)
    if stale:
        print("\nRemove the stale entries so the list keeps matching the code.", file=stream)

    if unowned or stale:
        return 1
    print(f"Local QSS ownership: {len(found)} owned files, no unowned call sites.", file=stream)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(REPO_ROOT), help="repository root to scan")
    arguments = parser.parse_args(argv)
    return report(call_sites(Path(arguments.root).resolve()), sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
