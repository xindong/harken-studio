import argparse
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


# PackStorage in the Unity SDK applies the same bound before allocating.
MAX_GZIP_MEMBER_BYTES = 16 * 1024 * 1024


def store(source, destination):
    size = source.stat().st_size
    if source.suffix == ".bytes" and 0 < size <= MAX_GZIP_MEMBER_BYTES:
        raw = source.read_bytes()
        # Keep foreign files intact, including any future auxiliary .bytes formats.
        if raw[:4] == b"VSPK":
            member = gzip.compress(raw, compresslevel=9, mtime=0)
            if len(member) < size:
                destination.write_bytes(member)
                return True
    shutil.copyfile(source, destination)
    return False


def publish(studio_cli, project, platform, output):
    if output.exists():
        raise FileExistsError(f"Select a new release directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".harken-release-") as work:
        canonical = Path(work) / "canonical"
        command = [str(studio_cli), "build", "--project", str(project), "--all",
                   "--platform-id", platform, "--self-clean", "on", "--output", str(canonical), "--json"]
        built = subprocess.run(command, check=True, stdout=subprocess.PIPE)
        result = json.loads(built.stdout)["result"]
        if not result["success"] or not result["finalization"]["success"]:
            raise RuntimeError("Studio failed to finalize the Pack publication.")
        stored = Path(work) / "release"
        stored.mkdir()
        raw_bytes = stored_bytes = raw_total = stored_total = compressed_count = 0
        for source in sorted(canonical.rglob("*")):
            if source.is_symlink():
                raise ValueError(f"Unexpected link in fresh Studio publication: {source}")
            if not source.is_file():
                continue
            dest = stored / source.relative_to(canonical)
            dest.parent.mkdir(parents=True, exist_ok=True)
            compressed_count += store(source, dest)
            before, after = source.stat().st_size, dest.stat().st_size
            if source.suffix == ".bytes":
                raw_bytes += before
                stored_bytes += after
            raw_total += before
            stored_total += after
        if not list(stored.glob("*/Init.bytes")):
            raise ValueError("Studio publication contains no platform Init Pack.")
        stored.rename(output)
    return dict(destination=str(output), gzip_members=compressed_count,
                canonical_binary_bytes=raw_bytes, distribution_binary_bytes=stored_bytes,
                canonical_total_bytes=raw_total, distribution_total_bytes=stored_total)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Export one project platform with Studio and publish compact Unity Pack files.")
    parser.add_argument("--studio-cli", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--platform-id", required=True)
    parser.add_argument("--output", type=Path, required=True, help="A new release directory")
    args = parser.parse_args()
    print(json.dumps(publish(args.studio_cli.resolve(), args.project.resolve(), args.platform_id, args.output.resolve()), indent=2))
