import math
import time

from scenarios.authoring import SAMPLE_RATE, export_all, exported_actions, write_tone

TONE_SECONDS = 1.5
POLL_SECONDS = 1.0
WARMUP_SECONDS = 0.25
POLL_INTERVAL = 0.02
BLOCK_FRAMES = 256
TOLERANCE_DB = 0.5
MUSIC_BUS_VOLUME_DB = -6.0
SOUND_VOLUME_DB = -3.0


def to_db(linear: float) -> float:
    return -96.0 if linear <= 0.0 else max(20.0 * math.log10(linear), -96.0)


def author(studio, project, work):
    for name in ("Music", "Dialogue"):
        studio.call(project, "create_bus", {"name": name}, save=True)
    buses = {bus["name"]: bus for bus in studio.call(project, "list_buses")["buses"]}
    layers, sounds = {}, {}
    for bus, name, frequency in (("Music", "Bed", 440), ("Dialogue", "Voice", 660)):
        layer = studio.call(project, "create_item", {"name": name + "Layer", "type": "Layer"}, save=True)
        layers[name] = layer["guid"]
        sound = studio.call(project, "create_item", {"parent": layer["path"], "name": name, "type": "Sound"}, save=True)
        sounds[name] = sound["guid"]
        tone = write_tone(work / f"{name}.wav", frequency, TONE_SECONDS)
        output = {"definition_guid": sound["guid"], "group": "output", "scope": "Shared"}
        studio.run(project, [
            {"cmd": "import_audio", "args": {"path": str(tone), "pack": name + "Layer",
                                             "target_parent_path": sound["path"]}},
            {"cmd": "set_field_group_mode", "args": {**output, "mode": "Local"}},
            {"cmd": "set_field_group_field", "args": {**output, "field": "outputBusRef", "value": buses[bus]["bus_id"]}},
        ], save=True)
    studio.run(project, [
        {"cmd": "set_bus_volume_db", "args": {"bus_name": "Music", "db": MUSIC_BUS_VOLUME_DB}},
        {"cmd": "set_item_property", "args": {"path": "Bed", "property": "volume", "value": SOUND_VOLUME_DB}},
        {"cmd": "create_action", "args": {"name": "PlayMix"}},
        {"cmd": "set_item_property", "args": {"path": "PlayMix", "property": "steps",
                                              "value": [{"type": "Play", "targetName": name} for name in sounds]}},
    ], save=True)
    return buses, layers["Voice"], sounds["Voice"]


def preview_levels(studio, project) -> dict:
    peaks = {}
    with studio.session(project) as session:
        session.call("init_engine", {"device_mode": "null_realtime"})
        session.call("start_output")
        session.call("post_action", {"name": "PlayMix"})
        start = time.monotonic()
        deadline = start + POLL_SECONDS
        while time.monotonic() < deadline:
            measuring = time.monotonic() >= start + WARMUP_SECONDS
            levels = session.call("get_mixer_levels")["buses"]
            if measuring:
                for bus in levels:
                    peaks[bus["name"]] = max(peaks.get(bus["name"], -96.0), bus["level_db"])
            time.sleep(POLL_INTERVAL)
        session.call("stop_all")
    return peaks


