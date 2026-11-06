from __future__ import annotations

import argparse
import json
import os
import queue
import subprocess
import tempfile
import threading
import time
import wave
from pathlib import Path

from verify_studio_instances import process_options, registry_directory


def verify(cli: Path, studio: Path, work: Path) -> None:
    environment = {**os.environ, "HARKEN_HUB_USER_ROOT": str(work / "hub")}
    if os.name == "nt":
        environment["QT_QPA_PLATFORM"] = "windows"
    processes = []
    readers = []
    logs = []
    registrations = []
    report = {"root": str(work), "checks": []}

    def call(*arguments: str, data=None, success=True):
        completed = subprocess.run(
            [str(cli), *arguments, "--json"],
            input=None if data is None else json.dumps(data).encode("utf-8"),
            capture_output=True, env=environment, timeout=40, **process_options())
        try:
            response = json.loads(completed.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise AssertionError((arguments, completed.returncode, completed.stdout, completed.stderr)) from error
        assert (completed.returncode == 0) == success, (arguments, response, completed.stderr)
        assert response["status"] == ("ok" if success else "error"), response
        return response

    def execute(instance: str, command: str, arguments=None, success=True):
        return call("exec", command, "--instance", instance, "--input", "-",
                    data=arguments or {}, success=success)

    def startup(*arguments: str, success=False, unavailable_input=False):
        completed = subprocess.run(
            [str(studio), *arguments],
            **({"stdin": subprocess.DEVNULL} if unavailable_input else {"input": b""}),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
            timeout=20, **process_options())
        assert (completed.returncode == 0) == success, (arguments, completed.returncode, completed.stdout, completed.stderr)
        response = json.loads(completed.stdout)
        assert response["status"] == ("ok" if success else "error"), response
        assert response["protocolVersion"] == 1, response
        if not success:
            assert response.get("code"), response
        return response

    def launch(name: str, shell=True, read_output=True):
        directory = work / name
        call("init", str(directory))
        project = directory / (name + ".hsp")
        error_log = (work / (name + ".stderr.log")).open("wb")
        output_log = (work / (name + ".stdout.log")).open("wb")
        logs.extend((error_log, output_log))
        process = subprocess.Popen(
            [str(studio), "--project", str(project), *(["--shell"] if shell else [])],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error_log,
            env=environment, **process_options())
        processes.append(process)
        responses = queue.Queue()

        def receive_output():
            try:
                for line in iter(process.stdout.readline, b""):
                    output_log.write(line)
                    output_log.flush()
                    responses.put(line)
            finally:
                responses.put(None)

        if read_output:
            reader = threading.Thread(target=receive_output, daemon=True)
            reader.start()
            readers.append(reader)
        deadline = time.monotonic() + 90
        instance = None
        while time.monotonic() < deadline:
            assert process.poll() is None, (name, process.returncode, error_log.name, output_log.name)
            entries = call("instances")["result"]["instances"]
            instance = next((entry for entry in entries if entry["pid"] == process.pid), None)
            if instance:
                break
            time.sleep(0.1)
        assert instance and instance["available"], (name, instance)
        assert Path(instance["project"]["path"]).resolve() == project.resolve(), instance
        registrations.append(registry_directory(environment) / (instance["id"] + ".json"))
        execute(instance["id"], "save_project")
        return process, responses, instance, project

    def send(process, responses, line: str, error=None):
        assert process.poll() is None, process.returncode
        process.stdin.write((line + "\n").encode("utf-8"))
        process.stdin.flush()
        try:
            raw = responses.get(timeout=30)
        except queue.Empty as timeout:
            raise AssertionError(("No response before EOF", line, process.poll())) from timeout
        assert raw is not None, (line, process.poll())
        response = json.loads(raw)
        assert response["status"] == ("error" if error else "ok"), (line, response)
        assert response["protocolVersion"] == 1, response
        if error:
            assert response["code"] == error, response
        return response

    def close_gui(process, instance: str):
        execute(instance, "save_project")
        windows = execute(instance, "list_ui_windows")["result"]["windows"]
        main = next(window for window in windows if window["name"] == "MainWindow")
        buttons = execute(instance, "find_widget", {
            "name": "titleBarClose", "scope_widget_id": main["widget_id"]})["result"]["widgets"]
        assert len(buttons) == 1, buttons
        execute(instance, "click_widget", {"widget_id": buttons[0]["widget_id"]})
        assert process.wait(timeout=20) == 0, process.returncode

    def check(description: str):
        report["checks"].append(description)
        print(description, flush=True)

    try:
        for arguments in (("--shell",), ("--shell", "--project"),
                          ("--shell", "--unknown-option"), ("--shell", "--project", str(work / "missing.hsp"))):
            startup(*arguments)
        help_response = startup("--shell", "--help", "--json", success=True, unavailable_input=True)
        assert "--shell" in help_response["result"]["help"], help_response
        version = startup("--version", "--json", success=True, unavailable_input=True)
        assert version["result"]["version"] == call("--version")["result"]["version"], version
        check("Shell startup validates missing projects, values and unknown options; help and version work without stdin")

        process, responses, instance, project = launch("DirectShellEof")
        instance_id = instance["id"]
        duplicate = startup("--project", str(project), "--shell")
        assert duplicate["status"] == "error" and process.poll() is None, duplicate
        info = send(process, responses, "get_project_info")
        assert Path(info["result"]["path"]).resolve() == project.resolve(), info
        send(process, responses, 'create_action {"name":"FromDirectShell"}')
        execute(instance_id, "get_item_info", {"path": "FromDirectShell"})
        execute(instance_id, "create_action", {"name": "FromAttachedCli"})
        send(process, responses, 'get_item_info {"path":"FromAttachedCli"}')
        unicode_name = "Shell_\u58f0\u97f3_\u03a9"
        created = send(process, responses, "create_bus " + json.dumps({"name": unicode_name}, ensure_ascii=False))
        assert created["result"]["name"] == unicode_name, created
        execute(instance_id, "get_bus_volume_db", {"bus_name": unicode_name})
        send(process, responses, "get_bus_volume_db " + json.dumps({"bus_name": unicode_name}, ensure_ascii=False))
        assert send(process, responses, "get_project_state")["result"]["is_dirty"] is True
        desktop = send(process, responses, "list_ui_windows")["result"]["windows"]
        assert any(window["name"] == "MainWindow" for window in desktop), desktop
        send(process, responses, "__invalid_shell_command__", error="UNKNOWN_COMMAND")
        send(process, responses, "close_project", error="UNSAVED_CHANGES")
        send(process, responses, "get_project_info")
        check("Direct shell responds before EOF, preserves UTF-8 input and shares GUI state; duplicate shell startup cannot attach silently")

        process.stdin.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            assert process.poll() is None, process.returncode
            windows = execute(instance_id, "list_ui_windows")["result"]["windows"]
            if any(window.get("visible") and window.get("modal") for window in windows):
                break
            time.sleep(0.05)
        else:
            raise AssertionError(("Dirty EOF did not ask about unsaved edits", windows))
        execute(instance_id, "answer_dialog", {"response": "cancel"})
        execute(instance_id, "get_item_info", {"path": "FromDirectShell"})
        execute(instance_id, "get_item_info", {"path": "FromAttachedCli"})
        assert execute(instance_id, "get_project_state")["result"]["is_dirty"] is True
        assert process.poll() is None
        close_gui(process, instance_id)
        check("Dirty EOF requests normal GUI close; cancelling preserves edits and the application can still save and close")

        process, responses, instance, exit_project = launch("DirectShellExit")
        send(process, responses, 'create_action {"name":"SavedBeforeExit"}')
        send(process, responses, "exit", error="UNSAVED_CHANGES")
        assert process.poll() is None
        send(process, responses, 'get_item_info {"path":"SavedBeforeExit"}')
        send(process, responses, "save_project")
        send(process, responses, "exit")
        assert process.wait(timeout=20) == 0, process.returncode
        call("exec", "get_item_info", "--project", str(exit_project), "--input", "-",
             data={"path": "SavedBeforeExit"})
        check("Direct exit refuses unsaved edits; saving then exiting terminates Studio and persists the project")

        process, responses, instance, discard_project = launch("DirectShellDiscard")
        send(process, responses, 'create_action {"name":"SavedBeforeDiscard"}')
        send(process, responses, "save_project")
        sources = discard_project.parent / "AudioSources"
        saved_sources = {path.relative_to(sources): path.read_bytes()
                         for path in sources.rglob("*") if path.is_file()}
        audio = work / "discard.wav"
        with wave.open(str(audio), "wb") as stream:
            stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            stream.writeframes(b"\x00\x00" * 4800)
        send(process, responses, "import_audio_files " + json.dumps({"files": [str(audio)]}))
        imported_sources = {path.relative_to(sources): path.read_bytes()
                            for path in sources.rglob("*") if path.is_file()}
        assert imported_sources != saved_sources, imported_sources.keys()
        send(process, responses, 'create_action {"name":"DiscardedOnExit"}')
        send(process, responses, "exit --discard")
        assert process.wait(timeout=20) == 0, process.returncode
        discarded_sources = {path.relative_to(sources): path.read_bytes()
                             for path in sources.rglob("*") if path.is_file()}
        assert discarded_sources == saved_sources, discarded_sources.keys()
        reopened = call("exec", "get_project_info", "--project", str(discard_project))
        names = {pack["name"] for pack in reopened["result"]["packs"]}
        assert "SavedBeforeDiscard" in names and "DiscardedOnExit" not in names, reopened
        check("Direct exit --discard terminates Studio, rolls back imported source files and preserves previously saved data")

        process, responses, instance, _ = launch("DirectShellCleanEof")
        send(process, responses, "get_project_info")
        process.stdin.close()
        assert process.wait(timeout=20) == 0, process.returncode
        check("Clean EOF terminates the Studio process and its GUI")

        process, responses, instance, _ = launch("DirectShellPendingInput")
        send(process, responses, "get_project_info")
        process.stdin.write(b"get_project_info {")
        process.stdin.flush()
        time.sleep(0.1)
        close_gui(process, instance["id"])
        check("Closing the GUI terminates the process while stdin remains open with an incomplete command")

        process, _, instance, _ = launch("DirectShellOutputClosed", read_output=False)
        process.stdout.close()
        process.stdin.write(b'create_action {"name":"SurvivesControllerOutputClose"}\n')
        process.stdin.flush()
        deadline = time.monotonic() + 10
        found = False
        while time.monotonic() < deadline:
            assert process.poll() is None, process.returncode
            info = execute(instance["id"], "get_project_info")
            if any(item["name"] == "SurvivesControllerOutputClose" for item in info["result"]["packs"]):
                found = True
                break
            time.sleep(0.05)
        assert found, info
        assert execute(instance["id"], "get_project_state")["result"]["is_dirty"] is True
        close_gui(process, instance["id"])
        check("Controller stdout disconnection ends shell output without terminating GUI or losing applied edits")

        process, responses, instance, _ = launch("NormalGui", shell=False)
        process.stdin.write(b"get_project_info\n")
        process.stdin.flush()
        execute(instance["id"], "get_project_info")
        try:
            unexpected = responses.get(timeout=0.25)
        except queue.Empty:
            unexpected = None
        assert unexpected is None, unexpected
        assert process.poll() is None
        close_gui(process, instance["id"])
        check("Normal GUI startup remains independent of stdin and does not emit shell responses")

        startup("--project", str(project), "--shell", unavailable_input=True)
        check("Requesting a shell without usable stdin fails promptly with a structured diagnostic")
        report["ok"] = True
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
        for reader in readers:
            reader.join(timeout=5)
        for process in processes:
            if process.stdout:
                process.stdout.close()
        for log in logs:
            log.close()
        for registration in registrations:
            registration.unlink(missing_ok=True)
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("Report: " + str(work / "report.json"), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify Studio --shell input, shared live state and GUI lifetime.")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--studio", type=Path, required=True)
    parser.add_argument("--work", type=Path)
    options = parser.parse_args()
    work = options.work or Path(tempfile.mkdtemp(prefix="harken-studio-shell-"))
    work.mkdir(parents=True, exist_ok=True)
    verify(options.cli.resolve(), options.studio.resolve(), work.resolve())
