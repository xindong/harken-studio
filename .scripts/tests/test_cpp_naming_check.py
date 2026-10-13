from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

from script_loader import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "cpp_naming_check.py"
MODULE = load_script_module("cpp_naming_check", SCRIPT)


class CppNamingCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _executable(self, relative: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("tool", encoding="utf-8")
        if os.name != "nt":
            path.chmod(0o755)
        return path

    def _source_database(self, directory: str = "build") -> Path:
        build = self.root / directory
        build.mkdir(parents=True, exist_ok=True)
        marker = build / "CMakeCache.txt"
        marker.write_text("configured\n", encoding="utf-8")
        source_db = build / "compile_commands.json"
        source_db.write_text("[]\n", encoding="utf-8")
        os.utime(marker, ns=(1_000_000_000, 1_000_000_000))
        os.utime(source_db, ns=(2_000_000_000, 2_000_000_000))
        return source_db

    @staticmethod
    def _write_provenance(tooling_db: Path, source_db: Path) -> None:
        provenance = MODULE.source_database_provenance(source_db)
        path = MODULE.tooling_provenance_path(tooling_db)
        path.write_text(json.dumps(provenance), encoding="utf-8")

    def test_explicit_clang_tidy_precedes_path_and_known_locations(self) -> None:
        explicit = self._executable("explicit/clang-tidy.exe")
        path_tool = self._executable("path/clang-tidy.exe")
        known = self._executable("known/clang-tidy.exe")

        result = MODULE.find_clang_tidy(
            str(explicit),
            environment={},
            path_lookup=lambda _: str(path_tool),
            known_candidates=[known],
            vs_installations=[],
        )

        self.assertEqual(result, explicit.resolve())

    def test_path_precedes_visual_studio_known_location(self) -> None:
        path_tool = self._executable("path/clang-tidy.exe")
        known = self._executable("known/clang-tidy.exe")

        result = MODULE.find_clang_tidy(
            None,
            environment={},
            path_lookup=lambda name: str(path_tool) if name == "clang-tidy" else None,
            known_candidates=[known],
            vs_installations=[],
        )

        self.assertEqual(result, path_tool.resolve())

    def test_visual_studio_known_location_is_used_after_path(self) -> None:
        known = self._executable("known/clang-tidy.exe")

        result = MODULE.find_clang_tidy(
            None,
            environment={},
            path_lookup=lambda _: None,
            known_candidates=[known],
            vs_installations=[],
        )

        self.assertEqual(result, known.resolve())

    def test_visual_studio_environment_lookup_is_case_insensitive(self) -> None:
        program_files = self.root / "Program Files"
        known = (
            program_files
            / "Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-tidy.exe"
        )
        known.parent.mkdir(parents=True)
        known.write_text("tool", encoding="utf-8")
        if os.name != "nt":
            known.chmod(0o755)

        candidates = MODULE._known_visual_studio_candidates(
            {"PROGRAMFILES": str(program_files)}
        )

        self.assertIn(known, candidates)

    def test_constructs_scoped_non_fixing_command(self) -> None:
        tool = self._executable("llvm/clang-tidy.exe")
        database = self.root / ".temp/tooling/compile_commands.json"
        database.parent.mkdir(parents=True)
        database.write_text("[]", encoding="utf-8")
        config = self.root / ".clang-tidy"
        config.write_text("Checks: test\n", encoding="utf-8")
        source = self.root / "HarkenEngine/Test/UnitTest/SessionTest.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("int Main();\n", encoding="utf-8")
        header = self.root / "HarkenEngine/Test/Common/TestUtil.h"
        header.parent.mkdir(parents=True)
        header.write_text("class EngineSession {};\n", encoding="utf-8")

        command = MODULE.build_clang_tidy_command(
            tool, database, config, [source], [header]
        )

        self.assertEqual(command[0], str(tool))
        self.assertIn(str(database.parent), command)
        self.assertIn(f"--config-file={config}", command)
        self.assertNotIn("--fix", command)
        self.assertNotIn("-fix", command)
        line_filter = next(
            argument.removeprefix("--line-filter=")
            for argument in command
            if argument.startswith("--line-filter=")
        )
        self.assertEqual(json.loads(line_filter)[0]["name"], header.as_posix())
        header_filter = next(
            argument for argument in command if argument.startswith("--header-filter=")
        )
        self.assertIn("TestUtil", header_filter)
        self.assertNotIn("SessionTest", header_filter)
        self.assertEqual(command[-1], str(source))

    def test_rejects_source_diagnostic_outside_selected_commands(self) -> None:
        tool = self._executable("llvm/clang-tidy.exe")
        database = self.root / ".temp/tooling/compile_commands.json"
        database.parent.mkdir(parents=True)
        database.write_text("[]", encoding="utf-8")
        config = self.root / ".clang-tidy"
        config.write_text("Checks: test\n", encoding="utf-8")
        selected = self.root / "HarkenEngine/Test/UnitTest/SelectedTest.cpp"
        unselected = self.root / "HarkenEngine/Test/UnitTest/UnselectedTest.cpp"
        selected.parent.mkdir(parents=True)
        selected.write_text("int Selected();\n", encoding="utf-8")
        unselected.write_text("int Unselected();\n", encoding="utf-8")

        with self.assertRaises(MODULE.NamingCheckError):
            MODULE.build_clang_tidy_command(
                tool, database, config, [selected], [unselected]
            )

    def test_runtime_directory_root_is_an_allowed_scope(self) -> None:
        runtime_root = self.root / "HarkenEngine/Test"
        unit_test = runtime_root / "UnitTest/SelectedTest.cpp"
        unit_test.parent.mkdir(parents=True)
        unit_test.write_text("int Selected();\n", encoding="utf-8")

        selected = MODULE.select_translation_units(
            [str(runtime_root)], [unit_test.resolve()], self.root
        )

        self.assertEqual(selected, [unit_test.resolve()])

    def test_editor_directory_root_is_an_allowed_scope(self) -> None:
        editor_root = self.root / "HarkenStudio/Test"
        editor_test = editor_root / "UnitTest/SelectedTest.cpp"
        editor_test.parent.mkdir(parents=True)
        editor_test.write_text("int Selected();\n", encoding="utf-8")

        selected = MODULE.select_translation_units(
            [str(editor_root)], [editor_test.resolve()], self.root
        )

        self.assertEqual(selected, [editor_test.resolve()])

    def test_directory_scope_rejects_missing_inventory_translation_unit(self) -> None:
        directory = self.root / "HarkenEngine/Test/UnitTest"
        directory.mkdir(parents=True)
        selected = directory / "SelectedTest.cpp"
        missing = directory / "MissingTest.cpp"
        selected.write_text("int Selected();\n", encoding="utf-8")
        missing.write_text("int Missing();\n", encoding="utf-8")

        with self.assertRaisesRegex(MODULE.NamingCheckError, 'MissingTest\\.cpp'):
            MODULE.select_translation_units(
                [str(directory)], [selected.resolve()], self.root
            )

    def test_provenance_mismatch_refreshes_for_older_alternate_source_db(self) -> None:
        original = self._source_database("build-original")
        alternate = self._source_database("build-alternate")
        tooling = self.root / ".temp/tooling/compile_commands.json"
        tooling.parent.mkdir(parents=True)
        tooling.write_text("[]\n", encoding="utf-8")
        self._write_provenance(tooling, original)

        def regenerate(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            tooling.write_text("[]\n", encoding="utf-8")
            self._write_provenance(tooling, alternate)
            return subprocess.CompletedProcess(args=[], returncode=0)

        with mock.patch.object(MODULE.subprocess, "run", side_effect=regenerate) as run:
            MODULE.ensure_tooling_database(alternate, tooling)

        run.assert_called_once()
        command = run.call_args.args[0]
        self.assertIn(str(alternate), command)

    def test_rejects_source_database_older_than_configure_marker(self) -> None:
        source_db = self._source_database()
        marker = source_db.parent / "build.ninja"
        marker.write_text("generated\n", encoding="utf-8")
        os.utime(source_db, ns=(1_000_000_000, 1_000_000_000))
        os.utime(marker, ns=(3_000_000_000, 3_000_000_000))
        tooling = self.root / ".temp/tooling/compile_commands.json"

        with mock.patch.object(MODULE.subprocess, "run") as run:
            with self.assertRaises(MODULE.NamingCheckError):
                MODULE.ensure_tooling_database(source_db, tooling)

        run.assert_not_called()

    def test_reuses_tooling_database_with_matching_provenance(self) -> None:
        source_db = self._source_database()
        tooling = self.root / ".temp/tooling/compile_commands.json"
        tooling.parent.mkdir(parents=True)
        tooling.write_text("[]\n", encoding="utf-8")
        self._write_provenance(tooling, source_db)

        with mock.patch.object(MODULE.subprocess, "run") as run:
            MODULE.ensure_tooling_database(source_db, tooling)

        run.assert_not_called()

    def test_content_digest_mismatch_refreshes_even_when_mtime_is_older(self) -> None:
        source_db = self._source_database()
        tooling = self.root / ".temp/tooling/compile_commands.json"
        tooling.parent.mkdir(parents=True)
        tooling.write_text("[]\n", encoding="utf-8")
        self._write_provenance(tooling, source_db)
        source_db.write_text("[ ]\n", encoding="utf-8")
        os.utime(source_db, ns=(2_000_000_000, 2_000_000_000))

        def regenerate(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            tooling.write_text("[]\n", encoding="utf-8")
            self._write_provenance(tooling, source_db)
            return subprocess.CompletedProcess(args=[], returncode=0)

        with mock.patch.object(MODULE.subprocess, "run", side_effect=regenerate) as run:
            MODULE.ensure_tooling_database(source_db, tooling)

        run.assert_called_once()

    def test_clang_tidy_rejects_lower_camel_and_accepts_exceptions(self) -> None:
        try:
            clang_tidy = MODULE.find_clang_tidy(None)
        except MODULE.NamingCheckError as error:
            self.skipTest(str(error))

        config = REPO_ROOT / ".clang-tidy"
        database = self.root / "clang-tidy/compile_commands.json"
        database.parent.mkdir(parents=True)

        def run_fixture(name: str, source_text: str) -> subprocess.CompletedProcess[str]:
            source = database.parent / name
            source.write_text(source_text, encoding="utf-8")
            database.write_text(
                json.dumps(
                    [
                        {
                            "directory": database.parent.as_posix(),
                            "command": (
                                f'clang++ -std=c++17 -c "{source.as_posix()}"'
                            ),
                            "file": source.as_posix(),
                        }
                    ]
                ),
                encoding="utf-8",
            )
            command = MODULE.build_clang_tidy_command(
                clang_tidy, database, config, [source], [source]
            )
            return subprocess.run(
                command,
                cwd=self.root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

        accepted = run_fixture(
            "Accepted.cpp",
            "struct AcceptedFixture {\n"
            "    void SetUp() {}\n"
            "    int size() const { return 0; }\n"
            "};\n"
            "int main() { return 0; }\n",
        )
        self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)

        rejected = run_fixture(
            "Rejected.cpp",
            "void lowerCamelFunction() {}\n"
            "struct RejectedFixture { void lowerCamelMethod() {} };\n",
        )
        output = rejected.stdout + rejected.stderr
        self.assertNotEqual(rejected.returncode, 0, output)
        self.assertIn("lowerCamelFunction", output)
        self.assertIn("lowerCamelMethod", output)


if __name__ == "__main__":
    unittest.main()
