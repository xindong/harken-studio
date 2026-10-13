#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / ".scripts" / "runtime_repository_history.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "runtime_repository_history", SCRIPT_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load repository history verifier")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RepositoryHistoryVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.verifier = load_module()

    def test_indexes_function_contracts_by_name(self):
        functions = [
            {
                "name": "Vn_Init",
                "returnType": "VnResult",
                "params": [],
            },
            {
                "name": "Vn_Shutdown",
                "returnType": "void",
                "params": [],
            },
        ]
        indexed = self.verifier.index_functions(
            "fixture", functions
        )
        self.assertEqual(
            list(indexed),
            ["Vn_Init", "Vn_Shutdown"],
        )
        self.assertIs(indexed["Vn_Init"], functions[0])

    def test_rejects_duplicate_function_names(self):
        with self.assertRaises(RuntimeError):
            self.verifier.index_functions(
                "fixture",
                [{"name": "Vn_Init"}, {"name": "Vn_Init"}],
            )

    def test_function_subset_requires_exact_old_signature(self):
        old = self.verifier.index_functions(
            "old",
            [{
                "name": "Vn_Init",
                "returnType": "VnResult",
                "params": [{"name": "config", "type": "VnConfig *"}],
            }],
        )
        current = self.verifier.index_functions(
            "current",
            [{
                "name": "Vn_Init",
                "returnType": "VnResult",
                "params": [{"name": "config", "type": "const VnConfig *"}],
            }],
        )
        with self.assertRaises(RuntimeError):
            self.verifier.require_old_subset(
                "functions", old, current
            )

    def test_managed_project_identity_is_root_relative(self):
        root = Path("C:/fixture/old").resolve()
        self.assertEqual(
            self.verifier.expected_managed_project(root),
            (
                root
                / "HarkenTest"
                / "Binding"
                / "AbiFidelity.csproj"
            ).resolve(),
        )

    def test_cache_record_rejects_wrong_source_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=C:/wrong/source\n"
                "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n",
                encoding="utf-8",
            )
            with self.assertRaises(RuntimeError):
                self.verifier.cache_record(
                    build,
                    {
                        "CMAKE_HOME_DIRECTORY": "C:/expected/source",
                        "CMAKE_GENERATOR":
                            "Visual Studio 17 2022",
                    },
                    {"CMAKE_HOME_DIRECTORY"},
                )

    def test_current_runtime_build_uses_explicit_scope(self):
        command = self.verifier.runtime_configure_command(
            REPO_ROOT, REPO_ROOT / ".temp" / "history-build"
        )
        self.assertIn("-DHARKEN_BUILD_CORE=ON", command)
        self.assertIn("-DHARKEN_BUILD_EDITOR=OFF", command)
        self.assertIn("-DHARKEN_BUILD_HUB=OFF", command)
        self.assertIn("-DHARKEN_BUILD_TOOLS=OFF", command)
        self.assertNotIn("-DHARKEN_RUNTIME_ONLY=ON", command)
        self.assertIn("-U", command)

    def test_historical_runtime_build_keeps_original_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                'option(HARKEN_RUNTIME_ONLY "Build only the runtime" OFF)\n',
                encoding="utf-8",
            )
            command = self.verifier.runtime_configure_command(root, root / "build")
            self.assertIn("-DHARKEN_RUNTIME_ONLY=ON", command)
            self.assertIn("-DHARKEN_BUILD_SHARED=ON", command)
            self.assertNotIn("-DHARKEN_BUILD_CORE=ON", command)


if __name__ == "__main__":
    unittest.main()
