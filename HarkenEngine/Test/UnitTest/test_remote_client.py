from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(
    0,
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Tool", "Remote"),
)

import remote_client  # noqa: E402
import harken_wire as wire  # noqa: E402

CURVE_HANDLES_OFFSET = 72  # inside VnCurveDesc


class PackStructTest(unittest.TestCase):
    def test_handle_array_packs_and_bounds(self) -> None:
        packed = remote_client.pack_struct(
            "VnCurveDesc",
            {"pointCount": 2,
             "points": [{"x": 0.0, "y": 0.0}, {"x": 1.0, "y": -6.0}],
             "handles": [
                 {"h1x": 0.0, "h1y": 0.0, "h2x": 1.0, "h2y": -6.0}
             ]})
        self.assertEqual(
            struct.unpack_from("<ffff", packed, CURVE_HANDLES_OFFSET),
            (0.0, 0.0, 1.0, -6.0),
        )
        with self.assertRaises(remote_client.WireProtocolError):
            remote_client.pack_struct(
                "VnCurveDesc",
                {"handles": [
                    {"h1x": 0.0, "h1y": 0.0, "h2x": 1.0, "h2y": 1.0}
                ] * 8},
            )


class CaptureRowsTest(unittest.TestCase):
    def test_capture_targets_command_payload_is_capacity_only(self) -> None:
        payload, out_count, has_rows = remote_client.encode_command(
            "Vn_CapturePropertyTargets", 16)
        self.assertEqual(out_count, 1)  # requiredCount
        self.assertTrue(has_rows)
        expected = struct.pack(
            "<HH", wire.OPCODES["Vn_CapturePropertyTargets"], 0)
        expected += struct.pack("<I", 16)
        self.assertEqual(payload, expected)

    def test_row_section_of_partial_stride_raises(self) -> None:
        with self.assertRaises(remote_client.WireProtocolError):
            remote_client.unpack_rows("VnPropertyTargetInfo", b"\x00" * 47)


if __name__ == "__main__":
    unittest.main()
