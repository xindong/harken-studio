#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
CANONICAL_HEADER = REPOSITORY / "HarkenEngine" / "Source" / "Runtime" / "api" / "include" / "harken" / "Harken.h"
EXPORTS = REPOSITORY / "HarkenEngine" / "Source" / "Runtime" / "api" / "abi" / "HarkenCore.def"
UNREAL = REPOSITORY / "HarkenSDK" / "Unreal" / "Harken"
FORBIDDEN = {
    "VnActionDesc",
    "VnActionInstanceHandle",
    "VnActionStep",
    "Vn_FindActionId",
    "Vn_RegisterAction",
    "Vn_StopAction",
    "Vn_TriggerActionEx",
    "VN_STEP_PLAY",
    "Vn_ClearEmitterValue",
    "Vn_ClearEmitterValues",
}


def source_text(sdk: Path) -> str:
    source_root = sdk / "Source"
    paths = [
        path for path in sorted(source_root.rglob("*"))
        if path.suffix in {".cpp", ".h"} and path.is_file()
        and not path.is_relative_to(source_root / "HarkenCore" / "Include")
    ]
    if not paths:
        raise SystemExit(f"Unreal SDK sources are missing: {source_root}")
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


def declared_functions(header: str) -> set[str]:
    return set(re.findall(r"\b(Vn_[A-Za-z0-9_]+)\s*\(", header))


def exported_functions(exports: str) -> set[str]:
    return {
        line.strip()
        for line in exports.splitlines()
        if line.strip().startswith("Vn_")
    }


def verify(require_embedded_header: bool, sdk: Path = UNREAL) -> None:
    header = CANONICAL_HEADER.read_text(encoding="utf-8")
    exports = EXPORTS.read_text(encoding="utf-8")
    source = source_text(sdk)
    called = set(re.findall(r"\b(Vn_[A-Za-z0-9_]+)\s*\(", source))
    missing_declarations = sorted(called - declared_functions(header))
    missing_exports = sorted(called - exported_functions(exports))
    forbidden = sorted(
        token
        for token in FORBIDDEN
        if re.search(r"\b" + re.escape(token) + r"\b", source)
    )
    errors = []
    if missing_declarations:
        errors.append("Unreal calls missing from Harken.h: " + ", ".join(missing_declarations))
    if missing_exports:
        errors.append("Unreal calls missing from HarkenCore.def: " + ", ".join(missing_exports))
    if forbidden:
        errors.append("Removed Action API tokens remain: " + ", ".join(forbidden))
    if require_embedded_header:
        for canonical in sorted(CANONICAL_HEADER.parent.rglob("*")):
            if not canonical.is_file():
                continue
            name = canonical.relative_to(CANONICAL_HEADER.parent)
            embedded = sdk / "Source" / "HarkenCore" / "Include" / "harken" / name
            if not embedded.is_file():
                errors.append(f"Embedded header is missing: {embedded}")
            elif embedded.read_bytes() != canonical.read_bytes():
                errors.append(f"Embedded Unreal {name} differs from the canonical header")
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Verified {len(called)} Unreal native calls against the canonical header and exports.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk-root", type=Path, default=UNREAL)
    parser.add_argument("--require-embedded-header", action="store_true")
    arguments = parser.parse_args()
    verify(arguments.require_embedded_header, arguments.sdk_root.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
