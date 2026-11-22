import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


tool_path = Path(__file__).resolve().parents[2] / "Unity/Harken/Tools~/publish_packs.py"
spec = importlib.util.spec_from_file_location("publish_packs", tool_path)
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PackZipPublicationTest(unittest.TestCase):
    def test_member_selection_paths_and_reproducibility(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            platform = root / "Windows"
            language = platform / "Japanese"
            language.mkdir(parents=True)
            (platform / "Init.bytes").write_bytes(b"VSPK" + bytes(256))
            (language / "Play.json").write_bytes(b"{}")
            auxiliary = language / "Media.vkd"
            with auxiliary.open("wb") as output:
                output.truncate(publisher.MAX_GZIP_MEMBER_BYTES + 1)
            first, second = root / "first.zip", root / "second.zip"
            publisher.write_platform_zip(platform, first)
            publisher.write_platform_zip(platform, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(archive.namelist(), ["Init.bytes", "Japanese/Media.vkd", "Japanese/Play.json"])
                self.assertEqual(archive.getinfo("Init.bytes").compress_type, zipfile.ZIP_DEFLATED)
                self.assertEqual(archive.getinfo("Japanese/Media.vkd").compress_type, zipfile.ZIP_STORED)
                self.assertEqual(archive.getinfo("Japanese/Play.json").compress_type, zipfile.ZIP_STORED)
                for member in archive.namelist():
                    self.assertEqual(archive.read(member), (platform / member).read_bytes())


if __name__ == "__main__":
    unittest.main()
