#!/usr/bin/env python3
"""Build the Harken Hub MSI installer (WiX v4+) and, when a setup stub is
supplied, the self-contained HarkenHubSetup EXE from a deployed hub
directory."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from ctypes import wintypes
from pathlib import Path
from xml.sax.saxutils import quoteattr

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import collect_legal_texts, product_legal, release_evidence

HUB_BINARY = "HarkenHub.exe"
BRIDGE_BINARY = "Updater/HarkenHubUpdateBridge.exe"
FOREIGN_BINARIES = (
    "HarkenStudio.exe",
    "HarkenEditor.exe",
    "HarkenHubSetup.exe",
)
PAYLOAD_RESOURCE_NAME = "HARKENHUBMSI"
RT_RCDATA = 10
HUB_EXE_FILE_ID = "HarkenHubExe"
COMPONENT_NAMESPACE = uuid.UUID("9b1a7f66-52a4-4f6e-9e01-0f30c2a7f4b3")
HANDOFF_SDDL = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OIIO;FA;;;CO)(A;;0x1200ab;;;BU)"
UPDATER_SDDL = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"
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
    if HUB_BINARY not in rel_names:
        raise SystemExit(
            f"error: input directory does not contain {HUB_BINARY} at "
            f"its root: {input_dir}"
        )
    if BRIDGE_BINARY not in rel_names:
        raise SystemExit(
            f"error: input directory does not contain {BRIDGE_BINARY}: "
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


def stable_id(prefix: str, rel_path: str) -> str:
    return f"{prefix}_{uuid.uuid5(COMPONENT_NAMESPACE, rel_path).hex}"


def component_guid(rel_path: str) -> str:
    return str(
        uuid.uuid5(COMPONENT_NAMESPACE, f"component/{rel_path.lower()}")
    ).upper()


def directory_id(rel_dir: str) -> str:
    if rel_dir == ".":
        return "INSTALLFOLDER"
    return stable_id("PayloadDir", f"dir/{rel_dir.lower()}")


def generate_payload_wxs(files: list[Path], input_dir: Path) -> str:
    rel_dirs: set[str] = set()
    for path in files:
        rel = path.relative_to(input_dir)
        parent = rel.parent.as_posix()
        while parent != ".":
            rel_dirs.add(parent)
            parent = str(Path(parent).parent.as_posix())

    tree: dict[str, dict] = {}
    for rel_dir in sorted(rel_dirs):
        node = tree
        for part in rel_dir.split("/"):
            node = node.setdefault(part, {})

    lines: list[str] = []
    lines.append('<?xml version="1.0" encoding="utf-8"?>')
    lines.append('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
    lines.append("  <Fragment>")
    lines.append('    <DirectoryRef Id="INSTALLFOLDER">')

    def emit_directories(node: dict, prefix: str, indent: str) -> None:
        for name in sorted(node):
            rel_dir = name if not prefix else f"{prefix}/{name}"
            dir_id = directory_id(rel_dir)
            children = node[name]
            if children:
                lines.append(
                    f"{indent}<Directory Id={quoteattr(dir_id)} "
                    f"Name={quoteattr(name)}>"
                )
                emit_directories(children, rel_dir, indent + "  ")
                lines.append(f"{indent}</Directory>")
            else:
                lines.append(
                    f"{indent}<Directory Id={quoteattr(dir_id)} "
                    f"Name={quoteattr(name)} />"
                )

    emit_directories(tree, "", "      ")
    lines.append("    </DirectoryRef>")
    lines.append("  </Fragment>")
    lines.append("  <Fragment>")
    lines.append('    <ComponentGroup Id="HubPayload">')
    for path in files:
        rel = path.relative_to(input_dir)
        rel_path = rel.as_posix()
        dir_id = directory_id(rel.parent.as_posix())
        file_id = (
            HUB_EXE_FILE_ID
            if rel_path == HUB_BINARY
            else stable_id("PayloadFile", f"file/{rel_path.lower()}")
        )
        lines.append(
            f"      <Component Id={quoteattr(stable_id('PayloadCmp', f'cmp/{rel_path.lower()}'))} "
            f"Directory={quoteattr(dir_id)} "
            f"Guid={quoteattr(component_guid(rel_path))}>"
        )
        lines.append(
            f"        <File Id={quoteattr(file_id)} "
            f"Source={quoteattr(str(path.resolve()))} KeyPath=\"yes\" />"
        )
        lines.append("      </Component>")
    lines.append("    </ComponentGroup>")
    lines.append("  </Fragment>")
    lines.append("</Wix>")
    return "\n".join(lines) + "\n"


def wix_command(
    product_wxs: Path,
    payload_wxs: Path,
    eula_rtf: Path,
    icon_ico: Path,
    version: str,
    msi_path: Path,
    wix_exe: str,
) -> list[str]:
    return [
        wix_exe,
        "build",
        "-arch",
        "x64",
        "-culture",
        "en-US",
        "-ext",
        "WixToolset.UI.wixext",
        "-d",
        f"HarkenVersion={version}",
        "-d",
        f"HarkenEulaRtf={eula_rtf}",
        "-d",
        f"HarkenIconIco={icon_ico}",
        "-d",
        f"HarkenHandoffSddl={HANDOFF_SDDL}",
        "-d",
        f"HarkenUpdaterSddl={UPDATER_SDDL}",
        "-o",
        str(msi_path),
        str(product_wxs),
        str(payload_wxs),
    ]


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
    args: argparse.Namespace, run=subprocess.run, embed=embed_payload
) -> dict:
    input_dir = Path(args.input_dir).resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"error: input directory not found: {input_dir}")

    script_dir = Path(__file__).resolve().parent
    product_wxs = Path(args.product_wxs or script_dir / "wix" / "HarkenHub.wxs")
    eula_rtf = Path(args.eula_rtf).resolve() if args.eula_rtf else None
    icon_ico = Path(
        args.icon_ico
        or script_dir.parents[1]
        / "HarkenHub" / "Resources" / "icons" / "HarkenHub.ico"
    )
    for required in (product_wxs, icon_ico) + ((eula_rtf,) if eula_rtf else ()):
        if not required.is_file():
            raise SystemExit(f"error: missing authoring input: {required}")

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
        payload_wxs = evidence / "packaging-inputs/payload.wxs"
        payload_wxs.parent.mkdir(parents=True, exist_ok=True)
        if eula_rtf is None:
            eula_rtf = payload_wxs.parent / "license.rtf"
            eula_rtf.write_bytes(product_legal.render_rtf(product_legal.installer_text()).encode("ascii"))
        payload_wxs.write_text(
            generate_payload_wxs(files, input_dir), encoding="utf-8"
        )
        release_evidence.retain_packaging_inputs(evidence, {
            "product.wxs": product_wxs, "payload.wxs": payload_wxs,
            "license.rtf": eula_rtf, "icon.ico": icon_ico})

        product_version = msi_version(args.version)
        msi_path = (
            staging / f"HarkenHubSetup-{args.version}-{args.build_id}.msi"
        )

        command = wix_command(
            product_wxs, payload_wxs, eula_rtf, icon_ico, product_version,
            msi_path, args.wix_exe,
        )
        result = run(command)
        if result.returncode != 0:
            raise SystemExit(f"error: wix build failed with exit code {result.returncode}")
        if not msi_path.is_file():
            raise SystemExit(f"error: wix build produced no MSI at {msi_path}")

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
                staging / f"HarkenHubSetup-{args.version}-{args.build_id}.exe"
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--product-wxs", default=None)
    parser.add_argument("--eula-rtf", default=None)
    parser.add_argument("--icon-ico", default=None)
    parser.add_argument("--wix-exe", default="wix")
    parser.add_argument("--stub-exe", default=None)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    summary = build_installer(parse_args(argv))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
