from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "build_tooling_compile_db.py"
MODULE = load_script_module("build_tooling_compile_db", SCRIPT)


class ToolingCompilationDatabaseTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.source_db = self.root / "build" / "compile_commands.json"
        self.output_db = self.root / ".temp" / "tooling" / "compile_commands.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _path(self, relative: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def _entry(
        self,
        relative: str,
        target: str,
        configuration: str = "Debug",
        extra: str = "",
    ) -> dict[str, str]:
        source = self._path(relative)
        output = f"CMakeFiles/{target}.dir/{configuration}/{source.name}.obj"
        return {
            "directory": (self.root / "build").as_posix(),
            "command": (
                f"clang++ -DCMAKE_INTDIR=\\\"{configuration}\\\" "
                f"-DKEEP_ALL_DEFINES=1 -Iinclude {extra} -c {source.as_posix()}"
            ),
            "file": source.as_posix(),
            "output": output,
        }

    def _required_entries(self) -> list[dict[str, str]]:
        return [
            self._entry(
                "HarkenEngine/Source/Runtime/core/src/Core.cpp", "harken_core"
            ),
            self._entry(
                "HarkenEngine/Tool/Host/Main.cpp", "harken_host"
            ),
            self._entry(
                "HarkenEngine/Test/UnitTest/CoreTest.cpp", "harken_runtime_tests"
            ),
            self._entry(
                "HarkenEngine/Test/Benchmark/RenderBenchmark.cpp",
                "harken_benchmarks",
            ),
            self._entry(
                "HarkenStudio/Test/UnitTest/StableIdTest.cpp",
                "harken_editor_tests",
            ),
        ]

    def _build(self, entries: list[dict[str, str]]) -> list[dict[str, str]]:
        self.source_db.parent.mkdir(parents=True, exist_ok=True)
        self.source_db.write_text(json.dumps(entries), encoding="utf-8")
        return MODULE.create_tooling_database(
            self.source_db, self.output_db, self.root
        )

    def test_selects_only_debug_and_preserves_compile_fields(self) -> None:
        entries = self._required_entries()
        entries.append(
            self._entry(
                "HarkenEngine/Source/Runtime/core/src/Core.cpp",
                "harken_core",
                "Release",
            )
        )

        result = self._build(entries)

        self.assertEqual(len(result), 5)
        self.assertTrue(all("Debug" in entry["output"] for entry in result))
        self.assertTrue(
            all("KEEP_ALL_DEFINES" in entry["command"] for entry in result)
        )
        self.assertTrue(
            all(
                tuple(entry) == ("directory", "command", "file", "output")
                for entry in result
            )
        )

    def test_prefers_static_runtime_and_documented_tool_target(self) -> None:
        entries = self._required_entries()
        runtime = entries[0]["file"]
        entries.extend(
            [
                self._entry(
                    "HarkenEngine/Source/Runtime/core/src/Core.cpp",
                    "harken_core_shared",
                    extra="-DSHARED_VARIANT=1",
                ),
                self._entry(
                    "HarkenEngine/Tool/Host/Main.cpp",
                    "harkenizer",
                    extra="-DALTERNATE_TOOL=1",
                ),
                self._entry(
                    "HarkenStudio/Test/UnitTest/StableIdTest.cpp",
                    "harken_runtime_tests",
                    extra="-DALTERNATE_EDITOR_TARGET=1",
                ),
            ]
        )

        result = self._build(entries)

        runtime_entry = next(entry for entry in result if entry["file"] == runtime)
        host_entry = next(entry for entry in result if entry["file"].endswith("Main.cpp"))
        editor_entry = next(
            entry for entry in result if entry["file"].endswith("StableIdTest.cpp")
        )
        self.assertIn("harken_core.dir", runtime_entry["output"])
        self.assertIn("harken_host.dir", host_entry["output"])
        self.assertIn("harken_editor_tests.dir", editor_entry["output"])

        for relative, target in (
            ("ResourceCompiler/ResourceCompiler.cpp", "harkenizer_resource_compiler"),
            ("Core/ProjectPackBuildService.cpp", "harkenizer_core"),
            ("ResourceCompilerClient.cpp", "harkenizer_compiler_client"),
            ("Main.cpp", "harkenizer"),
        ):
            with self.subTest(source=relative):
                source = "HarkenStudio/Source/Tooling/Harkenizer/" + relative
                owned = self._entry(source, target)
                alternate = self._entry(source, "harken_editor_tests", extra="-DTEST_VARIANT=1")
                selected = MODULE._choose_entry(
                    Path(owned["file"]), [alternate, owned], "tooling", self.root)
                self.assertEqual(selected, owned)

    def test_excludes_dependencies_studio_and_standalone_clients(self) -> None:
        entries = self._required_entries()
        entries.extend(
            [
                self._entry(
                    "HarkenEngine/Source/Runtime/third_party/vendor/Vendor.cpp",
                    "vendor",
                ),
                self._entry(
                    "HarkenEngine/Source/Tooling/_deps/library/Dep.cpp", "dep"
                ),
                self._entry("HarkenEngine/Test/gtest/GTest.cpp", "gtest"),
                self._entry(
                    "HarkenEngine/Test/E2E/AbiHistoryClient/AbiHistoryClient.cpp",
                    "harken_abi_history_client",
                ),
                self._entry("HarkenStudio/Source/Editor.cpp", "HarkenStudio"),
                self._entry(
                    "HarkenStudio/Test/ThirdParty/VendorTest.cpp", "vendor_test"
                ),
                self._entry(
                    "HarkenEngine/Tool/Remote/generated/Wire.cpp",
                    "harken_remote",
                ),
            ]
        )

        result = self._build(entries)

        files = "\n".join(entry["file"] for entry in result)
        for excluded in (
            "Vendor.cpp",
            "Dep.cpp",
            "GTest.cpp",
            "AbiHistoryClient.cpp",
            "Editor.cpp",
            "VendorTest.cpp",
            "Wire.cpp",
        ):
            self.assertNotIn(excluded, files)

    def test_rejects_non_equivalent_duplicate_without_precedence(self) -> None:
        entries = self._required_entries()
        entries.append(
            self._entry(
                "HarkenEngine/Test/UnitTest/CoreTest.cpp",
                "harken_runtime_tests",
                extra="-DOTHER_TEST_VARIANT=1",
            )
        )

        with self.assertRaises(MODULE.CompilationDatabaseError):
            self._build(entries)

    def test_orders_output_by_normalized_file_path(self) -> None:
        entries = list(reversed(self._required_entries()))

        result = self._build(entries)

        paths = [entry["file"] for entry in result]
        self.assertEqual(paths, sorted(paths, key=str.casefold))
        self.assertEqual(
            json.loads(self.output_db.read_text(encoding="utf-8")), result
        )
        provenance = json.loads(
            MODULE.tooling_provenance_path(self.output_db).read_text(encoding="utf-8")
        )
        self.assertEqual(provenance["schema_version"], 1)
        self.assertEqual(provenance["source_db"], self.source_db.resolve().as_posix())
        self.assertEqual(
            provenance["source_sha256"],
            hashlib.sha256(self.source_db.read_bytes()).hexdigest(),
        )

    def test_fails_when_inventory_translation_unit_is_missing_from_database(self) -> None:
        entries = self._required_entries()
        self._path("HarkenEngine/Source/Runtime/core/src/Unlisted.cpp")

        with self.assertRaisesRegex(MODULE.CompilationDatabaseError, 'Unlisted\\.cpp'):
            self._build(entries)

    def test_fails_when_database_contains_unexpected_translation_unit(self) -> None:
        entries = self._required_entries()
        unexpected = self._entry(
            "HarkenEngine/Source/Runtime/core/src/Removed.cpp", "harken_core"
        )
        Path(unexpected["file"]).unlink()
        entries.append(unexpected)

        with self.assertRaisesRegex(MODULE.CompilationDatabaseError, 'Removed\\.cpp'):
            self._build(entries)

    def test_fails_when_required_scope_is_absent(self) -> None:
        entries = self._required_entries()
        omitted = next(
            entry for entry in entries if entry["file"].endswith("RenderBenchmark.cpp")
        )
        entries.remove(omitted)
        Path(omitted["file"]).unlink()

        with self.assertRaisesRegex(
            MODULE.CompilationDatabaseError, "runtime_benchmarks"
        ):
            self._build(entries)


if __name__ == "__main__":
    unittest.main()
