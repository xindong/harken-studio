import hashlib
import importlib.util
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "release" / "build_hub_pkg.py"
DAEMON_PLIST_SOURCE = (
    REPO_ROOT
    / "HarkenHub"
    / "Resources"
    / "macOS"
    / "com.harkensound.harkenhub.updatebridge.plist"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "build_hub_pkg", SCRIPT_PATH
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildHubPkgTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bundle = self.root / "HarkenHub.app"

    def tearDown(self):
        self.tmp.cleanup()

    def make_bundle(self, with_binary=True, with_daemon=True,
                    with_foreign=False):
        self.module.collect_legal_texts.assemble(
            "hub", self.bundle / "Contents/Resources/Legal", "6.8.3"
        )
        (self.bundle / "Contents" / "MacOS").mkdir(parents=True)
        if with_binary:
            (self.bundle / "Contents" / "MacOS" / "HarkenHub").write_bytes(
                b"stub-binary"
            )
        if with_daemon:
            daemon_dir = (
                self.bundle / "Contents" / "Library" / "LaunchDaemons"
            )
            daemon_dir.mkdir(parents=True)
            (
                daemon_dir
                / "com.harkensound.harkenhub.updatebridge.plist"
            ).write_bytes(b"<plist/>")
        if with_foreign:
            (self.bundle / "Contents" / "MacOS" / "HarkenStudio").write_bytes(
                b"studio"
            )

    def test_validate_accepts_complete_bundle(self):
        self.make_bundle()
        self.module.validate_bundle(self.bundle)

    def build_args(self):
        return self.module.parse_args([
            "--bundle", str(self.bundle), "--output-dir", str(self.root / "output"),
            "--version", "0.1.0", "--build-id", "test-build",
        ])

    def test_evidence_failure_prevents_native_packaging(self):
        self.make_bundle()
        staging_output = self.module.release_evidence.package_output
        with mock.patch.object(self.module, "release_evidence") as evidence, \
                mock.patch.object(self.module.subprocess, "run") as run:
            evidence.prepare.side_effect = ValueError("missing sources")
            evidence.package_output.side_effect = staging_output
            with self.assertRaisesRegex(ValueError, "missing sources"):
                self.module.build_pkg(self.build_args())
            run.assert_not_called()

    def test_pkg_and_zip_are_finalized_without_deleting_previous_evidence(self):
        self.make_bundle()
        staging_output = self.module.release_evidence.package_output
        output = self.root / "output"
        previous = output / "previous-evidence/receipt.json"
        previous.parent.mkdir(parents=True)
        previous.write_bytes(b"previous")

        def run(command, **kwargs):
            if command[0] == "productbuild":
                resources = Path(command[command.index("--resources") + 1])
                license_path = resources / "License.txt"
                self.assertEqual(license_path.read_text(encoding="utf-8"),
                                 self.module.product_legal.installer_text())
                self.assertEqual(evidence.retain_packaging_inputs.call_args.args[1]["License.txt"], license_path)
            Path(command[-1]).write_bytes(command[0].encode("ascii"))

        with mock.patch.object(self.module, "release_evidence") as evidence, \
                mock.patch.object(self.module.subprocess, "run", side_effect=run):
            evidence.prepare.return_value = output / "evidence"
            evidence.package_output.side_effect = staging_output
            evidence.finalize.side_effect = lambda directory, artifacts, payload: [
                {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size": path.stat().st_size} for path in artifacts]
            summary = self.module.build_pkg(self.build_args())
            evidence.finalize.assert_called_once()
            self.assertEqual([path.name for path in evidence.finalize.call_args.args[1]],
                             [Path(summary["pkg"]).name, Path(summary["bundleZip"]).name])
            evidence.retain_packaging_inputs.assert_called_once()
            self.assertEqual(previous.read_bytes(), b"previous")
            self.assertTrue(Path(summary["pkg"]).is_file())
            self.assertTrue(Path(summary["bundleZip"]).is_file())
            with self.assertRaises(SystemExit):
                self.module.build_pkg(self.build_args())

    def test_failed_native_packaging_removes_staging_and_allows_retry(self):
        self.make_bundle()
        staging_output = self.module.release_evidence.package_output

        def run(command, **kwargs):
            Path(command[-1]).write_bytes(command[0].encode("ascii"))

        with mock.patch.object(self.module, "release_evidence") as evidence, \
                mock.patch.object(self.module.subprocess, "run") as native:
            evidence.package_output.side_effect = staging_output
            evidence.prepare.return_value = Path("evidence")
            evidence.finalize.side_effect = lambda directory, artifacts, payload: [
                {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size": path.stat().st_size} for path in artifacts]
            native.side_effect = RuntimeError("pkgbuild failed")
            with self.assertRaisesRegex(RuntimeError, "pkgbuild failed"):
                self.module.build_pkg(self.build_args())
            self.assertEqual(list((self.root / "output").iterdir()), [])
            native.side_effect = run
            summary = self.module.build_pkg(self.build_args())
            self.assertTrue(Path(summary["pkg"]).is_file())
            self.assertTrue(Path(summary["bundleZip"]).is_file())

    def test_validate_rejects_missing_binary(self):
        self.make_bundle(with_binary=False)
        with self.assertRaises(SystemExit):
            self.module.validate_bundle(self.bundle)

    def test_validate_rejects_incomplete_legal_directory(self):
        self.make_bundle()
        (self.bundle / "Contents/Resources/Legal/third-party/Qt6-THIRD-PARTY-NOTICES.txt").unlink()
        with self.assertRaises(SystemExit):
            self.module.validate_bundle(self.bundle)

    def test_validate_rejects_missing_daemon_plist(self):
        self.make_bundle(with_daemon=False)
        with self.assertRaises(SystemExit):
            self.module.validate_bundle(self.bundle)

    def test_validate_rejects_foreign_product_binaries(self):
        self.make_bundle(with_foreign=True)
        with self.assertRaises(SystemExit):
            self.module.validate_bundle(self.bundle)

    def test_pkg_version_requires_semver_prefix(self):
        self.assertEqual(self.module.pkg_version("1.2.3-dev"), "1.2.3")
        with self.assertRaises(SystemExit):
            self.module.pkg_version("dev")

    def test_postinstall_provisions_machine_data_directories(self):
        script = self.module.generate_postinstall()
        self.assertTrue(script.startswith("#!/bin/sh"))
        self.assertIn(
            '"/Library/Application Support/HarkenSound/HarkenHub"', script
        )
        self.assertIn("handoff", script)
        self.assertIn("pins", script)
        self.assertIn("chmod 1777", script)
        self.assertIn("chown root:admin", script)

    def test_distribution_declares_license_and_architectures(self):
        text = self.module.generate_distribution_xml(
            "0.1.0-dev", "HarkenHubComponent-0.1.0.pkg"
        )
        root = ET.fromstring(text)
        license_node = root.find("license")
        self.assertIsNotNone(license_node)
        self.assertEqual(license_node.get("file"), "License.txt")
        options = root.find("options")
        self.assertEqual(options.get("hostArchitectures"), "x86_64,arm64")
        pkg_ref = root.find(
            "pkg-ref[@id='com.harkensound.harkenhub'][@version]"
        )
        self.assertIsNotNone(pkg_ref)
        self.assertEqual(pkg_ref.get("version"), "0.1.0")
        self.assertEqual(
            pkg_ref.text, "HarkenHubComponent-0.1.0.pkg"
        )

    def test_custom_license_is_used_and_missing_override_is_rejected(self):
        self.make_bundle()
        custom = self.root / "custom.txt"
        custom.write_bytes(b"custom terms\r\n")
        args = self.build_args()
        args.license_file = str(custom)
        staging_output = self.module.release_evidence.package_output

        def run(command, **kwargs):
            if command[0] == "productbuild":
                resources = Path(command[command.index("--resources") + 1])
                self.assertEqual((resources / "License.txt").read_bytes(), custom.read_bytes())
            Path(command[-1]).write_bytes(command[0].encode("ascii"))

        with mock.patch.object(self.module, "release_evidence") as evidence, \
                mock.patch.object(self.module.subprocess, "run", side_effect=run):
            evidence.prepare.return_value = self.root / "evidence"
            evidence.package_output.side_effect = staging_output
            evidence.finalize.return_value = [{"sha256": "hash", "size": 1}]
            self.module.build_pkg(args)
            custom.unlink()
            args.build_id = "missing"
            with self.assertRaises(SystemExit):
                self.module.build_pkg(args)

    def test_daemon_plist_watches_machine_handoff_directory(self):
        text = DAEMON_PLIST_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "/Library/Application Support/HarkenSound/HarkenHub/handoff",
            text,
        )
        self.assertIn("--consume-update-queue", text)
        self.assertIn("Contents/MacOS/HarkenHub", text)


if __name__ == "__main__":
    unittest.main()
