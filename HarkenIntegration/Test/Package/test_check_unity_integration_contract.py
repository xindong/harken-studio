from __future__ import annotations

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "check_unity_integration_contract",
    Path(__file__).resolve().with_name("check_unity_integration_contract.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class UnityIntegrationContractTest(unittest.TestCase):
    def copy_integration(self, directory: str) -> Path:
        target = Path(directory) / "HarkenIntegration"
        shutil.copytree(REPOSITORY / "HarkenIntegration", target)
        return target / "Unity" / "Harken"

    def test_repository_source_contract_passes(self) -> None:
        MODULE.verify(REPOSITORY / "HarkenIntegration", False)

    def test_package_file_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            (integration / "README.md.meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_folder_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            (integration / "Runtime/API.meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_meta_without_asset_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            (integration / "Runtime/Removed.cs.meta").write_text(
                "fileFormatVersion: 2\n", encoding="utf-8"
            )
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_repository_native_binary_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            binary = integration / "Runtime/Plugins/x86_64/HarkenEngine.dll"
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_csharp_source_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            source = integration / "Runtime/API/Voice.cs"
            source.with_name(source.name + ".meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_assembly_definition_without_meta_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            assembly = integration / "Runtime/Harken.Core.asmdef"
            assembly.with_name(assembly.name + ".meta").unlink()
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_assembled_package_requires_every_native_binary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            missing = "Runtime/Plugins/iOS/libHarkenEngineDev.a"
            for relative in MODULE.NATIVE_PLUGIN_PATHS - {missing}:
                binary = integration / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, True)

    def test_assembled_package_with_release_binaries_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            for relative in MODULE.NATIVE_PLUGIN_PATHS:
                binary = integration / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            MODULE.verify(integration, True)

    def test_assembled_package_rejects_android_armv7(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            for relative in MODULE.NATIVE_PLUGIN_PATHS:
                binary = integration / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"runtime")
            binary = integration / "Runtime/Plugins/Android/armeabi-v7a/libHarkenEngine.so"
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b"runtime")
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, True)

    def test_sample_declaration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            package_path = integration / "package.json"
            package = json.loads(package_path.read_text(encoding="utf-8"))
            package["samples"] = [{"path": "Samples~/Demo"}]
            package_path.write_text(json.dumps(package), encoding="utf-8")
            with self.assertRaises(SystemExit):
                MODULE.verify(integration, False)

    def test_generated_binding_must_not_project_the_preview_surface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            binding = integration / "Runtime/Generated/HarkenNative.cs"
            binding.write_text(
                binding.read_text(encoding="utf-8") + "\n// VnPreview_PumpNullMixer\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "development preview surface"):
                MODULE.verify(integration, False)

    def test_voice_wrapper_must_forward_to_the_matching_native_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            source_path = integration / "Runtime/API/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            source_path.write_text(
                source.replace(
                    "HarkenNative.Vn_SetVoicePan(Handle, pan)",
                    "HarkenNative.Vn_SetVoiceWidth(Handle, pan)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, 'Voice\\.SetPan'):
                MODULE.verify(integration, False)

    def test_voice_wrapper_ignores_a_correct_call_left_only_in_a_comment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            source_path = integration / "Runtime/API/Voice.cs"
            source = source_path.read_text(encoding="utf-8")
            correct = "HarkenNative.Vn_SetVoicePan(Handle, pan)"
            source_path.write_text(
                source.replace(correct, "HarkenNative.Vn_SetVoiceWidth(Handle, pan)")
                + f"\n/* public VnResult SetPan(float pan) => {correct}; */\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, 'Voice\\.SetPan'):
                MODULE.verify(integration, False)

    def test_voice_wrapper_contract_rejects_interpolated_strings(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            source_path = integration / "Runtime/API/Voice.cs"
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
                MODULE.verify(integration, False)

    def test_voice_wrapper_contract_rejects_preprocessor_branches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            integration = self.copy_integration(directory)
            source_path = integration / "Runtime/API/Voice.cs"
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
                MODULE.verify(integration, False)

if __name__ == "__main__":
    unittest.main()
