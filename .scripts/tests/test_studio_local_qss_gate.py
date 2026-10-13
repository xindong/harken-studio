from __future__ import annotations

import io
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "studio_local_qss_gate.py"
MODULE = load_script_module("studio_local_qss_gate", SCRIPT)

OWNED = "HarkenStudio/Source/Core/ThemeManager.cpp"
UNOWNED = "HarkenStudio/Source/Widgets/Common/NewPanel.cpp"


class StudioLocalQssGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write(self, relative: str, body: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def _run(self, owners: dict[str, str]) -> tuple[int, str]:
        stream = io.StringIO()
        with mock.patch.object(MODULE, "LOCAL_QSS_OWNERS", owners):
            code = MODULE.report(MODULE.call_sites(self.root), stream)
        return code, stream.getvalue()

    def test_owned_call_site_passes(self) -> None:
        self._write(OWNED, "void apply() { app->setStyleSheet(skin); }\n")
        code, output = self._run({OWNED: "application skin"})
        self.assertEqual(code, 0, output)

    def test_unowned_call_site_fails_with_location(self) -> None:
        self._write(UNOWNED, "void build()\n{\n    label->setStyleSheet(frozen);\n}\n")
        code, output = self._run({})
        self.assertEqual(code, 1, output)
        self.assertIn(f"{UNOWNED}:3", output)
        self.assertIn("LOCAL_QSS_OWNERS", output)

    def test_stale_owner_entry_fails(self) -> None:
        self._write(UNOWNED, "void build() { label->setProperty(\"textRole\", role); }\n")
        code, output = self._run({OWNED: "application skin"})
        self.assertEqual(code, 1, output)
        self.assertIn("installs no local QSS", output)

    def test_comments_and_literals_are_not_call_sites(self) -> None:
        self._write(
            UNOWNED,
            "// label->setStyleSheet(dead);\n"
            "/* block\n   w->setStyleSheet(dead);\n*/\n"
            'const char* hint = "call setStyleSheet( to freeze";\n'
            'const char* raw = R"(w->setStyleSheet(dead);)";\n',
        )
        code, output = self._run({})
        self.assertEqual(code, 0, output)

    def test_quoted_slashes_do_not_hide_a_real_call(self) -> None:
        self._write(UNOWNED, 'auto url = "https://x"; w->setStyleSheet(frozen);\n')
        code, output = self._run({})
        self.assertEqual(code, 1, output)
        self.assertIn(f"{UNOWNED}:1", output)

    def test_third_party_and_test_sources_are_out_of_scope(self) -> None:
        self._write("HarkenStudio/ThirdParty/ads/DockWidget.cpp", "w->setStyleSheet(x);\n")
        self._write("HarkenStudio/Test/UnitTest/WidgetTest.cpp", "w->setStyleSheet(x);\n")
        code, output = self._run({})
        self.assertEqual(code, 0, output)

    def test_repository_tree_satisfies_the_gate(self) -> None:
        stream = io.StringIO()
        self.assertEqual(MODULE.report(MODULE.call_sites(REPO_ROOT), stream), 0, stream.getvalue())


if __name__ == "__main__":
    unittest.main()
