#!/usr/bin/env python3
"""Independent reference check for the engine's master output downmix.

Consumes the artifacts written by `harken_host downmix-null-drill`: per layout,
the engine's pre-fold multichannel mix (source_<layout>.f32) and its folded
stereo output (engine_downmix_<layout>.f32), both raw native-endian interleaved
float32, described by a manifest.json in the same directory.

For each drill this tool folds the pre-fold mix to stereo using ONLY the
ITU-R BS.775 coefficient table transcribed below from the specification's
Appendix A, then null-compares its own fold to the engine's, sample by sample,
and reports the peak and RMS residual in dBFS (full scale 1.0).

Independence is the point: this file deliberately does not import, read, or
mirror any engine source. It depends only on (a) the raw sample bytes the
engine emitted and (b) the published coefficient table. Its fold and the
engine's fold are two independent implementations of the same specification;
agreement to the float32 rounding floor is the null-test acceptance evidence
that replaces a listening comparison — the coefficients are an international
standard, so proving the arithmetic matches them is the objective form of the
same judgment.

Verdict: a drill passes when its peak residual is at or below -120 dBFS. That
threshold is the float32 arithmetic floor, not an arbitrary slack: the engine
sums the fold in single precision while this reference sums in Python double,
so a legitimate result differs by about one float32 ULP relative to full scale
(~2^-24 ~= 6e-8 ~= -144 dBFS); the coefficient constant (1/sqrt(2)) likewise
differs between a float32 engine constant and this double one by ~2e-8. Their
combination lands a correct fold near -130 to -140 dBFS, so -120 dBFS passes a
correct engine with margin while still failing any real coefficient, ordering,
channel-mapping, or alignment error (those show up tens of dB louder).

Pure standard library (struct/array/math/json); no numpy (this repository has
no Python venv precedent). Run:

    python HarkenEngine/Test/E2E/downmix_null_reference.py [<dir>]

<dir> defaults to .temp/nulldrill.
"""

import argparse
import array
import json
import math
import os
import sys

INV2 = 1.0 / math.sqrt(2.0)  # -3 dB, the BS.775 surround/center fold gain
SILENCE_FLOOR = 1e-4         # a source below this is not real test signal
PASS_DBFS = -120.0           # peak-residual gate (see module docstring)

# Downmix coefficient tables (dst x src), transcribed from the specification's
# Appendix A. Source channel order per layout (Appendix A canonical order):
#   5.1: FL, FR, FC, LFE, BL, BR
#   7.1: FL, FR, FC, LFE, BL, BR, SL, SR
# Destination stereo order: Lo (row 0), Ro (row 1). Only the nonzero terms are
# listed as (dst_row, src_col, coeff); every other coefficient is 0 (notably
# the LFE column, which the default stereo fold drops entirely).
MATRICES = {
    # Lo = FL + .7071 FC + .7071 BL ; Ro = FR + .7071 FC + .7071 BR
    "5.1": {
        "src_channels": 6,
        "terms": [
            (0, 0, 1.0), (0, 2, INV2), (0, 4, INV2),
            (1, 1, 1.0), (1, 2, INV2), (1, 5, INV2),
        ],
    },
    # Lo = FL + .7071 (FC + BL + SL) ; Ro = FR + .7071 (FC + BR + SR)
    # (BS.775 fronts + the documented engineering extension for the 7.1 sides.)
    "7.1": {
        "src_channels": 8,
        "terms": [
            (0, 0, 1.0), (0, 2, INV2), (0, 4, INV2), (0, 6, INV2),
            (1, 1, 1.0), (1, 2, INV2), (1, 5, INV2), (1, 7, INV2),
        ],
    },
}


def read_f32(path):
    """Read a raw native-endian float32 file into an array('f')."""
    samples = array.array("f")
    if samples.itemsize != 4:
        raise RuntimeError("this platform's array('f') is not 32-bit float")
    size = os.path.getsize(path)
    if size % 4 != 0:
        raise RuntimeError("%s size %d is not a whole number of float32" %
                           (path, size))
    with open(path, "rb") as handle:
        samples.frombytes(handle.read())
    return samples


def dbfs(linear):
    """Convert a linear amplitude (full scale 1.0) to dBFS; 0 -> -inf."""
    if linear <= 0.0:
        return float("-inf")
    return 20.0 * math.log10(linear)


