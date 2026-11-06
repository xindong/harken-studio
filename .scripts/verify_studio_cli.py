from __future__ import annotations

import argparse
import json
import math
import os
import queue
import struct
import subprocess
import tempfile
import threading
import wave
from pathlib import Path


def run(binary: Path, arguments: list[str], environment: dict, data: bytes | None = None):
    return subprocess.run([str(binary), *arguments], input=data, capture_output=True,
                          env=environment, timeout=180)


def verify_shell_help(send, commands) -> None:
    def request(line, error=None):
        response = send(line, error)
        assert response["protocolVersion"] == 1, response
        assert response["status"] == ("error" if error else "ok"), (line, response)
        if error:
            assert response["code"] == error, (line, response)
        return response

    def directory(response):
        return {entry["command"]: entry for entry in response["result"]["commands"]}

    before = request("get_project_state")["result"]["is_dirty"]
    catalog = directory(commands())
    assert directory(request("help")) == catalog
    assert directory(request("commands")) == catalog
    selected = ["create_action", "save_project"]
    if "list_ui_windows" in catalog:
        selected.append("list_ui_windows")
    for name in selected:
        expected = commands(name)["result"]
        for line in ("help " + name, "commands " + name, name + " --help"):
            assert request(line)["result"] == expected, line
    category = next(entry["category"] for entry in catalog.values() if entry["readOnly"])
    for options in (("--search", "project"), ("--search", "current project"),
                    ("--search", "__no_matching_help_command__"),
                    ("--category", category), ("--read-only",),
                    ("--category", category, "--read-only", "--search", "get")):
        expected = directory(commands(*options))
        arguments = " ".join(option if option.startswith("--") else json.dumps(option) for option in options)
        for prefix in ("help ", "commands "):
            assert directory(request(prefix + arguments)) == expected, (prefix, options)
    for name in ("help", "commands", "exit"):
        builtin = request("help " + name)["result"]
        assert builtin.get("help"), (name, builtin)
        assert request(name + " --help")["result"] == builtin
    for line in ("help --unknown-option", "commands --search", "help --category",
                 "help --search --read-only", "commands --category --read-only",
                 "help create_action --read-only", "help --search project --search audio",
                 'help --search ""', 'commands --category ""', 'help --search "unterminated'):
        request(line, "USAGE")
    request("help __missing_command__", "UNKNOWN_COMMAND")
    assert request("get_project_state")["result"]["is_dirty"] == before


