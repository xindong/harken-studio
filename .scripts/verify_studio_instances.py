from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path


def process_options() -> dict:
    if os.name != "nt":
        return {}
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return {"creationflags": subprocess.CREATE_NO_WINDOW, "startupinfo": startup}


def registry_directory(environment: dict[str, str]) -> Path:
    if os.name == "nt":
        root = Path(environment["LOCALAPPDATA"])
    elif sys.platform == "darwin":
        root = Path.home() / "Library" / "Application Support"
    else:
        root = Path(environment.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share")))
    return root / "Harken" / "Studio" / "instances"


@contextmanager
def local_connection(endpoint: str):
    if os.name == "nt":
        with open("\\\\.\\pipe\\" + endpoint, "r+b", buffering=0) as stream:
            yield stream
    else:
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(endpoint)
            with connection.makefile("rwb", buffering=0) as stream:
                yield stream


@contextmanager
def live_version_fixture(descriptor: dict):
    hello = (json.dumps({"protocolVersion": 1, "status": "ok", "result": {"instance": descriptor}}) + "\n").encode()
    stopped = threading.Event()
    errors = []
    endpoint = descriptor["endpoint"]
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                           wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                           wintypes.DWORD, ctypes.c_void_p]
        kernel.CreateNamedPipeW.restype = wintypes.HANDLE
        kernel.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        kernel.ConnectNamedPipe.restype = wintypes.BOOL
        kernel.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
        kernel.DisconnectNamedPipe.restype = wintypes.BOOL
        kernel.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        kernel.WriteFile.restype = wintypes.BOOL
        kernel.ReadFile.argtypes = kernel.WriteFile.argtypes
        kernel.ReadFile.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.CreateNamedPipeW("\\\\.\\pipe\\" + endpoint, 3, 0, 1, 65536, 65536, 0, None)
        assert handle != ctypes.c_void_p(-1).value, ctypes.get_last_error()

        def serve():
            try:
                while not stopped.is_set():
                    connected = kernel.ConnectNamedPipe(handle, None)
                    if not connected and ctypes.get_last_error() != 535:
                        raise OSError(ctypes.get_last_error(), "Cannot connect version fixture")
                    if stopped.is_set():
                        break
                    transferred = wintypes.DWORD()
                    assert kernel.WriteFile(handle, hello, len(hello), ctypes.byref(transferred), None)
                    request = ctypes.create_string_buffer(4096)
                    transferred.value = 0
                    kernel.ReadFile(handle, request, len(request), ctypes.byref(transferred), None)
                    assert transferred.value == 0, "An incompatible client sent a command"
                    kernel.DisconnectNamedPipe(handle)
            except BaseException as error:
                errors.append(error)
            finally:
                kernel.CloseHandle(handle)
    else:
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(endpoint)
        listener.listen()
        listener.settimeout(0.1)

        def serve():
            try:
                while not stopped.is_set():
                    try:
                        connection, _ = listener.accept()
                    except TimeoutError:
                        continue
                    with connection:
                        connection.settimeout(5)
                        connection.sendall(hello)
                        assert not connection.recv(4096), "An incompatible client sent a command"
            except BaseException as error:
                errors.append(error)
            finally:
                listener.close()
                Path(endpoint).unlink(missing_ok=True)

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        yield
    finally:
        stopped.set()
        if os.name == "nt" and worker.is_alive():
            deadline = time.monotonic() + 3
            while worker.is_alive() and time.monotonic() < deadline:
                try:
                    with local_connection(endpoint):
                        pass
                    break
                except OSError:
                    time.sleep(0.01)
        worker.join(timeout=6)
        assert not worker.is_alive(), "Version fixture did not stop"
        assert not errors, errors


