from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class HeadlessDependenciesTests(unittest.TestCase):
    def configure(self, graph: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory(prefix="headless-boundary-") as temporary:
            root = Path(temporary)
            source = (REPO_ROOT / "HarkenStudio/CMakeLists.txt").read_text(encoding="utf-8")
            start = source.index("function(harken_verify_headless_dependencies)")
            end = source.index("endfunction()", start) + len("endfunction()")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(HeadlessBoundary LANGUAGES NONE)\n'
                + source[start:end] + '\n' + graph
                + '\nharken_verify_headless_dependencies(host)\n',
                encoding="utf-8",
            )
            return subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                capture_output=True, text=True, encoding="utf-8", check=False,
            )

    def test_headless_modules_and_shared_dependency_cycle(self):
        result = self.configure('''
add_library(host INTERFACE)
add_library(core INTERFACE)
add_library(storage INTERFACE)
add_library(Qt6::Core INTERFACE IMPORTED)
add_library(Qt6::Network INTERFACE IMPORTED)
add_library(Qt6::Concurrent INTERFACE IMPORTED)
target_link_libraries(host INTERFACE core)
target_link_libraries(core INTERFACE storage Qt6::Core Qt6::Concurrent)
target_link_libraries(storage INTERFACE core Qt6::Network)
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_private_transitive_dependency_reports_chain(self):
        result = self.configure('''
add_library(host INTERFACE)
add_library(core STATIC IMPORTED)
add_library(adapter INTERFACE)
add_library(Qt6::Gui INTERFACE IMPORTED)
set_property(TARGET core PROPERTY LINK_LIBRARIES adapter)
target_link_libraries(host INTERFACE core)
target_link_libraries(adapter INTERFACE Qt6::Gui)
''')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("host -> core -> adapter -> Qt6::Gui", result.stderr)

    def test_conditional_link_only_and_alias_dependencies(self):
        result = self.configure('''
add_library(host INTERFACE)
add_library(core INTERFACE)
add_library(desktop INTERFACE)
add_library(bridge::desktop ALIAS desktop)
add_library(Qt6::Widgets INTERFACE IMPORTED)
target_link_libraries(host INTERFACE "$<LINK_ONLY:core>")
target_link_libraries(core INTERFACE "$<$<CONFIG:Debug>:bridge::desktop>")
target_link_libraries(desktop INTERFACE Qt6::Widgets)
''')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Headless dependency violation", result.stderr)
        self.assertIn("Qt6::Widgets", result.stderr)

    def test_raw_gui_library_is_rejected(self):
        for library in ("Qt6Guid.lib", "libQt6Svg.so.6", "QtGui.framework/QtGui"):
            with self.subTest(library=library):
                result = self.configure(
                    'add_library(host INTERFACE)\n'
                    f'target_link_libraries(host INTERFACE "{library}")\n'
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Headless dependency violation", result.stderr)

    def test_unknown_root_is_rejected(self):
        result = self.configure("")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot verify unknown headless target", result.stderr)


if __name__ == "__main__":
    unittest.main()