def verify_shell(cli: Path, environment: dict, root: Path, audio_output: bool) -> None:
    directory = root / "ShellAudio"
    project = directory / "ShellAudio.hsp"
    created = run(cli, ["init", str(directory), "--json"], environment)
    assert created.returncode == 0, created.stderr

    def shell(lines, exit_code=0, json_output=True, project_path: Path | None = project):
        arguments = ["shell"] + (["--project", str(project_path)] if project_path else [])
        arguments += ["--json"] if json_output else []
        completed = run(cli, arguments, environment,
                        ("\n".join(lines) + ("\n" if lines else "")).encode("utf-8"))
        assert completed.returncode == exit_code, (completed.stdout, completed.stderr)
        responses = [json.loads(line) for line in completed.stdout.splitlines()]
        assert all(response["protocolVersion"] == 1 for response in responses), responses
        return responses

    for _ in range(2):
        unchanged = shell(["get_project_state", "exit"])
        assert len(unchanged) == 2 and all(response["status"] == "ok" for response in unchanged), unchanged
        assert unchanged[0]["result"]["is_dirty"] is False, unchanged
    created_in_shell = shell([
        "create_project " + json.dumps({"name": "ShellCreated", "location": str(root)}),
        "get_project_state", "exit"], project_path=None)
    assert len(created_in_shell) == 3 and all(response["status"] == "ok" for response in created_in_shell), created_in_shell
    assert created_in_shell[1]["result"]["is_dirty"] is False, created_in_shell
    reopened = shell(["get_project_state", "exit"], project_path=root / "ShellCreated" / "ShellCreated.hsp")
    assert len(reopened) == 2 and all(response["status"] == "ok" for response in reopened), reopened
    assert reopened[0]["result"]["is_dirty"] is False, reopened

    responses = shell([
        "help", "commands create_action", "create_action [", "create_action []",
        "not_a_registered_command", 'create_action {"name":"SessionAction"}',
        'get_item_info {"path":"SessionAction"}', "exit", "close_project",
        "open_project " + json.dumps({"path": str(project)}), "save_project", "exit"])
    assert responses[0]["status"] == "ok" and responses[1]["result"]["command"] == "create_action"
    assert [response.get("code") for response in responses[2:5]] == [
        "INVALID_JSON", "INVALID_ARGS", "UNKNOWN_COMMAND"], responses
    assert responses[6]["result"]["name"] == "SessionAction", responses
    assert all(response["code"] == "UNSAVED_CHANGES" for response in responses[7:10]), responses
    assert all(response["status"] == "ok" for response in responses[10:]), responses
    persisted = shell(['get_item_info {"path":"SessionAction"}'])
    assert persisted[0]["result"]["name"] == "SessionAction", persisted
    assert shell([]) == []
    piped = shell(["get_project_state", "commands save_project", "exit"], json_output=False)
    assert len(piped) == 3 and all(response["status"] == "ok" for response in piped), piped
    refused = shell(['create_action {"name":"UnsavedEof"}'], exit_code=1)
    assert refused[-1]["code"] == "UNSAVED_CHANGES", refused
    discarded = shell(['create_action {"name":"Discarded"}', "exit --discard"])
    assert discarded[-1]["result"]["discarded"] is True, discarded
    absent = shell(['get_item_info {"path":"UnsavedEof"}', 'get_item_info {"path":"Discarded"}'])
    assert all(response["status"] == "error" for response in absent), absent

    seeded = shell(['create_folder {"path":"Layers/Destination"}', "save_project", "exit"])
    assert all(response["status"] == "ok" for response in seeded), seeded
    expected_folders = {"Layers/Destination"}
    for mutation, saved_folders in (
        ('create_folder {"path":"Layers/Source"}', {"Layers/Destination", "Layers/Source"}),
        ('rename_folder {"path":"Layers/Source","new_name":"Renamed"}',
         {"Layers/Destination", "Layers/Renamed"}),
        ('move_folder {"path":"Layers/Renamed","new_parent":"Layers/Destination"}',
         {"Layers/Destination", "Layers/Destination/Renamed"}),
        ('delete_folder {"path":"Layers/Destination/Renamed"}', {"Layers/Destination"}),
    ):
        edited = shell([
            "get_project_state", mutation, "get_project_state", "exit", "undo",
            "get_project_state", 'list_folders {"path":"Layers"}',
            'list_folders {"path":"Layers/Destination"}', "redo", "get_project_state",
            "save_project", "get_project_state", "exit"])
        assert len(edited) == 13, (mutation, edited)
        assert edited[3]["code"] == "UNSAVED_CHANGES", (mutation, edited)
        assert all(response["status"] == "ok" for index, response in enumerate(edited)
                   if index != 3), (mutation, edited)
        assert [edited[index]["result"]["is_dirty"] for index in (0, 2, 5, 9, 11)] == [
            False, True, False, True, False], (mutation, edited)
        restored = {folder["path"] for index in (6, 7)
                    for folder in edited[index]["result"]["folders"]}
        assert restored == expected_folders, (mutation, restored, expected_folders)
        reopened = shell(["get_project_state", 'list_folders {"path":"Layers"}',
                          'list_folders {"path":"Layers/Destination"}'])
        assert all(response["status"] == "ok" for response in reopened), reopened
        assert reopened[0]["result"]["is_dirty"] is False, (mutation, reopened)
        persisted = {folder["path"] for response in reopened[1:]
                     for folder in response["result"]["folders"]}
        assert persisted == saved_folders, (mutation, persisted, saved_folders)
        expected_folders = saved_folders

    process = subprocess.Popen([str(cli), "shell", "--project", str(project), "--json"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, env=environment)
    responses = queue.Queue()

    def read_responses():
        for line in process.stdout:
            responses.put(json.loads(line))

    reader = threading.Thread(target=read_responses, daemon=True)
    reader.start()
    try:
        def send(line, error=None):
            process.stdin.write((line + "\n").encode("utf-8"))
            process.stdin.flush()
            return responses.get(timeout=30)

        def commands(*arguments):
            completed = run(cli, ["commands", *arguments, "--json"], environment)
            assert completed.returncode == 0, (completed.stdout, completed.stderr)
            return json.loads(completed.stdout)

        verify_shell_help(send, commands)
        initialize = "init_engine " + json.dumps({"device_mode": "real" if audio_output else "null"})
        for line in ['get_item_info {"path":"SessionAction"}', "commands save_project", initialize,
                     *(["start_output"] if audio_output else []), "close_project"]:
            response = send(line)
            assert response["status"] == "ok", response
        released = run(cli, ["list-targets", "--project", str(project), "--json"], environment)
        assert released.returncode == 0, (released.stdout, released.stderr)
        for line in ("open_project " + json.dumps({"path": str(project)}),
                     'get_item_info {"path":"SessionAction"}', "exit"):
            response = send(line)
            assert response["status"] == "ok", response
        assert process.wait(timeout=30) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=30)
        process.stdin.close()
        reader.join(timeout=30)
        process.stdout.close()
    assert not reader.is_alive()


