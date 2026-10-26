#!/usr/bin/env python3
"""Build the Harken Hub macOS installer package (.pkg) from a deployed app bundle."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import collect_legal_texts, product_legal, release_evidence

BUNDLE_BINARY = Path("Contents/MacOS/HarkenHub")
DAEMON_PLIST = Path(
    "Contents/Library/LaunchDaemons/"
    "com.harkensound.harkenhub.updatebridge.plist"
)
FOREIGN_BINARIES = ("HarkenStudio", "HarkenEditor")
PACKAGE_IDENTIFIER = "com.harkensound.harkenhub"
INSTALL_LOCATION = "/Applications"
MACHINE_DATA_ROOT = "/Library/Application Support/HarkenSound/HarkenHub"
PKG_VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+")


def pkg_version(version: str) -> str:
    match = PKG_VERSION_PATTERN.match(version)
    if not match:
        raise SystemExit(
            f"error: version must start with MAJOR.MINOR.PATCH: {version}"
        )
    return match.group(0)


def validate_bundle(bundle: Path) -> None:
    if not bundle.is_dir() or bundle.suffix != ".app":
        raise SystemExit(f"error: not an app bundle directory: {bundle}")
    collect_legal_texts.verify("hub", bundle / "Contents" / "Resources" / "Legal")
    if not (bundle / BUNDLE_BINARY).is_file():
        raise SystemExit(
            f"error: bundle does not contain {BUNDLE_BINARY.as_posix()}: "
            f"{bundle}"
        )
    if not (bundle / DAEMON_PLIST).is_file():
        raise SystemExit(
            "error: bundle does not embed the update bridge daemon plist "
            f"({DAEMON_PLIST.as_posix()}): {bundle}"
        )
    foreign = sorted(
        path.relative_to(bundle).as_posix()
        for path in bundle.rglob("*")
        if path.is_file() and not path.is_symlink()
        and path.name in FOREIGN_BINARIES
    )
    if foreign:
        raise SystemExit(
            "error: bundle holds non-hub product binaries "
            f"({', '.join(foreign)}); point --bundle at a hub-only "
            "build"
        )


def generate_postinstall() -> str:
    return (
        "#!/bin/sh\n"
        "set -e\n"
        f'DATA_ROOT="{MACHINE_DATA_ROOT}"\n'
        'mkdir -p "$DATA_ROOT/handoff" "$DATA_ROOT/pins"\n'
        'chown root:admin "$DATA_ROOT" "$DATA_ROOT/handoff" '
        '"$DATA_ROOT/pins"\n'
        'chmod 755 "$DATA_ROOT"\n'
        'chmod 1777 "$DATA_ROOT/handoff" "$DATA_ROOT/pins"\n'
        "exit 0\n"
    )


def generate_distribution_xml(version: str, component_filename: str) -> str:
    safe_version = escape(pkg_version(version))
    safe_component = escape(component_filename)
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<installer-gui-script minSpecVersion="2">\n'
        "    <title>Harken Hub</title>\n"
        '    <license file="License.txt"/>\n'
        '    <options customize="never" require-scripts="false" '
        'hostArchitectures="x86_64,arm64"/>\n'
        '    <domains enable_localSystem="true"/>\n'
        "    <choices-outline>\n"
        '        <line choice="default"/>\n'
        "    </choices-outline>\n"
        f'    <choice id="default" title="Harken Hub">\n'
        f'        <pkg-ref id="{PACKAGE_IDENTIFIER}"/>\n'
        "    </choice>\n"
        f'    <pkg-ref id="{PACKAGE_IDENTIFIER}" version="{safe_version}" '
        f'onConclusion="none">{safe_component}</pkg-ref>\n'
        "</installer-gui-script>\n"
    )


def build_pkg(args: argparse.Namespace) -> dict:
    bundle = Path(args.bundle).resolve()
    validate_bundle(bundle)
    version = pkg_version(args.version)

    output_dir = Path(args.output_dir).resolve()
    product_pkg = output_dir / f"HarkenHubSetup-{args.version}-{args.build_id}.pkg"
    bundle_zip = output_dir / f"HarkenHub-macos-universal-{args.version}-{args.build_id}.zip"
    for path in (product_pkg, bundle_zip):
        if path.exists():
            raise SystemExit(f"error: package output already exists: {path}")
    with release_evidence.package_output(output_dir) as temporary:
        product_pkg = temporary / product_pkg.name
        bundle_zip = temporary / bundle_zip.name
        staging = temporary / "staging"
        evidence = release_evidence.prepare(
            "hub", bundle, Path(args.build_dir).resolve(), temporary, args.version, args.build_id)

        payload_root = staging / "root"
        scripts_dir = staging / "scripts"
        resources_dir = staging / "resources"
        for directory in (payload_root, scripts_dir, resources_dir):
            directory.mkdir(parents=True)

        shutil.copytree(bundle, payload_root / bundle.name, symlinks=True)

        postinstall = scripts_dir / "postinstall"
        postinstall.write_text(generate_postinstall(), encoding="utf-8")
        postinstall.chmod(0o755)

        license_source = resources_dir / "License.txt"
        if args.license_file:
            custom_license = Path(args.license_file).resolve()
            if not custom_license.is_file():
                raise SystemExit(f"error: license file is missing: {custom_license}")
            shutil.copyfile(custom_license, license_source)
        else:
            license_source.write_bytes(product_legal.installer_text().encode("utf-8"))

        component_name = f"HarkenHubComponent-{version}.pkg"
        component_pkg = staging / component_name
        subprocess.run(
            [
                "pkgbuild",
                "--root", str(payload_root),
                "--install-location", INSTALL_LOCATION,
                "--identifier", PACKAGE_IDENTIFIER,
                "--version", version,
                "--scripts", str(scripts_dir),
                str(component_pkg),
            ],
            check=True,
        )

        distribution = staging / "distribution.xml"
        distribution.write_text(
            generate_distribution_xml(version, component_name), encoding="utf-8"
        )
        release_evidence.retain_packaging_inputs(evidence, {
            "distribution.xml": distribution, "postinstall": postinstall,
            "License.txt": license_source})

        subprocess.run(
            [
                "productbuild",
                "--distribution", str(distribution),
                "--resources", str(resources_dir),
                "--package-path", str(staging),
                str(product_pkg),
            ],
            check=True,
        )

        shutil.rmtree(staging)
        subprocess.run(["ditto", "-c", "-k", "--keepParent", str(bundle), str(bundle_zip)], check=True)
        artifacts = release_evidence.finalize(evidence, [product_pkg, bundle_zip], bundle)
    return {
        "pkg": str(output_dir / product_pkg.name),
        "evidence": str(output_dir / evidence.name),
        "bundleZip": str(output_dir / bundle_zip.name),
        "sha256": artifacts[0]["sha256"],
        "sizeBytes": artifacts[0]["size"],
        "version": args.version,
        "pkgVersion": version,
        "buildId": args.build_id,
        "identifier": PACKAGE_IDENTIFIER,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--license-file", default=None)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    summary = build_pkg(parse_args(argv))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
