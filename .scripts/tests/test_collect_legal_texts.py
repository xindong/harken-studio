import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from script_loader import load_script_module

REPO_ROOT = Path(__file__).resolve().parents[2]


class CollectLegalTextsTest(unittest.TestCase):
    def setUp(self):
        self.module = load_script_module(
            "collect_legal_texts", REPO_ROOT / ".scripts/release/collect_legal_texts.py"
        )
        (REPO_ROOT / ".temp").mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT / ".temp")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_desktop_bundles_include_embedded_qt_attributions_and_sources(self):
        for product in ("studio", "hub"):
            with self.subTest(product=product):
                output = self.root / product / "Legal"
                self.module.assemble(product, output, "6.8.3")
                self.module.verify(product, output)
                text = (output / "third-party/Qt6-THIRD-PARTY-NOTICES.txt").read_text(encoding="utf-8")
                for required in (
                    "TinyCBOR", "the V8 project authors", "Unicode",
                    "Independent JPEG Group", "The FreeType Project",
                    "Cocoa Platform Plugin", "D3D12 Memory Allocator",
                ):
                    self.assertIn(required, text)
                sources = json.loads((output / "third-party/Qt6-SOURCES.json").read_text(encoding="utf-8"))
                self.assertEqual(sources["qt_version"], "6.8.3")
                self.assertEqual({entry["module"] for entry in sources["source_archives"]}, {"qtbase", "qtsvg"})
                for source in sources["source_archives"]:
                    self.assertRegex(source["commit"], r"^[a-f0-9]{40}$")
                    self.assertRegex(source["sha256"], r"^[a-f0-9]{64}$")
                    self.assertTrue(source["url"].endswith("/" + source["commit"]))

    def test_unreviewed_qt_version_does_not_replace_existing_output(self):
        output = self.root / "Legal"
        output.mkdir()
        marker = output / "existing.txt"
        marker.write_text("preserve", encoding="utf-8")
        with self.assertRaises(SystemExit):
            self.module.assemble("studio", output, "6.9.0")
        self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")

    def test_desktop_products_validate_their_own_legal_materials(self):
        for product, entries in self.module.THIRD_PARTY_FILES.items():
            with self.subTest(product=product):
                source_dir = self.module.LEGAL_SOURCE_DIRS[product]
                isolated_dir = self.root / product / "sources"
                shutil.copytree(source_dir, isolated_dir)
                qt_sources = isolated_dir / "Qt6-SOURCES.json"
                inventory = json.loads(qt_sources.read_text(encoding="utf-8"))
                inventory["qt_version"] = f"{product}-qt"
                qt_sources.write_text(json.dumps(inventory), encoding="utf-8")
                license_text = f"{product} dependency license"
                (isolated_dir / "QWindowKit-LICENSE.txt").write_text(license_text, encoding="utf-8")
                isolated_entries = tuple(
                    (isolated_dir / source.name if source.is_relative_to(source_dir) else source, name)
                    for source, name in entries
                )
                output = self.root / product / "Legal"
                with mock.patch.object(self.module, "LEGAL_SOURCE_DIRS", {product: isolated_dir}), \
                        mock.patch.object(self.module, "THIRD_PARTY_FILES", {product: isolated_entries}):
                    self.module.assemble(product, output, f"{product}-qt")
                    self.module.verify(product, output)
                    license_file = output / "third-party/QWindowKit-LICENSE.txt"
                    self.assertEqual(license_file.read_text(encoding="utf-8"), license_text)
                    with self.assertRaises(SystemExit):
                        self.module.assemble(product, output, "another-product-qt")
                    license_file.write_text("another product's license", encoding="utf-8")
                    with self.assertRaises(SystemExit):
                        self.module.verify(product, output)

    def test_desktop_bundles_require_both_languages_of_product_legal_documents(self):
        names = (
            "USER_TERMS.md", "USER_TERMS.zh-CN.md",
            "OPEN_SOURCE_RELEASE_NOTICE.txt", "OPEN_SOURCE_RELEASE_NOTICE.zh-CN.txt",
        )
        for product in ("studio", "hub"):
            output = self.root / product / "Legal"
            self.module.assemble(product, output, "6.8.3")
            for name in names:
                with self.subTest(product=product, name=name):
                    path = output / name
                    content = path.read_bytes()
                    source = REPO_ROOT / name
                    self.assertEqual(content, source.read_bytes())
                    path.unlink()
                    with self.assertRaises(SystemExit):
                        self.module.verify(product, output)
                    path.write_bytes(b"outdated legal text")
                    with self.assertRaises(SystemExit):
                        self.module.verify(product, output)
                    path.write_bytes(content)

    def test_empty_and_changed_staged_license_are_rejected(self):
        output = self.root / "Legal"
        self.module.assemble("hub", output, "6.8.3")
        for content in (b"", b"unrelated license"):
            with self.subTest(content=content):
                (output / "third-party/Qt6-LGPL-3.0.txt").write_bytes(content)
                with self.assertRaises(SystemExit):
                    self.module.verify("hub", output)

    def test_standalone_sdk_requires_license_and_notice(self):
        sdk = self.root / "HarkenSDK"
        destination = sdk / "Unity/Harken"
        destination.mkdir(parents=True)
        for name in ("LICENSE.md", "NOTICE.md"):
            shutil.copyfile(REPO_ROOT / "HarkenSDK/Unity/Harken" / name, destination / name)
        self.module.verify_sdk(sdk)
        for name in ("LICENSE.md", "NOTICE.md"):
            with self.subTest(name=name):
                path = destination / name
                content = path.read_bytes()
                path.unlink()
                with self.assertRaises(SystemExit):
                    self.module.verify_sdk(sdk)
                path.write_bytes(content)

    def test_staged_text_accepts_platform_line_endings(self):
        output = self.root / "Legal"
        self.module.assemble("hub", output, "6.8.3")
        path = output / "LICENSE.txt"
        normalized = path.read_text(encoding="utf-8")
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=newline):
                path.write_bytes(normalized.replace("\n", newline).encode("utf-8"))
                self.module.verify("hub", output)


if __name__ == "__main__":
    unittest.main()
