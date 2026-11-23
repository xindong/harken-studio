"""Publish one independent Windows resident Pack for each canonical Pack/JSON pair."""
import ctypes
import hashlib
import shutil
import struct
from pathlib import Path

# The Unity reader stores each part and the complete envelope in a byte array.
MAX_ARRAY_BYTES = 0x7FFFFFC7
HEADER = struct.Struct("<4sIII")
DIGEST_BYTES = hashlib.sha256().digest_size


def compress(raw):
    cabinet = ctypes.WinDLL("cabinet", use_last_error=True)
    handle = ctypes.c_void_p()
    if not cabinet.CreateCompressor(4, None, ctypes.byref(handle)):  # COMPRESS_ALGORITHM_XPRESS_HUFF
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        needed = ctypes.c_size_t()
        cabinet.Compress(handle, raw, len(raw), None, 0, ctypes.byref(needed))
        if ctypes.get_last_error() != 122:
            raise ctypes.WinError(ctypes.get_last_error())
        data = ctypes.create_string_buffer(needed.value)
        if not cabinet.Compress(handle, raw, len(raw), data, needed, ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        return data.raw[:needed.value]
    finally:
        cabinet.CloseCompressor(handle)


BLOCK_BYTES = 8192


def write_member(binary, metadata, destination):
    raw_len, meta_len = binary.stat().st_size, metadata.stat().st_size
    data_limit = MAX_ARRAY_BYTES - HEADER.size - DIGEST_BYTES
    if not 0 < raw_len <= MAX_ARRAY_BYTES or not 0 < meta_len < data_limit:
        raise ValueError(f"Resident member exceeds the Windows byte array range: {binary}")

    with destination.open("x+b") as stream:
        try:
            stream.write(bytes(HEADER.size))
            with metadata.open("rb") as src:
                shutil.copyfileobj(src, stream)
            if stream.tell() != HEADER.size + meta_len:
                raise ValueError(f"Metadata changed during publication: {metadata}")
            packed_len = raw_read = 0
            with binary.open("rb") as src:
                while block := src.read(BLOCK_BYTES):
                    compressed = compress(block)
                    stored = compressed if len(compressed) <= len(block) * 7 // 8 else block
                    raw_read += len(block)
                    packed_len += 4 + len(stored)
                    if raw_read > raw_len or packed_len > data_limit - meta_len:
                        raise ValueError(f"Resident envelope exceeds the Windows byte array range: {binary}")
                    stream.write(struct.pack("<I", len(stored)))
                    stream.write(stored)
            if raw_read != raw_len:
                raise ValueError(f"Pack changed during publication: {binary}")
            stream.seek(0)
            stream.write(HEADER.pack(b"HRP2", raw_len, meta_len, packed_len))
            stream.seek(0)
            digest = hashlib.sha256()
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
            stream.write(digest.digest())
        except BaseException:
            stream.close()
            destination.unlink()
            raise


def write_release(canonical: Path, output: Path):
    sources = sorted(canonical.rglob("*"))
    if any(p.is_symlink() for p in sources):
        raise ValueError("A Studio publication must not contain a symbolic link.")
    if not list(canonical.glob("*/Init.bytes")):
        raise ValueError("Studio publication contains no platform Init Pack.")
    consumed = set()
    total = count = 0
    for binary in sources:
        if binary.suffix != ".bytes":
            continue
        metadata = binary.with_suffix(".json")
        if not metadata.is_file():
            continue
        with binary.open("rb") as stream:
            if stream.read(4) != b"VSPK":
                continue
        dest = (output / binary.relative_to(canonical)).with_suffix(".hrp")
        dest.parent.mkdir(parents=True, exist_ok=True)
        write_member(binary, metadata, dest)
        consumed.update((binary, metadata))
        count += 1
        total += dest.stat().st_size
    for platform in canonical.iterdir():
        if platform.is_dir() and not (output / platform.name / "Init.hrp").is_file():
            raise ValueError(f"Missing Windows resident Init for {platform}")
    for source in sources:
        if source.is_file() and source not in consumed:
            dest = output / source.relative_to(canonical)
            if dest.exists() or source.suffix == ".hrp":
                raise ValueError(f"Reserved Windows resident path: {source}")
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
            total += dest.stat().st_size
    return dict(resident_members=count, distribution_total_bytes=total)
