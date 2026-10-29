"""Deploy Studio's macOS dependencies and record their build-to-bundle hashes."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import release_evidence as evidence


def deploy(build_dir: Path, bundle: Path, tool: Path, configuration: str) -> None:
    receipt = evidence.macos_deployment_path(build_dir, "studio", configuration)
    receipt.unlink(missing_ok=True)
    model = evidence.build_model(build_dir, "studio", configuration)
    binary = bundle / "Contents/MacOS/HarkenStudio"
    if (bundle.name != "HarkenStudio.app" or not bundle.is_relative_to(build_dir)
            or not model["identity"]["platform"].startswith("macos-")
            or not any(artifact["target"] == "harken_editor"
                       and build_dir / artifact["path"] == binary
                       for artifact in model["artifacts"])):
        raise ValueError("deployment bundle does not match the configured Studio target")
    frameworks = bundle / "Contents/Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    libraries = []
    executables = []
    names = set()
    for artifact in model["artifacts"]:
        if (artifact["target"] == "harkenizer" and artifact["type"] == "EXECUTABLE"
                and artifact["name"] == "Harkenizer"):
            destination = bundle / "Contents/MacOS/Harkenizer"
            records = executables
        elif artifact["type"] == "SHARED_LIBRARY" and artifact["name"].endswith(".dylib"):
            destination = frameworks / artifact["name"]
            records = libraries
        else:
            continue
        if artifact["name"] in names:
            raise ValueError("macOS deployment has duplicate binary names")
        names.add(artifact["name"])
        source = build_dir / artifact["path"]
        if destination.is_symlink() or not source.resolve().is_relative_to(build_dir):
            raise ValueError("macOS deployment binary escapes its build directory")
        if records is executables:
            shutil.copy2(source, destination)
        else:
            shutil.copyfile(source, destination)
        if evidence.sha256(destination) != artifact["sha256"]:
            raise ValueError("macOS deployment source changed while staging")
        records.append({"sourceArtifact": artifact,
                        "path": destination.relative_to(bundle.parent).as_posix()})
    if len(executables) != 1:
        raise ValueError("macOS deployment requires the built Harkenizer executable")
    deployment_tool = {"name": tool.name, "sha256": evidence.sha256(tool)}
    subprocess.run([str(tool), str(bundle), "-always-overwrite",
                    f"-executable={bundle / 'Contents/MacOS/Harkenizer'}"], check=True)
    for record in libraries + executables:
        artifact = record["sourceArtifact"]
        destination = bundle.parent / record["path"]
        if (evidence.sha256(build_dir / artifact["path"]) != artifact["sha256"]
                or destination.is_symlink() or not destination.is_file()):
            raise ValueError("macOS deployment binary changed outside the bundle")
        record["sha256"] = evidence.sha256(destination)
    evidence.write_json(receipt, {"schemaVersion": 1, "product": "studio",
                                "configuration": configuration, "identity": model["identity"],
                                "tool": deployment_tool, "libraries": libraries,
                                "executables": executables})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--configuration", required=True)
    args = parser.parse_args()
    deploy(args.build_dir.resolve(), args.bundle.resolve(), args.tool.resolve(), args.configuration)


if __name__ == "__main__":
    main()
