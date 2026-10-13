#!/usr/bin/env python3
"""Remote-channel acceptance driver: one full external authoring session
against harken_host remote-serve, over the named pipe, with no engine
restart — load and hot-reload a pack, tweak bus state, and stream
vintage-stamped snapshots.

The driver builds its own content: it synthesizes wav sources with the
standard-library wave module, compiles two pack generations through the
real packc tool, and never touches audio hardware (the server runs
--null). Every step is checked; the first unexpected engine result or
protocol fault fails the run with a nonzero exit code.

Usage:
  py -3 .scripts/remote/remote_acceptance_drive.py \
      --host <harken_host.exe> --packc <packc.exe> [--pipe <name>]
      [--keep-server] [--trace <journal.vtrc>]

With --trace the server journals the whole drill session (remote-serve
--trace, a bounded run that exits on its own so Vn_TraceStop seals the
journal), and the driver then replays the journal twice through
`harken_host trace-replay` and byte-compares the captures: the trace
recorded during the IPC drill must replay deterministically, under the
ORDINARY success verdict (exit 0 — the remote pump issues no
callback-context API calls).

A global watchdog aborts the process if the whole drill overruns, so a
wedged server can never hang a CI job.
"""

import argparse
import json
import math
import os
import struct
import subprocess
import sys
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remote_client
import harken_wire as wire

REPO_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
WORK_DIR = os.path.join(REPO_ROOT, ".temp", "remote_drive")

SOUND_ID = 101
WATCHDOG_SECONDS = 90

PASSED = []


def step(name):
    PASSED.append(name)
    print("ok: %s" % name)


def fail(message):
    print("FAIL: %s" % message, file=sys.stderr)
    sys.exit(1)


def write_wav(path, freq_hz, seconds=0.15, rate=48000):
    frames = int(seconds * rate)
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        samples = bytearray()
        for n in range(frames):
            value = int(0.5 * 32767.0 *
                        math.sin(2.0 * math.pi * freq_hz * n / rate))
            samples += struct.pack("<h", value)
        out.writeframes(bytes(samples))


