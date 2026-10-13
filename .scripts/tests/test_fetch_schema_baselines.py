from __future__ import annotations

import hashlib
import io
import json
import os
import runpy
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "release" / "fetch_schema_baselines.py"
MODULE = runpy.run_path(str(SCRIPT))
GLOBALS = MODULE["fetch_baselines"].__globals__


def package_bytes(members: list[tuple[str, bytes, int]]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, content, mode in members:
            member = zipfile.ZipInfo(name)
            member.create_system = 3
            member.external_attr = mode << 16
            archive.writestr(member, content)
    return output.getvalue()


class Response(io.BytesIO):
    def __init__(self, content: bytes, url: str = "https://packages.example.test/studio.zip") -> None:
        super().__init__(content)
        self.url = url

    def geturl(self) -> str:
        return self.url


class FetchSchemaBaselinesTest(unittest.TestCase):
    def setUp(self) -> None:
        (REPO_ROOT / ".temp").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="fetch-baselines-test-", dir=REPO_ROOT / ".temp")
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "baselines"
        self.payload = package_bytes([
            ("HarkenStudio.exe", b"studio executable", stat.S_IFREG | 0o755),
            ("schema/build.json", b"{}", stat.S_IFREG | 0o644),
        ])
        self.entry = {
            "tag": "v1.1.0", "platform": "windows-x64",
            "url": "https://packages.example.test/studio.zip",
            "sha256": hashlib.sha256(self.payload).hexdigest(),
        }

    def fetch(self, entries: list[dict], platform: str = "windows-x64") -> dict[str, str]:
        return MODULE["fetch_baselines"](entries, platform, self.output)

    def test_verified_archive_is_extracted_and_reported_as_absolute_reader_directory(self) -> None:
        with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(self.payload)}):
            readers = self.fetch([self.entry])
        root = Path(readers["v1.1.0"])
        self.assertTrue(root.is_absolute())
        self.assertTrue(root.is_relative_to(self.output))
        self.assertEqual((root / "HarkenStudio.exe").read_bytes(), b"studio executable")
        self.assertEqual((root / "schema" / "build.json").read_bytes(), b"{}")
        self.assertEqual(json.loads((self.output / "readers.json").read_text(encoding="utf-8")), readers)
        self.assertFalse(any(path.suffix == ".zip" for path in self.output.rglob("*")))

    def test_hash_mismatch_does_not_extract_or_replace_previous_readers(self) -> None:
        self.output.mkdir()
        sentinel = self.output / "previous-reader"
        sentinel.write_bytes(b"keep")
        entry = {**self.entry, "sha256": "0" * 64}
        with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(self.payload)}):
            with self.assertRaises(ValueError):
                self.fetch([entry])
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertEqual({path.name for path in self.output.iterdir()}, {"previous-reader"})

    def test_platform_filter_accepts_same_tag_for_other_platforms(self) -> None:
        requested: list[str] = []

        def download(url: str, timeout: int) -> Response:
            requested.append(url)
            return Response(self.payload, url)

        other = {**self.entry, "platform": "macos-arm64", "url": "https://packages.example.test/mac.zip"}
        with patch.dict(GLOBALS, {"urlopen": download}):
            readers = self.fetch([other, self.entry], "macos-arm64")
        self.assertEqual(set(readers), {self.entry["tag"]})
        self.assertEqual(requested, [other["url"]])

    def test_duplicate_tag_for_one_platform_is_rejected_before_download(self) -> None:
        def unexpected_download(*args, **kwargs):
            self.fail("invalid input must not download")

        with patch.dict(GLOBALS, {"urlopen": unexpected_download}):
            with self.assertRaises(ValueError):
                self.fetch([self.entry, {**self.entry, "url": "https://packages.example.test/other.zip"}])
        self.assertFalse(self.output.exists())

    def test_first_release_and_unselected_platform_need_no_download(self) -> None:
        def unexpected_download(*args, **kwargs):
            self.fail("empty platform selection must not download")

        with patch.dict(GLOBALS, {"urlopen": unexpected_download}):
            self.assertEqual(self.fetch([]), {})
            self.assertEqual(self.fetch([self.entry], "macos-x64"), {})
        self.assertEqual(json.loads((self.output / "readers.json").read_text(encoding="utf-8")), {})

    def test_invalid_entry_fields_and_non_https_urls_are_rejected(self) -> None:
        invalid = (
            {**self.entry, "tag": ""}, {**self.entry, "tag": "v1 2"},
            {**self.entry, "platform": "linux-x64"}, {**self.entry, "sha256": "missing"},
            {**self.entry, "url": "http://packages.example.test/studio.zip"},
            {**self.entry, "url": "https://name:secret@packages.example.test/studio.zip"},
            {**self.entry, "url": "https:///studio.zip"},
            {**self.entry, "extra": True}, {"tag": "v1"},
        )
        for entry in invalid:
            with self.subTest(entry=entry):
                with self.assertRaises(ValueError):
                    MODULE["selected_entries"]([entry], "windows-x64")
        with self.assertRaises(ValueError):
            MODULE["selected_entries"]({}, "windows-x64")

    def test_redirect_cannot_downgrade_to_http(self) -> None:
        with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(self.payload, "http://packages.example.test/studio.zip")}):
            with self.assertRaises(ValueError):
                self.fetch([self.entry])
        self.assertFalse(self.output.exists())

    def test_unsafe_zip_paths_and_symlinks_are_rejected(self) -> None:
        names = ("../outside", "/outside", "C:/outside", "folder/../../outside", "folder\\..\\outside", "data:stream")
        members = [(name, stat.S_IFREG | 0o644) for name in names]
        members.extend([
            ("link", stat.S_IFLNK | 0o777),
            ("pipe", stat.S_IFIFO | 0o600),
        ])
        for name, mode in members:
            with self.subTest(name=name):
                payload = package_bytes([(name, b"../outside", mode)])
                entry = {**self.entry, "sha256": hashlib.sha256(payload).hexdigest()}
                with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(payload)}):
                    with self.assertRaises(ValueError):
                        self.fetch([entry])
                self.assertFalse(self.output.exists())
                self.assertFalse((Path(self.directory.name) / "outside").exists())

    def test_ambiguous_zip_paths_are_rejected(self) -> None:
        payload = package_bytes([
            ("HarkenStudio.exe", b"first", stat.S_IFREG | 0o755),
            ("harkenstudio.exe", b"second", stat.S_IFREG | 0o755),
        ])
        entry = {**self.entry, "sha256": hashlib.sha256(payload).hexdigest()}
        with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(payload)}):
            with self.assertRaises(ValueError):
                self.fetch([entry])

    def test_unix_executable_permissions_are_restored(self) -> None:
        payload = package_bytes([
            ("HarkenStudio.app/Contents/MacOS/HarkenStudio", b"executable", stat.S_IFREG | 0o755),
        ])
        entry = {**self.entry, "platform": "macos-arm64", "sha256": hashlib.sha256(payload).hexdigest()}
        original_chmod = os.chmod
        with patch.dict(GLOBALS, {"urlopen": lambda url, timeout: Response(payload)}):
            with patch.object(os, "chmod", wraps=original_chmod) as chmod:
                readers = self.fetch([entry], "macos-arm64")
        executable = Path(readers[self.entry["tag"]]) / "HarkenStudio.app" / "Contents" / "MacOS" / "HarkenStudio"
        self.assertEqual(executable.read_bytes(), b"executable")
        self.assertTrue(any(call.args[1] == 0o755 for call in chmod.call_args_list))
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(executable.stat().st_mode), 0o755)

    def test_output_is_confined_to_an_explicit_repository_temp_directory(self) -> None:
        for output in (REPO_ROOT / ".temp", REPO_ROOT / "HarkenSchema", REPO_ROOT / ".." / "outside"):
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    MODULE["fetch_baselines"]([], "windows-x64", output)

    def test_cli_reads_json_from_the_named_environment_variable(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--input-json-env", "HARKEN_SCHEMA_BASELINES_TEST",
             "--platform", "windows-x64", "--output-dir", str(self.output)],
            cwd=REPO_ROOT, env={**os.environ, "HARKEN_SCHEMA_BASELINES_TEST": "[]"},
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {})
        self.assertEqual(json.loads((self.output / "readers.json").read_text(encoding="utf-8")), {})


if __name__ == "__main__":
    unittest.main()
