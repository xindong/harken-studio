import json
import time

from drivers import engine
from drivers.process import kill_tree
from scenarios.authoring import export_all, exported_actions, write_tone

SERVE_SECONDS = 60
CONNECT_SECONDS = 15
EVENT_SECONDS = 15


def wait(predicate, seconds: float, what: str):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(f"timed out after {seconds}s waiting for {what}")


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-development")
    studio = ctx.studio()
    project = studio.create_project(ctx.work / "Project")
    tone = write_tone(ctx.work / "tone.wav", 440, 0.2)
    layer = studio.call(project, "create_item", {"name": "Game", "type": "Layer"}, save=True)["path"]
    sound = studio.call(project, "create_item", {"parent": layer, "name": "Tone", "type": "Sound"}, save=True)
    studio.call(project, "import_audio", {"path": str(tone), "pack": "Game", "target_parent_path": sound["path"]},
                save=True)
    studio.run(project, [
        {"cmd": "create_action", "args": {"name": "PlayTone"}},
        {"cmd": "set_item_property", "args": {"path": "PlayTone", "property": "steps",
                                              "value": [{"type": "Play", "targetName": "Tone"}]}},
    ], save=True)
    directory = export_all(studio, project, ctx.work / "Export")
    action = exported_actions(directory)["PlayTone"]

    ready, stop = ctx.work / "game-ready.json", ctx.work / "game-stop"
    plan = {"install": str(ctx.engine_install), "edition": "development", "sample_rate": 48000,
            "block_frames": 256, "measure": True, "init_pack": str(directory / "Init.bytes"),
            "content_packs": [{"path": action["pack"], "root_action": action["id"]}], "actions": [],
            "serve": {"seconds": SERVE_SECONDS, "post_interval": 0.3, "action_id": action["id"],
                      "ready_file": str(ready), "stop_file": str(stop)}}
    game, result_file = engine.spawn_session(ctx.python, plan, ctx.work)
    try:
        port = wait(lambda: ready.exists() and json.loads(ready.read_text(encoding="utf-8"))["port"],
                    CONNECT_SECONDS, "the game process to listen")
        with studio.session(project) as session:
            session.call("remote_connect", {"address": "127.0.0.1", "port": port})
            wait(lambda: session.call("get_remote_status")["state"] == "connected", CONNECT_SECONDS,
                 "Studio to connect")
            catalog = wait(lambda: session.call("get_remote_catalog")["entries"], CONNECT_SECONDS,
                           "the remote catalog")
            ctx.details["catalog"] = catalog
            named = [entry for entry in catalog if entry["name"] == "PlayTone"]
            if not named or named[0]["id"].lower() != action["id"].lower():
                raise AssertionError(f"remote catalog does not resolve PlayTone to {action['id']}: {catalog}")
            recording = ctx.work / "remote-session.vnmon"
            started = session.call("start_remote_capture", {"output_path": str(recording)})
            initial = started["capture"]["recording_bytes"]
            cursor = 0
            seen = []

            def action_event():
                nonlocal cursor
                batch = session.call("poll_remote_events", {"cursor": cursor, "max": 500})
                cursor = batch["next_cursor"]
                seen.extend(batch["events"])
                return [event for event in batch["events"] if event["action_name"] == "PlayTone"]

            events = wait(action_event, EVENT_SECONDS, "a captured PlayTone event")
            ctx.details["first_event"] = events[0]
            ctx.details["events_seen"] = len(seen)
            recorded = session.call("get_remote_status")["capture"]["recording_bytes"]
            session.call("stop_remote_capture")
            session.call("remote_disconnect")
            ctx.details["recording_bytes"] = recorded
            if not recording.is_file() or recording.stat().st_size != recorded or recorded <= initial:
                raise AssertionError(f"remote capture was not recorded to {recording}: {recorded} bytes reported")
    finally:
        stop.write_text("stop", encoding="utf-8")
        try:
            game.wait(timeout=30)
        except Exception:
            kill_tree(game)
    if result_file.exists():
        ctx.details["game"] = json.loads(result_file.read_text(encoding="utf-8"))
