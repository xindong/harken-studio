#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
UNITY_PACKAGE = REPOSITORY / "HarkenIntegration" / "Unity" / "Harken" / "package.json"


def version(package: Path | None = None) -> str:
    package = UNITY_PACKAGE if package is None else package
    manifest = json.loads(package.read_text(encoding="utf-8"))
    value = manifest.get("version") if isinstance(manifest, dict) else None
    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(f"{package} must define a non-empty version without surrounding whitespace")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.parse_args()
    print(f"Verified Harken Unity SDK {version()}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
