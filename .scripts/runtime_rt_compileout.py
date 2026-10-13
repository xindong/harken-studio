#!/usr/bin/env python3
"""Verify that private Runtime qualification probes compile out of shipping."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


class CompileoutError(RuntimeError):
    pass


def find_dumpbin() -> Path:
    roots = [
        Path(r"C:\Program Files\Microsoft Visual Studio\2022"),
        Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022"),
    ]
    candidates: list[Path] = []
    for root in roots:
        if root.exists():
            candidates.extend(
                root.glob(
                    r"*\VC\Tools\MSVC\*\bin\HostX64\x64\dumpbin.exe"
                )
            )
    if not candidates:
        raise CompileoutError("dumpbin.exe was not found")

    def version(path: Path) -> tuple[int, ...]:
        try:
            text = path.parents[3].name
            return tuple(int(part) for part in text.split("."))
        except (ValueError, IndexError):
            return ()

    return max(candidates, key=version)


def run_dumpbin(dumpbin: Path, argument: str, target: Path) -> str:
    if not target.is_file():
        raise CompileoutError(f"required artifact is missing: {target}")
    result = subprocess.run(
        [str(dumpbin), argument, str(target)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise CompileoutError(
            f"dumpbin failed for {target}: {result.stderr.strip()}"
        )
    return result.stdout


def git_identity(repo: Path) -> dict:
    commit = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout.strip()
    status = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "status",
            "--porcelain=v1",
            "--untracked-files=no",
        ],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout.splitlines()
    if status:
        raise CompileoutError(
            "compile-out evidence requires a clean tracked worktree"
        )
    return {"commit": commit, "trackedWorktreeClean": True}


def normalize_disassembly(text: str) -> str:
    lines = text.replace("\r\n", "\n").splitlines()
    start = next(
        (index + 1 for index, line in enumerate(lines)
         if line.startswith("File Type:")),
        None,
    )
    if start is None:
        raise CompileoutError("dumpbin output has no object file marker")
    body: list[str] = []
    for line in lines[start:]:
        if line.strip() == "Summary":
            break
        body.append(line.rstrip())
    return "\n".join(body).strip()


def require_probe_absent(label: str, text: str) -> None:
    if re.search(r"rtprobe|HARKEN_RT_PROBE_", text, re.IGNORECASE):
        raise CompileoutError(f"{label} contains private probe symbols")


def require_probe_present(label: str, text: str) -> None:
    if not re.search(r"rtprobe", text, re.IGNORECASE):
        raise CompileoutError(f"{label} contains no probe symbols")


def object_path(build: Path, target: str, source: str) -> Path:
    return build / f"{target}.dir" / "Release" / f"{source}.obj"


def project_text(build: Path, target: str) -> str:
    path = build / f"{target}.vcxproj"
    if not path.is_file():
        raise CompileoutError(f"required project is missing: {path}")
    return path.read_text(encoding="utf-8-sig", errors="replace")


def validate(off_build: Path, all_build: Path) -> dict:
    repo = Path(__file__).resolve().parents[1]
    dumpbin = find_dumpbin()
    off_qualification = (
        off_build / "lib" / "Release" /
        "harken_core_rt_qualification.lib"
    )
    all_qualification = (
        all_build / "lib" / "Release" /
        "harken_core_rt_qualification.lib"
    )
    all_shipping = all_build / "lib" / "Release" / "HarkenCore.lib"
    all_shared = all_build / "bin" / "Release" / "HarkenCore.dll"

    require_probe_absent(
        "all-Off qualification library",
        run_dumpbin(dumpbin, "/symbols", off_qualification),
    )
    require_probe_present(
        "all-On qualification library",
        run_dumpbin(dumpbin, "/symbols", all_qualification),
    )
    require_probe_absent(
        "all-On shipping static library",
        run_dumpbin(dumpbin, "/symbols", all_shipping),
    )
    require_probe_absent(
        "all-On shipping shared exports",
        run_dumpbin(dumpbin, "/exports", all_shared),
    )

    all_shipping_project = project_text(all_build, "harken_core")
    all_shared_project = project_text(all_build, "harken_core_shared")
    all_qualification_project = project_text(
        all_build, "harken_core_rt_qualification"
    )
    off_qualification_project = project_text(
        off_build, "harken_core_rt_qualification"
    )
    require_probe_absent("all-On shipping static project",
                         all_shipping_project)
    require_probe_absent("all-On shipping shared project",
                         all_shared_project)
    require_probe_present("all-On qualification project",
                          all_qualification_project)
    require_probe_absent("all-Off qualification project",
                         off_qualification_project)

    compared: list[str] = []
    for source in ("DeviceBackend", "MixerLoop", "RenderWorkerPool"):
        shipping_off = normalize_disassembly(
            run_dumpbin(
                dumpbin,
                "/disasm:nobytes",
                object_path(off_build, "harken_core", source),
            )
        )
        qualification_off = normalize_disassembly(
            run_dumpbin(
                dumpbin,
                "/disasm:nobytes",
                object_path(
                    off_build,
                    "harken_core_rt_qualification",
                    source,
                ),
            )
        )
        shipping_all = normalize_disassembly(
            run_dumpbin(
                dumpbin,
                "/disasm:nobytes",
                object_path(all_build, "harken_core", source),
            )
        )
        if shipping_off != qualification_off:
            raise CompileoutError(
                f"{source} all-Off qualification code differs from shipping"
            )
        if shipping_off != shipping_all:
            raise CompileoutError(
                f"{source} shipping code changes in the all-On build tree"
            )
        compared.append(source)

    return {
        "schemaVersion": 1,
        "status": "passed",
        "git": git_identity(repo),
        "claim": (
            "private qualification probes are absent from shipping targets "
            "and all-Off qualification code is instruction-identical"
        ),
        "dumpbin": str(dumpbin),
        "offBuild": str(off_build.resolve()),
        "allBuild": str(all_build.resolve()),
        "instructionComparedSources": compared,
        "allOffQualificationProbeSymbols": 0,
        "allOnShippingProbeSymbols": 0,
        "allOnSharedProbeExports": 0,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--off-build", type=Path, required=True)
    parser.add_argument("--all-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = validate(args.off_build, args.all_build)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except CompileoutError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
