import json
import os
import queue
import subprocess
import threading
from pathlib import Path

from drivers.process import capture_bounded, hidden_process_options, kill_tree


class StudioCommandError(RuntimeError):
    def __init__(self, message: str, code: str | None = None):
        super().__init__(message)
        self.code = code


class StudioCli:
    def __init__(self, executable: Path, work: Path, timeout: float = 300):
        self.executable = executable
        self.work = work
        self.timeout = timeout
        self.log = work / "studio-cli.log"
        self.environment = {**os.environ, "HARKEN_HUB_USER_ROOT": str(work / "hub-user")}

    def _invoke(self, arguments: list[str], payload=None) -> dict:
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        code, out = capture_bounded([str(self.executable), *arguments, "--json"], timeout=self.timeout,
                                    log=self.log, stdin=data, env=self.environment)
        try:
            response = json.loads(out)
        except json.JSONDecodeError as error:
            raise StudioCommandError(f"{arguments[0]} exited {code} without a JSON response; see {self.log}") from error
        if code != 0 or response.get("status") != "ok":
            raise StudioCommandError(f"{arguments[0]} failed ({code}): {json.dumps(response)[:2000]}",
                                     response.get("code"))
        return response

    def version(self) -> str:
        return self._invoke(["--version"])["result"]["version"]

    def command_schema(self, name: str) -> dict:
        return self._invoke(["commands", name])["result"]

    def create_project(self, directory: Path) -> Path:
        self._invoke(["init", str(directory)])
        return directory / f"{directory.name}.hsp"

    def run(self, project: Path, requests: list[dict], save: bool = False) -> list[dict]:
        arguments = ["run", "--project", str(project), "--input", "-"]
        if save:
            arguments.append("--save")
        return [entry["result"] for entry in self._invoke(arguments, requests)["results"]]

    def call(self, project: Path, command: str, arguments: dict | None = None, save: bool = False):
        return self.run(project, [{"cmd": command, "args": arguments or {}}], save)[0]

    def session(self, project: Path) -> "StudioSession":
        return StudioSession(self, project)


class StudioSession:
    def __init__(self, cli: StudioCli, project: Path):
        self.cli = cli
        self.log = cli.log.open("a", encoding="utf-8")
        self.process = subprocess.Popen(
            [str(cli.executable), "shell", "--project", str(project), "--json"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.log, env=cli.environment, **hidden_process_options())
        self.responses = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.responses.put(line)
        self.responses.put(None)

    def call(self, command: str, arguments: dict | None = None):
        line = f"{command} {json.dumps(arguments or {})}\n"
        self.log.write("shell> " + line)
        self.process.stdin.write(line.encode("utf-8"))
        self.process.stdin.flush()
        try:
            raw = self.responses.get(timeout=self.cli.timeout)
        except queue.Empty as error:
            raise StudioCommandError(f"shell {command} timed out after {self.cli.timeout}s") from error
        if raw is None:
            raise StudioCommandError(f"shell exited while running {command}")
        self.log.write(raw.decode("utf-8", "replace"))
        response = json.loads(raw)
        if response.get("status") != "ok":
            raise StudioCommandError(f"shell {command} failed: {json.dumps(response)[:2000]}", response.get("code"))
        return response.get("result", {})

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write(b"exit --discard\n")
                self.process.stdin.flush()
                self.process.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            pass
        kill_tree(self.process)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
