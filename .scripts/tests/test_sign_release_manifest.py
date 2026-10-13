import base64
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "release" / "sign_release_manifest.py"

try:
    import cryptography  # noqa: F401

    HAVE_CRYPTOGRAPHY = True
except ImportError:
    HAVE_CRYPTOGRAPHY = False


def load_module():
    spec = importlib.util.spec_from_file_location("sign_release_manifest", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(
    HAVE_CRYPTOGRAPHY,
    "cryptography is not installed (pip install -r requirements.txt)",
)
class SignReleaseManifestTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.package = self.root / "studio.zip"
        self.package.write_bytes(b"package-bytes" * 1000)

    def tearDown(self):
        self.tmp.cleanup()

    def run_command(self, argv):
        args = self.module.parse_args(argv)
        if args.command == "generate-keypair":
            return self.module.generate_keypair(args)
        if args.command == "sign":
            return self.module.sign_manifest(args)
        return self.module.verify_manifest(args)

    def generate_and_sign(self):
        keys = self.run_command(
            ["generate-keypair", "--output-dir", str(self.root / "keys")]
        )
        manifest_path = self.root / "release.manifest"
        self.run_command(
            [
                "sign",
                "--private-key", keys["privateKey"],
                "--package", str(self.package),
                "--output", str(manifest_path),
                "--version", "1.4.0",
                "--build-id", "run12.abcd1234",
                "--package-url", "https://releases.example/studio.zip",
                "--release-sequence", "7",
            ]
        )
        return keys, manifest_path

    def test_sign_and_verify_roundtrip(self):
        keys, manifest_path = self.generate_and_sign()
        payload = self.run_command(
            ["verify", "--public-key", keys["publicKey"], "--manifest", str(manifest_path)]
        )

        self.assertEqual(payload["product"], "HarkenStudio")
        self.assertEqual(payload["version"], "1.4.0")
        self.assertEqual(payload["buildId"], "run12.abcd1234")
        self.assertEqual(payload["channel"], "stable")
        self.assertEqual(payload["releaseSequence"], 7)
        self.assertEqual(payload["packageSize"], self.package.stat().st_size)
        self.assertEqual(payload["sha256"], self.module.sha256_file(self.package))
        self.assertLess(payload["publishedAt"], payload["expiresAt"])

    def test_tampered_payload_fails_verification(self):
        keys, manifest_path = self.generate_and_sign()
        envelope = json.loads(manifest_path.read_text(encoding="ascii"))
        payload = json.loads(base64.b64decode(envelope["payloadBase64"]))
        payload["version"] = "9.9.9"
        envelope["payloadBase64"] = base64.b64encode(
            json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        ).decode("ascii")
        manifest_path.write_text(json.dumps(envelope), encoding="ascii")

        with self.assertRaises(SystemExit):
            self.run_command(
                [
                    "verify",
                    "--public-key", keys["publicKey"],
                    "--manifest", str(manifest_path),
                ]
            )

    def test_wrong_key_fails_verification(self):
        _, manifest_path = self.generate_and_sign()
        other = self.run_command(
            ["generate-keypair", "--output-dir", str(self.root / "other-keys")]
        )

        with self.assertRaises(SystemExit):
            self.run_command(
                [
                    "verify",
                    "--public-key", other["publicKey"],
                    "--manifest", str(manifest_path),
                ]
            )

    def test_existing_keys_are_never_overwritten(self):
        self.run_command(["generate-keypair", "--output-dir", str(self.root / "keys")])
        with self.assertRaises(SystemExit):
            self.run_command(
                ["generate-keypair", "--output-dir", str(self.root / "keys")]
            )


if __name__ == "__main__":
    unittest.main()
