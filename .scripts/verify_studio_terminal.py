from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import traceback

def hidden_options():
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return {"startupinfo": startup, "creationflags": subprocess.CREATE_NO_WINDOW}


def worker(work: Path, host: str, cli: Path, studio: Path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel.GetConsoleWindow.restype = wintypes.HWND
    user.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsWindowVisible.restype = wintypes.BOOL
    window = kernel.GetConsoleWindow()
    assert window, "Worker has no console"
    user.ShowWindow(window, 0)
    assert not user.IsWindowVisible(window), "Test console must remain hidden"

    class COORD(ctypes.Structure):
        _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]

    class SMALL_RECT(ctypes.Structure):
        _fields_ = [("Left", wintypes.SHORT), ("Top", wintypes.SHORT),
                    ("Right", wintypes.SHORT), ("Bottom", wintypes.SHORT)]

    class SCREEN_INFO(ctypes.Structure):
        _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD),
                    ("wAttributes", wintypes.WORD), ("srWindow", SMALL_RECT),
                    ("dwMaximumWindowSize", COORD)]

    class KEY_EVENT(ctypes.Structure):
        _fields_ = [("bKeyDown", wintypes.BOOL), ("wRepeatCount", wintypes.WORD),
                    ("wVirtualKeyCode", wintypes.WORD), ("wVirtualScanCode", wintypes.WORD),
                    ("UnicodeChar", wintypes.WCHAR), ("dwControlKeyState", wintypes.DWORD)]

    class EVENT_UNION(ctypes.Union):
        _fields_ = [("key", KEY_EVENT), ("padding", ctypes.c_byte * 16)]

    class INPUT_RECORD(ctypes.Structure):
        _fields_ = [("EventType", wintypes.WORD), ("event", EVENT_UNION)]

    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.SetConsoleScreenBufferSize.argtypes = [wintypes.HANDLE, COORD]
    kernel.GetConsoleScreenBufferInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(SCREEN_INFO)]
    kernel.ReadConsoleOutputCharacterW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR,
        wintypes.DWORD, COORD, ctypes.POINTER(wintypes.DWORD)]
    kernel.WriteConsoleInputW.argtypes = [wintypes.HANDLE, ctypes.POINTER(INPUT_RECORD),
        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetConsoleProcessList.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    console_in = kernel.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
    console_out = kernel.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)
    assert console_in != ctypes.c_void_p(-1).value and console_out != ctypes.c_void_p(-1).value
    kernel.SetConsoleScreenBufferSize(console_out, COORD(180, 2000))
    kernel.SetStdHandle.argtypes = [wintypes.DWORD, wintypes.HANDLE]
    kernel.SetHandleInformation.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
    assert kernel.SetHandleInformation(console_in, 1, 1)
    assert kernel.SetHandleInformation(console_out, 1, 1)
    assert kernel.SetStdHandle(-10, console_in)
    assert kernel.SetStdHandle(-11, console_out)
    assert kernel.SetStdHandle(-12, console_out)

    report = {"checks": [], "console_hidden": True, "work": str(work), "host": host}
    kernel.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]

    def mode():
        m = wintypes.DWORD()
        assert kernel.GetConsoleMode(console_in, ctypes.byref(m))
        return m.value

    studio_pid = None
    studio_handle = None
    registration = None
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    process = None
    environment = {**os.environ, "HARKEN_HUB_USER_ROOT": str(work / "hub"), "QT_QPA_PLATFORM": "windows"}

    def record(message):
        report["checks"].append(message)
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    def call(*arguments):
        result = subprocess.run([str(cli), *arguments, "--json"], capture_output=True,
            timeout=25, env=environment, **hidden_options())
        parsed = json.loads(result.stdout)
        return result.returncode, parsed

    def execute(instance, name, args=None):
        code, result = call("exec", name, "--instance", instance, "--args", json.dumps(args or {}))
        return code, result

    def screen():
        info = SCREEN_INFO()
        assert kernel.GetConsoleScreenBufferInfo(console_out, ctypes.byref(info))
        size = info.dwSize.X * info.dwSize.Y
        text = ctypes.create_unicode_buffer(size)
        read = wintypes.DWORD()
        assert kernel.ReadConsoleOutputCharacterW(console_out, text, size, COORD(0, 0), ctypes.byref(read))
        return "\n".join(text.value[index:index + info.dwSize.X].rstrip()
            for index in range(0, len(text.value), info.dwSize.X)).rstrip()

    def send(line):
        chars = line + "\r"
        records = (INPUT_RECORD * (len(chars) * 2))()
        for index, char in enumerate(chars):
            for release in (0, 1):
                record = records[index * 2 + release]
                record.EventType = 1
                record.event.key = KEY_EVENT(not release, 1,
                    13 if char == "\r" else ord(char.upper()) if char.isascii() and char.isalpha() else 0,
                    0, char, 0)
        written = wintypes.DWORD()
        assert kernel.WriteConsoleInputW(console_in, records, len(records), ctypes.byref(written))
        assert written.value == len(records)

    def await_action(instance, name):
        deadline = time.monotonic() + 20
        last = None
        while time.monotonic() < deadline:
            assert process.poll() is None, (process.returncode, screen())
            code, last = execute(instance, "get_item_info", {"path": name})
            if code == 0:
                return last
            time.sleep(0.1)
        raise AssertionError((name, last, screen()))

    try:
        project_dir = work / "Console Project"
        if host != "wait":
            assert call("init", str(project_dir))[0] == 0
        if host == "raw":
            assert kernel.SetConsoleMode(console_in, (mode() & ~7) | 0x200)
            report["mode_before_launch"] = mode()
            process = subprocess.Popen([str(studio), "--shell", "--json", "--project",
                str(project_dir / "Console Project.hsp")], env=environment, close_fds=True)
            record("Launched Studio with inherited raw console input without line mode or echo")
        else:
            process = subprocess.Popen(["powershell.exe", "-NoLogo", "-NoProfile"],
                env=environment, close_fds=False)
            time.sleep(1)
            report["mode_before_launch"] = mode()
            if host == "wait":
                launch = f"Start-Process '{studio}' -ArgumentList '--shell --json --create-project \"{project_dir}\"' -NoNewWindow -Wait"
            else:
                launch = f"& '{studio}' --shell --json --project '{project_dir / 'Console Project.hsp'}' | Out-Host"
            send(launch)
            record("Launched Studio by typing into an interactive parent shell")
        deadline = time.monotonic() + 90
        descriptor = None
        while time.monotonic() < deadline:
            assert process.poll() is None, (process.returncode, screen())
            _, response = call("instances")
            descriptor = next((entry for entry in response["result"]["instances"]
                if entry.get("project", {}).get("name") == "Console Project"
                and Path(entry["project"]["path"]).resolve()
                    == (project_dir / "Console Project.hsp").resolve()), None)
            if descriptor:
                break
            time.sleep(0.2)
        assert descriptor, screen()
        instance = descriptor["id"]
        registration = Path(os.environ["LOCALAPPDATA"]) / "Harken/Studio/instances" / (instance + ".json")
        studio_pid = descriptor["pid"]
        report["pid"] = studio_pid
        studio_handle = kernel.OpenProcess(0x100000 | 0x1000, False, studio_pid)
        assert studio_handle, ctypes.get_last_error()
        report["mode_after_launch"] = mode()
        time.sleep(1)
        (work / "before-input.txt").write_text(screen(), encoding="utf-8")
        process_ids = (wintypes.DWORD * 32)()
        count = kernel.GetConsoleProcessList(process_ids, len(process_ids))
        assert studio_pid in process_ids[:min(count, len(process_ids))], list(process_ids)
        assert not user.IsWindowVisible(window)
        record("Studio attached to the existing hidden console; no new console was allocated")

        send('create_action {"name":"ConsoleFirstCommand"}')
        await_action(instance, "ConsoleFirstCommand")
        record("Native ReadConsoleW input executed a shared core command in the live GUI project")

        send('create_action {"name":"ConsoleSecondCommand"}')
        await_action(instance, "ConsoleSecondCommand")
        record("The interactive terminal continues accepting subsequent commands")

        assert execute(instance, "save_project")[0] == 0
        assert execute(instance, "get_project_state")[1]["result"]["is_dirty"] is False
        contents = screen()
        (work / "console.txt").write_text(contents, encoding="utf-8")
        assert '"command":"create_action"' in contents and '"status":"ok"' in contents, contents
        record("Native console output contains JSON command results; external CLI sees and saves the same state")

        window_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        candidates = []

        @window_callback
        def collect(hwnd, context):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == studio_pid:
                title = ctypes.create_unicode_buffer(512)
                user.GetWindowTextW(hwnd, title, len(title))
                if user.IsWindowVisible(hwnd):
                    candidates.append((hwnd, title.value))
            return True

        user.EnumWindows.argtypes = [window_callback, wintypes.LPARAM]
        assert user.EnumWindows(collect, 0)
        main = next((hwnd for hwnd, title in candidates if "Console Project" in title), None)
        assert main, candidates
        start = time.monotonic()
        if host == "raw":
            send("exit")
        else:
            assert user.PostMessageW(main, 0x0010, 0, 0)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            _, found = call("instances")
            if not any(entry["pid"] == studio_pid for entry in found["result"]["instances"]):
                break
            time.sleep(0.1)
        assert kernel.WaitForSingleObject(studio_handle, 20000) == 0, "Studio process did not exit"
        studio_exit = wintypes.DWORD()
        assert kernel.GetExitCodeProcess(studio_handle, ctypes.byref(studio_exit))
        report["studio_exit"] = studio_exit.value
        assert studio_exit.value == 0, studio_exit.value
        report["close_seconds"] = time.monotonic() - start
        report["mode_after_close"] = mode()
        if host == "raw":
            assert mode() == report["mode_before_launch"], report
            record("Shell exit closes Studio and restores the exact inherited raw input mode")
        else:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and not screen().rstrip().endswith(">"):
                time.sleep(0.1)
            assert screen().rstrip().endswith(">"), screen()
            report["prompt_seconds"] = time.monotonic() - start
            send("Write-Output TERMINAL_RETURNED")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and "TERMINAL_RETURNED" not in screen().splitlines():
                time.sleep(0.1)
            restored = screen()
            (work / "after-close.txt").write_text(restored, encoding="utf-8")
            assert "TERMINAL_RETURNED" in restored.splitlines(), restored
            send("exit")
        result = process.wait(timeout=5)
        report["child_exit"] = result
        assert result == 0, result
        record("Closing the GUI exits its exact process and returns control to its parent")
        report["ok"] = True
    except BaseException:
        report["error"] = traceback.format_exc()
        try:
            (work / "console.txt").write_text(screen(), encoding="utf-8")
        except BaseException:
            pass
        raise
    finally:
        if studio_handle:
            if kernel.WaitForSingleObject(studio_handle, 0) != 0:
                kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
                terminate = kernel.OpenProcess(1 | 0x100000, False, studio_pid)
                if terminate:
                    kernel.TerminateProcess(terminate, 1)
                    kernel.WaitForSingleObject(terminate, 10000)
                    kernel.CloseHandle(terminate)
            kernel.CloseHandle(studio_handle)
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=20)
        if registration:
            registration.unlink(missing_ok=True)
        kernel.CloseHandle(console_in)
        kernel.CloseHandle(console_out)
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify Studio shell ownership and GUI exit through a real interactive PowerShell terminal.")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--studio", type=Path, required=True)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--worker", choices=("wait", "pipeline", "raw"))
    options = parser.parse_args()
    if os.name != "nt":
        print("Skipped: this verifier exercises Windows PowerShell console ownership.")
        raise SystemExit(0)
    if options.worker:
        worker(options.work.resolve(), options.worker, options.cli.resolve(), options.studio.resolve())
    else:
        work = options.work or Path(tempfile.mkdtemp(prefix="studio-terminal-"))
        work.mkdir(parents=True, exist_ok=True)
        for host in ("wait", "pipeline", "raw"):
            scenario = work / host
            scenario.mkdir(parents=True, exist_ok=True)
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            with (scenario / "worker.log").open("wb") as log:
                child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                    "--worker", host, "--work", str(scenario.resolve()),
                    "--cli", str(options.cli.resolve()), "--studio", str(options.studio.resolve())],
                    stdout=log, stderr=log, creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
                try:
                    result = child.wait(timeout=150)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    child.wait(timeout=20)
                    raise
            report = scenario / "report.json"
            print(f"{host}: exit {result}; report: {report}", flush=True)
            assert result == 0, report.read_text(encoding="utf-8") if report.exists() else str(scenario)
        print("Interactive PowerShell input, GUI process exit and parent terminal recovery passed.")
