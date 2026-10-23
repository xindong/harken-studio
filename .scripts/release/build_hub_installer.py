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

HUB_BINARY = "HarkenStudioHub.exe"
LEGACY_HUB_BINARY = "HarkenHub.exe"
BRIDGE_BINARY = "Updater/HarkenHubUpdateBridge.exe"
FOREIGN_BINARIES = (
    "HarkenStudio.exe",
    "HarkenEditor.exe",
    "HarkenHubSetup.exe",
    "HarkenStudioHubSetup.exe",
)
PAYLOAD_RESOURCE_NAME = "HARKENHUBMSI"
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


def embed_payload(stub_exe: Path, msi_path: Path, exe_path: Path) -> None:
    shutil.copyfile(stub_exe, exe_path)
    payload = msi_path.read_bytes()

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
    buffer = ctypes.create_string_buffer(payload, len(payload))
    updated = kernel32.UpdateResourceW(
        handle,
        ctypes.c_void_p(RT_RCDATA),
        PAYLOAD_RESOURCE_NAME,
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
    eula_rtf = Path(args.eula_rtf).resolve() if args.eula_rtf else None
    if eula_rtf is not None and not eula_rtf.is_file():
        raise SystemExit(f"error: missing authoring input: {eula_rtf}")

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
        if eula_rtf is None:
            eula_rtf = packaging_inputs / "license.rtf"
            eula_rtf.write_bytes(product_legal.render_rtf(product_legal.installer_text()).encode("ascii"))
        product_version = msi_version(args.version)
        msi_path = staging / f"HarkenStudioHubSetup-{args.version}-{args.build_id}.msi"
        build_msi(files, input_dir, product_version, eula_rtf, msi_path, packaging_inputs)
        if not msi_path.is_file():
            raise SystemExit(f"error: Windows Installer build produced no MSI at {msi_path}")
        release_evidence.retain_packaging_inputs(evidence, {
            "hub_msi.py": script_dir / "hub_msi.py",
            "windows_installer.py": script_dir / "windows_installer.py",
            "msi-tables.json": packaging_inputs / "msi-tables.json",
            "license.rtf": eula_rtf})

        signed = False
        signing = sign_command(msi_path)
        if signing is not None:
            sign_result = run(signing)
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
            "evidence": str(output_dir / evidence.name),
            "fileCount": len(files),
            "version": args.version,
            "msiVersion": product_version,
            "buildId": args.build_id,
            "signed": signed,
        }

        if stub_exe is not None:
            exe_path = (
                staging / f"HarkenStudioHubSetup-{args.version}-{args.build_id}.exe"
            )
            embed(stub_exe, msi_path, exe_path)
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

        artifacts = [msi_path]
        if stub_exe is not None:
            artifacts.append(exe_path)
        records = release_evidence.finalize(evidence, artifacts, input_dir,
                                            intermediate_artifacts=(msi_path,) if stub_exe is not None else ())
        summary.update(sha256=records[0]["sha256"], size=records[0]["size"])
        if stub_exe is not None:
            summary.update(setupExeSha256=records[1]["sha256"], setupExeSize=records[1]["size"])
    return summary


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the Harken Studio Hub MSI and setup EXE with Windows Installer.")
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--eula-rtf", default=None)
    parser.add_argument("--stub-exe", default=None)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    summary = build_installer(parse_args(argv))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