def verify(cli: Path, studio: Path, work: Path) -> None:
    environment = {**os.environ, "HARKEN_HUB_USER_ROOT": str(work / "hub")}
    if os.name == "nt":
        environment["QT_QPA_PLATFORM"] = "windows"
    processes: list[subprocess.Popen] = []
    logs = []
    fixtures: list[Path] = []
    report = {"root": str(work), "checks": []}

    def call(*arguments: str, data=None, success: bool = True):
        completed = subprocess.run(
            [str(cli), *arguments, "--json"],
            input=None if data is None else json.dumps(data).encode("utf-8"),
            capture_output=True, env=environment, timeout=40, **process_options())
        try:
            response = json.loads(completed.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise AssertionError((arguments, completed.returncode, completed.stdout, completed.stderr)) from error
        assert (completed.returncode == 0) == success, (arguments, response, completed.stderr)
        assert response["status"] == ("ok" if success else "error"), (arguments, response)
        assert response["protocolVersion"] == 1, response
        return response

    def instances():
        return call("instances")["result"]["instances"]

    def execute(instance: str, command: str, arguments=None, success: bool = True):
        return call("exec", command, "--instance", instance, "--input", "-",
                    data=arguments or {}, success=success)

    def attached_shell(instance: str, lines: list[str], errors: dict[int, str] | None = None):
        completed = subprocess.run(
            [str(cli), "shell", "--instance", instance, "--json"],
            input=("\n".join(lines) + "\n").encode("utf-8"),
            capture_output=True, env=environment, timeout=40, **process_options())
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        responses = [json.loads(line) for line in completed.stdout.splitlines()]
        assert len(responses) == len(lines), responses
        for index, response in enumerate(responses):
            expected_error = (errors or {}).get(index)
            assert response["status"] == ("error" if expected_error else "ok"), responses
            if expected_error:
                assert response["code"] == expected_error, response
        assert all(response["protocolVersion"] == 1 for response in responses), responses
        return responses

    def check(description: str):
        report["checks"].append(description)
        print(description, flush=True)

    try:
        version = call("--version")["result"]["version"]
        projects = []
        for name in ("FirstProject", "SecondProject"):
            directory = work / name
            call("init", str(directory))
            projects.append(directory / (name + ".hsp"))
            log = (work / (name + ".log")).open("wb")
            logs.append(log)
            processes.append(subprocess.Popen(
                [str(studio), "--project", str(projects[-1])], stdout=log, stderr=log,
                env=environment, **process_options()))
        expected_pids = {process.pid for process in processes}
        deadline = time.monotonic() + 90
        discovered = {}
        while time.monotonic() < deadline:
            assert all(process.poll() is None for process in processes), [process.poll() for process in processes]
            discovered = {entry["pid"]: entry for entry in instances() if entry["pid"] in expected_pids}
            if set(discovered) == expected_pids and all(entry.get("project", {}).get("path") for entry in discovered.values()):
                break
            time.sleep(0.2)
        assert set(discovered) == expected_pids, discovered
        first, second = (discovered[process.pid] for process in processes)
        assert first["id"] != second["id"], discovered
        for index, entry in enumerate((first, second)):
            assert entry["version"] == version, entry
            assert entry["available"] is True, entry
            assert Path(entry["executable"]).resolve() == studio.resolve(), entry
            assert Path(entry["project"]["path"]).resolve() == projects[index].resolve(), entry
        first_id, second_id = first["id"], second["id"]
        fixtures.extend(registry_directory(environment) / (entry["id"] + ".json") for entry in (first, second))
        report["instances"] = [first, second]
        for instance_id in (first_id, second_id):
            execute(instance_id, "save_project")
            assert execute(instance_id, "get_project_state")["result"]["is_dirty"] is False
        check("Two simultaneous desktop instances discovered by identity and project with matching software versions")

        core = {entry["command"]: entry for entry in call("commands")["result"]["commands"]}
        desktop = {entry["command"]: entry for entry in call("commands", "--instance", first_id)["result"]["commands"]}
        shared = {name: entry for name, entry in desktop.items() if entry["scope"] == "shared"}
        assert shared == core, (core.keys() - shared.keys(), shared.keys() - core.keys())
        assert "list_ui_windows" in desktop and "list_ui_windows" not in core
        detail = call("commands", "list_ui_windows", "--instance", first_id)
        help_response = call("exec", "list_ui_windows", "--instance", first_id, "--help")
        assert detail["result"] == help_response["result"], (detail, help_response)
        assert execute(first_id, "get_project_state")["result"]["is_dirty"] is False
        execute(first_id, "create_action", {"name": "LiveUnsavedAction"})
        assert execute(first_id, "get_project_state")["result"]["is_dirty"] is True
        execute(first_id, "get_item_info", {"path": "LiveUnsavedAction"})
        execute(second_id, "get_item_info", {"path": "LiveUnsavedAction"}, success=False)
        assert execute(second_id, "get_project_state")["result"]["is_dirty"] is False
        assert execute(first_id, "close_project", success=False)["code"] == "UNSAVED_CHANGES"
        assert execute(first_id, "open_project", {"path": str(projects[1])}, success=False)["code"] == "UNSAVED_CHANGES"
        nested = execute(first_id, "batch", {"commands": [{"cmd": "close_project"}]}, success=False)
        assert nested["code"] == "BATCH_FAILED", nested
        assert nested["result"]["results"][0]["code"] == "UNSAVED_CHANGES", nested
        execute(first_id, "get_item_info", {"path": "LiveUnsavedAction"})
        call("exec", "get_project_info", "--instance", first_id, "--project", str(projects[0]), success=False)
        call("exec", "get_project_info", "--project", str(projects[0]), success=False)
        check("Attached commands use desktop contracts and preserve unsaved state across target isolation and refused project switching")

        windows = execute(first_id, "list_ui_windows")["result"]["windows"]
        main = next(window for window in windows if window["name"] == "MainWindow")
        execute(first_id, "get_ui_tree", {"widget_id": main["widget_id"], "depth": 0})
        before = execute(first_id, "list_inspector_panels")["result"]["count"]
        rejected = call("run", "--instance", first_id, "--input", "-", data=[
            {"cmd": "create_inspector_panel"},
            {"cmd": "get_project_info", "args": {"__invalid_argument__": True}}], success=False)
        assert rejected["code"] == "INVALID_ARGUMENTS", rejected
        assert execute(first_id, "list_inspector_panels")["result"]["count"] == before
        execute(first_id, "create_inspector_panel")
        assert execute(first_id, "list_inspector_panels")["result"]["count"] == before + 1
        check("GUI commands share the live window and run preflight rejects invalid batches before any UI mutation")

        attached_shell(first_id, [
            'create_action {"name":"AttachedEofAction"}', "get_project_state"])
        attached_shell(first_id, ["commands list_ui_windows", "close_project", "exit"],
                       errors={1: "UNSAVED_CHANGES"})
        assert all(process.poll() is None for process in processes)
        execute(first_id, "get_item_info", {"path": "AttachedEofAction"})
        assert execute(first_id, "get_project_state")["result"]["is_dirty"] is True
        execute(second_id, "get_item_info", {"path": "AttachedEofAction"}, success=False)
        check("Attached shell EOF and exit detach while preserving the GUI session and its unsaved edits")

        registry = registry_directory(environment)
        assert (registry / (first_id + ".json")).is_file(), registry
        incompatible_id = str(uuid.uuid4())
        incompatible = {**first, "id": incompatible_id, "version": "9999.0.0",
                        "endpoint": "harken-version-verifier-" + incompatible_id}
        if os.name != "nt":
            incompatible["endpoint"] = str(Path(tempfile.gettempdir()) / (incompatible_id + ".sock"))
        incompatible_path = registry / (incompatible_id + ".json")
        fixtures.append(incompatible_path)
        with live_version_fixture(incompatible):
            incompatible_path.write_text(json.dumps(incompatible), encoding="utf-8")
            unavailable = next(entry for entry in instances() if entry["id"] == incompatible_id)
            assert unavailable["version"] == "9999.0.0" and unavailable["available"] is False, unavailable
            assert unavailable["unavailableReason"], unavailable
            assert execute(incompatible_id, "get_project_info", success=False)["code"] == "VERSION_MISMATCH"
        incompatible_path.unlink()
        with local_connection(first["endpoint"]) as stream:
            hello = json.loads(stream.readline())
            assert hello["result"]["instance"]["id"] == first_id, hello
            request = {"protocolVersion": 1, "version": "9999.0.0", "instance": first_id,
                       "request_id": str(uuid.uuid4()), "cmd": "create_action",
                       "args": {"name": "VersionRejectedAction"}}
            stream.write((json.dumps(request) + "\n").encode())
            rejected = json.loads(stream.readline())
            assert rejected["code"] == "VERSION_MISMATCH", rejected
            assert rejected["request_id"] == request["request_id"], rejected
        execute(first_id, "get_item_info", {"path": "VersionRejectedAction"}, success=False)
        for mismatched in (False, True):
            fixture_id = str(uuid.uuid4())
            fixture = {**first, "id": fixture_id}
            if mismatched:
                fixture["version"] = "9999.0.0"
            else:
                fixture["endpoint"] = "harken-verifier-missing-" + fixture_id
            path = registry / (fixture_id + ".json")
            path.write_text(json.dumps(fixture), encoding="utf-8")
            fixtures.append(path)
            assert fixture_id not in {entry["id"] for entry in instances()}
            execute(fixture_id, "get_project_info", success=False)
        missing_id = str(uuid.uuid4())
        execute(missing_id, "get_project_info", success=False)
        call("run", "--instance", missing_id, "--input", "-", data=[], success=False)
        check("Discovery lists live incompatible versions as unavailable, filters dead records, and both peers reject mismatched execution")

        processes[1].terminate()
        processes[1].wait(timeout=30)
        assert second_id not in {entry["id"] for entry in instances()}
        execute(second_id, "get_project_info", success=False)
        execute(first_id, "get_item_info", {"path": "LiveUnsavedAction"})
        call("run", "--instance", first_id, "--input", "-", "--save", data=[
            {"cmd": "get_project_info"}, {"cmd": "get_project_state"}])
        assert execute(first_id, "get_project_state")["result"]["is_dirty"] is False
        check("Disconnected instances cannot receive commands; remaining instance continues and explicit save persists edits")
        platform = execute(first_id, "list_project_platforms")["result"]["platforms"][0]["id"]
        output_directory = work / "attached-export"
        exported = execute(first_id, "export_pack", {
            "output_path": str(output_directory), "platform_id": platform, "all": True})
        assert exported["result"]["success"] and not exported["result"].get("async"), exported
        assert execute(first_id, "get_pack_build_status")["result"]["in_progress"] is False
        assert any(output_directory.rglob("*.bytes")), exported
        check("Attached asynchronous authoring commands return completed results with output artifacts ready")
        report["ok"] = True
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        for log in logs:
            log.close()
        for path in fixtures:
            path.unlink(missing_ok=True)
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("Report: " + str(work / "report.json"), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify Studio CLI instance selection, contracts and live session ownership.")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--studio", type=Path, required=True)
    parser.add_argument("--work", type=Path)
    options = parser.parse_args()
    work = options.work or Path(tempfile.mkdtemp(prefix="harken-studio-instances-"))
    work.mkdir(parents=True, exist_ok=True)
    verify(options.cli.resolve(), options.studio.resolve(), work.resolve())
