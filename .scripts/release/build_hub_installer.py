#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import collect_legal_texts, hub_msi, product_legal, release_evidence

HUB_BINARY = "HarkenHub.exe"
LEGACY_HUB_BINARY = "HarkenStudioHub.exe"
BRIDGE_BINARY = "Updater/HarkenHubUpdateBridge.exe"
FOREIGN_BINARIES = (
    "HarkenStudio.exe",
    "HarkenEditor.exe",
    "HarkenHubSetup.exe",
    "HarkenStudioHubSetup.exe",
)
PAYLOAD_RESOURCE_NAME = "HARKENHUBMSI"
CHINESE_PAYLOAD_RESOURCE_NAME = "HARKENHUBMSI_ZHCN"
RT_RCDATA = 10
MSI_VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+")


def msi_version(version: str) -> str:
    match = MSI_VERSION_PATTERN.match(version)
    if not match:
        raise SystemExit(
            f"error: version must start with MAJOR.MINOR.PATCH: {version}"
        )
    return match.group(0)


def collect_payload(input_dir: Path) -> list[Path]:
    collect_legal_texts.verify("hub", input_dir / "Legal")
    files = [
        path
        for path in sorted(input_dir.rglob("*"))
        if path.is_file() and not path.is_symlink()
    ]
    if not files:
        raise SystemExit(f"error: input directory is empty: {input_dir}")
    rel_names = {path.relative_to(input_dir).as_posix() for path in files}
    for required in (HUB_BINARY, LEGACY_HUB_BINARY, BRIDGE_BINARY):
        if required not in rel_names:
            raise SystemExit(
                f"error: input directory does not contain {required}: "
                f"{input_dir}"
            )
    foreign = sorted(
        name
        for name in rel_names
        if Path(name).name in FOREIGN_BINARIES
    )
    if foreign:
        raise SystemExit(
            "error: input directory holds non-hub product binaries "
            f"({', '.join(foreign)}); point --input-dir at a directory that "
            "was built for the hub only"
        )
    return files


def embed_payload(stub_exe: Path, msi_path: Path, localized_msi_path: Path, exe_path: Path) -> None:
    shutil.copyfile(stub_exe, exe_path)

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    kernel32.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel32.UpdateResourceW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        wintypes.LPCWSTR,
        wintypes.WORD,
        wintypes.LPVOID,
        wintypes.DWORD,
    ]
    kernel32.UpdateResourceW.restype = wintypes.BOOL
    kernel32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    kernel32.EndUpdateResourceW.restype = wintypes.BOOL

    handle = kernel32.BeginUpdateResourceW(str(exe_path), False)
    if not handle:
        raise SystemExit(
            f"error: BeginUpdateResource failed for {exe_path} "
            f"(winerror {ctypes.get_last_error()})"
        )
    for name, package in ((PAYLOAD_RESOURCE_NAME, msi_path),
                          (CHINESE_PAYLOAD_RESOURCE_NAME, localized_msi_path)):
        payload = package.read_bytes()
        buffer = ctypes.create_string_buffer(payload, len(payload))
        updated = kernel32.UpdateResourceW(
            handle,
            ctypes.c_void_p(RT_RCDATA),
            name,
            0,
            ctypes.cast(buffer, wintypes.LPVOID),
            len(payload),
        )
        if not updated:
            error = ctypes.get_last_error()
            kernel32.EndUpdateResourceW(handle, True)
            raise SystemExit(
                f"error: UpdateResource failed for {exe_path} "
                f"(winerror {error})"
            )
    if not kernel32.EndUpdateResourceW(handle, False):
        raise SystemExit(
            f"error: EndUpdateResource failed for {exe_path} "
            f"(winerror {ctypes.get_last_error()})"
        )


def sign_command(msi_path: Path) -> list[str] | None:
    thumbprint = os.environ.get("HARKEN_AUTHENTICODE_THUMBPRINT", "")
    if not thumbprint:
        return None
    command = ["signtool", "sign", "/sha1", thumbprint, "/fd", "SHA256"]
    timestamp_url = os.environ.get("HARKEN_AUTHENTICODE_TIMESTAMP_URL", "")
    if timestamp_url:
        command += ["/tr", timestamp_url, "/td", "SHA256"]
    command.append(str(msi_path))
    return command


