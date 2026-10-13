#!/usr/bin/env python3
"""Generate Hub resources and installer text from the root user terms."""

from __future__ import annotations

import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TERMS_FILES = {"en": "USER_TERMS.md", "zh-CN": "USER_TERMS.zh-CN.md"}
SECTION_SEPARATOR = "\n---\n\n"


def read_documents(source: Path) -> tuple[str, str]:
    sections = source.read_text(encoding="utf-8").split(SECTION_SEPARATOR)
    if len(sections) != 2:
        raise ValueError(f"{source}: expected two documents separated by a blank line, --- and a blank line")
    documents = []
    for section in sections:
        if not section.startswith("# ") or not section.endswith("\n"):
            raise ValueError(f"{source}: each document must start with an H1 title and end with a newline")
        document = section[2:]
        title, separator, body = document.partition("\n\n")
        if not separator or not title.strip() or "\n" in title or not body.strip():
            raise ValueError(f"{source}: each document needs a title, a blank line and nonempty text")
        if any(line.startswith("# ") for line in body.splitlines()):
            raise ValueError(f"{source}: unexpected additional H1 title")
        documents.append(document)
    return documents[0], documents[1]


def write_hub_resources(output_dir: Path, source_root: Path = REPO_ROOT) -> None:
    documents = {
        language: read_documents(source_root / name)
        for language, name in TERMS_FILES.items()
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    for language, texts in documents.items():
        suffix = ".txt" if language == "en" else ".zh-CN.txt"
        for name, text in zip(("USER_AGREEMENT", "PRIVACY_POLICY"), texts):
            (output_dir / (name + suffix)).write_bytes(text.encode("utf-8"))


def installer_text(source_root: Path = REPO_ROOT) -> str:
    return "\n\n".join(
        document.rstrip()
        for language in ("zh-CN", "en")
        for document in read_documents(source_root / TERMS_FILES[language])
    ) + "\n"


def render_rtf(text: str) -> str:
    parts = ["{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Microsoft YaHei;}}\n\\f0\\fs20\\uc1\n"]
    for char in text:
        if char in "\\{}":
            parts.append("\\" + char)
        elif char == "\n":
            parts.append("\\par\n")
        elif char == "\t":
            parts.append("\\tab ")
        elif 32 <= ord(char) < 127:
            parts.append(char)
        else:
            encoded = char.encode("utf-16-le")
            for offset in range(0, len(encoded), 2):
                unit = int.from_bytes(encoded[offset:offset + 2], "little", signed=True)
                parts.append(f"\\u{unit}?")
    parts.append("}\n")
    return "".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--hub-resources", type=Path, required=True)
    args = parser.parse_args()
    write_hub_resources(args.hub_resources, args.source_root)


if __name__ == "__main__":
    main()
