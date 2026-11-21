import argparse
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile
import zlib


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


def write_platform_zip(platform_dir, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    inputs = deflated = 0
    with zipfile.ZipFile(destination, "x") as archive:
        for source in sorted(platform_dir.rglob("*"), key=lambda p: p.relative_to(platform_dir).as_posix()):
            if source.is_symlink():
                raise ValueError(f"Unexpected link in fresh Studio publication: {source}")
            if not source.is_file():
                continue
            name = source.relative_to(platform_dir).as_posix()
            info = zipfile.ZipInfo(name)  # Fixed timestamp, permissions and member order.
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.file_size = source.stat().st_size
            inputs += info.file_size
            if info.file_size <= MAX_GZIP_MEMBER_BYTES:
                raw = source.read_bytes()
                # The raw DEFLATE stream inside a zlib frame excludes its 2-byte header and 4-byte checksum.
                method = zipfile.ZIP_DEFLATED if len(zlib.compress(raw, 9)) - 6 < len(raw) else zipfile.ZIP_STORED
                archive.writestr(info, raw, compress_type=method, compresslevel=9)
                deflated += method == zipfile.ZIP_DEFLATED
            else:
                with source.open("rb") as src, archive.open(info, "w") as dst:
                    shutil.copyfileobj(src, dst)
    return dict(canonical_total_bytes=inputs, archive_bytes=destination.stat().st_size,
                deflated_members=deflated)


def publish(studio_cli, project, platform, output, release_format="directory"):
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
        if release_format == "windows-resident":
            from windows_resident import write_release
            result = write_release(canonical, stored)
            stored.rename(output)
            return dict(destination=str(output), **result)
        if release_format == "zip":
            platforms = sorted(canonical.iterdir())
            if not platforms:
                raise ValueError("Studio publication contains no platform.")
            archives = {}
            for directory in platforms:
                if directory.is_symlink() or not (directory / "Init.bytes").is_file():
                    raise ValueError(f"Invalid platform in fresh Studio publication: {directory}")
                archives[directory.name] = write_platform_zip(directory, stored / directory.name / "Packs.zip")
            stored.rename(output)
            return dict(destination=str(output), archives=archives)
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
    parser.add_argument("--format", choices=("directory", "zip", "windows-resident"), default="directory",
                        help="Publish independent Unity files (default) or <platform>/Packs.zip, or Windows resident .hrp files")
    args = parser.parse_args()
    print(json.dumps(publish(args.studio_cli.resolve(), args.project.resolve(), args.platform_id, args.output.resolve(), args.format), indent=2))
