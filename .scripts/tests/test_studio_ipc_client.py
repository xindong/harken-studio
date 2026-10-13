"""Behavior tests for the repository's Harken Studio IPC PowerShell client."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import threading
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CLIENT = REPO_ROOT / ".scripts" / "studio" / "harken_ipc.ps1"


def powershell_executable() -> str | None:
    for candidate in ("pwsh", "powershell"):
        executable = shutil.which(candidate)
        if executable:
            return executable
    return None


class OneShotServer:
    def __init__(self, response: dict[str, object]) -> None:
        self.response = response
        self.request: dict[str, object] | None = None
        self.error: Exception | None = None
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(1)
        self.port = self.socket.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self) -> "OneShotServer":
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.thread.join(timeout=5)
        self.socket.close()
        if self.thread.is_alive():
            raise AssertionError("IPC mock server did not finish.")
        if self.error is not None:
            raise self.error

    def _serve(self) -> None:
        try:
            connection, _ = self.socket.accept()
            with connection:
                with connection.makefile("rwb") as file:
                    request_line = file.readline()
                    self.request = json.loads(request_line.decode("utf-8"))
                    response_line = json.dumps(
                        self.response, separators=(",", ":")
                    ).encode("utf-8")
                    file.write(response_line + b"\n")
                    file.flush()
        except Exception as error:
            self.error = error


@unittest.skipUnless(powershell_executable(), "PowerShell is not available")
class StudioIpcClientTest(unittest.TestCase):
    def run_client(
        self,
        port: int,
        *,
        command: str = "ping",
        args_json: str = "{}",
        request_id: str = "test-request",
    ) -> subprocess.CompletedProcess[str]:
        shell = powershell_executable()
        assert shell is not None
        command_line = [
            shell,
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
        ]
        if os.name == "nt":
            command_line.extend(["-ExecutionPolicy", "Bypass"])
        command_line.extend(
            [
                "-File",
                str(CLIENT),
                "-Command",
                command,
                "-ArgsJson",
                args_json,
                "-RequestId",
                request_id,
                "-Port",
                str(port),
                "-ConnectTimeoutMs",
                "2000",
                "-ResponseTimeoutMs",
                "2000",
            ]
        )
        return subprocess.run(
            command_line,
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=10,
        )

    def test_sends_one_json_line_and_prints_success_response(self) -> None:
        response = {
            "status": "ok",
            "result": {"pong": True},
            "request_id": "test-request",
        }
        with OneShotServer(response) as server:
            result = self.run_client(
                server.port,
                args_json='{"echo":"hello"}',
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), response)
        self.assertEqual(
            server.request,
            {
                "cmd": "ping",
                "args": {"echo": "hello"},
                "request_id": "test-request",
            },
        )

    def test_command_error_returns_nonzero_exit_code(self) -> None:
        response = {
            "status": "error",
            "code": "INVALID_ARGS",
            "message": "bad request",
            "request_id": "test-request",
        }
        with OneShotServer(response) as server:
            result = self.run_client(server.port)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("INVALID_ARGS", result.stderr)
        self.assertIsNotNone(server.request)

    def test_rejects_non_object_arguments_before_connecting(self) -> None:
        result = self.run_client(1, args_json="[]")

        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
