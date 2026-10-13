from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]


class UnrealSdkContractTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary_root = REPOSITORY / ".temp"
        temporary_root.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        repository = self.root / "repository"
        self.script = repository / ".scripts/check_unreal_sdk_contract.py"
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(REPOSITORY / ".scripts/check_unreal_sdk_contract.py", self.script)
        api = repository / "HarkenEngine/Source/Runtime/api"
        self.headers = api / "include/harken"
        self.public_headers = {
            "Harken.h": '#include "harken/ObjectId.h"\nvoid Vn_FixturePlay();\n',
            "ObjectId.h": '#include "generated/Types.hpp"\nusing ObjectId = FixtureId;\n',
            "generated/Types.hpp": "using FixtureId = unsigned long long;\n",
        }
        for name, content in self.public_headers.items():
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        exports = api / "abi/HarkenCore.def"
        exports.parent.mkdir(parents=True)
        exports.write_text("EXPORTS\nVn_FixturePlay\n", encoding="utf-8")
        self.default_sdk = repository / "HarkenSDK/Unreal/Harken"
        self.sdk = self.root / "selected artifact"
        for sdk in (self.default_sdk, self.sdk):
            shutil.copytree(self.headers, sdk / "Source/HarkenCore/Include/harken")
            source = sdk / "Source/HarkenRuntime/Private/Fixture.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("void Play() { Vn_FixturePlay(); }\n", encoding="utf-8")

    def check_sdk(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.script), "--sdk-root", str(self.sdk),
             "--require-embedded-header"],
            cwd=self.root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=30,
        )

    def test_complete_selected_artifact_does_not_depend_on_default_sdk(self) -> None:
        shutil.rmtree(self.default_sdk)
        result = self.check_sdk()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_public_header_must_be_present_and_identical(self) -> None:
        result = self.check_sdk()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for relative in self.public_headers:
            embedded = self.sdk / "Source/HarkenCore/Include/harken" / relative
            original = embedded.read_bytes()
            for corruption in ("missing", "changed"):
                with self.subTest(header=relative, corruption=corruption):
                    if corruption == "missing":
                        embedded.unlink()
                    else:
                        embedded.write_bytes(original + b"\nusing UnexpectedId = int;\n")
                    try:
                        result = self.check_sdk()
                        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    finally:
                        embedded.write_bytes(original)

    def test_embedded_headers_cannot_substitute_for_sdk_sources(self) -> None:
        for missing in ("HarkenRuntime", "Source"):
            with self.subTest(missing=missing):
                source = self.sdk / "Source"
                shutil.rmtree(source / "HarkenRuntime" if missing == "HarkenRuntime" else source)
                result = self.check_sdk()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_calls_are_checked_in_the_selected_artifact(self) -> None:
        source = self.sdk / "Source/HarkenRuntime/Private/Fixture.cpp"
        source.write_text("void Play() { Vn_NotInContract(); }\n", encoding="utf-8")
        result = self.check_sdk()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
