import hashlib
import importlib.util
import os
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "release" / "build_hub_installer.py"
ACL_HEADER_PATH = (
    REPO_ROOT / "HarkenHub" / "Source" / "Platform" / "AclHardening.h"
)
WXS_PATH = REPO_ROOT / ".scripts" / "release" / "wix" / "HarkenHub.wxs"

WXS_NS = "http://wixtoolset.org/schemas/v4/wxs"
WIXUI_NS = "http://wixtoolset.org/schemas/v4/wxs/ui"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "build_hub_installer", SCRIPT_PATH
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildHubInstallerTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        staging_output = self.module.release_evidence.package_output
        self.evidence = mock.patch.object(self.module, "release_evidence").start()
        self.evidence.package_output.side_effect = staging_output
        self.addCleanup(mock.patch.stopall)
        self.evidence.prepare.side_effect = lambda product, payload, build, output, version, build_id, **kwargs: (
            output / f"{product}-{version}-{build_id}-evidence")
        self.evidence.finalize.side_effect = lambda directory, artifacts, payload, **kwargs: [
            {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size": path.stat().st_size} for path in artifacts]
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.input_dir = self.root / "deployed"
        self.output_dir = self.root / "out"

    def tearDown(self):
        self.tmp.cleanup()

    def make_input(self, with_binary=True, with_foreign=False,
                   with_bridge=True):
        self.legal_files = self.module.collect_legal_texts.assemble(
            "hub", self.input_dir / "Legal", "6.8.3"
        )
        (self.input_dir / "platforms").mkdir(parents=True)
        if with_binary:
            (self.input_dir / "HarkenHub.exe").write_bytes(b"stub-binary")
        (self.input_dir / "Qt6Core.dll").write_bytes(b"qt-core")
        (self.input_dir / "platforms" / "qwindows.dll").write_bytes(b"plugin")
        if with_bridge:
            (self.input_dir / "Updater").mkdir(parents=True)
            (self.input_dir / "Updater" / "HarkenHubUpdateBridge.exe")\
                .write_bytes(b"bridge")
            (self.input_dir / "Updater" / "Qt6Core.dll")\
                .write_bytes(b"bridge-core")
        if with_foreign:
            (self.input_dir / "HarkenEditor.exe").write_bytes(b"editor")

    def payload_root(self):
        files = self.module.collect_payload(self.input_dir)
        text = self.module.generate_payload_wxs(files, self.input_dir)
        return ET.fromstring(text)

    def test_payload_lists_all_files_with_stable_component_guids(self):
        self.make_input()
        first = self.module.generate_payload_wxs(
            self.module.collect_payload(self.input_dir), self.input_dir
        )
        second = self.module.generate_payload_wxs(
            self.module.collect_payload(self.input_dir), self.input_dir
        )
        self.assertEqual(first, second)

        root = ET.fromstring(first)
        components = root.findall(
            f".//{{{WXS_NS}}}ComponentGroup/{{{WXS_NS}}}Component"
        )
        self.assertEqual(len(components), 5 + len(self.legal_files))
        guids = {component.get("Guid") for component in components}
        self.assertEqual(len(guids), 5 + len(self.legal_files))
        for component in components:
            files = component.findall(f"{{{WXS_NS}}}File")
            self.assertEqual(len(files), 1)
            self.assertEqual(files[0].get("KeyPath"), "yes")

    def test_hub_exe_gets_referenced_file_id(self):
        self.make_input()
        root = self.payload_root()
        file_ids = {
            file.get("Id")
            for file in root.iter(f"{{{WXS_NS}}}File")
        }
        self.assertIn("HarkenHubExe", file_ids)

    def test_plugin_directory_is_materialized(self):
        self.make_input()
        root = self.payload_root()
        names = {
            directory.get("Name")
            for directory in root.iter(f"{{{WXS_NS}}}Directory")
        }
        self.assertIn("platforms", names)
        self.assertIn("Updater", names)

    def test_missing_hub_binary_is_rejected(self):
        self.make_input(with_binary=False)
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_incomplete_legal_directory_is_rejected(self):
        self.make_input()
        (self.input_dir / "Legal/third-party/Qt6-THIRD-PARTY-NOTICES.txt").unlink()
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_missing_bridge_binary_is_rejected(self):
        self.make_input(with_bridge=False)
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_foreign_product_binary_is_rejected(self):
        self.make_input(with_foreign=True)
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_setup_stub_in_payload_is_rejected(self):
        self.make_input()
        (self.input_dir / "HarkenHubSetup.exe").write_bytes(b"stub")
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_msi_version_requires_numeric_triple(self):
        self.assertEqual(self.module.msi_version("0.1.0"), "0.1.0")
        self.assertEqual(self.module.msi_version("0.1.0-dev"), "0.1.0")
        with self.assertRaises(SystemExit):
            self.module.msi_version("dev")

    def test_wix_command_carries_authoring_inputs(self):
        command = self.module.wix_command(
            Path("Product.wxs"), Path("Payload.wxs"), Path("Eula.rtf"),
            Path("Icon.ico"), "0.1.0", Path("out.msi"), "wix",
        )
        self.assertEqual(command[:2], ["wix", "build"])
        self.assertIn("-ext", command)
        self.assertIn("WixToolset.UI.wixext", command)
        self.assertIn("HarkenVersion=0.1.0", command)
        self.assertIn("HarkenIconIco=Icon.ico", command)
        self.assertIn(f"HarkenHandoffSddl={self.module.HANDOFF_SDDL}", command)
        self.assertIn(f"HarkenUpdaterSddl={self.module.UPDATER_SDDL}", command)
        self.assertEqual(command[-2:], ["Product.wxs", "Payload.wxs"])

    def test_installer_allows_install_directory_selection(self):
        root = ET.parse(WXS_PATH).getroot()
        install_ui = root.find(f".//{{{WIXUI_NS}}}WixUI")
        self.assertIsNotNone(install_ui)
        self.assertEqual(install_ui.get("Id"), "WixUI_InstallDir")
        self.assertEqual(install_ui.get("InstallDirectory"), "INSTALLFOLDER")

    def test_installer_reuses_registered_install_directory(self):
        root = ET.parse(WXS_PATH).getroot()
        properties = root.findall(f".//{{{WXS_NS}}}Property")
        install_folder = next(
            item for item in properties if item.get("Id") == "INSTALLFOLDER"
        )
        search = install_folder.find(f"{{{WXS_NS}}}RegistrySearch")
        self.assertIsNotNone(search)
        self.assertEqual(search.get("Root"), "HKLM")
        self.assertEqual(search.get("Key"), r"Software\HarkenSound\HarkenHub")
        self.assertEqual(search.get("Name"), "InstallDir")
        self.assertEqual(search.get("Type"), "raw")
        self.assertEqual(search.get("Bitness"), "always64")

    def test_build_produces_summary_with_fake_wix(self):
        self.make_input()

        def fake_run(command):
            if command[0] == "wix":
                eula_path = Path(next(arg.split("=", 1)[1] for arg in command
                                      if arg.startswith("HarkenEulaRtf=")))
                self.assertEqual(eula_path.read_text(encoding="ascii"),
                                 self.module.product_legal.render_rtf(self.module.product_legal.installer_text()))
                out_index = command.index("-o") + 1
                Path(command[out_index]).write_bytes(b"fake-msi")
            return subprocess.CompletedProcess(command, 0)

        args = self.module.parse_args([
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "0.1.0-dev",
            "--build-id", "run12.abcd1234",
        ])
        summary = self.module.build_installer(args, run=fake_run)

        package = Path(summary["package"])
        self.assertTrue(package.exists())
        self.assertEqual(
            package.name, "HarkenHubSetup-0.1.0-dev-run12.abcd1234.msi"
        )
        self.assertEqual(summary["msiVersion"], "0.1.0")
        self.assertEqual(summary["fileCount"], 5 + len(self.legal_files))
        self.assertFalse(summary["signed"])
        self.assertNotIn("setupExe", summary)
        self.evidence.finalize.assert_called_once()
        self.assertEqual(self.evidence.finalize.call_args.args[1][0].name, package.name)
        self.assertEqual(self.evidence.finalize.call_args.kwargs["intermediate_artifacts"], ())
        self.assertEqual(list(self.output_dir.rglob("*.wxs")),
                         [Path(summary["evidence"]) / "packaging-inputs/payload.wxs"])
        self.assertTrue((Path(summary["evidence"]) / "packaging-inputs/license.rtf").is_file())

    def test_custom_eula_is_used_and_missing_override_is_rejected(self):
        self.make_input()
        custom = self.root / "custom.rtf"
        custom.write_bytes(b"{\\rtf1 custom terms}")
        args = self.module.parse_args([
            "--input-dir", str(self.input_dir), "--output-dir", str(self.output_dir),
            "--version", "0.1.0", "--build-id", "custom", "--eula-rtf", str(custom),
        ])

        def fake_run(command):
            self.assertIn(f"HarkenEulaRtf={custom}", command)
            Path(command[command.index("-o") + 1]).write_bytes(b"fake-msi")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(self.module, "sign_command", return_value=None):
            self.module.build_installer(args, run=fake_run)
        self.assertEqual(self.evidence.retain_packaging_inputs.call_args.args[1]["license.rtf"], custom)
        custom.unlink()
        with self.assertRaises(SystemExit):
            self.module.build_installer(args, run=fake_run)

    def test_build_with_stub_produces_setup_exe(self):
        self.make_input()
        stub = self.root / "HarkenHubSetup.exe"
        stub.write_bytes(b"stub-image")
        embeds = []

        def fake_run(command):
            if command[0] == "wix":
                out_index = command.index("-o") + 1
                Path(command[out_index]).write_bytes(b"fake-msi")
            return subprocess.CompletedProcess(command, 0)

        def fake_embed(stub_exe, msi_path, exe_path):
            embeds.append((stub_exe, msi_path, exe_path))
            exe_path.write_bytes(
                stub_exe.read_bytes() + msi_path.read_bytes()
            )

        args = self.module.parse_args([
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "0.1.0-dev",
            "--build-id", "run12.abcd1234",
            "--stub-exe", str(stub),
        ])
        summary = self.module.build_installer(
            args, run=fake_run, embed=fake_embed
        )

        exe = Path(summary["setupExe"])
        self.assertTrue(exe.exists())
        self.assertEqual(
            exe.name, "HarkenHubSetup-0.1.0-dev-run12.abcd1234.exe"
        )
        self.assertEqual(len(embeds), 1)
        self.assertEqual(embeds[0][0], stub.resolve())
        self.assertEqual(embeds[0][1].name, Path(summary["package"]).name)
        self.assertEqual(summary["setupExeSize"], exe.stat().st_size)

    def test_missing_stub_exe_is_rejected(self):
        self.make_input()

        args = self.module.parse_args([
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "0.1.0",
            "--build-id", "run12.abcd1234",
            "--stub-exe", str(self.root / "absent-stub.exe"),
        ])
        with self.assertRaises(SystemExit):
            self.module.build_installer(
                args,
                run=lambda command: subprocess.CompletedProcess(command, 0),
            )

    def test_signing_covers_msi_and_setup_exe(self):
        self.make_input()
        stub = self.root / "HarkenHubSetup.exe"
        stub.write_bytes(b"stub-image")
        commands = []

        def fake_run(command):
            commands.append(command)
            if command[0] == "wix":
                out_index = command.index("-o") + 1
                Path(command[out_index]).write_bytes(b"fake-msi")
            return subprocess.CompletedProcess(command, 0)

        def fake_embed(stub_exe, msi_path, exe_path):
            exe_path.write_bytes(b"fake-exe")

        args = self.module.parse_args([
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "0.1.0",
            "--build-id", "run12.abcd1234",
            "--stub-exe", str(stub),
        ])
        with mock.patch.dict(
            os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": "AB12CD"}
        ):
            summary = self.module.build_installer(
                args, run=fake_run, embed=fake_embed
            )

        sign_calls = [c for c in commands if c[0] == "signtool"]
        self.assertEqual(len(sign_calls), 2)
        self.assertTrue(sign_calls[0][-1].endswith(".msi"))
        self.assertTrue(sign_calls[1][-1].endswith(".exe"))
        self.assertTrue(summary["signed"])

    def test_evidence_failure_prevents_wix(self):
        self.make_input()
        self.evidence.prepare.side_effect = ValueError("missing sources")
        args = self.module.parse_args([
            "--input-dir", str(self.input_dir), "--output-dir", str(self.output_dir),
            "--version", "0.1.0", "--build-id", "test-build",
        ])
        run = mock.Mock()
        with self.assertRaisesRegex(ValueError, "missing sources"):
            self.module.build_installer(args, run=run)
        run.assert_not_called()

    def test_wix_failure_aborts_build(self):
        self.make_input()

        def failing_run(command):
            Path(command[command.index("-o") + 1]).write_bytes(b"partial installer")
            return subprocess.CompletedProcess(command, 311)

        args = self.module.parse_args([
            "--input-dir", str(self.input_dir),
            "--output-dir", str(self.output_dir),
            "--version", "0.1.0",
            "--build-id", "run12.abcd1234",
        ])
        with self.assertRaises(SystemExit):
            self.module.build_installer(args, run=failing_run)
        self.assertEqual(list(self.output_dir.iterdir()), [])

        def successful_run(command):
            Path(command[command.index("-o") + 1]).write_bytes(b"complete installer")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.dict(os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": ""}):
            summary = self.module.build_installer(args, run=successful_run)
            args.build_id = "another-build"
            second = self.module.build_installer(args, run=successful_run)
        self.assertEqual(Path(summary["package"]).read_bytes(), b"complete installer")
        self.assertTrue(Path(second["package"]).is_file())

    def test_handoff_sddl_matches_hub_source(self):
        header = ACL_HEADER_PATH.read_text(encoding="utf-8")
        self.assertIn(self.module.HANDOFF_SDDL, header)

    def test_updater_sddl_matches_hub_source(self):
        header = ACL_HEADER_PATH.read_text(encoding="utf-8")
        self.assertIn(self.module.UPDATER_SDDL, header)

    def test_wxs_wires_generated_payload_and_eula(self):
        wxs = WXS_PATH.read_text(encoding="utf-8")
        self.assertIn('ComponentGroupRef Id="HubPayload"', wxs)
        self.assertIn('PermissionEx Sddl="$(var.HarkenHandoffSddl)"', wxs)
        self.assertIn('Extension Id="hsp"', wxs)
        self.assertIn("--await-health-handshake", wxs)
        self.assertIn('Scope="perMachine"', wxs)
        self.assertIn('WixVariable Id="WixUILicenseRtf" Value="$(var.HarkenEulaRtf)"', wxs)

    def test_wxs_wires_update_bridge_and_pins_area(self):
        wxs = WXS_PATH.read_text(encoding="utf-8")
        self.assertIn('Directory Id="PinsDir" Name="pins"', wxs)
        self.assertIn('Component Id="PinsAreaAcl" Directory="PinsDir"', wxs)
        self.assertIn('Directory Id="UpdaterDir" Name="Updater"', wxs)
        self.assertIn(
            'Component Id="UpdaterAreaAcl" Directory="UpdaterDir"', wxs
        )
        self.assertEqual(
            wxs.count('PermissionEx Sddl="$(var.HarkenHandoffSddl)"'), 2
        )
        self.assertEqual(
            wxs.count('PermissionEx Sddl="$(var.HarkenUpdaterSddl)"'), 1
        )
        self.assertIn("--register-update-bridge", wxs)
        self.assertIn("--unregister-update-bridge", wxs)
        self.assertIn('Property Id="HARKENBRIDGE" Secure="yes"', wxs)
        self.assertIn(
            "HARKENSELFUPDATE=1 AND NOT HARKENBRIDGE=1 AND NOT REMOVE", wxs
        )
        self.assertIn(
            "REMOVE=&quot;ALL&quot; AND NOT UPGRADINGPRODUCTCODE", wxs
        )

    def test_bridge_register_action_is_deferred_without_impersonation(self):
        root = ET.fromstring(WXS_PATH.read_text(encoding="utf-8"))
        actions = {
            action.get("Id"): action
            for action in root.iter(f"{{{WXS_NS}}}CustomAction")
        }
        register = actions["HarkenRegisterUpdateBridge"]
        self.assertEqual(register.get("Execute"), "deferred")
        self.assertEqual(register.get("Impersonate"), "no")
        self.assertEqual(register.get("Return"), "check")
        unregister = actions["HarkenUnregisterUpdateBridge"]
        self.assertEqual(unregister.get("Execute"), "deferred")
        self.assertEqual(unregister.get("Impersonate"), "no")


if __name__ == "__main__":
    unittest.main()
