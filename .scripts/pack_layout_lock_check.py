#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORMAT_DIR = "HarkenEngine/Format/Pack"
LOCK_LINE = re.compile(r"^\s*HK_PACK_(?:SIZE|OFFSET)\(.*\);\s*$", re.M)
VERSION = re.compile(r"^#define HK_PACK_FORMAT_VERSION (\d+)u\s*$", re.M)


def lock_lines(files: dict[str, str]) -> list[str]:
    lines = []
    for name in sorted(files):
        lines += [f"{name}: {line.strip()}" for line in LOCK_LINE.findall(files[name])]
    return lines


def format_version(files: dict[str, str]) -> int | None:
    for text in files.values():
        match = VERSION.search(text)
        if match:
            return int(match.group(1))
    return None


def check(base: dict[str, str], head: dict[str, str]) -> list[str]:
    if not base:
        return []
    if lock_lines(base) == lock_lines(head):
        return []
    if format_version(head) != format_version(base):
        return []
    removed = sorted(set(lock_lines(base)) - set(lock_lines(head)))
    added = sorted(set(lock_lines(head)) - set(lock_lines(base)))
    return ["Pack layout lock changed without raising HK_PACK_FORMAT_VERSION"] + \
        [f"- {line}" for line in removed] + [f"+ {line}" for line in added]


def current_files(root: Path) -> dict[str, str]:
    directory = root / FORMAT_DIR
    return {path.name: path.read_text(encoding="utf-8") for path in sorted(directory.glob("*.h"))}


def base_files(root: Path, ref: str) -> dict[str, str]:
    listing = subprocess.run(["git", "ls-tree", "--name-only", f"{ref}:{FORMAT_DIR}"], cwd=root,
                             capture_output=True, text=True)
    if listing.returncode != 0:
        return {}
    files = {}
    for name in listing.stdout.split():
        if name.endswith(".h"):
            files[name] = subprocess.run(["git", "show", f"{ref}:{FORMAT_DIR}/{name}"], cwd=root,
                                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    problems = check(base_files(args.root, args.base), current_files(args.root))
    for line in problems:
        print(line)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
