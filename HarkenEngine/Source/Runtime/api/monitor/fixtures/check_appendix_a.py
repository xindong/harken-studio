#!/usr/bin/env python3
"""Mechanical Appendix A gate for Monitor M0-6.

`--check` never rewrites. It parses APPENDIX_A_COVERAGE.md, requires every
row to have a category, and for non-deferred rows requires an exact gtest
id present in UnitTest sources. Fixture paths must exist and match a
checked-in manifest SHA when the path is listed. ModelOnly ids from
MonitorModelOnlyTests.txt must not appear in RuntimeVerified or
ContractVerified counts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[6]
APPENDIX = REPO / "HarkenEngine" / "Source" / "Runtime" / "api" / "monitor" / "APPENDIX_A_COVERAGE.md"
MODEL_ONLY = (
    REPO
    / "HarkenEngine"
    / "Source"
    / "Runtime"
    / "api"
    / "monitor"
    / "fixtures"
    / "MonitorModelOnlyTests.txt"
)
UNITTEST = REPO / "HarkenEngine" / "Test" / "UnitTest"
CATEGORIES = {"RuntimeVerified", "ContractVerified", "ModelOnly", "deferred"}
TEST_ID = re.compile(r"^Monitor(?:Fixture|AbiPin|ModelOnly)\.[A-Za-z0-9_]+$")
STRIP_TICKS = re.compile(r"^`+|`+$")


def strip_cell(value: str) -> str:
    return STRIP_TICKS.sub("", value.strip())


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_gtest_ids() -> set[str]:
    ids: set[str] = set()
    pattern = re.compile(r"^TEST\((\w+),\s*(\w+)\)", re.M)
    for path in UNITTEST.glob("Monitor*.cpp"):
        text = path.read_text(encoding="utf-8")
        for suite, name in pattern.findall(text):
            ids.add("%s.%s" % (suite, name))
    return ids


def load_model_only() -> set[str]:
    ids: set[str] = set()
    for raw in MODEL_ONLY.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        ids.add(line)
    return ids


def parse_appendix() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for raw in APPENDIX.read_text(encoding="utf-8").splitlines():
        if not raw.startswith("|"):
            continue
        cells = [cell.strip() for cell in raw.strip().strip("|").split("|")]
        if len(cells) < 9:
            continue
        if cells[0] in ("Later milestone fact", "---"):
            continue
        if set(cells[0]) <= {"-", " "}:
            continue
        rows.append(
            {
                "fact": strip_cell(cells[0]),
                "field": strip_cell(cells[1]),
                "category": strip_cell(cells[2]),
                "testId": strip_cell(cells[3]),
                "producingPackage": strip_cell(cells[4]),
                "fixturePath": strip_cell(cells[5]).replace("\\", "/"),
                "deferredOwner": strip_cell(cells[6]),
                "deferredTarget": strip_cell(cells[7]),
                "deferredReason": strip_cell(cells[8]),
            }
        )
    return rows


def fixture_manifest_entry(rel: str) -> dict | None:
    path = UNITTEST / rel
    manifest = path.parent / "manifest.json"
    if not manifest.is_file():
        return None
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    name = path.name
    for item in payload.get("files") or []:
        listed = str(item.get("path") or "").replace("\\", "/")
        if listed == name or listed == rel or listed.endswith("/" + name):
            return item
    return None


def check(rows: list[dict[str, str]]) -> int:
    errors: list[str] = []
    if not rows:
        errors.append("appendix has no data rows")
    gtests = load_gtest_ids()
    model_only = load_model_only()
    counts = {key: 0 for key in CATEGORIES}
    runtime_ids: set[str] = set()
    for row in rows:
        fact = row["fact"] or "<empty fact>"
        if not row["fact"] or not row["field"]:
            errors.append("%s: empty fact or header field" % fact)
        category = row["category"]
        if category not in CATEGORIES:
            errors.append("%s: bad category %r" % (fact, category))
            continue
        counts[category] += 1
        if category == "deferred":
            if row["testId"]:
                errors.append("%s: deferred row must not claim a testId" % fact)
            if not row["deferredOwner"] or not row["deferredTarget"] or not row["deferredReason"]:
                errors.append("%s: deferred row missing owner/target/reason" % fact)
            continue
        test_id = row["testId"]
        if not TEST_ID.match(test_id):
            errors.append("%s: testId must be exact Suite.Case, got %r" % (fact, test_id))
            continue
        if test_id not in gtests:
            errors.append("%s: testId %s not found in UnitTest sources" % (fact, test_id))
        if category in ("RuntimeVerified", "ContractVerified"):
            if test_id in model_only:
                errors.append("%s: %s is ModelOnly and cannot count as %s" % (fact, test_id, category))
            if not row["producingPackage"]:
                errors.append("%s: producingPackage required" % fact)
            runtime_ids.add(test_id)
        if category == "ModelOnly" and test_id not in model_only:
            errors.append("%s: ModelOnly testId missing from MonitorModelOnlyTests.txt" % fact)
        fixture = row["fixturePath"]
        if fixture:
            path = UNITTEST / fixture
            if not path.is_file():
                errors.append("%s: missing fixture %s" % (fact, fixture))
            else:
                entry = fixture_manifest_entry(fixture)
                if entry is None:
                    errors.append("%s: fixture %s not in a monitor manifest" % (fact, fixture))
                else:
                    listed_sha = entry.get("sha256")
                    if listed_sha != sha256_file(path):
                        errors.append("%s: fixture SHA drift %s" % (fact, fixture))
                    listed_pkg = entry.get("producingPackage")
                    if listed_pkg:
                        if listed_pkg != row["producingPackage"]:
                            errors.append(
                                "%s: producingPackage %s != manifest %s"
                                % (fact, row["producingPackage"], listed_pkg)
                            )
                    elif row["producingPackage"] not in ("m0-1", "m0-2"):
                        errors.append(
                            "%s: manifest missing producingPackage for %s" % (fact, fixture)
                        )
    leaked = sorted(runtime_ids & model_only)
    if leaked:
        errors.append("ModelOnly ids counted in Runtime/Contract: %s" % leaked)
    missing_model_only = sorted(model_only - gtests)
    if missing_model_only:
        errors.append("ModelOnly list ids have no TEST(): %s" % missing_model_only)
    if counts["RuntimeVerified"] == 0:
        errors.append("RuntimeVerified count is 0")
    if errors:
        for item in errors:
            print("error:", item, file=sys.stderr)
        return 1
    print(
        "ok: appendix rows=%d runtime=%d contract=%d modelOnly=%d deferred=%d"
        % (
            len(rows),
            counts["RuntimeVerified"],
            counts["ContractVerified"],
            counts["ModelOnly"],
            counts["deferred"],
        )
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.parse_args()
    if not APPENDIX.is_file():
        print("error: appendix missing", file=sys.stderr)
        return 1
    if not MODEL_ONLY.is_file():
        print("error: MonitorModelOnlyTests.txt missing", file=sys.stderr)
        return 1
    return check(parse_appendix())


if __name__ == "__main__":
    raise SystemExit(main())
