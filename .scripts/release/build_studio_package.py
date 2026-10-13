#!/usr/bin/env python3
"""Build a HarkenStudio version package (zip + embedded manifests) from a deployed directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import schema_archive
import studio_schema_release_audit
from release import collect_legal_texts, release_evidence

STUDIO_BINARIES = ("HarkenStudio.exe", "HarkenStudio.app/Contents/MacOS/HarkenStudio")
PACKAGE_MANIFEST_NAME = "package-manifest.json"
CAPABILITIES_NAME = "capabilities.json"
SCHEMA_SOURCE = (
    Path(__file__).resolve().parents[2]
    / "HarkenSchema"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def schema_bundle() -> dict:
    try:
        return schema_archive.build_bundle(SCHEMA_SOURCE)
    except (OSError, ValueError) as error:
        raise SystemExit(f"error: invalid Schema inputs: {error}") from error


def verify_schema_directory(schema_dir: Path) -> dict:
    if not schema_dir.is_dir() or schema_dir.is_symlink():
        raise SystemExit(f"error: deployed Studio is missing schema/: {schema_dir}")
    bundle = schema_bundle()
    expected = bundle["installed"]
    actual = set()
    expected_directories = {
        parent.as_posix()
        for name in expected
        for parent in Path(name).parents
        if parent != Path(".")
    }
    for path in schema_dir.rglob("*"):
        relative = path.relative_to(schema_dir).as_posix()
        if path.is_symlink():
            raise SystemExit(f"error: deployed Schema contains a symlink: {relative}")
        if path.is_dir():
            if relative not in expected_directories:
                raise SystemExit(f"error: deployed Schema contains an unexpected directory: {relative}")
            continue
        actual.add(relative)
    if actual != set(expected):
        raise SystemExit("error: deployed schema/ contains missing or unexpected files")
    for name, content in expected.items():
        if (schema_dir / name).read_bytes() != content:
            raise SystemExit(f"error: deployed Schema does not match this build's inputs: {name}")
    return bundle["manifest"]


def studio_binary(input_dir: Path) -> Path:
    for relative in STUDIO_BINARIES:
        path = input_dir / relative
        if path.is_file():
            return path
    raise SystemExit("error: deployed Studio binary is missing")


def run_studio_self_test(binary: Path) -> dict:
    try:
        completed = subprocess.run(
            [str(binary), "--self-test"],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"error: cannot run Studio self-test: {error}") from error
    try:
        report = json.loads(completed.stdout.strip())
    except json.JSONDecodeError as error:
        raise SystemExit("error: Studio self-test did not return valid JSON") from error
    if completed.returncode != 0 or not isinstance(report, dict):
        raise SystemExit("error: Studio self-test failed")
    return report


def verify_studio_self_test(report: dict, schema: dict) -> None:
    if (
        report.get("ok") is not True
        or report.get("product") != "HarkenStudio"
        or report.get("schemaArchiveOk") is not True
        or type(report.get("capabilitiesVersion")) is not int
        or report["capabilitiesVersion"] != 2
        or report.get("schemaSnapshotHash") != schema["snapshotHash"]
        or type(report.get("editorSchemaVersion")) is not int
        or report["editorSchemaVersion"] != schema["trees"]["Editor"]["currentVersion"]
        or type(report.get("packSchemaVersion")) is not int
        or report["packSchemaVersion"] != schema["trees"]["Pack"]["currentVersion"]
        or not isinstance(report.get("gitCommit"), str)
        or len(report["gitCommit"]) != 40
        or any(char not in "0123456789abcdef" for char in report["gitCommit"])
        or type(report.get("gitDirty")) is not bool
    ):
        raise SystemExit(
            "error: Studio self-test does not match the packaged Schema snapshot"
        )


def metadata_entry(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def verify_release_candidate(args: argparse.Namespace, self_test: dict) -> None:
    if args.channel not in {"stable", "beta"}:
        return
    try:
        report = studio_schema_release_audit.verify_current_release("WORKTREE")
        if report["errors"]:
            raise ValueError("; ".join(report["errors"]))
        studio_schema_release_audit.require_current_reader({"currentReader": self_test})
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f"error: Schema release verification failed: {error}") from error


def collect_payload(input_dir: Path) -> list[Path]:
    files = [
        path
        for path in sorted(input_dir.rglob("*"))
        if path.is_file() and not path.is_symlink()
    ]
    if not files:
        raise SystemExit(f"error: input directory is empty: {input_dir}")
    rel_names = {path.relative_to(input_dir).as_posix() for path in files}
    if not any(binary in rel_names for binary in STUDIO_BINARIES):
        raise SystemExit(
            "error: input directory does not contain a Studio binary "
            f"({' or '.join(STUDIO_BINARIES)})"
        )
    for name in (PACKAGE_MANIFEST_NAME, CAPABILITIES_NAME):
        if name in rel_names:
            raise SystemExit(f"error: input directory already contains {name}")
    return files


def sdk_components(input_dir: Path) -> list[dict]:
    layouts = [
        (
            input_dir / "HarkenSDK" / "VERSION",
            input_dir / "HarkenSDK" / "Unity" / "Harken" / "package.json",
            input_dir / "HarkenSDK" / "Unreal" / "Harken" / "Harken.uplugin",
            "HarkenSDK/Unity/Harken",
            "HarkenSDK/Unreal/Harken",
        ),
        (
            input_dir / "HarkenStudio.app" / "Contents" / "MacOS" / "HarkenSDK" / "VERSION",
            input_dir / "HarkenStudio.app" / "Contents" / "MacOS" / "HarkenSDK" / "Unity" / "Harken" / "package.json",
            input_dir / "HarkenStudio.app" / "Contents" / "MacOS" / "HarkenSDK" / "Unreal" / "Harken" / "Harken.uplugin",
            "HarkenStudio.app/Contents/MacOS/HarkenSDK/Unity/Harken",
            "HarkenStudio.app/Contents/MacOS/HarkenSDK/Unreal/Harken",
        ),
    ]
    matches = [layout for layout in layouts if any(path.exists() for path in layout[:3])]
    if len(matches) != 1:
        raise SystemExit("error: expected one complete Unity and Unreal SDK layout")
    version_path, unity_path, unreal_path, unity_root, unreal_root = matches[0]
    for path in (version_path, unity_path, unreal_path):
        if not path.is_file():
            raise SystemExit(f"error: required SDK identity is missing: {path}")
    version = version_path.read_text(encoding="utf-8").strip()
    unity_version = json.loads(unity_path.read_text(encoding="utf-8"))["version"]
    unreal_version = json.loads(unreal_path.read_text(encoding="utf-8"))["VersionName"]
    if not version or not unity_version or unreal_version != version:
        raise SystemExit(
            "error: HarkenSDK/VERSION must match the Unreal plugin and the "
            "Harken Unity package must declare a version"
        )
    return [{
        "product": "HarkenSDK",
        "version": version,
        "artifacts": {"unity": unity_root, "unreal": unreal_root},
    }]


def build_package(args: argparse.Namespace) -> dict:
    input_dir = Path(args.input_dir).resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"error: input directory not found: {input_dir}")

    binary = studio_binary(input_dir)
    legal_dir = input_dir / "Legal" if binary.suffix == ".exe" else binary.parent.parent / "Resources" / "Legal"
    collect_legal_texts.verify("studio", legal_dir)
    collect_legal_texts.verify_sdk(binary.parent / "HarkenSDK")
    deployed_schema = verify_schema_directory(binary.parent / "schema")
    self_test = run_studio_self_test(binary)
    verify_studio_self_test(self_test, deployed_schema)
    if (self_test.get("version") != args.version
            or self_test.get("buildId") != args.build_id
            or self_test.get("channel") != args.channel):
        raise SystemExit("error: Studio binary identity does not match package arguments")
    verify_release_candidate(args, self_test)

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    package_path = output_dir / f"{args.version}-{args.build_id}.zip"
    if package_path.exists():
        raise SystemExit(f"error: package already exists: {package_path}")

    payload = collect_payload(input_dir)
    file_records = []
    for path in payload:
        rel = path.relative_to(input_dir).as_posix()
        file_records.append(
            {"path": rel, "size": path.stat().st_size, "sha256": sha256_file(path)}
        )
    package_manifest = {
        "product": "HarkenStudio",
        "version": args.version,
        "buildId": args.build_id,
        "gitCommit": self_test["gitCommit"],
        "gitDirty": self_test["gitDirty"],
        "channel": args.channel,
        "createdAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "releaseSequence": args.release_sequence,
        "components": sdk_components(input_dir),
        "files": file_records,
    }
    capabilities = {
        "studioVersion": args.version,
        "buildId": args.build_id,
        "capabilitiesVersion": 2,
    }

    manifest_content = json.dumps(package_manifest, indent=2).encode("utf-8")
    with release_evidence.package_output(output_dir) as staging:
        staged_package = staging / package_path.name
        evidence = release_evidence.prepare(
            "studio", input_dir, Path(args.build_dir).resolve(), staging,
            args.version, args.build_id, binary_identity=self_test, payload_records=file_records,
            payload_manifest={"artifact": package_path.name, "path": PACKAGE_MANIFEST_NAME,
                              "sha256": hashlib.sha256(manifest_content).hexdigest(),
                              "fileCount": len(file_records)})
        with zipfile.ZipFile(staged_package, "x", zipfile.ZIP_DEFLATED) as archive:
            for path, record in zip(payload, file_records):
                archive.write(path, record["path"])
            archive.writestr(
                metadata_entry(PACKAGE_MANIFEST_NAME),
                manifest_content,
            )
            archive.writestr(
                metadata_entry(CAPABILITIES_NAME),
                json.dumps(capabilities, indent=2),
            )

        artifacts = release_evidence.finalize(
            evidence, [staged_package] + [Path(path).resolve() for path in args.additional_artifact], input_dir)
    return {
        "package": str(package_path),
        "evidence": str(output_dir / evidence.name),
        "sha256": artifacts[0]["sha256"],
        "size": artifacts[0]["size"],
        "fileCount": len(file_records),
        "version": args.version,
        "buildId": args.build_id,
        "channel": args.channel,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--additional-artifact", action="append", default=[])
    parser.add_argument(
        "--channel",
        choices=("stable", "beta", "dev", "nightly"),
        default="stable",
    )
    parser.add_argument("--release-sequence", type=int, default=0)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    summary = build_package(parse_args(argv))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
