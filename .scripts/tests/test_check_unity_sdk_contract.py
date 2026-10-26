from __future__ import annotations

import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path

from script_loader import load_script_module


SCRIPT = Path(__file__).resolve().parents[1] / "check_unity_sdk_contract.py"
MODULE = load_script_module("check_unity_sdk_contract", SCRIPT)
REPOSITORY = Path(__file__).resolve().parents[2]
SDK_GATE = REPOSITORY / ".github/workflows/pr-gate-integration.yml"


def workflow_job(text: str, name: str) -> str:
    match = re.search(rf"^  {re.escape(name)}:\s*$", text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"Missing workflow job: {name}")
    next_job = re.search(r"^  [a-z0-9-]+:\s*$", text[match.end():], re.MULTILINE)
    end = match.end() + next_job.start() if next_job else len(text)
    return text[match.start():end]


class UnitySdkContractTest(unittest.TestCase):
    def copy_sdk(self, directory: str) -> Path:
        target = Path(directory) / "HarkenIntegration"
        shutil.copytree(REPOSITORY / "HarkenIntegration", target)
        return target / "Unity" / "Harken"

    def test_repository_source_contract_passes(self) -> None:
        MODULE.verify(REPOSITORY / "HarkenIntegration", False)

    def test_unity_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            (sdk / "README.md.meta").write_text(
                "fileFormatVersion: 2\n", encoding="utf-8"
            )
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_repository_native_binary_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            binary = sdk / "Runtime/Plugins/x86_64/HarkenCore.dll"
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_csharp_source_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            source = sdk / "Runtime/Sdk/Voice.cs"
            source.with_name(source.name + ".meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_assembly_definition_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            assembly = sdk / "Runtime/Harken.Core.asmdef"
            assembly.with_name(assembly.name + ".meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_assembled_package_requires_every_native_binary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            missing = "Runtime/Plugins/iOS/libHarkenCore.a"
            for relative in MODULE.NATIVE_PLUGIN_PATHS - {missing}:
                binary = sdk / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, True)

    def test_assembled_package_with_release_binaries_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            for relative in MODULE.NATIVE_PLUGIN_PATHS:
                binary = sdk / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            MODULE.verify(sdk, True)

    def test_assembled_package_rejects_android_armv7(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            for relative in MODULE.NATIVE_PLUGIN_PATHS:
                binary = sdk / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            binary = sdk / "Runtime/Plugins/Android/armeabi-v7a/libHarkenCore.so"
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, True)

    def test_sample_declaration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            package_path = sdk / "package.json"
            package = json.loads(package_path.read_text(encoding="utf-8"))
            package["samples"] = [{"path": "Samples~/Demo"}]
            package_path.write_text(json.dumps(package), encoding="utf-8")
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_voice_wrapper_must_forward_to_the_matching_native_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            source_path = sdk / "Runtime/Sdk/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            source_path.write_text(
                source.replace(
                    "HarkenNative.Vn_SetVoicePan(Handle, pan)",
                    "HarkenNative.Vn_SetVoiceWidth(Handle, pan)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, 'Voice\\.SetPan'):
                MODULE.verify(sdk, False)

    def test_voice_wrapper_ignores_a_correct_call_left_only_in_a_comment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            source_path = sdk / "Runtime/Sdk/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            correct = "HarkenNative.Vn_SetVoicePan(Handle, pan)"
            source_path.write_text(
                source.replace(correct, "HarkenNative.Vn_SetVoiceWidth(Handle, pan)")
                + f"\n/* public VnResult SetPan(float pan) => {correct}; */\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, 'Voice\\.SetPan'):
                MODULE.verify(sdk, False)

    def test_voice_wrapper_contract_rejects_interpolated_strings(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            source_path = sdk / "Runtime/Sdk/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            correct = (
                "public VnResult SetPan(float pan) => "
                "HarkenNative.Vn_SetVoicePan(Handle, pan);"
            )
            source_path.write_text(
                source.replace(
                    "HarkenNative.Vn_SetVoicePan(Handle, pan)",
                    "HarkenNative.Vn_SetVoiceWidth(Handle, pan)",
                ).replace(
                    "public bool Equals(Voice other)",
                    f'private string Probe() => $"{{Identity("{correct}")}}";\n\n'
                    "        public bool Equals(Voice other)",
                ),
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_voice_wrapper_contract_rejects_preprocessor_branches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.copy_sdk(directory)
            source_path = sdk / "Runtime/Sdk/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            correct = (
                "public VnResult SetPan(float pan)\n"
                "            => HarkenNative.Vn_SetVoicePan(Handle, pan);"
            )
            wrong = (
                "public VnResult SetPan(float pan)\n"
                "            => HarkenNative.Vn_SetVoiceWidth(Handle, pan);"
            )
            source_path.write_text(
                source.replace(
                    correct,
                    f"#if false\n        {correct}\n#else\n        {wrong}\n#endif",
                ),
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit):
                MODULE.verify(sdk, False)

    def test_self_hosted_job_rejects_external_pull_requests(self) -> None:
        job = workflow_job(
            SDK_GATE.read_text(encoding="utf-8"), "integration-windows"
        )
        self.assertIn("github.event_name != 'pull_request'", job)
        self.assertIn(
            "github.event.pull_request.head.repo.full_name == github.repository",
            job,
        )

if __name__ == "__main__":
    unittest.main()
