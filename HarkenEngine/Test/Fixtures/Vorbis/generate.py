"""Regenerate legal, synthetic Ogg fixtures with ffmpeg/libvorbis (quality 3)."""
import math
import pathlib
import struct
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    for channels, layout in ((1, "mono"), (2, "stereo"), (4, "quad"), (6, "5.1"), (8, "7.1")):
        raw = pathlib.Path(directory) / "signal.f32"
        with raw.open("wb") as output:
            for frame in range(144137):
                for channel in range(channels):
                    value = 0.12 * math.sin(2 * math.pi * (213 + 97 * channel) * frame / 48000)
                    if frame % 6000 < 40:
                        value += 0.16 * ((frame * 73 + channel * 19) % 101 / 50 - 1)
                    output.write(struct.pack("<f", value))
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", "48000",
                        "-ac", str(channels), "-channel_layout", layout, "-i", str(raw),
                        "-c:a", "libvorbis", "-q:a", "3", str(root / f"signal-{channels}.ogg")],
                       check=True, timeout=30)
