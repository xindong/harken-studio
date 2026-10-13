from __future__ import annotations

import os
import subprocess
import tempfile
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
        (self.root / "VERSION").write_text("0.3.0\n", encoding="utf-8")
        (self.root / "HarkenHub/VERSION").write_text("0.1.47\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(BuildInfoTest LANGUAGES NONE)\n"
            f'include("{(REPO_ROOT / "cmake/BuildInfo.cmake").as_posix()}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/identity.txt" '
            '"${HARKEN_VERSION}|${HARKEN_HUB_VERSION}|${HARKEN_BI_BUILD_CHANNEL}|'
            '${HARKEN_BI_VERSION_QUAD}|${HARKEN_BI_HUB_VERSION_QUAD}|'
            '${HARKEN_BI_FILEFLAGS}|${HARKEN_BI_HUB_FILEFLAGS}")\n',
            encoding="utf-8",
        )

    def configure(self, channel: str) -> subprocess.CompletedProcess[str]:
        environment = dict(os.environ, GITHUB_ACTIONS="false", HARKEN_CI_RUN_NUMBER="",
                           GITHUB_RUN_NUMBER="", HARKEN_CI_RUN_ATTEMPT="", GITHUB_RUN_ATTEMPT="")
        return subprocess.run(
            ["cmake", "-S", str(self.root), "-B", str(self.root / "build"),
             f"-DHARKEN_BUILD_CHANNEL={channel}"],
            env=environment, capture_output=True, text=True, encoding="utf-8", check=False,
        )

    def test_channel_controls_prerelease_flags_without_changing_versions(self) -> None:
        for channel in ("stable", "beta", "dev", "nightly"):
            with self.subTest(channel=channel):
                result = self.configure(channel)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                flags = "8L" if channel == "stable" else "10L"
                self.assertEqual(
                    (self.root / "build/identity.txt").read_text(encoding="utf-8"),
                    f"0.3.0|0.1.47|{channel}|0.3.0.0|0.1.47.0|{flags}|{flags}",
                )

    def test_studio_version_rejects_channel_suffix(self) -> None:
        (self.root / "VERSION").write_text("0.3.0-dev\n", encoding="utf-8")
        result = self.configure("dev")
        self.assertNotEqual(result.returncode, 0)

    def test_hub_version_rejects_channel_suffix(self) -> None:
        (self.root / "HarkenHub/VERSION").write_text("0.1.47-dev\n", encoding="utf-8")
        result = self.configure("dev")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