def compare(ctx, studio, project, buses, stage):
    preview = preview_levels(studio, project)
    directory = export_all(studio, project, ctx.work / "Export", force=False)
    action = exported_actions(directory)["PlayMix"]
    session = ctx.engine({
        "edition": "development", "measure": True, "init_pack": str(directory / "Init.bytes"),
        "sample_rate": SAMPLE_RATE, "block_frames": BLOCK_FRAMES,
        "content_packs": [{"path": action["pack"], "root_action": action["id"]}],
        "actions": [{"name": "PlayMix", "id": action["id"],
                     "blocks": math.ceil(POLL_SECONDS * SAMPLE_RATE / BLOCK_FRAMES),
                     "warmup_blocks": math.ceil(WARMUP_SECONDS * SAMPLE_RATE / BLOCK_FRAMES)}],
    })
    result = session["actions"][0]["bus_peaks"]
    exported = {}
    for name, bus in buses.items():
        key = bus["bus_id"].replace("-", "").lower() if bus["parent_bus_ref"].strip("0-") else session["master_bus"]
        exported[name] = to_db(result.get(key, 0.0))
    ctx.details[stage] = {"preview_db": preview, "exported_db": exported}
    mismatched = {name: (preview.get(name), level) for name, level in exported.items()
                  if preview.get(name) is None or abs(preview[name] - level) > TOLERANCE_DB}
    if mismatched:
        raise AssertionError(f"{stage}: preview and exported Pack disagree beyond {TOLERANCE_DB} dB: {mismatched}")
    if abs(exported["Dialogue"] - to_db(0.3)) > TOLERANCE_DB:
        raise AssertionError(f"{stage}: dialogue source peaked at {exported['Dialogue']:.2f} dB")
    return exported["Music"]


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-development")
    studio = ctx.studio()
    project = studio.create_project(ctx.work / "Project")
    buses, voice_layer, voice = author(studio, project, ctx.work)
    baseline = compare(ctx, studio, project, buses, "baseline")
    expected = to_db(0.3) + SOUND_VOLUME_DB + MUSIC_BUS_VOLUME_DB
    if abs(baseline - expected) > TOLERANCE_DB:
        raise AssertionError(f"Music bus peaked at {baseline:.2f} dB, expected {expected:.2f}")

    sidechain = {"type": "Sidechain", "duckTarget": False, "targetBusID": buses["Music"]["bus_id"], "sendLevel": -30.0}
    inherited = {"definition_guid": voice_layer, "group": "send", "scope": "Shared"}
    studio.call(project, "replace_field_group_sends", {**inherited, "sends": [sidechain]}, save=True)
    if abs(compare(ctx, studio, project, buses, "sidechain_unchecked") - baseline) > TOLERANCE_DB:
        raise AssertionError("an unchecked sidechain changed the music level")

    previous = baseline
    for gain in (-30.0, -24.0):
        studio.call(project, "replace_field_group_sends", {
            **inherited, "sends": [{**sidechain, "duckTarget": True, "sendLevel": gain}],
        }, save=True)
        level = compare(ctx, studio, project, buses, f"inherited_{gain:g}dB")
        if level >= previous - TOLERANCE_DB:
            raise AssertionError(f"raising the inherited send to {gain:g} dB left music at {level:.2f} dB")
        previous = level

    local = {"definition_guid": voice, "group": "send", "scope": "Shared"}
    studio.run(project, [
        {"cmd": "set_field_group_mode", "args": {**local, "mode": "Local"}},
        {"cmd": "replace_field_group_sends", "args": {**local, "sends": []}},
    ], save=True)
    if abs(compare(ctx, studio, project, buses, "local_empty") - baseline) > TOLERANCE_DB:
        raise AssertionError("empty local sends did not restore the music level")

    platform = studio.call(project, "list_project_platforms")["platforms"][0]["id"]
    bus_send = {"path": buses["Dialogue"]["bus_id"], "property": "sends", "platform_ids": [platform]}
    with studio.session(project) as edit:
        music = next(bus for bus in edit.call("list_buses")["buses"] if bus["name"] == "Music")
        edit.call("set_item_property", {
            **bus_send, "value": [{"type": "Sidechain", "duckTarget": True, "targetBusID": music["id"], "sendLevel": -24.0}],
        })
        edit.call("save_project")
    if compare(ctx, studio, project, buses, "platform_bus_ducking") >= baseline - TOLERANCE_DB:
        raise AssertionError("the dialogue bus did not duck the music bus")
    studio.call(project, "set_item_property", {**bus_send, "value": []}, save=True)
    if abs(compare(ctx, studio, project, buses, "bus_removed") - baseline) > TOLERANCE_DB:
        raise AssertionError("removing the final bus send did not restore the music level")
