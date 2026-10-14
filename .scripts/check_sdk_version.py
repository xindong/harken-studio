#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
VERSION_FILE = REPOSITORY / "HarkenSDK" / "VERSION"
UNITY_PACKAGE = REPOSITORY / "HarkenSDK" / "Unity" / "Harken" / "package.json"
UNREAL_PLUGIN = REPOSITORY / "HarkenSDK" / "Unreal" / "Harken" / "Harken.uplugin"
UNITY_PACKAGE_VERSION = "0.3.2"


def version() -> str:
    value = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not value:
        raise ValueError("HarkenSDK/VERSION is empty")
    return value


def verify() -> str:
    expected = version()
    unity = json.loads(UNITY_PACKAGE.read_text(encoding="utf-8"))["version"]
    unreal = json.loads(UNREAL_PLUGIN.read_text(encoding="utf-8"))["VersionName"]
    if unity != UNITY_PACKAGE_VERSION:
        raise ValueError(
            f"Harken Unity SDK version {unity} does not match "
            f"{UNITY_PACKAGE_VERSION}"
        )
    if unreal != expected:
        raise ValueError(f"Unreal SDK version {unreal} does not match {expected}")
    return expected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.parse_args()
    print(
        f"Verified Harken Unity SDK {UNITY_PACKAGE_VERSION} and "
        f"Harken Unreal SDK {verify()}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
