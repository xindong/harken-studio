import json
import shutil
from pathlib import Path

from scenarios.authoring import inspector_fields, write_tone

LANGUAGES = ("English", "Japanese")
PLATFORMS = ("Android_Low", "Android_High")
VARIABLE = "Speed"
ACTIONS = {"tone": "PlayTone", "short": "PlayShort", "spatial": "PlaySpatial", "flat": "PlayFlat",
           "localized": "PlayVoice"}
RENAMED = "PlayToneRenamed"


def find_key(document, key: str):
    if isinstance(document, dict):
        if key in document:
            return document[key]
        values = document.values()
    elif isinstance(document, list):
        values = document
    else:
        return None
    for value in values:
        found = find_key(value, key)
        if found is not None:
            return found
    return None


def _create(studio, project, parent, name, kind) -> str:
    arguments = {"name": name, "type": kind}
    if parent:
        arguments["parent"] = parent
    return studio.call(project, "create_item", arguments, save=True)["path"]


def _sound(studio, project, layer, name, tone) -> str:
    path = _create(studio, project, layer, name, "Sound")
    studio.call(project, "import_audio", {"path": str(tone), "pack": "Content", "target_parent_path": path}, save=True)
    return path


def _action(studio, project, name, target) -> None:
    studio.run(project, [
        {"cmd": "create_action", "args": {"name": name}},
        {"cmd": "set_item_property", "args": {"path": name, "property": "steps",
                                              "value": [{"type": "Play", "targetName": target}]}},
    ], save=True)


def _platform_id(studio, project, name: str) -> str:
    for platform in studio.call(project, "list_project_platforms")["platforms"]:
        if platform["name"] == name:
            return platform["id"]
    raise AssertionError(f"project has no platform named {name}")


def _export(studio, project, platform_id: str, output: Path) -> Path:
    result = studio.call(project, "export_pack", {"output_path": str(output), "platform_id": platform_id,
                                                  "all": True, "force": True})
    if not result.get("success"):
        raise AssertionError(f"export failed: {json.dumps(result)[:2000]}")
    directories = {path.parent for path in output.rglob("Init.bytes")}
    if len(directories) != 1:
        raise AssertionError(f"expected one exported platform directory, found {sorted(map(str, directories))}")
    return directories.pop()


def build(studio, work: Path) -> dict:
    """Author two projects through Studio's shared commands and export every directory the Integration tests use."""
    tones = {name: write_tone(work / f"{name}.wav", hz, seconds)
             for name, hz, seconds in (("tone", 440, 4.0), ("short", 550, 0.3), ("spatial", 660, 4.0),
                                       ("english", 330, 4.0), ("japanese", 770, 4.0))}
    project = studio.create_project(work / "Game")
    sound_fields = inspector_fields(studio, project, "Sound")
    mix = sound_fields["spatializationMix"]
    layer = _create(studio, project, None, "Content", "Layer")

    _sound(studio, project, layer, "Tone", tones["tone"])
    _sound(studio, project, layer, "Short", tones["short"])
    for name, value in (("Spatial", mix["max"]), ("Flat", mix["min"])):
        path = _sound(studio, project, layer, name, tones["spatial"])
        studio.call(project, "set_item_property", {"path": path, "property": "spatializationMix", "value": value},
                    save=True)
    for language in LANGUAGES:
        studio.call(project, "add_project_language", {"name": language}, save=True)
    voice = _create(studio, project, layer, "Voice", "Sound")
    studio.call(project, "set_item_property", {"path": voice, "property": "distinguishLanguage", "value": True},
                save=True)
    for language in LANGUAGES:
        studio.call(project, "import_audio", {"path": str(tones[language.lower()]), "pack": "Content",
                                              "target_parent_path": voice}, save=True)
    sources = find_key(studio.call(project, "get_item_info", {"path": voice}), "sources")
    imported = sources["localized"][LANGUAGES[0]]["entries"]
    sources["localized"] = {language: {"active": entry["entryId"], "entries": [entry], "platforms": {}}
                            for language, entry in zip(LANGUAGES, imported)}
    studio.call(project, "set_item_property", {"path": voice, "property": "sources", "value": sources}, save=True)
    for key, target in (("tone", "Tone"), ("short", "Short"), ("spatial", "Spatial"), ("flat", "Flat"),
                        ("localized", "Voice")):
        _action(studio, project, ACTIONS[key], target)
    _create(studio, project, None, VARIABLE, "Variable")
    for setting in ("set_build_project_data_export", "set_build_pack_metadata_export"):
        studio.call(project, setting, {"enabled": True}, save=True)
    for platform in PLATFORMS:
        studio.call(project, "create_project_platform", {"name": platform, "base_platform": "Android"}, save=True)

    default_platform = studio.call(project, "list_project_platforms")["platforms"][0]["id"]
    main = _export(studio, project, default_platform, work / "Export" / "Main")
    platforms_root = work / "Export" / "Platforms"
    for platform in PLATFORMS:
        exported = _export(studio, project, _platform_id(studio, project, platform), work / "Export" / platform)
        shutil.copytree(exported, platforms_root / platform)

    tone_metadata = json.loads((main / f"{ACTIONS['tone']}.json").read_text(encoding="utf-8"))
    studio.call(project, "rename_item", {"path": ACTIONS["tone"], "new_name": RENAMED}, save=True)
    renamed = _export(studio, project, default_platform, work / "Export" / "Renamed")

    other = studio.create_project(work / "OtherGame")
    other_layer = _create(studio, other, None, "Content", "Layer")
    _sound(studio, other, other_layer, "Tone", tones["tone"])
    _action(studio, other, ACTIONS["tone"], "Tone")
    other_main = _export(studio, other, studio.call(other, "list_project_platforms")["platforms"][0]["id"],
                         work / "Export" / "Other")

    tone_pack_stable_id = find_key(tone_metadata, "packStableId")
    if not tone_pack_stable_id:
        raise AssertionError(f"{ACTIONS['tone']}.json has no packStableId")
    project_data = list((work / "Export" / "Main").rglob("Project.json"))
    if len(project_data) != 1:
        raise AssertionError(f"expected one Project.json, found {sorted(map(str, project_data))}")
    project_id = json.loads(project_data[0].read_text(encoding="utf-8")).get("projectGuid")
    if not project_id:
        raise AssertionError(f"{project_data[0]} has no projectGuid")
    return {
        "main": main, "renamed": renamed, "other": other_main, "platforms_root": platforms_root,
        "project_id": project_id,
        "tone_action_id": tone_metadata["guid"],
        "tone_pack_stable_id": tone_pack_stable_id,
    }