def verify(cli: Path, host: Path | None, work: Path, audio_output: bool = True) -> None:
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
    assert call("--help")["result"]["help"]
    for command in ("commands", "init", "exec", "run", "shell", "list-targets", "build"):
        help_result = run(cli, [command, "--help"], environment)
        assert help_result.returncode == 0 and f"Usage: HarkenStudioCLI {command}".encode() in help_result.stdout
        assert call(command, "--help")["result"]["help"]
    call("not_a_subcommand", "--help", exit_code=2)
    worker = run(cli, ["--resource-worker"], environment, b"{}\n")
    assert worker.returncode == 2, worker.stderr.decode("utf-8", "replace")
    identity = json.loads(worker.stdout)
    assert identity["protocol"] == 1 and identity["compiler"], identity
    assert call("--version")["result"]["version"]
    catalog = call("commands")["result"]["commands"]
    assert call("exec", "list_commands")["result"]["commands"] == catalog
    schema = call("exec", "get_command_schema", "--input", "-", data={"command": "save_project"})
    assert call("commands", "save_project")["result"] == schema["result"]
    assert call("exec", "save_project", "--help")["result"] == schema["result"]
    human_schema = run(cli, ["commands", "save_project"], environment)
    assert human_schema.returncode == 0 and b"Arguments: none." in human_schema.stdout, human_schema.stdout
    assert not human_schema.stderr, human_schema.stderr
    human_schema = run(cli, ["exec", "open_project", "--help", "--project", "missing.hsp"], environment)
    assert human_schema.returncode == 0 and b"path" in human_schema.stdout and b"[required]" in human_schema.stdout
    names = {entry["command"] for entry in catalog}
    assert len(names) == len(catalog) and {"create_project", "set_item_property", "export_pack"} <= names
    for entry in catalog:
        assert entry["registered"] and entry["inputSchema"]["type"] == "object", entry
        assert entry["schemaSource"] == "declared" and entry["inputSchema"]["additionalProperties"] is False, entry
        assert not entry["requiresDesktop"], entry
        assert "hostRestriction" not in entry, entry
    for search in ("audio", "PROJECT", "no_such_command_987654321"):
        expected = [entry for entry in catalog if search.lower() in
                    " ".join(entry.get(key, "") for key in ("command", "description", "category")).lower()]
        assert call("commands", "--search", search)["result"]["commands"] == expected
    for category in {entry["category"] for entry in catalog}:
        expected = [entry for entry in catalog if entry["category"] == category and entry["readOnly"]]
        assert call("commands", "--category", category, "--read-only")["result"]["commands"] == expected
    call("commands", "save_project", "--search", "project", exit_code=2)
    for option in ("--search", "--category"):
        for value in ("", " ", "--read-only"):
            rejected = call("commands", option, value, exit_code=2)
            assert rejected["code"] == "USAGE", rejected
    literal = call("commands", "--search=--read-only")["result"]["commands"]
    assert literal == [entry for entry in catalog if "--read-only" in
                       " ".join(entry.get(key, "") for key in ("command", "description", "category")).lower()]
    invalid_lines = [entry["command"] + ' {"__unsupported_argument__":true}' for entry in catalog] + ["exit"]
    invalid = run(cli, ["shell", "--json"], environment, ("\n".join(invalid_lines) + "\n").encode())
    assert invalid.returncode == 0, invalid.stderr
    invalid_responses = [json.loads(line) for line in invalid.stdout.splitlines()]
    assert len(invalid_responses) == len(invalid_lines), invalid_responses
    assert all(response.get("code") == "INVALID_ARGUMENTS" for response in invalid_responses[:-1]), invalid_responses
    for arguments in ({}, {"path": 42}, {"path": "test.hsp", "typo": True}):
        rejected = call("exec", "open_project", "--input", "-", data=arguments, exit_code=2)
        assert rejected["code"] == "INVALID_ARGUMENTS" and rejected["suggestion"], rejected
    rejected = call("exec", "save_project", "--project", "missing.hsp", "--input", "-",
                    data={"typo": True}, exit_code=2)
    assert rejected["code"] == "INVALID_ARGUMENTS" and "typo" in rejected["message"], rejected
    for excluded in ("get_theme", "click_widget", "batch", "list_codecs", "add_listener",
                     "set_platform_profile", "set_preferred_format", "audition_get_speed"):
        assert excluded not in names
        assert call("commands", excluded, exit_code=1)["code"] == "UNKNOWN_COMMAND"
        assert call("exec", excluded, exit_code=1)["code"] == "UNKNOWN_COMMAND"
    for shared in ("audition_get_state", "get_inspector_schema"):
        assert shared in names
    call("exec", "not_a_registered_command", exit_code=1)
    call("not_a_subcommand", exit_code=2)
    call("exec", "list_items", "--args", "[", exit_code=2)
    call("run", "--input", "-", data=[{"cmd": "create_project"}, 1], exit_code=2)
    call("run", exit_code=2)
    call("--save", exit_code=2)
    call("exec", "ping", "--project", "", exit_code=2)
    for command in (["commands"], ["commands", "save_project"], ["init", "unused"], ["shell"]):
        for option in (["--args", "{}"], ["--input", "-"], ["--save"]):
            call(*command, *option, exit_code=2)
    call("commands", "--project", "missing.hsp", exit_code=2)
    for option, value in (("--project", "missing.hsp"), ("--args", "{}"), ("--input", "-"), ("--timeout", "1")):
        call("exec", "ping", option, value, option, value, exit_code=2)

    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="studio cli \u00e9-\u5de5\u7a0b-", dir=work) as temporary:
        root = Path(temporary).resolve()
        assert root.is_relative_to(work.resolve())
        for invalid_request in ({"cmd": "not_a_registered_command"},
                                {"cmd": "save_project", "args": {"typo": True}}):
            rejected = call("run", "--input", "-", data=[
                {"cmd": "create_project", "args": {"name": "Preflight", "location": str(root)}},
                invalid_request], exit_code=1 if invalid_request["cmd"].startswith("not_") else 2)
            assert rejected["index"] == 1 and rejected["results"] == [], rejected
            assert not (root / "Preflight").exists()
        reference_file = root / "reference.bin"
        candidate_file = root / "candidate.bin"
        reference_file.write_bytes(b"\x00\x01\x02")
        candidate_file.write_bytes(b"\x00\x01\x03\x04")
        compared = call("exec", "compare_files", "--input", "-", data={
            "reference_path": str(reference_file), "candidate_path": str(candidate_file)})["result"]
        assert not compared["identical"] and not compared["length_match"]
        assert compared["compared_bytes"] == 4 and compared["different_bytes"] == 2, compared
        reference_file.write_bytes(candidate_file.read_bytes())
        assert call("exec", "compare_files", "--input", "-", data={
            "reference_path": str(reference_file), "candidate_path": str(candidate_file)})["result"]["identical"]
        project_dir = root / "GameAudio"
        project = project_dir / "GameAudio.hsp"
        call("init", str(project_dir))
        assert project.is_file()
        call("init", str(project_dir), exit_code=1)
        verify_shell(cli, environment, root, audio_output)
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

        for invalid_request in ({"cmd": "list_items", "args": []}, {"cmd": "list_items", "arguments": {}}):
            call("run", "--project", str(project), "--input", "-", "--save", data=[
                {"cmd": "create_action", "args": {"name": "ShouldNotExist"}}, invalid_request], exit_code=2)
        execute("get_item_info", {"path": "ShouldNotExist"}, exit_code=1)
        execute("create_action", {"name": "Unsaved"})
        execute("get_item_info", {"path": "Unsaved"}, exit_code=1)
        stopped = call("run", "--project", str(project), "--input", "-", "--save", data=[
            {"cmd": "create_action", "args": {"name": "BeforeFailure"}},
            {"cmd": "get_item_info", "args": {"path": "NotAnExistingObject"}},
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
        batch = execute("batch_convert_media", {"source_ids": [imported["source_id"]], "platform_id": platform})
        assert batch["result"]["success"] and batch["result"]["completed"] == batch["result"]["total"] == 1, batch
        assert batch["result"]["results"][0]["response"]["result"]["cache_path"] == converted["result"]["cache_path"]
        for invalid in ([imported["source_id"], "invalid"], [imported["source_id"], imported["source_id"]]):
            execute("batch_convert_media", {"source_ids": invalid, "platform_id": platform}, exit_code=2)
        workflow = root / "workflow.json"
        workflow.write_text(json.dumps([
            {"cmd": "create_action", "args": {"name": "PlayTone"}},
            {"cmd": "set_item_property", "args": {"path": "PlayTone", "property": "steps",
                "value": [{"type": "Play", "targetName": "tone"}]}}
        ]), encoding="utf-8")
        call("run", "--project", str(project), "--input", str(workflow), "--save")
        execute("get_item_info", {"path": "PlayTone"})
        execute("get_action_definition", {"name": "PlayTone"})
        if audio_output:
            playback = call("run", "--project", str(project), "--input", "-", data=[
                {"cmd": "init_engine", "args": {"sample_rate": 48000, "buffer_size": 480}},
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
            {"cmd": "init_engine", "args": {"device_mode": "real" if audio_output else "null"}},
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
            execute("export_pack", {"output_path": str(rejected), **invalid}, exit_code=2)
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
    parser.add_argument("--skip-audio-output", action="store_true",
                        help="Skip hardware audio output checks for runners without an audio device.")
    args = parser.parse_args()
    verify(args.cli.resolve(), args.host.resolve() if args.host else None,
           args.work_dir.resolve(), not args.skip_audio_output)


if __name__ == "__main__":
    main()
