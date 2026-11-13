import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pack_layout_lock_check", ROOT / ".scripts/pack_layout_lock_check.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

FILE = """#define HK_PACK_FORMAT_VERSION {version}u
typedef struct HkPackRow {{
    uint32_t value;
}} HkPackRow;
HK_PACK_SIZE(HkPackRow, {size});
HK_PACK_OFFSET(HkPackRow, value, 0);
"""


STREAM = """HK_PACK_TABLE HkPackStreamStep hkPackStreamRow[] = {{
    {steps}
}};
"""


def files(version: int, size: int, steps: tuple[str, ...] = ()) -> dict[str, str]:
    text = FILE.format(version=version, size=size)
    if steps:
        text += STREAM.format(steps="\n    ".join(steps))
    return {"PackFile.h": text}


class PackLayoutLockCheckTest(unittest.TestCase):
    def test_unchanged_layout_passes(self) -> None:
        self.assertEqual([], MODULE.check(files(4, 4), files(4, 4)))

    def test_layout_change_requires_version_bump(self) -> None:
        self.assertTrue(MODULE.check(files(4, 4), files(4, 8)))
        self.assertEqual([], MODULE.check(files(4, 4), files(5, 8)))

    def test_stream_order_change_requires_version_bump(self) -> None:
        steps = ("HK_PACK_STREAM_RECORD(HkPackRow),", "HK_PACK_STREAM_RECORD_EACH(HkPackItem, HkPackRow, value),")
        self.assertTrue(MODULE.check(files(4, 4, steps), files(4, 4, steps[::-1])))
        self.assertEqual([], MODULE.check(files(4, 4, steps), files(5, 4, steps[::-1])))
        self.assertEqual([], MODULE.check(files(4, 4), files(4, 4, steps)))

    def test_first_introduction_passes(self) -> None:
        self.assertEqual([], MODULE.check({}, files(4, 4)))

    def test_repository_headers_carry_a_version_and_locks(self) -> None:
        current = MODULE.current_files(ROOT)
        self.assertIsNotNone(MODULE.format_version(current))
        self.assertTrue(MODULE.lock_lines(current))


if __name__ == "__main__":
    unittest.main()
