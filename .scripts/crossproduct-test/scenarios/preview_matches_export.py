import math
import time

from scenarios.authoring import export_all, exported_actions, write_tone

TONE_SECONDS = 1.5
POLL_SECONDS = 1.0
POLL_INTERVAL = 0.02
PLAY_BLOCKS = 300
TOLERANCE_DB = 0.5
CHILD_VOLUME_DB = -6.0
SOUND_VOLUME_DB = -3.0


def to_db(linear: float) -> float:
    return -96.0 if linear <= 0.0 else max(20.0 * math.log10(linear), -96.0)


def author(studio, project, work) -> dict:
    tone = write_tone(work / "tone.wav", 440, TONE_SECONDS)
    layer = studio.call(project, "create_item", {"name": "Mix", "type": "Layer"}, save=True)["path"]
    sound = studio.call(project, "create_item", {"parent": layer, "name": "Tone", "type": "Sound"}, save=True)
    studio.call(project, "import_audio", {"path": str(tone), "pack": "Mix", "target_parent_path": sound["path"]},
                save=True)
    studio.call(project, "create_bus", {"name": "Child"}, save=True)
    buses = studio.call(project, "list_buses")["buses"]
    child = next(bus for bus in buses if bus["name"] == "Child")
    studio.run(project, [
        {"cmd": "set_bus_volume_db", "args": {"bus_name": "Child", "db": CHILD_VOLUME_DB}},
        {"cmd": "set_item_property", "args": {"path": sound["path"], "property": "volume", "value": SOUND_VOLUME_DB}},
        {"cmd": "set_field_group_mode", "args": {"definition_guid": sound["guid"], "group": "output",
                                                 "mode": "Local", "scope": "Shared"}},
        {"cmd": "set_field_group_field", "args": {"definition_guid": sound["guid"], "group": "output",
                                                  "field": "outputBusRef", "value": child["bus_id"],
                                                  "scope": "Shared"}},
        {"cmd": "create_action", "args": {"name": "PlayTone"}},
        {"cmd": "set_item_property", "args": {"path": "PlayTone", "property": "steps",
                                              "value": [{"type": "Play", "targetName": "Tone"}]}},
    ], save=True)
    return {bus["name"]: (None if bus["parent_bus_ref"].strip("0-") == "" else bus["bus_id"]) for bus in buses}


def preview_levels(studio, project) -> dict:
    peaks = {}
    with studio.session(project) as session:
        session.call("init_engine", {"device_mode": "null_realtime"})
        session.call("start_output")
        session.call("post_action", {"name": "PlayTone"})
        deadline = time.monotonic() + POLL_SECONDS
        while time.monotonic() < deadline:
            for bus in session.call("get_mixer_levels")["buses"]:
                peaks[bus["name"]] = max(peaks.get(bus["name"], -96.0), bus["level_db"])
            time.sleep(POLL_INTERVAL)
        session.call("stop_all")
    return peaks


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-development")
    studio = ctx.studio()
    project = studio.create_project(ctx.work / "Project")
    bus_ids = author(studio, project, ctx.work)
    preview = preview_levels(studio, project)

    directory = export_all(studio, project, ctx.work / "Export")
    action = exported_actions(directory)["PlayTone"]
    session = ctx.engine({
        "edition": "development", "measure": True, "init_pack": str(directory / "Init.bytes"),
        "content_packs": [{"path": action["pack"], "root_action": action["id"]}],
        "actions": [{"name": "PlayTone", "id": action["id"], "blocks": PLAY_BLOCKS}],
    })
    result = session["actions"][0]["bus_peaks"]
    exported = {name: to_db(result.get((bus_id or "").replace("-", "").lower() or session["master_bus"], 0.0))
                for name, bus_id in bus_ids.items()}
    ctx.details.update(preview_db=preview, exported_db=exported, bus_ids=bus_ids)

    expected_child = to_db(0.3) + SOUND_VOLUME_DB + CHILD_VOLUME_DB
    if abs(exported["Child"] - expected_child) > TOLERANCE_DB:
        raise AssertionError(f"exported Child bus peaked at {exported['Child']:.2f} dB, expected {expected_child:.2f}")
    mismatched = {name: (preview.get(name), level) for name, level in exported.items()
                  if preview.get(name) is None or abs(preview[name] - level) > TOLERANCE_DB}
    if mismatched:
        raise AssertionError(f"preview and exported Pack disagree beyond {TOLERANCE_DB} dB: {mismatched}")
