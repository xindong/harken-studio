from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


FIXTURES = (
    ("golden", "HARKEN_PACK_PAIR_GOLDEN_OUT", "EditorProjectPackExporterManifest.ProductionPackPairGoldenIsRepeatable"),
    ("six-containers", "HARKEN_UNITY_FIXTURE_OUT", "EditorProjectPackExporterManifest.ExportsPlayableDemoSampleFixtures"),
)
REGRESSIONS = ":".join((
    "EditorProjectPackExporterManifest.*",
    "ProjectPlatformBuild.*",
    "SoundSourceSemanticsTest.*",
    "AudioFormatDecodeTest.*",
    "AuthoredPackLifecycle.ExportIsDeterministic",
    "ExportedPackReadOnlyLifecycle.*",
))


def sha256(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def inventory(root: Path) -> dict:
    if not root.is_dir():
        raise ValueError(f"Artifact directory does not exist: {root}")
    return {
        path.relative_to(root).as_posix(): {"size": path.stat().st_size, "sha256": sha256(path)}
        for path in sorted(root.rglob("*")) if path.is_file()
    }


def publication_errors(root: Path) -> list[str]:
    if not root.is_dir():
        return [f"Fixture directory does not exist: {root}"]
    packs = sorted(root.rglob("*.bytes"))
    if not packs:
        return [f"Fixture has no published packs: {root}"]
    errors = []
    for pack in packs:
        if not pack.is_file() or pack.stat().st_size == 0:
            errors.append(f"Published pack is not a nonempty file: {pack}")
        companion = pack.with_suffix(".json")
        try:
            data = json.loads(companion.read_text(encoding="utf-8"))
            if not isinstance(data, dict) or not data:
                errors.append(f"Pack companion is not a nonempty JSON object: {companion}")
        except (OSError, UnicodeError, ValueError) as error:
            errors.append(f"Cannot read pack companion {companion}: {error}")
    return errors


def artifact_errors(root: Path) -> list[str]:
    return [
        error
        for relative in ("golden/first", "golden/second", "six-containers")
        for error in publication_errors(root / relative)
    ]


def differences(reference: Path, candidate: Path) -> dict:
    try:
        previous = inventory(reference)
        current = inventory(candidate)
    except (OSError, ValueError) as error:
        return {"equal": False, "errors": [str(error)]}
    if not previous or not current:
        return {"equal": False, "errors": ["Cannot compare empty artifact directories"]}
    missing = sorted(previous.keys() - current.keys())
    extra = sorted(current.keys() - previous.keys())
    changed = []
    for name in sorted(previous.keys() & current.keys()):
        left = (reference / name).read_bytes()
        right = (candidate / name).read_bytes()
        if left != right:
            first_difference = next((index for index, (a, b) in enumerate(zip(left, right)) if a != b), min(len(left), len(right)))
            changed.append({"path": name, "first_different_offset": first_difference, "reference": previous[name], "candidate": current[name]})
    return {"equal": not missing and not extra and not changed, "file_count": len(current), "missing": missing, "extra": extra, "changed": changed}


def run_test(executable: Path, output: Path, name: str, test_filter: str, overrides: dict) -> dict:
    environment = os.environ.copy()
    environment.pop("HARKEN_PACK_PAIR_GOLDEN_OUT", None)
    environment.pop("HARKEN_UNITY_FIXTURE_OUT", None)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment.update(overrides)
    command = [str(executable), f"--gtest_filter={test_filter}", f"--gtest_output=json:{output / (name + '.json')}"]
    start = time.perf_counter()
    with (output / (name + ".log")).open("wb") as log:
        completed = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
    result = {"name": name, "command": command, "exit_code": completed.returncode, "elapsed_seconds": time.perf_counter() - start}
    report = output / (name + ".json")
    if report.is_file():
        data = json.loads(report.read_text(encoding="utf-8"))
        result.update({key: data[key] for key in ("tests", "failures", "disabled", "errors", "time") if key in data})
    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor-tests", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--regressions", action="store_true")
    args = parser.parse_args()
    executable = args.editor_tests.resolve()
    output = args.output.resolve()
    reference = args.reference.resolve() if args.reference else None
    if reference:
        if output == reference or reference in output.parents or output in reference.parents:
            parser.error("Reference and output directories must not overlap")
        errors = artifact_errors(reference / "artifacts")
        if errors:
            parser.error("Invalid reference artifacts: " + "; ".join(errors))
    if output.exists():
        parser.error(f"Output must not already exist: {output}")
    suffix = ".exe" if executable.suffix.lower() == ".exe" else ""
    binaries = [executable, *(executable.parent / (name + suffix) for name in ("packc", "harken_host"))]
    for binary in binaries:
        if not binary.is_file():
            parser.error(f"Required executable does not exist: {binary}")
    for name in ("HarkenStudioRuntimeTest.dll", "libHarkenStudioRuntimeTest.so", "libHarkenStudioRuntimeTest.dylib"):
        runtime = executable.parent / name
        if runtime.is_file():
            binaries.append(runtime)
    output.mkdir(parents=True)
    artifacts = output / "artifacts"
    artifacts.mkdir()
    report = {"schema": "pack-build-byte-equivalence-v1", "binaries": {binary.name: sha256(binary) for binary in binaries}, "runs": []}
    for name, variable, test_filter in FIXTURES:
        report["runs"].append(run_test(executable, output, name, test_filter, {variable: str(artifacts / name)}))
    if args.regressions:
        report["runs"].append(run_test(executable, output, "regressions", REGRESSIONS, {}))
    report["artifact_errors"] = artifact_errors(artifacts)
    report["inventory"] = inventory(artifacts)
    report["fixture_repeatability"] = differences(artifacts / "golden" / "first", artifacts / "golden" / "second")
    if reference:
        report["reference"] = str(reference)
        report["comparison"] = differences(reference / "artifacts", artifacts)
    report["passed"] = all(item["exit_code"] == 0 and item.get("tests", 0) > 0 and item.get("failures", 0) == 0 and item.get("errors", 0) == 0 for item in report["runs"]) and not report["artifact_errors"] and report["fixture_repeatability"]["equal"] and report.get("comparison", {"equal": True})["equal"]
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "files": len(report["inventory"]), "comparison": report.get("comparison"), "report": str(output / "report.json")}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