def build_pack(packc, wav_path, pack_path):
    manifest_path = pack_path + ".manifest.json"
    manifest = {"sounds": [{"path": wav_path.replace("\\", "/"),
                             "id": SOUND_ID}]}
    with open(manifest_path, "w", encoding="utf-8") as out:
        json.dump(manifest, out)
    result = subprocess.run([packc, manifest_path, pack_path],
                            capture_output=True, text=True)
    if result.returncode != 0:
        fail("packc failed for %s: %s" % (pack_path, result.stderr.strip()))
    return pack_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--packc", required=True)
    parser.add_argument("--pipe", default="HarkenRemoteDrive.%d" % os.getpid())
    parser.add_argument("--keep-server", action="store_true")
    parser.add_argument("--trace", default=None,
                        help="journal the drill session to this .vtrc path "
                             "and prove deterministic double replay")
    args = parser.parse_args()

    watchdog_budget = WATCHDOG_SECONDS + (60 if args.trace else 0)
    watchdog = threading.Timer(watchdog_budget, lambda: (
        print("FAIL: watchdog expired", file=sys.stderr), os._exit(2)))
    watchdog.daemon = True
    watchdog.start()

    os.makedirs(WORK_DIR, exist_ok=True)
    wav_v1 = os.path.join(WORK_DIR, "tone_v1.wav")
    wav_v2 = os.path.join(WORK_DIR, "tone_v2.wav")
    write_wav(wav_v1, 440.0)
    write_wav(wav_v2, 880.0)
    pack_v1 = build_pack(args.packc, wav_v1, os.path.join(WORK_DIR, "v1.pack"))
    pack_v2 = build_pack(args.packc, wav_v2, os.path.join(WORK_DIR, "v2.pack"))
    step("content built (two pack generations)")

    pipe_name = args.pipe
    if not pipe_name.startswith("\\\\.\\pipe\\"):
        pipe_name = "\\\\.\\pipe\\" + pipe_name
    serve_cmd = [args.host, "remote-serve", "--null", "--pipe", pipe_name]
    if args.trace:
        # A bounded run: the server exits on its own after the drill so
        # the clean-shutdown path stops the trace and seals the journal.
        serve_cmd += ["--seconds", "15", "--trace", args.trace]
    else:
        serve_cmd += ["--seconds", "80"]
    server = subprocess.Popen(serve_cmd)
    client = remote_client.RemoteClient()
    try:
        client.connect(pipe_name, timeout_s=15.0)
        welcome = client.hello()
        if welcome["major"] != wire.PROTOCOL_MAJOR:
            fail("protocol major mismatch in Welcome")
        if welcome["sampleRate"] == 0 or welcome["blockFrames"] == 0:
            fail("Welcome carries no runtime facts")
        step("handshake (version %s, %d Hz, %d frames/block)" % (
            welcome["version"], welcome["sampleRate"],
            welcome["blockFrames"]))

        # Load the first pack generation.
        (pack_handle,) = client.check(
            "Vn_LoadPack",
            ({"reserved0": 0}, {"utf8Path": pack_v1.replace("\\", "/")}))
        if pack_handle == 0:
            fail("Vn_LoadPack returned a null handle")
        step("pack loaded over the wire (handle %#x)" % pack_handle)

        # Tweak the built-in master bus through a mirrored request.
        client.check("Vn_SetBusVolume", 1, -3.0, 100)
        step("bus state tweaked")

        # Stream snapshots.
        if client.subscribe(100) != 100:
            fail("SubscribeAck did not echo the period")
        client.drain_advisories(1.0)
        snaps = client.snapshots()
        if len(snaps) < 5:
            fail("expected >=5 snapshots in 1 s at 100 ms, got %d" %
                 len(snaps))
        stamp = struct.unpack_from("<I", snaps[-1])[0]
        if stamp != len(snaps[-1]):
            fail("snapshot vintage stamp %d != frame payload %d" %
                 (stamp, len(snaps[-1])))
        block_off = remote_client.snapshot_field_offset("blockIndex")
        first_block = struct.unpack_from("<Q", snaps[0], block_off)[0]
        last_block = struct.unpack_from("<Q", snaps[-1], block_off)[0]
        if last_block <= first_block:
            fail("snapshot blockIndex did not advance (%d -> %d)" %
                 (first_block, last_block))
        step("snapshot stream at 100 ms cadence, vintage-stamped, advancing")

        # Hot reload the sound through a whole-pack generation swap.
        client.check(
            "Vn_ReplacePack", pack_handle,
            ({"reserved0": 0}, {"utf8Path": pack_v2.replace("\\", "/")}))
        step("hot reload: pack generation swapped")

        # Unsubscribe and confirm the session is still command-capable.
        client.drain_advisories(0.3)
        client.subscribe(0)
        client.check("Vn_SetBusVolume", 1, 0.0, 0)
        step("unsubscribed and session still serving commands")

        if args.trace:
            # The journal seals on the server's own clean shutdown; then
            # the drill trace must replay deterministically — twice, byte
            # identical, ordinary success (exit 0, never the limited-
            # replay verdict 3).
            client.close()
            try:
                server.wait(timeout=40)
            except subprocess.TimeoutExpired:
                fail("traced server never exited its bounded run")
            if server.returncode != 0:
                fail("traced server exited %d" % server.returncode)
            if not os.path.exists(args.trace):
                fail("no journal at %s" % args.trace)
            step("traced server exited cleanly, journal sealed")

            captures = []
            for n in (1, 2):
                capture = args.trace + ".replay%d.f32" % n
                result = subprocess.run(
                    [args.host, "trace-replay", args.trace,
                     "--capture", capture],
                    capture_output=True, text=True)
                if result.returncode != 0:
                    fail("trace-replay #%d exited %d (expected ordinary "
                         "success 0):\n%s%s" % (n, result.returncode,
                                                result.stdout,
                                                result.stderr))
                captures.append(capture)
            with open(captures[0], "rb") as f1, \
                 open(captures[1], "rb") as f2:
                bytes1, bytes2 = f1.read(), f2.read()
            if not bytes1:
                fail("replay capture is empty")
            if bytes1 != bytes2:
                fail("the two replays of the drill journal diverged")
            step("drill trace replayed twice, byte-identical, exit 0 "
                 "(%d capture bytes)" % len(bytes1))

    except remote_client.WireProtocolError as error:
        fail(str(error))
    finally:
        client.close()
        if not args.keep_server:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
        watchdog.cancel()

    print("acceptance drive PASSED: %d steps, one engine, one server "
          "process, no restart" % len(PASSED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
