from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "runtime_rt_compileout.py"
SPEC = importlib.util.spec_from_file_location(
    "runtime_rt_compileout", SCRIPT
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RuntimeRtCompileoutTest(unittest.TestCase):
    def test_disassembly_normalization_ignores_artifact_path(self):
        first = """banner
Dump of file first.obj
File Type: COFF OBJECT

fn:
  0000: ret

  Summary
        10 .debug$S
"""
        second = first.replace("first.obj", "second.obj")
        self.assertEqual(
            MODULE.normalize_disassembly(first),
            MODULE.normalize_disassembly(second),
        )

    def test_probe_absence_rejects_private_symbol(self):
        with self.assertRaises(MODULE.CompileoutError):
            MODULE.require_probe_absent(
                "shipping", "?Attach@rtprobe@harken@@"
            )

    def test_probe_presence_requires_private_symbol(self):
        with self.assertRaises(MODULE.CompileoutError):
            MODULE.require_probe_present("qualification", "ordinary")


if __name__ == "__main__":
    unittest.main()
