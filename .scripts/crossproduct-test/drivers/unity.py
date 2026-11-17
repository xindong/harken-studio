import json
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

from drivers.process import run_bounded

TEST_FRAMEWORK = "1.1.33"


def create_project(root: Path, package: Path, test_assets: Path) -> Path:
    (root / "Assets").mkdir(parents=True)
    (root / "Packages").mkdir()
    manifest = {"dependencies": {"com.harken.core": "file:" + package.resolve().as_posix(),
                                 "com.unity.test-framework": TEST_FRAMEWORK}}
    (root / "Packages" / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    shutil.copytree(test_assets, root / "Assets" / "HarkenCrossProductTests")
    return root


def streaming_root(project: Path) -> Path:
    return project / "Assets" / "StreamingAssets" / "HarkenCrossProduct"


def run_tests(editor: Path, project: Path, platform: str, work: Path, timeout: float) -> list[dict]:
    results = work / f"unity-{platform}.xml"
    code = run_bounded([str(editor), "-batchmode", "-nographics", "-projectPath", str(project), "-runTests",
                        "-testPlatform", platform, "-testResults", str(results), "-logFile", str(work / f"unity-{platform}.log")],
                       timeout=timeout, log=work / f"unity-{platform}.process.log")
    if not results.is_file():
        raise AssertionError(f"Unity {platform} run exited {code} without results; see {work / f'unity-{platform}.log'}")
    cases = []
    for case in ET.parse(results).getroot().iter("test-case"):
        failure = case.find("failure/message")
        cases.append({"name": case.get("fullname"), "result": case.get("result"),
                      "message": (failure.text or "").strip() if failure is not None else ""})
    if not cases:
        raise AssertionError(f"Unity {platform} run found no tests; see {work / f'unity-{platform}.log'}")
    return cases
