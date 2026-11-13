import os
import signal
import subprocess
from pathlib import Path


def hidden_process_options() -> dict:
    if os.name != "nt":
        return {"start_new_session": True}
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return {"creationflags": subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP,
            "startupinfo": startup}


def kill_tree(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        os.killpg(process.pid, signal.SIGKILL)
    process.wait(timeout=30)


def run_bounded(command: list[str], *, timeout: float, log: Path, stdin: bytes | None = None,
                env: dict | None = None, cwd: Path | None = None) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as stream:
        stream.write(("$ " + subprocess.list2cmdline(command) + "\n").encode("utf-8"))
        stream.flush()
        process = subprocess.Popen(command, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, env=env, cwd=cwd, **hidden_process_options())
        try:
            process.communicate(stdin, timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(process)
            stream.write(f"\n[crossproduct-test] killed after {timeout}s timeout\n".encode("utf-8"))
            return -1
        return process.returncode


def capture_bounded(command: list[str], *, timeout: float, log: Path, stdin: bytes | None = None,
                    env: dict | None = None) -> tuple[int, bytes]:
    log.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(command, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, **hidden_process_options())
    try:
        out, err = process.communicate(stdin, timeout=timeout)
        code = process.returncode
    except subprocess.TimeoutExpired:
        kill_tree(process)
        out, err = process.communicate()
        code = -1
    with log.open("ab") as stream:
        stream.write(("$ " + subprocess.list2cmdline(command) + "\n").encode("utf-8"))
        if stdin:
            stream.write(b"<stdin>\n" + stdin + b"\n")
        stream.write(out + b"\n" + err + f"\n[exit {code}]\n".encode("utf-8"))
    return code, out
