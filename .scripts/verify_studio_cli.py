from __future__ import annotations

import argparse
import json
import math
import os
import struct
import subprocess
import tempfile
import wave
from pathlib import Path


def run(binary: Path, arguments: list[str], environment: dict, data: bytes | None = None):
    return subprocess.run([str(binary), *arguments], input=data, capture_output=True,
                          env=environment, timeout=180)


def verify(cli: Path, host: Path | None, work: Path) -> None:
    environment = {**os.environ, "QT_QPA_PLATFORM": "harken-cli-must-not-load-a-display"}

    def call(*arguments, data=None, exit_code=0):
        completed = run(cli, [*arguments, "--json"], environment,
                        None if data is None else json.dumps(data).encode("utf-8"))
        assert completed.returncode == exit_code, (
            arguments, completed.returncode, completed.stdout.decode("utf-8", "replace"),
            completed.stderr.decode("utf-8", "replace"))
        response = json.loads(completed.stdout)
        assert response["protocolVersion"] == 1
        assert response["status"] == ("ok" if exit_code == 0 else "error"), response
        return response

    help_result = run(cli, ["--help"], environment)
    assert help_result.returncode == 0 and b"--input" in help_result.stdout
    worker = run(cli, ["--resource-worker"], environment, b"{}\n")
    assert worker.returncode == 2, worker.stderr.decode("utf-8", "replace")
    identity = json.loads(worker.stdout)
    assert identity["protocol"] == 1 and identity["compiler"], identity
    assert call("--version")["result"]["version"]
    catalog = call("commands")["result"]["commands"]
    names = {entry["command"] for entry in catalog}
    assert len(names) == len(catalog) and {"create_project", "set_item_property", "export_pack"} <= names
    for entry in catalog:
        assert entry["registered"] and entry["inputSchema"]["type"] == "object", entry
        assert not entry["requiresDesktop"], entry
        assert "hostRestriction" not in entry, entry
    for excluded in ("get_theme", "click_widget", "batch", "list_codecs", "add_listener"):
        assert excluded not in names
        assert call("commands", excluded, exit_code=1)["code"] == "UNKNOWN_COMMAND"
        assert call("exec", excluded, exit_code=1)["code"] == "UNKNOWN_COMMAND"
    for shared in ("audition_get_state", "audition_get_position", "audition_get_speed"):
        assert shared in names
    call("exec", "not_a_registered_command", exit_code=1)
    call("not_a_subcommand", exit_code=2)
    call("exec", "list_items", "--args", "[", exit_code=2)
    call("run", "--input", "-", data=[{"cmd": "create_project"}, 1], exit_code=2)

    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="studio cli \u00e9-", dir=work) as temporary:
        root = Path(temporary).resolve()
        assert root.is_relative_to(work.resolve())
        project_dir = root / "GameAudio"
        project = project_dir / "GameAudio.hsp"
        call("init", str(project_dir))
        assert project.is_file()
        call("init", str(project_dir), exit_code=1)
        worker_request = json.dumps({"manifest": str(root / "missing-manifest.json")}).encode("utf-8") + b"\n"
        worker = run(cli, ["--resource-worker"], environment, worker_request * 2)
        assert worker.returncode == 0, worker.stderr.decode("utf-8", "replace")
        responses = [json.loads(line) for line in worker.stdout.splitlines()]
        assert len(responses) == 3 and responses[0] == identity, responses
        assert all(response["success"] is False and response["error"] for response in responses[1:]), responses

        def execute(name, arguments=None, save=False, exit_code=0):
            flags = ["exec", name, "--project", str(project), "--input", "-"]
            if save:
                flags.append("--save")
            return call(*flags, data=arguments or {}, exit_code=exit_code)

        def authored_contents():
            return {path.relative_to(project_dir).as_posix(): path.read_bytes()
                    for path in project_dir.rglob("*") if path.is_file()
                    and ".cache" not in path.relative_to(project_dir).parts}

        call("run", "--project", str(project), "--input", "-", "--save", data=[
            {"cmd": "create_action", "args": {"name": "ShouldNotExist"}},
            {"cmd": "list_items", "args": []}], exit_code=2)
        execute("get_item_info", {"path": "ShouldNotExist"}, exit_code=1)
        execute("create_action", {"name": "Unsaved"})
        execute("get_item_info", {"path": "Unsaved"}, exit_code=1)
        stopped = call("run", "--project", str(project), "--input", "-", "--save", data=[
            {"cmd": "create_action", "args": {"name": "BeforeFailure"}},
            {"cmd": "not_a_registered_command"},
            {"cmd": "create_action", "args": {"name": "AfterFailure"}}], exit_code=1)
        assert stopped["index"] == 1 and len(stopped["results"]) == 2, stopped
        execute("get_item_info", {"path": "BeforeFailure"}, exit_code=1)
        execute("get_item_info", {"path": "AfterFailure"}, exit_code=1)
        call("run", "--project", str(project), "--input", "-", "--save", data=[
            {"cmd": "close_project"},
            {"cmd": "create_action", "args": {"name": "AfterClose"}}], exit_code=1)
        execute("get_item_info", {"path": "AfterClose"}, exit_code=1)
        call("run", "--project", str(project), "--input", "-", data=[
            {"cmd": "close_project"},
            {"cmd": "open_project", "args": {"path": str(project)}},
            {"cmd": "list_items"}])

        audio = root / "tone.wav"
        with wave.open(str(audio), "wb") as stream:
            stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            stream.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 440 * i / 48000)))
                                       for i in range(24000)))
        imported = execute("import_audio", {"path": str(audio)}, save=True)["result"]
        assert "languages" in execute("list_project_languages")["result"]
        states = call("run", "--project", str(project), "--input", "-", data=[
            {"cmd": "get_project_state"},
            {"cmd": "set_item_property", "args": {"path": "tone", "property": "volume", "value": -6}},
            {"cmd": "get_project_state"},
            {"cmd": "undo"},
            {"cmd": "get_project_state"},
            {"cmd": "redo"},
            {"cmd": "save_project"},
            {"cmd": "get_project_state"}])["results"]
        assert [states[i]["result"]["is_dirty"] for i in (0, 2, 4, 7)] == [False, True, False, False], states
        platforms = execute("list_project_platforms")["result"]["platforms"]
        platform = platforms[0]["id"]
        saved = authored_contents()
        targets = call("list-targets", "--project", str(project))["result"]
        assert any(value["id"] == platform for value in targets["platforms"]), targets
        assert authored_contents() == saved
        build_flags = ["build", "--project", str(project), "--all", "--platform-id", platform,
                       "--self-clean", "off"]
        for flags in (["--save"], ["--args", "{}"], ["--input", "-"], ["--timeout", "1"]):
            call(*build_flags, *flags, exit_code=2)
        call("list-targets", "--project", str(project), "--save", exit_code=2)
        call("list-targets", "--project", str(project), "--all", exit_code=2)
        call(*build_flags, "--platform-id", platform, exit_code=2)
        assert authored_contents() == saved
        converted = execute("transcode_asset", {"source_id": imported["source_id"], "platform_id": platform})
        assert converted["result"]["success"] and not converted["result"].get("async"), converted
        workflow = root / "workflow.json"
        workflow.write_text(json.dumps([
            {"cmd": "create_action", "args": {"name": "PlayTone"}},
            {"cmd": "set_item_property", "args": {"path": "PlayTone", "property": "steps",
                "value": [{"type": "Play", "targetName": "tone"}]}}
        ]), encoding="utf-8")
        call("run", "--project", str(project), "--input", str(workflow), "--save")
        execute("get_item_info", {"path": "PlayTone"})
        execute("get_action_definition", {"name": "PlayTone"})
        playback = call("run", "--project", str(project), "--input", "-", data=[
            {"cmd": "init", "args": {"sample_rate": 48000, "buffer_size": 480}},
            {"cmd": "set_master_volume", "args": {"volume": 0}},
            {"cmd": "start_output"},
            {"cmd": "post_action", "args": {"name": "PlayTone"}},
            {"cmd": "audition_play", "args": {"object_path": "PlayTone"}},
            {"cmd": "audition_get_state"},
            {"cmd": "set_listener", "args": {"pos": [1, 2, 3], "forward": [0, 0, 1], "up": [0, 1, 0]}},
            {"cmd": "get_listener_info"},
            {"cmd": "get_status"},
            {"cmd": "get_buffer_config"},
            {"cmd": "stop_all"}])["results"]
        assert playback[3]["result"]["action_handle"] != 0, playback
        assert playback[4]["result"]["auditionID"] != 0, playback
        assert playback[5]["result"]["activeCount"] > 0, playback
        assert playback[7]["result"]["position"] == [1, 2, 3], playback
        buffer = playback[9]["result"]
        assert math.isclose(playback[8]["result"]["output_latency_ms"],
                            1000 * buffer["block_size"] / buffer["sample_rate"]), playback
        rejected = call("run", "--project", str(project), "--input", "-", exit_code=1, data=[
            {"cmd": "post_action", "args": {"name": "PlayTone"}}])
        assert rejected["code"] == "PLAY_FAILED", rejected
        destination = root / "cli-export"
        asset = execute("get_asset_info", {"source_id": imported["source_id"]})["result"]
        source = Path(asset["absolute_path"]).resolve()
        assert source.is_relative_to(root)
        original = source.read_bytes()
        try:
            source.write_bytes(b"invalid audio input")
            execute("export_pack", {"output_path": str(destination), "platform_id": platform}, exit_code=1)
            failed = call(*build_flags, "--output", str(root / "failed-saved-export"), exit_code=1)
            assert not failed["result"]["success"] and failed["result"]["platforms"], failed
            rejected_output = root / "legacy-batch-export"
            execute("batch", {"commands": [
                {"cmd": "create_action", "args": {"name": "HiddenBatch"}},
                {"cmd": "export_pack", "args": {"output_path": str(rejected_output), "platform_id": platform}}
            ]}, save=True, exit_code=1)
            assert not rejected_output.exists()
            execute("get_item_info", {"path": "HiddenBatch"}, exit_code=1)
        finally:
            source.write_bytes(original)
        exported = execute("export_pack", {"output_path": str(destination), "platform_id": platform})
        assert exported["result"]["success"] and not exported["result"].get("async"), exported
        packs = sorted(destination.rglob("*.bytes"))
        assert packs and any(pack.stem == "PlayTone" for pack in packs), exported
        for pack in packs:
            assert pack.stat().st_size and json.loads(pack.with_suffix(".json").read_text(encoding="utf-8"))
        init_pack = next(pack for pack in packs if pack.stem == "Init")
        content_pack = next(pack for pack in packs if pack.stem == "PlayTone")
        lifecycle = call("run", "--input", "-", data=[
            {"cmd": "init"},
            {"cmd": "load_pack", "args": {"path": str(init_pack)}},
            {"cmd": "load_pack", "args": {"path": str(content_pack)}},
            {"cmd": "get_loaded_packs"},
            {"cmd": "unload_pack", "args": {"path": str(content_pack)}},
            {"cmd": "get_loaded_packs"},
            {"cmd": "unload_pack", "args": {"path": str(init_pack)}},
            {"cmd": "get_loaded_packs"}])["results"]
        for index, expected in ((3, {init_pack, content_pack}), (5, {init_pack}), (7, set())):
            loaded = lifecycle[index]["result"]
            assert loaded["count"] == len(expected), lifecycle
            assert {Path(pack["path"]) for pack in loaded["packs"]} == expected, lifecycle
        reference = root / "saved-export"
        saved = authored_contents()
        generated = call(*build_flags, "--output", str(reference))
        assert generated["result"]["success"], generated
        assert authored_contents() == saved
        for pack in packs:
            for artifact in (pack, pack.with_suffix(".json")):
                counterpart = reference / artifact.relative_to(destination)
                assert counterpart.read_bytes() == artifact.read_bytes(), artifact.name

        transient = root / "unsaved-export"
        call("run", "--project", str(project), "--input", "-", data=[
            {"cmd": "create_action", "args": {"name": "TransientTone"}},
            {"cmd": "set_item_property", "args": {"path": "TransientTone", "property": "steps",
                "value": [{"type": "Play", "targetName": "tone"}]}},
            {"cmd": "export_pack", "args": {"output_path": str(transient),
                "platform_ids": [platform], "all": True, "force": True, "self_clean": False}}
        ])
        assert any(path.stem == "TransientTone" for path in transient.rglob("*.bytes"))
        assert authored_contents() == saved
        assert not any(action["name"] == "TransientTone"
                       for action in call("list-targets", "--project", str(project))["result"]["actions"])
        call(*build_flags, "--output", str(reference), "--force")
        assert not any(path.stem == "TransientTone" for path in reference.rglob("*.bytes"))
        assert authored_contents() == saved

        rejected = root / "rejected-export"
        for invalid in ({"all": "true"}, {"force": 1}, {"self_clean": "off"},
                        {"platform_ids": []}, {"platform_ids": [platform, platform]},
                        {"platform_id": platform, "platform_ids": [platform]},
                        {"all": True, "action_name": "PlayTone"},
                        {"self_clean": True, "action_name": "PlayTone"}):
            execute("export_pack", {"output_path": str(rejected), **invalid}, exit_code=1)
        assert not rejected.exists()

        execute("create_project_platform", {"name": "Alternate", "base_platform": "Windows"}, save=True)
        platform_ids = [value["id"] for value in execute("list_project_platforms")["result"]["platforms"]]
        session_all = root / "session-all"
        saved_all = root / "saved-all"
        session_all.mkdir()
        saved_all.mkdir()
        (session_all / "obsolete.txt").write_text("obsolete", encoding="utf-8")
        (saved_all / "obsolete.txt").write_text("obsolete", encoding="utf-8")
        saved = authored_contents()
        multiple = execute("export_pack", {"output_path": str(session_all), "platform_ids": platform_ids,
                           "all": True, "force": True, "self_clean": True})
        assert multiple["result"]["success"], multiple
        multiple_flags = ["build", "--project", str(project), "--all", "--self-clean", "on",
                          "--force", "--output", str(saved_all)]
        for platform_id in platform_ids:
            multiple_flags.extend(["--platform-id", platform_id])
        assert call(*multiple_flags)["result"]["success"]
        assert not (session_all / "obsolete.txt").exists()
        assert not (saved_all / "obsolete.txt").exists()
        session_artifacts = {path.relative_to(session_all): path.read_bytes()
                             for path in session_all.rglob("*") if path.is_file()}
        saved_artifacts = {path.relative_to(saved_all): path.read_bytes()
                           for path in saved_all.rglob("*") if path.is_file()}
        assert session_artifacts == saved_artifacts
        assert authored_contents() == saved

        journal = project_dir.with_name(project_dir.name + ".harken-project-transaction.json")
        journal.write_text("{}", encoding="utf-8")
        recovery_output = root / "recovery-rejected"
        for flags in (["list-targets", "--project", str(project)],
                      [*build_flags, "--output", str(recovery_output)]):
            refused = call(*flags, exit_code=1)
            assert refused["code"] == "OPEN_PROJECT_FAILED", refused
            assert journal.read_text(encoding="utf-8") == "{}"
            assert authored_contents() == saved
            assert not recovery_output.exists()
        journal.unlink()
        if host:
            platform_dir = next(pack.parent for pack in packs if pack.stem == "PlayTone")
            playback = run(host, ["pack-play", "--root", str(platform_dir), "--action-name", "PlayTone",
                                  "--seconds", "1", "--null"], environment)
            assert playback.returncode == 0, (
                playback.stdout.decode("utf-8", "replace"), playback.stderr.decode("utf-8", "replace"))
    print("CLI protocol, desktop isolation, persistence, conversion, read-only builds and pack equivalence passed.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the native Studio CLI against real project and pack services.")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--host", type=Path)
    parser.add_argument("--work-dir", type=Path, default=Path(".temp"))
    args = parser.parse_args()
    verify(args.cli.resolve(), args.host.resolve() if args.host else None,
           args.work_dir.resolve())


if __name__ == "__main__":
    main()
