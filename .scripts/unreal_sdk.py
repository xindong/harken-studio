#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from release.collect_legal_texts import verify_sdk_plugin

REPOSITORY = Path(__file__).resolve().parents[1]
PLUGIN = REPOSITORY / "HarkenSDK" / "Unreal" / "Harken"
CORE_MODULE = PLUGIN / "Source" / "HarkenCore"
HEADER = REPOSITORY / "HarkenEngine" / "Source" / "Runtime" / "api" / "include" / "harken" / "Harken.h"


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=REPOSITORY, check=True)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file() or source.stat().st_size == 0:
        raise FileNotFoundError(f"Required artifact is missing or empty: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def build_windows_core(configuration: str) -> tuple[Path, Path]:
    build_directory = REPOSITORY / "build-unreal-sdk"
    run([
        "cmake", "--fresh", "--preset", "windows-release", "-B", str(build_directory),
        "-DHARKEN_BUILD_CORE=ON",
        "-DHARKEN_BUILD_SHARED=ON",
    ])
    run([
        "cmake", "--build", str(build_directory), "--config", configuration,
        "--target", "harken_core_shared", "--parallel",
    ])
    return (
        build_directory / "bin" / configuration / "HarkenCore.dll",
        build_directory / "lib" / configuration / "HarkenCoreImport.lib",
    )


def stage_header() -> None:
    for header in sorted(HEADER.parent.rglob("*")):
        if header.is_file():
            copy_file(header, CORE_MODULE / "Include" / "harken" / header.relative_to(HEADER.parent))


def stage_platform(platform: str, runtime: Path, import_library: Path | None) -> None:
    stage_header()
    if platform == "Win64":
        if import_library is None:
            raise ValueError("Win64 staging requires --import-library")
        copy_file(runtime, PLUGIN / "Binaries" / "ThirdParty" / "HarkenCore" / "Win64" / "HarkenCore.dll")
        copy_file(import_library, CORE_MODULE / "Lib" / "Win64" / "HarkenCoreImport.lib")
    elif platform == "Mac":
        copy_file(runtime, PLUGIN / "Binaries" / "ThirdParty" / "HarkenCore" / "Mac" / "libHarkenCore.dylib")
    elif platform == "Linux":
        copy_file(runtime, PLUGIN / "Binaries" / "ThirdParty" / "HarkenCore" / "Linux" / "x86_64" / "libHarkenCore.so")
    elif platform == "Android":
        copy_file(runtime, CORE_MODULE / "Lib" / "Android" / "arm64-v8a" / "libHarkenCore.so")
    elif platform == "IOS":
        copy_file(runtime, CORE_MODULE / "Lib" / "IOS" / "libHarkenCore.a")
    else:
        raise ValueError(f"Unsupported Unreal platform: {platform}")


def build_plugin(engine_root: Path, package_directory: Path, platform: str,
                 no_host_platform: bool) -> None:
    run_uat = engine_root / "Engine" / "Build" / "BatchFiles" / ("RunUAT.bat" if sys.platform == "win32" else "RunUAT.sh")
    if not run_uat.is_file():
        raise FileNotFoundError(f"RunUAT was not found: {run_uat}")
    if package_directory.exists():
        shutil.rmtree(package_directory)
    package_directory.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(run_uat), "BuildPlugin",
        f"-Plugin={PLUGIN / 'Harken.uplugin'}",
        f"-Package={package_directory}",
        f"-TargetPlatforms={platform}",
        "-Rocket",
    ]
    if no_host_platform:
        command.append("-NoHostPlatform")
    run(command)
    verify_sdk_plugin("Unreal", package_directory)


def run_automation_tests(engine_root: Path, packaged_plugin: Path) -> None:
    host = REPOSITORY / ".temp" / "unreal-sdk-smoke"
    if host.exists():
        shutil.rmtree(host)
    project_plugins = host / "Plugins" / "Harken"
    project_plugins.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(packaged_plugin, project_plugins)
    project = host / "HarkenSdkSmoke.uproject"
    project.write_text(json.dumps({
        "FileVersion": 3,
        "EngineAssociation": "5.8",
        "Category": "",
        "Description": "Harken SDK smoke test",
        "Plugins": [{"Name": "Harken", "Enabled": True}],
    }, indent=2), encoding="utf-8")
    config = host / "Config" / "DefaultGame.ini"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text(
        "[/Script/HarkenRuntime.HarkenSettings]\n"
        "bAutoInitialize=False\n",
        encoding="utf-8")
    editor = engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
    if not editor.is_file():
        raise FileNotFoundError(f"UnrealEditor-Cmd was not found: {editor}")
    report_directory = host / "Saved" / "AutomationReport"
    run([
        str(editor), str(project),
        "-unattended", "-nop4", "-NullRHI", "-nosplash",
        "-ExecCmds=Automation RunTests Harken;Quit",
        "-TestExit=Automation Test Queue Empty",
        f"-ReportExportPath={report_directory}",
    ])
    report_file = report_directory / "index.json"
    if not report_file.is_file():
        raise FileNotFoundError(f"Unreal automation report was not produced: {report_file}")
    report = json.loads(report_file.read_text(encoding="utf-8-sig"))
    summary = {key.casefold(): value for key, value in report.items()}
    expected = {
        "Harken.Coordinates.Direction",
        "Harken.Coordinates.Position",
        "Harken.Runtime.Lifecycle",
        "Harken.Runtime.SessionHandleGuard",
        "Harken.Runtime.SettingsDefaults",
    }
    succeeded_paths = {
        test.get("fullTestPath")
        for test in report.get("tests", [])
        if test.get("state") == "Success"
    }
    warnings = int(summary.get("succeededwithwarnings", 0))
    failed = int(summary.get("failed", 0))
    not_run = int(summary.get("notrun", 0))
    in_process = int(summary.get("inprocess", 0))
    missing = sorted(expected - succeeded_paths)
    if missing or warnings != 0 or failed != 0 or not_run != 0 or in_process != 0:
        raise RuntimeError(
            "Unexpected Unreal automation result: "
            f"missing={missing}, warnings={warnings}, failed={failed}, "
            f"not_run={not_run}, in_process={in_process}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=["Win64", "Mac", "Linux", "Android", "IOS"], required=True)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--import-library", type=Path)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--build-core", action="store_true")
    parser.add_argument("--build-plugin", action="store_true")
    parser.add_argument("--run-tests", action="store_true")
    parser.add_argument("--no-host-platform", action="store_true")
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--package-directory", type=Path,
                        default=REPOSITORY / ".temp" / "unreal-sdk-package" / "Harken")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    runtime = arguments.runtime
    import_library = arguments.import_library
    if arguments.build_core:
        if arguments.platform != "Win64":
            raise ValueError("--build-core currently builds the local Win64 runtime only")
        runtime, import_library = build_windows_core(arguments.configuration)
    if runtime is None:
        raise ValueError("Provide --runtime or use --build-core")
    stage_platform(arguments.platform, runtime.resolve(),
                   import_library.resolve() if import_library else None)
    if arguments.build_plugin or arguments.run_tests:
        if arguments.engine_root is None:
            raise ValueError("--engine-root is required to build or test the plugin")
        build_plugin(arguments.engine_root.resolve(), arguments.package_directory.resolve(), arguments.platform,
                     arguments.no_host_platform)
    if arguments.run_tests:
        if arguments.no_host_platform:
            raise ValueError("--run-tests requires the host editor build")
        if arguments.platform != "Win64":
            raise ValueError("Local automation tests are currently available for Win64")
        run_automation_tests(arguments.engine_root.resolve(), arguments.package_directory.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