def check_drill(drill, base_dir):
    """Fold the drill's source independently and null-compare to the engine.

    Returns (passed, message).
    """
    name = drill["name"]
    layout = drill["srcLayout"]
    matrix = MATRICES.get(layout)
    if matrix is None:
        return False, "%s: unknown source layout %r" % (name, layout)

    src_channels = drill["sourceChannels"]
    if src_channels != matrix["src_channels"]:
        return False, ("%s: manifest source channels %d != %s layout %d" %
                       (name, src_channels, layout, matrix["src_channels"]))
    dst_channels = drill["deviceChannels"]
    if dst_channels != 2:
        return False, "%s: expected a stereo device fold, got %d channels" % (
            name, dst_channels)

    source = read_f32(os.path.join(base_dir, os.path.basename(drill["source"])))
    engine = read_f32(os.path.join(base_dir, os.path.basename(drill["engine"])))

    if len(source) % src_channels != 0:
        return False, "%s: source is not a whole number of frames" % name
    if len(engine) % dst_channels != 0:
        return False, "%s: engine output is not a whole number of frames" % name
    src_frames = len(source) // src_channels
    eng_frames = len(engine) // dst_channels
    if src_frames != eng_frames:
        return False, ("%s: frame count mismatch (source %d, engine %d) — the "
                       "fold must be sample-aligned with no added latency" %
                       (name, src_frames, eng_frames))
    if src_frames == 0:
        return False, "%s: empty capture" % name

    # The source's own peak level, so a silent (trivially passing) capture is
    # rejected rather than greened. Computed once over the whole buffer.
    source_peak = max(map(abs, source)) if len(source) else 0.0
    if source_peak < SILENCE_FLOOR:
        return False, ("%s: source peak %.3e is below the silence floor — a "
                       "silent capture is not valid null evidence" %
                       (name, source_peak))

    terms = matrix["terms"]
    peak_residual = 0.0
    sum_sq = 0.0
    peak_frame = -1
    for frame in range(src_frames):
        s0 = frame * src_channels
        # Independent fold from the coefficient table.
        ref = [0.0, 0.0]
        for dst, src, coeff in terms:
            ref[dst] += coeff * source[s0 + src]
        e0 = frame * dst_channels
        for dst in range(2):
            residual = ref[dst] - engine[e0 + dst]
            mag = abs(residual)
            sum_sq += residual * residual
            if mag > peak_residual:
                peak_residual = mag
                peak_frame = frame

    rms = math.sqrt(sum_sq / (2.0 * src_frames))
    peak_db = dbfs(peak_residual)
    rms_db = dbfs(rms)
    passed = peak_db <= PASS_DBFS
    verdict = "NULL PASS" if passed else "NULL FAIL"
    message = ("%s [%s->stereo]: %s — peak residual %.6e (%.2f dBFS) at frame "
               "%d, RMS residual %.6e (%.2f dBFS); %d frames, source peak "
               "%.4f; gate %.1f dBFS" %
               (name, layout, verdict, peak_residual, peak_db, peak_frame,
                rms, rms_db, src_frames, source_peak, PASS_DBFS))
    return passed, message


def main(argv):
    parser = argparse.ArgumentParser(
        description="Independent null check of the engine downmix fold.")
    parser.add_argument("dir", nargs="?", default=os.path.join(".temp",
                                                               "nulldrill"),
                        help="directory holding manifest.json and the .f32 "
                             "artifacts (default: .temp/nulldrill)")
    args = parser.parse_args(argv)

    manifest_path = os.path.join(args.dir, "manifest.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except OSError as error:
        sys.stderr.write("error: cannot read %s: %s\n"
                         "run `harken_host downmix-null-drill` first\n" %
                         (manifest_path, error))
        return 1
    except ValueError as error:
        sys.stderr.write("error: %s is not valid JSON: %s\n" %
                         (manifest_path, error))
        return 1

    drills = manifest.get("drills", [])
    if not drills:
        sys.stderr.write("error: manifest lists no drills\n")
        return 1

    all_passed = True
    for drill in drills:
        try:
            passed, message = check_drill(drill, args.dir)
        except (OSError, RuntimeError) as error:
            passed, message = False, "%s: %s" % (drill.get("name", "?"), error)
        print(message)
        all_passed = all_passed and passed

    if all_passed:
        print("downmix null-drill: ALL PASS")
        return 0
    print("downmix null-drill: FAIL — at least one fold did not null")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
