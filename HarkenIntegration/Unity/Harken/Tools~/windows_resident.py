"""Publish one independent Windows resident Pack for each canonical Pack/JSON pair."""
import ctypes
import hashlib
import shutil
import struct
from pathlib import Path

MAX_BYTES = 16 * 1024 * 1024


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
        if max(binary.stat().st_size, metadata.stat().st_size) > MAX_BYTES:
            raise ValueError(f"Resident Pack exceeds the bounded Windows publication size: {binary}")
        raw, json_bytes = binary.read_bytes(), metadata.read_bytes()
        if raw[:4] != b"VSPK":
            continue
        packed = compress(raw)
        header = struct.pack("<4sIII", b"HRP1", len(raw), len(json_bytes), len(packed))
        content = header + json_bytes + packed
        dest = (output / binary.relative_to(canonical)).with_suffix(".hrp")
        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("xb") as stream:
            stream.write(content)
            stream.write(hashlib.sha256(content).digest())
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
