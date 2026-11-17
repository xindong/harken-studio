import json
import shutil
from pathlib import Path

from drivers import engine, unity
from scenarios import integration_content as content

HERE = Path(__file__).resolve().parents[1]


def meter_layout(install: Path) -> dict:
    abi = json.loads(engine.manifest_path(install).read_text(encoding="utf-8"))["contract"]["nativeAbi"]
    peaks = abi["types"]["VnPreviewChannelPeaksV1"]
    offsets = {field["name"]: field["offset"] for field in peaks["fields"]}
    return {"size": peaks["size"], "mixChannelCount": offsets["mixChannelCount"], "busCount": offsets["busCount"],
            "channelPeaks": offsets["channelPeaks"], "maxChannels": abi["defines"]["VN_MAX_CHANNELS"]["value"]}


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-development", "integration-package", "unity")
    exported = content.build(ctx.studio(), ctx.work)
    project = unity.create_project(ctx.work / "UnityGame", ctx.args.integration_package, HERE / "unity")
    root = unity.streaming_root(project)
    for name, source in (("Main", exported["main"]), ("Renamed", exported["renamed"]),
                         ("Other", exported["other"]), ("Platforms", exported["platforms_root"])):
        shutil.copytree(source, root / name)
    inputs = {
        "projectId": exported["project_id"],
        "main": "HarkenCrossProduct/Main",
        "renamed": "HarkenCrossProduct/Renamed",
        "otherProject": "HarkenCrossProduct/Other",
        "platformRoot": "HarkenCrossProduct/Platforms",
        "platforms": list(content.PLATFORMS),
        "languages": list(content.LANGUAGES),
        "toneAction": content.ACTIONS["tone"],
        "toneActionId": exported["tone_action_id"],
        "tonePackStableId": str(exported["tone_pack_stable_id"]),
        "shortAction": content.ACTIONS["short"],
        "spatialAction": content.ACTIONS["spatial"],
        "flatAction": content.ACTIONS["flat"],
        "localizedAction": content.ACTIONS["localized"],
        "renamedAction": content.RENAMED,
        "variable": content.VARIABLE,
        "meter": meter_layout(ctx.engine_install),
    }
    (root / "inputs.json").write_text(json.dumps(inputs, indent=2), encoding="utf-8")

    cases = []
    for platform in ("EditMode", "PlayMode"):
        cases += unity.run_tests(ctx.args.unity, project, platform, ctx.work, ctx.args.unity_timeout)
    ctx.details["unity_tests"] = {case["name"]: case["result"] for case in cases}
    failed = [f"{case['name']}: {case['message']}" for case in cases if case["result"] != "Passed"]
    if failed:
        raise AssertionError("Unity tests did not pass:\n" + "\n".join(failed))
