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


def verify(cli: Path, executor: Path, host: Path | None, work: Path) -> None:
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
    assert call("--version")["result"]["version"]
    catalog = call("commands")["result"]["commands"]
    names = {entry["command"] for entry in catalog}
    assert len(names) == len(catalog) and {"create_project", "set_item_property", "export_pack"} <= names
    for entry in catalog:
        assert entry["registered"] and entry["inputSchema"]["type"] == "object", entry
        if entry["requiresDesktop"]:
            assert call("exec", entry["command"], exit_code=1)["code"] == "DESKTOP_REQUIRED", entry
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

        def execute(name, arguments=None, save=False, exit_code=0):
            flags = ["exec", name, "--project", str(project), "--input", "-"]
            if save:
                flags.append("--save")
            return call(*flags, data=arguments or {}, exit_code=exit_code)

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
        platforms = execute("list_project_platforms")["result"]["platforms"]
        platform = platforms[0]["id"]
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
        destination = root / "cli-export"
        asset = execute("get_asset_info", {"source_id": imported["source_id"]})["result"]
        source = Path(asset["absolute_path"]).resolve()
        assert source.is_relative_to(root)
        original = source.read_bytes()
        try:
            source.write_bytes(b"invalid audio input")
            execute("export_pack", {"output_path": str(destination), "platform_id": platform}, exit_code=1)
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
        reference = root / "executor-export"
        generated = run(executor, ["generate-packs", str(project), "--all", "--platform-id", platform,
                                  "--self-clean", "off", "--output", str(reference)], environment)
        assert generated.returncode == 0, generated.stderr.decode("utf-8", "replace")
        for pack in packs:
            for artifact in (pack, pack.with_suffix(".json")):
                counterpart = reference / artifact.relative_to(destination)
                assert counterpart.read_bytes() == artifact.read_bytes(), artifact.name
        if host:
            platform_dir = next(pack.parent for pack in packs if pack.stem == "PlayTone")
            playback = run(host, ["pack-play", "--root", str(platform_dir), "--action-name", "PlayTone",
                                  "--seconds", "1", "--null"], environment)
            assert playback.returncode == 0, (
                playback.stdout.decode("utf-8", "replace"), playback.stderr.decode("utf-8", "replace"))
    print("CLI protocol, desktop isolation, persistence, conversion and pack equivalence passed.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the native Studio CLI against real project and pack services.")
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--executor", type=Path, required=True)
    parser.add_argument("--host", type=Path)
    parser.add_argument("--work-dir", type=Path, default=Path(".temp"))
    args = parser.parse_args()
    verify(args.cli.resolve(), args.executor.resolve(), args.host.resolve() if args.host else None,
           args.work_dir.resolve())


if __name__ == "__main__":
    main()
