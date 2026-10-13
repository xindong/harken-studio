import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (
    ROOT
    / "HarkenEngine"
    / "Source"
    / "Tooling"
    / "Qualification"
    / "GenerateSourceIdentity.cmake"
)


def read_defines(path: Path) -> dict[str, str]:
    defines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"#define ([A-Z0-9_]+) (.+)", line)
        if match:
            defines[match.group(1)] = match.group(2).strip('"')
    return defines


class RuntimeRtqSourceIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.git = shutil.which("git")
        self.cmake = shutil.which("cmake")
        if self.git is None or self.cmake is None:
            self.skipTest("git and cmake are required")
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.source = self.repo / "source.cpp"
        self.output = self.root / "generated" / "identity.h"
        self.run_git("init")
        self.run_git("config", "user.email", "test@example.invalid")
        self.run_git("config", "user.name", "Runtime Identity Test")
        self.source.write_text("int value = 1;\n", encoding="utf-8")
        self.run_git("add", "source.cpp")
        self.run_git("commit", "-m", "initial")
        self.commit = self.run_git("rev-parse", "HEAD").stdout.strip().lower()

    def tearDown(self) -> None:
        if hasattr(self, "temporary"):
            self.temporary.cleanup()

    def run_git(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self.git, *arguments],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        )

    def generate(self, git_executable: str | None = None) -> dict[str, str]:
        executable = self.git if git_executable is None else git_executable
        subprocess.run(
            [
                self.cmake,
                f"-DHARKEN_RTQ_SOURCE_DIR={self.repo}",
                f"-DHARKEN_RTQ_GIT_EXECUTABLE={executable}",
                f"-DHARKEN_RTQ_OUTPUT={self.output}",
                "-P",
                str(SCRIPT),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        return read_defines(self.output)

    def test_clean_tracked_dirty_and_untracked_states(self) -> None:
        clean = self.generate()
        self.assertEqual(clean["HARKEN_RTQ_GIT_COMMIT"], self.commit)
        self.assertEqual(clean["HARKEN_RTQ_GIT_DIRTY"], "0")
        self.assertEqual(clean["HARKEN_RTQ_TRACKED_WORKTREE_CLEAN"], "1")
        self.assertEqual(clean["HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT"], "0")

        self.source.write_text("int value = 2;\n", encoding="utf-8")
        tracked_dirty = self.generate()
        self.assertEqual(tracked_dirty["HARKEN_RTQ_GIT_COMMIT"], self.commit)
        self.assertEqual(tracked_dirty["HARKEN_RTQ_GIT_DIRTY"], "1")
        self.assertEqual(
            tracked_dirty["HARKEN_RTQ_TRACKED_WORKTREE_CLEAN"], "0"
        )
        self.assertEqual(
            tracked_dirty["HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT"], "0"
        )

        self.source.write_text("int value = 1;\n", encoding="utf-8")
        (self.repo / "local.txt").write_text("local\n", encoding="utf-8")
        untracked = self.generate()
        self.assertEqual(untracked["HARKEN_RTQ_GIT_COMMIT"], self.commit)
        self.assertEqual(untracked["HARKEN_RTQ_GIT_DIRTY"], "1")
        self.assertEqual(untracked["HARKEN_RTQ_TRACKED_WORKTREE_CLEAN"], "1")
        self.assertEqual(
            untracked["HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT"], "1"
        )

    def test_missing_git_fails_closed(self) -> None:
        identity = self.generate(git_executable="")
        self.assertEqual(identity["HARKEN_RTQ_GIT_COMMIT"], "unknown")
        self.assertEqual(identity["HARKEN_RTQ_GIT_DIRTY"], "1")
        self.assertEqual(identity["HARKEN_RTQ_TRACKED_WORKTREE_CLEAN"], "0")
        self.assertEqual(
            identity["HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT"], "0"
        )

    def test_partial_git_failure_fails_closed(self) -> None:
        if os.name == "nt":
            fake_git = self.root / "fake-git.cmd"
            fake_git.write_text(
                "@echo off\r\n"
                'if "%1"=="ls-files" exit /b 17\r\n'
                f'"{self.git}" %*\r\n',
                encoding="utf-8",
            )
        else:
            fake_git = self.root / "fake-git"
            fake_git.write_text(
                "#!/bin/sh\n"
                'if [ "$1" = "ls-files" ]; then exit 17; fi\n'
                f"exec {shlex.quote(self.git)} \"$@\"\n",
                encoding="utf-8",
            )
            fake_git.chmod(0o755)

        identity = self.generate(git_executable=str(fake_git))
        self.assertEqual(identity["HARKEN_RTQ_GIT_COMMIT"], "unknown")
        self.assertEqual(identity["HARKEN_RTQ_GIT_DIRTY"], "1")
        self.assertEqual(identity["HARKEN_RTQ_TRACKED_WORKTREE_CLEAN"], "0")
        self.assertEqual(
            identity["HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT"], "0"
        )

if __name__ == "__main__":
    unittest.main()
