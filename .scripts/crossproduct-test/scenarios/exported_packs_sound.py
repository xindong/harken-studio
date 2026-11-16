from scenarios.authoring import export_all, exported_actions, inspector_fields, option, write_tone

TONE_SECONDS = 0.6
PLAY_BLOCKS = 160
AUDIBLE_PEAK = 0.01
CONTAINERS = (("Random", "Single"), ("Random", "Queue"), ("Sequence", "Single"), ("Sequence", "Queue"))


def author(studio, project, work) -> list[str]:
    tones = [write_tone(work / f"tone_{hz}.wav", hz, TONE_SECONDS) for hz in (440, 660)]
    container = inspector_fields(studio, project, "Container")

    def create(parent, name, kind):
        arguments = {"name": name, "type": kind}
        if parent:
            arguments["parent"] = parent
        return studio.call(project, "create_item", arguments, save=True)["path"]

    def sound(parent, name, tone):
        path = create(parent, name, "Sound")
        studio.call(project, "import_audio", {"path": str(tone), "pack": layer_name, "target_parent_path": path},
                    save=True)

    layer_name = "Content"
    layer = create(None, layer_name, "Layer")
    sound(layer, "Direct", tones[0])
    targets = ["Direct"]
    for mode, play_type in CONTAINERS:
        name = f"{mode}{play_type}"
        path = create(layer, name, "Container")
        studio.run(project, [
            {"cmd": "set_item_property", "args": {"path": path, "property": "mode",
                                                  "value": option(container, "mode", mode)}},
            {"cmd": "set_item_property", "args": {"path": path, "property": "playType",
                                                  "value": option(container, "playType", play_type)}},
        ], save=True)
        for index, tone in enumerate(tones):
            sound(path, f"{name}_{index}", tone)
        targets.append(name)
    requests = []
    for target in targets:
        requests += [
            {"cmd": "create_action", "args": {"name": f"Play{target}"}},
            {"cmd": "set_item_property", "args": {"path": f"Play{target}", "property": "steps",
                                                  "value": [{"type": "Play", "targetName": target}]}},
        ]
    studio.run(project, requests, save=True)
    return [f"Play{target}" for target in targets]


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-release")
    studio = ctx.studio()
    project = studio.create_project(ctx.work / "Project")
    names = author(studio, project, ctx.work)
    directory = export_all(studio, project, ctx.work / "Export")
    actions = exported_actions(directory)
    missing = sorted(set(names) - set(actions))
    if missing:
        raise AssertionError(f"exported Packs lack Actions {missing}")

    plan = {
        "init_pack": str(directory / "Init.bytes"),
        "content_packs": [{"path": actions[name]["pack"], "root_action": actions[name]["id"]} for name in names],
        "actions": [{"name": name, "id": actions[name]["id"], "blocks": PLAY_BLOCKS} for name in names],
    }
    release = ctx.engine({**plan, "edition": "release", "measure": False})
    ctx.details["release"] = {a["name"]: a["max_live_playables"] for a in release["actions"]}
    silent = [a["name"] for a in release["actions"] if a["max_live_playables"] < 1]
    if silent:
        raise AssertionError(f"release edition started no playable for {silent}")

    ctx.require("engine-development")
    development = ctx.engine({**plan, "edition": "development", "measure": True})
    peaks = {a["name"]: a["peak"] for a in development["actions"]}
    ctx.details["development_peak"] = peaks
    quiet = sorted(name for name, peak in peaks.items() if peak < AUDIBLE_PEAK)
    if quiet:
        raise AssertionError(f"development edition rendered no audio above {AUDIBLE_PEAK} for {quiet}")
