from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


class BuildInfoTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary_root = REPO_ROOT / ".temp"
        temporary_root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="build-info-", dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "HarkenHub").mkdir()
        (self.root / "VERSION").write_text("0.7.0\n", encoding="utf-8")
        (self.root / "HarkenHub/VERSION").write_text("0.1.56\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(BuildInfoTest LANGUAGES NONE)\n"
            'set(HARKEN_SCHEMA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/HarkenSchema")\n'
            f'include("{(REPO_ROOT / "cmake/BuildInfo.cmake").as_posix()}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/identity.txt" '
            '"${HARKEN_VERSION}|${HARKEN_HUB_VERSION}|${HARKEN_BI_BUILD_CHANNEL}|'
            '${HARKEN_BI_VERSION_QUAD}|${HARKEN_BI_HUB_VERSION_QUAD}|'
            '${HARKEN_BI_FILEFLAGS}|${HARKEN_BI_HUB_FILEFLAGS}")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/contract-definitions.txt" '
            'CONTENT "$<JOIN:$<TARGET_PROPERTY:harken_buildinfo,INTERFACE_COMPILE_DEFINITIONS>,\\n>")\n'
            'add_custom_target(version_snapshot COMMAND "${CMAKE_COMMAND}" -E true)\n',
            encoding="utf-8",
        )
        self.environment = dict(os.environ, GITHUB_ACTIONS="false", HARKEN_CI_RUN_NUMBER="",
                                GITHUB_RUN_NUMBER="", HARKEN_CI_RUN_ATTEMPT="", GITHUB_RUN_ATTEMPT="")

    def configure(self, channel: str, *options: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["cmake", "-S", str(self.root), "-B", str(self.root / "build"),
             f"-DHARKEN_BUILD_CHANNEL={channel}", *options],
            env=self.environment, capture_output=True, text=True, encoding="utf-8", check=False,
        )

    def write_manifest(self, tree: str, versions: list[int]) -> None:
        path = self.root / "HarkenSchema" / tree / "manifest.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"published": versions}), encoding="utf-8")

    def contract_definitions(self) -> set[str]:
        return set((self.root / "build/contract-definitions.txt").read_text(encoding="utf-8").splitlines())

    def test_channel_controls_prerelease_flags_without_changing_versions(self) -> None:
        for channel in ("stable", "beta", "dev", "nightly"):
            with self.subTest(channel=channel):
                result = self.configure(channel)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                flags = "8L" if channel == "stable" else "10L"
                self.assertEqual(
                    (self.root / "build/identity.txt").read_text(encoding="utf-8"),
                    f"0.7.0|0.1.56|{channel}|0.7.0.0|0.1.56.0|{flags}|{flags}",
                )

    def test_studio_version_rejects_channel_suffix(self) -> None:
        (self.root / "VERSION").write_text("0.7.0-dev\n", encoding="utf-8")
        result = self.configure("dev")
        self.assertNotEqual(result.returncode, 0)

    def test_hub_version_rejects_channel_suffix(self) -> None:
        (self.root / "HarkenHub/VERSION").write_text("0.1.56-dev\n", encoding="utf-8")
        result = self.configure("dev")
        self.assertNotEqual(result.returncode, 0)

    def test_build_refreshes_independent_product_and_contract_versions(self) -> None:
        self.write_manifest("Editor", [7, 14])
        self.write_manifest("Pack", [3, 29])
        result = self.configure("dev", "-DHARKEN_BUILD_CORE=ON", "-DHARKEN_BUILD_EDITOR=ON",
                                "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for source, value, expected in (
            ("VERSION", "0.8.1\n", ("0.8.1", 14, 29)),
            ("HarkenSchema/Editor/manifest.json", json.dumps({"published": [7, 14, 15]}), ("0.8.1", 15, 29)),
            ("HarkenSchema/Pack/manifest.json", json.dumps({"published": [3, 29, 30]}), ("0.8.1", 15, 30)),
        ):
            with self.subTest(source=source):
                time.sleep(1.1)
                (self.root / source).write_text(value, encoding="utf-8")
                result = subprocess.run(
                    ["cmake", "--build", str(self.root / "build"), "--target", "version_snapshot"],
                    env=self.environment, capture_output=True, text=True, encoding="utf-8", check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                product_version, project_version, pack_version = expected
                self.assertEqual(
                    (self.root / "build/identity.txt").read_text(encoding="utf-8").split("|")[:2],
                    [product_version, "0.1.56"],
                )
                self.assertEqual(self.contract_definitions(), {
                    f"HARKEN_PROJECT_SCHEMA_VERSION={project_version}",
                    f"HARKEN_PACK_SCHEMA_VERSION={pack_version}",
                })

    def test_core_identity_requires_only_pack_contract_without_python(self) -> None:
        self.write_manifest("Pack", [3, 31])
        result = self.configure("dev", "-DHARKEN_BUILD_CORE=ON",
                                "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.contract_definitions(), {"HARKEN_PACK_SCHEMA_VERSION=31"})


if __name__ == "__main__":
    unittest.main()
