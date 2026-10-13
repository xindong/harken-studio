#!/usr/bin/env python3
"""Regression tests for the Windows export allowlist parser.

The parser decides which entry points reach the DLL's export table, so a
declaration shape it fails to recognize does not merely go unlisted -- it ships a
library missing that entry point while the allowlist check, the surface check and
the build all report success. An earlier version audited only names followed by
"(", which let a macro-wrapped declaration (where the name is followed by ")")
and a name separated from its argument list by a newline through unseen. These
cases pin that closed, and pin the inverse defect too: an identifier mentioned
inside a string literal is prose, not a missing export.
"""

import importlib.util
import pathlib
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_PATH = (REPO_ROOT / "HarkenEngine" / "Source" / "Tooling" / "Abi" /
               "gen_abi_exports.py")
REAL_HEADER = (REPO_ROOT / "HarkenEngine" / "Source" / "Runtime" / "api" /
               "include" / "harken" / "Harken.h")

_spec = importlib.util.spec_from_file_location("gen_abi_exports", SCRIPT_PATH)
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)

ANCHOR = "VnResult Vn_Term(void);"


class ExportParserTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = REAL_HEADER.read_text(encoding="utf-8")
        assert cls.source.count(ANCHOR) == 1, "anchor declaration is not unique"
        cls.baseline = gen.find_public_exports(str(REAL_HEADER))

    def parse_variant(self, replacement):
        """Parses the real header with ANCHOR replaced, via a temporary copy."""
        tmp = REPO_ROOT / ".temp" / "abi_export_parser_variant.h"
        tmp.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(self.source.replace(ANCHOR, replacement, 1),
                       encoding="utf-8")
        try:
            return gen.find_public_exports(str(tmp))
        finally:
            tmp.unlink(missing_ok=True)

    def test_unchanged_header_parses_the_known_entry_points(self):
        self.assertGreater(len(self.baseline), 0)
        self.assertEqual(sorted(set(self.baseline)), self.baseline,
                         "entry points must be unique and sorted")

    def test_newline_before_argument_list_is_still_a_declaration(self):
        # Legal C and unambiguous, so it belongs in the allowlist rather than
        # being rejected for style.
        names = self.parse_variant(
            ANCHOR + "\nVnResult Vn_FutureNewline\n(void);")
        self.assertIn("Vn_FutureNewline", names)
        self.assertEqual(len(self.baseline) + 1, len(names))

    def test_macro_wrapped_declaration_is_a_hard_error(self):
        # The name is followed by ")", so nothing call-shaped appears. This must
        # fail loudly instead of quietly omitting the entry point.
        with self.assertRaises(SystemExit) as caught:
            self.parse_variant(
                ANCHOR + "\n#define VN_API_DECL(name) VnResult name\n"
                         "VN_API_DECL(Vn_Future)(void);")
        self.assertIn("Vn_Future", str(caught.exception))

    def test_unrecognized_return_type_is_a_hard_error(self):
        with self.assertRaises(SystemExit) as caught:
            self.parse_variant(ANCHOR + "\nuint32_t Vn_FutureOddType(void);")
        self.assertIn("Vn_FutureOddType", str(caught.exception))

    def test_function_pointer_typedef_needs_an_explicit_exemption(self):
        typedef = (ANCHOR +
                   "\ntypedef VnResult (*Vn_FutureFn)(const VnConfig* config);")
        # Unlisted, it is refused: the parser cannot tell a deliberate
        # non-export from an entry point it failed to understand.
        with self.assertRaises(SystemExit) as caught:
            self.parse_variant(typedef)
        self.assertIn("Vn_FutureFn", str(caught.exception))
        # Listed, it is allowed through and is not mistaken for an export.
        original = gen.NON_EXPORT_IDENTIFIERS
        gen.NON_EXPORT_IDENTIFIERS = frozenset({"Vn_FutureFn"})
        try:
            names = self.parse_variant(typedef)
        finally:
            gen.NON_EXPORT_IDENTIFIERS = original
        self.assertNotIn("Vn_FutureFn", names)
        self.assertEqual(self.baseline, names)

    def test_string_literal_mention_is_not_audited_as_an_export(self):
        names = self.parse_variant(
            ANCHOR + '\nstatic const char* kNote = "call Vn_Ghost( first)";')
        self.assertNotIn("Vn_Ghost", names)
        self.assertEqual(self.baseline, names)

    def test_checked_in_allowlist_matches_the_header(self):
        self.assertEqual(0, gen.main(["--check"]))


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
