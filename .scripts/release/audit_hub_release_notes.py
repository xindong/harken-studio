#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
CATALOG = ROOT / "HarkenHub" / "Resources" / "release-notes.json"
VERSION = ROOT / "HarkenHub" / "VERSION"
VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-dev)?$")
ALLOWED_CATEGORIES = {"added", "improved", "removed"}


def fail(message):
    raise ValueError(message)


def load_catalog():
    with CATALOG.open("r", encoding="utf-8") as stream:
        catalog = json.load(stream)
    if catalog.get("schemaVersion") != 1:
        fail("unsupported schemaVersion")
    releases = catalog.get("releases")
    if not isinstance(releases, list) or not releases:
        fail("releases must be a non-empty list")
    return catalog


def version_key(value):
    match = VERSION_PATTERN.fullmatch(value)
    if not match:
        fail(f"invalid Hub version: {value}")
    return tuple(int(part) for part in match.groups())


def validate(catalog):
    releases = catalog["releases"]
    current = VERSION.read_text(encoding="utf-8").strip()
    if releases[0].get("version") != current:
        fail(f"latest release {releases[0].get('version')} does not match {current}")

    versions = [entry.get("version", "") for entry in releases]
    if len(versions) != len(set(versions)):
        fail("release versions must be unique")
    if [version_key(value) for value in versions] != sorted(
        (version_key(value) for value in versions), reverse=True
    ):
        fail("release versions must be newest first")

    referenced = []
    expected_authors = {}
    for entry in releases:
        if not entry.get("date"):
            fail(f"release {entry.get('version')} has no date")
        changes = entry.get("changes")
        if not isinstance(changes, list) or not changes:
            fail(f"release {entry.get('version')} has no user-facing changes")
        for change in changes:
            if change.get("category") not in ALLOWED_CATEGORIES:
                fail(f"release {entry.get('version')} has an invalid category")
            if not isinstance(change.get("text"), str) or not change["text"].strip():
                fail(f"release {entry.get('version')} has an empty change")
        contributors = entry.get("contributors")
        if not isinstance(contributors, list) or not contributors:
            fail(f"release {entry.get('version')} has no contributors")
        if len(contributors) != len(set(contributors)) or any(
            not isinstance(account, str) or not account.strip()
            for account in contributors
        ):
            fail(f"release {entry.get('version')} has invalid contributors")
        for number in entry.get("pullRequests", []):
            if not isinstance(number, int) or number <= 0:
                fail(f"release {entry.get('version')} has an invalid PR number")
            referenced.append(number)
            expected_authors[number] = set(contributors)

    excluded = []
    for numbers in catalog.get("excludedPullRequests", {}).values():
        excluded.extend(numbers)
    classified = referenced + excluded
    if len(classified) != len(set(classified)):
        fail("a pull request is classified more than once")
    return set(referenced), set(excluded), expected_authors


def github_pull_requests(repository):
    command = [
        "gh", "pr", "list", "--repo", repository, "--state", "merged",
        "--limit", "1000", "--json", "number,title,body,mergedAt,files,author",
    ]
    result = subprocess.run(
        command, check=True, capture_output=True, text=True, encoding="utf-8"
    )
    return json.loads(result.stdout)


def verify_github(repository, referenced, excluded, expected_authors):
    pull_requests = github_pull_requests(repository)
    hub_pull_requests = {
        item["number"]: item
        for item in pull_requests
        if any(file["path"].startswith("HarkenHub/") for file in item["files"])
    }
    missing = sorted(set(hub_pull_requests) - referenced - excluded)
    stale = sorted((referenced | excluded) - set(hub_pull_requests))
    empty_descriptions = sorted(
        number for number in referenced
        if not hub_pull_requests.get(number, {}).get("body", "").strip()
    )
    contributor_mismatches = sorted(
        number for number in referenced
        if number in hub_pull_requests
        and hub_pull_requests[number]["author"]["login"]
        not in expected_authors[number]
    )
    if missing:
        fail(f"unclassified merged Hub PRs: {missing}")
    if stale:
        fail(f"classified PRs are not merged Hub PRs: {stale}")
    if empty_descriptions:
        fail(f"release-note source PRs have empty descriptions: {empty_descriptions}")
    if contributor_mismatches:
        fail(f"release contributor lists do not include PR authors: {contributor_mismatches}")
    print(
        f"Verified {len(referenced)} release-note PRs and "
        f"{len(excluded)} excluded PRs against {repository}."
    )


def main():
    parser = argparse.ArgumentParser(
        description="Validate Hub release notes and their GitHub PR sources."
    )
    parser.add_argument("--github", action="store_true")
    parser.add_argument("--repository", default="xindong/harken-studio")
    args = parser.parse_args()

    try:
        catalog = load_catalog()
        referenced, excluded, expected_authors = validate(catalog)
        print(
            f"Validated {len(catalog['releases'])} Hub releases, "
            f"{len(referenced)} source PRs, and {len(excluded)} exclusions."
        )
        if args.github:
            verify_github(args.repository, referenced, excluded, expected_authors)
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"release-notes audit failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