def build_installer(
    args: argparse.Namespace, run=subprocess.run, embed=embed_payload, build_msi=hub_msi.build
) -> dict:
    input_dir = Path(args.input_dir).resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"error: input directory not found: {input_dir}")

    script_dir = Path(__file__).resolve().parent
    legal_overrides = {
        "en": (args.eula_rtf, args.privacy_rtf),
        "zh-CN": (args.eula_rtf_zh_cn, args.privacy_rtf_zh_cn),
    }
    for documents in legal_overrides.values():
        for document in documents:
            if document and not Path(document).is_file():
                raise SystemExit(f"error: missing authoring input: {document}")

    stub_exe = None
    if args.stub_exe:
        stub_exe = Path(args.stub_exe).resolve()
        if not stub_exe.is_file():
            raise SystemExit(f"error: setup stub not found: {stub_exe}")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with release_evidence.package_output(output_dir) as staging:
        files = collect_payload(input_dir)
        evidence = release_evidence.prepare(
            "hub", input_dir, Path(args.build_dir).resolve(), staging, args.version, args.build_id,
            setup_stub=stub_exe)
        packaging_inputs = evidence / "packaging-inputs"
        packaging_inputs.mkdir(parents=True, exist_ok=True)
        legal_inputs = {}
        for language, overrides in legal_overrides.items():
            documents = product_legal.read_documents(product_legal.REPO_ROOT / product_legal.TERMS_FILES[language])
            suffix = "" if language == "en" else ".zh-CN"
            legal_inputs[language] = []
            for name, text, custom in zip(("license", "privacy"), documents, overrides):
                document = packaging_inputs / f"{name}{suffix}.rtf"
                if custom:
                    shutil.copyfile(Path(custom).resolve(), document)
                else:
                    document.write_bytes(product_legal.render_rtf(text).encode("ascii"))
                legal_inputs[language].append(document)
        product_version = msi_version(args.version)
        msi_path = staging / f"HarkenHubSetup-{args.version}-{args.build_id}.msi"
        localized_msi_path = msi_path.with_suffix(".zh-CN.msi")
        packages = (msi_path, localized_msi_path)
        build_msi(files, input_dir, product_version, legal_inputs, msi_path, localized_msi_path, packaging_inputs)
        for package in packages:
            if not package.is_file():
                raise SystemExit(f"error: Windows Installer build produced no MSI at {package}")
        release_evidence.retain_packaging_inputs(evidence, {
            "hub_msi.py": script_dir / "hub_msi.py",
            "hub_msi_zh_CN.json": script_dir / "hub_msi_zh_CN.json",
            "windows_installer.py": script_dir / "windows_installer.py",
            "msi-tables.json": packaging_inputs / "msi-tables.json",
            "HubIcon.ico": script_dir.parents[1] / "HarkenHub/Resources/icons/HubIcon.ico",
            **{path.name: path for documents in legal_inputs.values() for path in documents}})

        signed = False
        if sign_command(msi_path) is not None:
            for package in packages:
                sign_result = run(sign_command(package))
                if sign_result.returncode != 0:
                    raise SystemExit(
                        f"error: signtool failed with exit code {sign_result.returncode}"
                    )
            signed = True
        else:
            print(
                "signing skipped: HARKEN_AUTHENTICODE_THUMBPRINT is not set",
                file=sys.stderr,
            )

        summary = {
            "package": str(output_dir / msi_path.name),
            "chinesePackage": str(output_dir / localized_msi_path.name),
            "evidence": str(output_dir / evidence.name),
            "fileCount": len(files),
            "version": args.version,
            "msiVersion": product_version,
            "buildId": args.build_id,
            "signed": signed,
        }

        if stub_exe is not None:
            exe_path = (
                staging / f"HarkenHubSetup-{args.version}-{args.build_id}.exe"
            )
            embed(stub_exe, msi_path, localized_msi_path, exe_path)
            if not exe_path.is_file():
                raise SystemExit(
                    f"error: payload embedding produced no EXE at {exe_path}"
                )
            if signed:
                exe_signing = sign_command(exe_path)
                exe_sign_result = run(exe_signing)
                if exe_sign_result.returncode != 0:
                    raise SystemExit(
                        "error: signtool failed with exit code "
                        f"{exe_sign_result.returncode}"
                    )
            summary["setupExe"] = str(output_dir / exe_path.name)

        artifacts = list(packages)
        if stub_exe is not None:
            artifacts.append(exe_path)
        records = release_evidence.finalize(evidence, artifacts, input_dir,
                                            intermediate_artifacts=packages if stub_exe is not None else ())
        summary.update(sha256=records[0]["sha256"], size=records[0]["size"])
        summary.update(chinesePackageSha256=records[1]["sha256"], chinesePackageSize=records[1]["size"])
        if stub_exe is not None:
            summary.update(setupExeSha256=records[-1]["sha256"], setupExeSize=records[-1]["size"])
    return summary


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the Harken Hub MSI and setup EXE with Windows Installer.")
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--eula-rtf", default=None, help="Override the English license agreement with an RTF file.")
    parser.add_argument("--privacy-rtf", default=None, help="Override the English privacy policy with an RTF file.")
    parser.add_argument("--eula-rtf-zh-cn", default=None, help="Override the Chinese license agreement with an RTF file.")
    parser.add_argument("--privacy-rtf-zh-cn", default=None, help="Override the Chinese privacy policy with an RTF file.")
    parser.add_argument("--stub-exe", default=None)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    summary = build_installer(parse_args(argv))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
