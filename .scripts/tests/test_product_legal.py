import hashlib
import tempfile
import unittest
from pathlib import Path

from script_loader import load_script_module

REPO_ROOT = Path(__file__).resolve().parents[2]


class ProductLegalTest(unittest.TestCase):
    def setUp(self):
        self.module = load_script_module("product_legal", REPO_ROOT / ".scripts/release/product_legal.py")
        (REPO_ROOT / ".temp").mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=REPO_ROOT / ".temp")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_20260930_r3_consent_hashes_match_supplied_documents(self):
        expected = {
            ".txt": "738bc81144d579c5bcb8fdca61c322dca8742ee79732d9a601c7bd98e16bbc3e",
            ".zh-CN.txt": "513b3d440dec257dc7e2f7a937c89fe87e63b79b49d413e32702d3f80b49adc2",
        }
        self.module.write_hub_resources(self.root)
        for suffix, digest in expected.items():
            with self.subTest(suffix=suffix):
                documents = [(self.root / (name + suffix)).read_bytes()
                             for name in ("USER_AGREEMENT", "PRIVACY_POLICY")]
                self.assertEqual(hashlib.sha256(b"\n\n".join(documents)).hexdigest(), digest)

    def test_20260930_r3_installer_text_matches_supplied_documents(self):
        digest = hashlib.sha256(self.module.installer_text().encode("utf-8")).hexdigest()
        self.assertEqual(digest, "2c3a2d3fbc5a900bbf20811d0d1663bdfcdc1b2ae139cd96fafa3e56458ed881")

    def test_source_edits_reach_resources_and_installer(self):
        for name in self.module.TERMS_FILES.values():
            (self.root / name).write_bytes(b"# Agreement\r\n\r\nOriginal\r\n\r\n---\r\n\r\n# Privacy\r\n\r\nPolicy\r\n")
        output = self.root / "generated"
        self.module.write_hub_resources(output, self.root)
        self.assertEqual((output / "USER_AGREEMENT.txt").read_bytes(), b"Agreement\n\nOriginal\n")
        source = self.root / "USER_TERMS.md"
        source.write_bytes(source.read_bytes().replace(b"Original", b"Updated"))
        self.module.write_hub_resources(output, self.root)
        self.assertEqual((output / "USER_AGREEMENT.txt").read_bytes(), b"Agreement\n\nUpdated\n")
        self.assertEqual(self.module.installer_text(self.root),
                         "Agreement\n\nOriginal\n\nPrivacy\n\nPolicy\n\nAgreement\n\nUpdated\n\nPrivacy\n\nPolicy\n")

    def test_invalid_document_structure_is_rejected(self):
        valid = "# Agreement\n\nTerms\n\n---\n\n# Privacy\n\nPolicy\n"
        for invalid in (
            "", valid.split("\n---")[0], valid + "\n---\n\n# Extra\n\nText\n",
            valid.replace("# Privacy", "Privacy"), valid.replace("Terms", ""),
            valid.replace("Terms", "Terms\n# Extra"), valid.rstrip(),
            valid.replace("# Agreement", "# "), valid.replace("Privacy\n\n", "Privacy\n"),
        ):
            with self.subTest(text=invalid):
                source = self.root / "terms.md"
                source.write_bytes(invalid.encode("utf-8"))
                with self.assertRaises(ValueError):
                    self.module.read_documents(source)

    def test_rtf_escapes_unicode_and_control_characters(self):
        text = self.module.render_rtf("{a}\\\t\n\u4e2d\uff01\U0001f600")
        self.assertTrue(text.startswith("{\\rtf1"))
        self.assertIn("\\{a\\}\\\\\\tab \\par\n\\u20013?\\u-255?\\u-10179?\\u-8704?", text)
        self.assertTrue(text.endswith("}\n"))
        text.encode("ascii")


if __name__ == "__main__":
    unittest.main()
