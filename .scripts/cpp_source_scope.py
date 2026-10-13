from __future__ import annotations

from pathlib import Path


EXCLUDED_PARTS = frozenset(
    {
        "thirdparty",
        "third_party",
        "third-party",
        "_deps",
        "gtest",
        "googletest",
        "generated",
    }
)
CPP_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".m", ".mm"})
TRANSLATION_UNIT_SCOPES = frozenset(
    {"runtime", "tooling", "runtime_tests", "runtime_benchmarks", "editor_tests"}
)
INVENTORY_ROOTS = (
    Path("HarkenEngine/Source/Runtime"),
    Path("HarkenEngine/Source/Tooling"),
    Path("HarkenEngine/Test/UnitTest"),
    Path("HarkenEngine/Test/E2E"),
    Path("HarkenEngine/Test/Benchmark"),
    Path("HarkenStudio/Test"),
)


def relative_parts(path: Path, repo_root: Path) -> tuple[str, ...] | None:
    try:
        return tuple(part.casefold() for part in path.relative_to(repo_root).parts)
    except ValueError:
        return None


def is_excluded_parts(parts: tuple[str, ...]) -> bool:
    return (
        any(part in EXCLUDED_PARTS for part in parts)
        or "studio" in parts
        or (
            parts
            and parts[0] == "harkenstudio"
            and parts[:2] != ("harkenstudio", "test")
        )
    )


def translation_unit_scope(path: Path, repo_root: Path) -> str | None:
    parts = relative_parts(path, repo_root)
    if parts is None or is_excluded_parts(parts):
        return None
    if parts[:4] == ("harkenengine", "test", "e2e", "abihistoryclient"):
        return None
    if parts[:3] == ("harkenengine", "source", "runtime"):
        return "runtime"
    if parts[:3] == ("harkenengine", "source", "tooling"):
        return "tooling"
    if parts[:3] == ("harkenengine", "test", "benchmark"):
        return "runtime_benchmarks"
    if parts[:2] == ("harkenstudio", "test"):
        return "editor_tests"
    if parts[:3] in {
        ("harkenengine", "test", "unittest"),
        ("harkenengine", "test", "e2e"),
    }:
        return "runtime_tests"
    return None


def path_sort_key(path: Path) -> tuple[str, str]:
    rendered = path.as_posix()
    return rendered.casefold(), rendered


def sorted_paths(paths) -> list[Path]:
    return sorted(paths, key=path_sort_key)


def membership_details(
    missing: list[Path], unexpected: list[Path], repo_root: Path
) -> str:
    def render(path: Path) -> str:
        try:
            return path.relative_to(repo_root).as_posix()
        except ValueError:
            return path.as_posix()

    details = []
    if missing:
        details.append("missing: " + ", ".join(render(path) for path in missing))
    if unexpected:
        details.append("unexpected: " + ", ".join(render(path) for path in unexpected))
    return "; ".join(details)


def eligible_translation_units(repo_root: Path) -> list[Path]:
    root = repo_root.resolve(strict=False)
    files: set[Path] = set()
    for relative_root in INVENTORY_ROOTS:
        inventory_root = root / relative_root
        if not inventory_root.is_dir():
            continue
        for candidate in inventory_root.rglob("*"):
            if (
                candidate.is_file()
                and candidate.suffix.casefold() in CPP_SOURCE_SUFFIXES
                and translation_unit_scope(candidate, root) is not None
            ):
                files.add(candidate.resolve(strict=False))
    return sorted_paths(files)
