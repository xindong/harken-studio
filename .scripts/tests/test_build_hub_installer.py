import ctypes
import hashlib
import importlib.util
import os
import subprocess
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "release" / "build_hub_installer.py"
ACL_HEADER_PATH = (
    REPO_ROOT / "HarkenHub" / "Source" / "Platform" / "AclHardening.h"
)
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
            (self.input_dir / "HarkenStudioHub.exe").write_bytes(b"stub-binary")
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

    def test_missing_hub_binary_is_rejected(self):
        self.make_input()
        for name in (self.module.HUB_BINARY, self.module.LEGACY_HUB_BINARY):
            with self.subTest(binary=name):
                binary = self.input_dir / name
                content = binary.read_bytes()
                binary.unlink()
                with self.assertRaises(SystemExit):
                    self.module.collect_payload(self.input_dir)
                binary.write_bytes(content)

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
        (self.input_dir / "HarkenStudioHubSetup.exe").write_bytes(b"stub")
        with self.assertRaises(SystemExit):
            self.module.collect_payload(self.input_dir)

    def test_msi_version_requires_numeric_triple(self):
        self.assertEqual(self.module.msi_version("0.1.0"), "0.1.0")
        self.assertEqual(self.module.msi_version("0.1.0-dev"), "0.1.0")
        with self.assertRaises(SystemExit):
            self.module.msi_version("dev")

    def args(self, *extra):
        return self.module.parse_args([
            "--input-dir", str(self.input_dir), "--output-dir", str(self.output_dir),
            "--version", "0.1.68", "--build-id", "run12.abcd1234", *extra,
        ])

    def fake_build(self, files, input_dir, version, legal_inputs, output, localized_output, evidence):
        for language, documents in legal_inputs.items():
            texts = self.module.product_legal.read_documents(REPO_ROOT / self.module.product_legal.TERMS_FILES[language])
            self.assertEqual([document.read_text(encoding="ascii") for document in documents],
                             [self.module.product_legal.render_rtf(text) for text in texts])
        output.write_bytes(b"MSI")
        localized_output.write_bytes(b"MSIZH")
        (evidence / "msi-tables.json").write_text("{}")

    def test_signing_precedes_embedding_and_final_hashes(self):
        self.make_input()
        stub = self.root / "stub.exe"
        stub.write_bytes(b"STUB")
        calls = []

        def sign(command):
            calls.append(command)
            target = Path(command[-1])
            target.write_bytes(target.read_bytes() + b"SIGNED")
            return subprocess.CompletedProcess(command, 0)

        def embed(stub, msi, localized_msi, exe):
            self.assertEqual(msi.read_bytes(), b"MSISIGNED")
            self.assertEqual(localized_msi.read_bytes(), b"MSIZHSIGNED")
            exe.write_bytes(stub.read_bytes() + msi.read_bytes() + localized_msi.read_bytes())

        with mock.patch.dict(os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": "TEST", "HARKEN_AUTHENTICODE_TIMESTAMP_URL": "https://timestamp.example"}):
            result = self.module.build_installer(self.args("--stub-exe", str(stub)),
                                                run=sign, embed=embed, build_msi=self.fake_build)
        self.assertEqual(len(calls), 3)
        self.assertIn("/tr", calls[0])
        self.assertEqual(Path(result["setupExe"]).read_bytes(), b"STUBMSISIGNEDMSIZHSIGNEDSIGNED")
        self.assertEqual(result["setupExeSha256"], hashlib.sha256(b"STUBMSISIGNEDMSIZHSIGNEDSIGNED").hexdigest())
        self.assertEqual(result["sha256"], hashlib.sha256(b"MSISIGNED").hexdigest())
        self.assertEqual(self.evidence.finalize.call_args.kwargs["intermediate_artifacts"][0].suffix, ".msi")

    def test_unsigned_package_preserves_publisher_contract(self):
        self.make_input()
        with mock.patch.dict(os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": ""}):
            result = self.module.build_installer(self.args(), build_msi=self.fake_build)
        self.assertEqual(Path(result["package"]).name, "HarkenHubSetup-0.1.68-run12.abcd1234.msi")
        self.assertEqual(Path(result["evidence"]).name, "hub-0.1.68-run12.abcd1234-evidence")
        self.assertEqual(result["fileCount"], 6 + len(self.legal_files))
        self.assertFalse(result["signed"])
        self.assertTrue((Path(result["evidence"]) / "packaging-inputs/license.rtf").is_file())
        self.assertTrue((Path(result["evidence"]) / "packaging-inputs/privacy.rtf").is_file())
        self.assertTrue((Path(result["evidence"]) / "packaging-inputs/license.zh-CN.rtf").is_file())
        self.assertTrue((Path(result["evidence"]) / "packaging-inputs/privacy.zh-CN.rtf").is_file())

    def test_embedded_packages_preserve_bytes_and_share_common_content(self):
        common = bytes(range(256)) * 128
        payloads = (common + b"English signed package", common + b"Chinese signed package\0")
        paths = (self.root / "en.msi", self.root / "zh.msi")
        for path, payload in zip(paths, payloads):
            path.write_bytes(payload)
        resources = self.module.payload_resources(paths)
        blocks = resources[self.module.SHARED_PAYLOAD_RESOURCE_NAME]
        for name, payload in zip((self.module.PAYLOAD_RESOURCE_NAME, self.module.CHINESE_PAYLOAD_RESOURCE_NAME), payloads):
            manifest = resources[name]
            magic, size, digest = struct.unpack_from("<8sI32s", manifest)
            self.assertEqual(magic, b"HKMSI001")
            restored = b"".join(blocks[index * 4096:(index + 1) * 4096]
                                for index, in struct.iter_unpack("<I", manifest[44:]))[:size]
            self.assertEqual(restored, payload)
            self.assertEqual(hashlib.sha256(restored).digest(), digest)
        self.assertLess(sum(map(len, resources.values())), sum(map(len, payloads)) * 0.75)

    def test_failed_build_or_signing_does_not_publish_partial_outputs(self):
        self.make_input()

        def fail_build(*args):
            args[4].write_bytes(b"partial")
            raise RuntimeError("build failed")

        with self.assertRaisesRegex(RuntimeError, "build failed"):
            self.module.build_installer(self.args(), build_msi=fail_build)
        self.assertEqual(list(self.output_dir.iterdir()), [])
        with mock.patch.dict(os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": "TEST"}):
            with self.assertRaises(SystemExit):
                self.module.build_installer(self.args(), build_msi=self.fake_build,
                    run=lambda command: subprocess.CompletedProcess(command, 1))
        self.assertEqual(list(self.output_dir.iterdir()), [])

    def test_evidence_failure_prevents_packaging(self):
        self.make_input()
        self.evidence.prepare.side_effect = ValueError("missing sources")
        build = mock.Mock()
        with self.assertRaisesRegex(ValueError, "missing sources"):
            self.module.build_installer(self.args(), build_msi=build)
        build.assert_not_called()

    def test_missing_inputs_are_rejected_before_packaging(self):
        self.make_input()
        for flag in ("--stub-exe", "--eula-rtf", "--privacy-rtf", "--eula-rtf-zh-cn", "--privacy-rtf-zh-cn"):
            with self.subTest(flag=flag), self.assertRaises(SystemExit):
                self.module.build_installer(self.args(flag, str(self.root / "missing")))

    @unittest.skipUnless(os.name == "nt", "Windows Installer requires Windows")
    def test_native_package_round_trip_and_upgrade_contract(self):
        from release.windows_installer import Database
        self.make_input()
        (self.input_dir / "nested directory").mkdir()
        (self.input_dir / "nested directory/settings.json").write_text('{"preserve":true}')
        with mock.patch.dict(os.environ, {"HARKEN_AUTHENTICODE_THUMBPRINT": ""}):
            result = self.module.build_installer(self.args())
        package = Path(result["package"])
        with Database(package, 0) as db:
            properties = dict(db.query("SELECT `Property`, `Value` FROM `Property`"))
            supported_languages = db.summary_value(7).split(";")[1].split(",")
            self.assertIn(properties["ProductLanguage"], supported_languages)
            self.assertEqual(properties["UpgradeCode"], "{4EA0003D-209E-45E6-8343-67529D7E7E5A}")
            self.assertEqual(properties["ALLUSERS"], "1")
            for name in ("HARKENSELFUPDATE", "HARKENBRIDGE", "INSTALLFOLDER"):
                self.assertIn(name, properties["SecureCustomProperties"].split(";"))
            components = {row[0]: row for row in db.query("SELECT * FROM `Component`")}
            self.assertEqual(components["HarkenProjectAssociation"][1], "{53FC615E-9B49-4762-B13C-B81820BDD7DB}")
            self.assertEqual(components["HarkenProjectAssociation"][5], "reg9Iw6ZFMWLFHU5l3K2mYjrhyy3bc")
            actions = {row[0]: row for row in db.query("SELECT * FROM `CustomAction`")}
            launch_events = db.query("SELECT `Dialog_`, `Condition` FROM `ControlEvent` WHERE `Event`='DoAction' AND `Argument`='HarkenLaunchHub'")
            self.assertEqual([row[0] for row in launch_events], ["SuccessDialog"])
            launch_condition = launch_events[0][1]
            launch_visibility = dict(db.query("SELECT `Action`, `Condition` FROM `ControlCondition` WHERE `Dialog_`='SuccessDialog' AND `Control_`='LaunchHub'"))
            launch_property = db.query("SELECT `Property` FROM `Control` WHERE `Dialog_`='SuccessDialog' AND `Control`='LaunchHub'")[0][0]
            checked_value = db.query(f"SELECT `Value` FROM `CheckBox` WHERE `Property`='{launch_property}'")[0][0]
            self.assertEqual(properties[launch_property], checked_value)
            for sequence in ("InstallExecuteSequence", "InstallUISequence", "AdminExecuteSequence"):
                self.assertFalse(db.query(f"SELECT `Action` FROM `{sequence}` WHERE `Action`='HarkenLaunchHub'"))
            launch_type = int(actions["HarkenLaunchHub"][1])
            self.assertEqual(launch_type & 0x3f, 18)
            self.assertFalse(launch_type & 0x400)
            self.assertTrue(db.query(f"SELECT `File` FROM `File` WHERE `File`='{actions['HarkenLaunchHub'][2]}'"))
            self.assertEqual(actions["HarkenRegisterUpdateBridge"][1:4],
                             ["3106", "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe" --register-update-bridge'])
            sequences = {row[0]: row for row in db.query("SELECT * FROM `InstallExecuteSequence`")}
            self.assertGreater(int(sequences["RemoveExistingProducts"][2]), int(sequences["InstallFinalize"][2]))
            self.assertEqual(sequences["HarkenUnregisterUpdateBridge"][1], 'REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE')
            acls = {row[1]: row[3] for row in db.query("SELECT * FROM `MsiLockPermissionsEx`")}
            source = ACL_HEADER_PATH.read_text(encoding="utf-8")
            for name in ("HandoffDir", "PinsDir", "UpdaterDir"):
                self.assertIn(acls[name], source)
            terms = db.query("SELECT `Text` FROM `Control` WHERE `Dialog_`='LicenseDialog' AND `Type`='ScrollableText' ORDER BY `X`")
            expected_documents = self.module.product_legal.read_documents(REPO_ROOT / "USER_TERMS.md")
            self.assertEqual([row[0] for row in terms], [
                self.module.product_legal.render_rtf(text) for text in expected_documents])
            for dialog, first in db.query("SELECT `Dialog`, `Control_First` FROM `Dialog`"):
                with self.subTest(dialog=dialog):
                    controls = dict(db.query(
                        f"SELECT `Control`, `Control_Next` FROM `Control` WHERE `Dialog_`='{dialog}'"))
                    self.assertIn(first, controls)
                    linked = {name for name, following in controls.items() if following}
                    if not linked:
                        continue
                    visited = set()
                    current = first
                    while current not in visited:
                        self.assertIn(current, linked)
                        visited.add(current)
                        current = controls[current]
                    self.assertEqual(current, first)
                    self.assertEqual(visited, linked)
            localized_package = Path(result["chinesePackage"])
            with Database(localized_package, 0) as localized:
                localized_properties = dict(localized.query("SELECT `Property`, `Value` FROM `Property`"))
                self.assertIn(localized_properties["ProductLanguage"], localized.summary_value(7).split(";")[1].split(","))
                self.assertNotEqual(localized.summary_value(9), db.summary_value(9))
                self.assertEqual(localized_properties.pop("ProductLanguage"), "2052")
                self.assertEqual(properties.pop("ProductLanguage"), "1033")
                self.assertNotEqual(localized_properties.pop("ProductCode"), properties.pop("ProductCode"))
                self.assertEqual(localized_properties, properties)
                for table in self.module.hub_msi.SCHEMA:
                    if table in {"Property", "Dialog", "Control", "LaunchCondition", "Icon"}:
                        continue
                    with self.subTest(table=table):
                        self.assertEqual(localized.query(f"SELECT * FROM `{table}`"),
                                         db.query(f"SELECT * FROM `{table}`"))
                terms = localized.query("SELECT `Text` FROM `Control` WHERE `Dialog_`='LicenseDialog' AND `Type`='ScrollableText' ORDER BY `X`")
                self.assertEqual([row[0] for row in terms], [self.module.product_legal.render_rtf(text)
                    for text in self.module.product_legal.read_documents(REPO_ROOT / "USER_TERMS.zh-CN.md")])
                self.assertEqual(localized.query("SELECT `Text` FROM `Control` WHERE `Dialog_`='LicenseDialog' AND `Control`='Next'"), [["下一步"]])
            self.assertTrue(all(not row[0] for row in db.query("SELECT `Language` FROM `Upgrade`")))
        msi = ctypes.WinDLL("msi.dll")
        msi.MsiSetInternalUI.argtypes = [ctypes.c_uint, ctypes.c_void_p]
        msi.MsiSetInternalUI.restype = ctypes.c_uint
        msi.MsiOpenPackageExW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
        msi.MsiDoActionW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p]
        msi.MsiSetPropertyW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_wchar_p]
        msi.MsiEvaluateConditionW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p]
        msi.MsiCloseHandle.argtypes = [ctypes.c_uint]
        msi.MsiInstallProductW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        previous_ui = msi.MsiSetInternalUI(2, None)
        try:
            session = ctypes.c_uint()
            self.assertEqual(msi.MsiOpenPackageExW(str(package), 1, ctypes.byref(session)), 0)
            try:
                self.assertEqual(msi.MsiDoActionW(session, "AppSearch"), 0)
                for scenario, overrides, visible, launches in [
                    ("new install", {}, True, True),
                    ("upgrade", {"HARKEN_UPGRADE_DETECTED": "old-product"}, True, True),
                    ("repair", {"Installed": "1"}, True, True),
                    ("unchecked", {launch_property: ""}, True, False),
                    ("uninstall", {"REMOVE": "ALL"}, False, False),
                    ("silent", {"UILevel": "2"}, False, False),
                    ("basic", {"UILevel": "3"}, False, False),
                    ("reduced", {"UILevel": "4"}, False, False),
                    ("self update", {"HARKENSELFUPDATE": "1"}, False, False),
                    ("bridge", {"HARKENBRIDGE": "1"}, False, False),
                ]:
                    with self.subTest(completion=scenario):
                        values = {"REMOVE": "", "Installed": "", "HARKEN_UPGRADE_DETECTED": "",
                                  "HARKENSELFUPDATE": "", "HARKENBRIDGE": "", "UILevel": "5",
                                  launch_property: checked_value, **overrides}
                        for name, value in values.items():
                            self.assertEqual(msi.MsiSetPropertyW(session, name, value), 0)
                        self.assertEqual(msi.MsiEvaluateConditionW(session, launch_condition), int(launches))
                        self.assertEqual(msi.MsiEvaluateConditionW(session, launch_visibility["Show"]), int(visible))
                        self.assertEqual(msi.MsiEvaluateConditionW(session, launch_visibility["Hide"]), int(not visible))
            finally:
                msi.MsiCloseHandle(session)
            for language, source in (("1033", package), ("2052", localized_package)):
                with self.subTest(language=language):
                    extracted = self.root / f"extracted-{language}"
                    self.assertEqual(msi.MsiInstallProductW(str(source),
                        f'ACTION=ADMIN TARGETDIR="{extracted}" REBOOT=ReallySuppress'), 0)
                    installed = extracted / "PFiles64/HarkenHub"
                    expected = {p.relative_to(self.input_dir) for p in self.input_dir.rglob("*") if p.is_file()}
                    actual = {p.relative_to(installed) for p in installed.rglob("*") if p.is_file()}
                    self.assertEqual(expected, actual)
                    for relative in expected:
                        self.assertEqual((self.input_dir / relative).read_bytes(),
                                         (installed / relative).read_bytes(), str(relative))
        finally:
            msi.MsiSetInternalUI(previous_ui, None)


if __name__ == "__main__":
    unittest.main()
