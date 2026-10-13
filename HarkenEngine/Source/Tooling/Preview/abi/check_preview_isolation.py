#!/usr/bin/env python3
"""Checks that the development preview surface stays out of the gameplay face.

Two independent claims, both read from the tree rather than trusted:

  1. The export manifest and the header agree, in both directions. An entry point
     the header declares but the manifest omits fails, and so does a manifest name
     the header does not declare. Without this the manifest would decay into a
     comment, and the second claim would then be checking a stale list.

  2. No token the preview surface owns appears in a gameplay product, or in either
     input a gameplay product is generated from:

       HarkenEngine/Source/Runtime/api/include/harken/Harken.h   public C API header
       HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json  binding table
       HarkenEngine/Source/Runtime/api/abi/HarkenCore.def       export allowlist
       HarkenSDK/Unity/Harken/Runtime/Generated/*.g.cs                      C# binding

     The two generators today derive their output from the header and the table
     alone and never read the preview header, so the isolation is structural. This
     check is what makes that structure load-bearing rather than incidental:
     declaring a preview entry in the public header, or naming one in the wire
     table, fails here immediately -- at the moment it is written, before anything
     is regenerated and before a preview symbol could reach a shipped DLL or a
     game's C# assembly.

Run:

    py -3 HarkenEngine/Source/Tooling/Preview/abi/check_preview_isolation.py

Exit status is zero when both claims hold; on failure every violation is printed
with its file and line, so one run reports the whole problem.

The second claim is intentionally a lexical tripwire over the four token names
listed in `DevelopmentPreview.exports`. A green result proves those tokens do
not appear in the scanned gameplay files; it does not prove that a preview
declaration renamed to avoid those tokens would be found. Structural isolation
comes from the build graph and from generators whose inputs are limited to the
gameplay face, not from this scanner.
"""

import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(
    os.path.join(SCRIPT_DIR, "..", "..", "..", "..", ".."))

MANIFEST = os.path.join(SCRIPT_DIR, "DevelopmentPreview.exports")
PREVIEW_HEADER = os.path.join(
    REPO_ROOT, "HarkenEngine", "Source", "Tooling", "Preview", "include",
    "harken_preview", "PreviewAbi.h")
PREVIEW_PROJECT_HEADER = os.path.join(
    os.path.dirname(PREVIEW_HEADER), "PreviewProject.h")

# The gameplay face: the two generator inputs first, then the products. Inputs
# are listed because failing on the input is what makes the failure early --
# a product only acquires the name once somebody regenerates it.
GAMEPLAY_FILES = [
    os.path.join(REPO_ROOT, "HarkenEngine", "Source", "Runtime", "api",
                 "include", "harken", "Harken.h"),
    os.path.join(REPO_ROOT, "HarkenEngine", "Source", "Tooling", "Remote",
                 "wire", "harken_wire_table.json"),
    os.path.join(REPO_ROOT, "HarkenEngine", "Source", "Runtime", "api", "abi",
                 "HarkenCore.def"),
    os.path.join(REPO_ROOT, "HarkenSDK", "Unity", "Harken", "Runtime", "Generated",
                 "HarkenNative.g.cs"),
    os.path.join(REPO_ROOT, "HarkenSDK", "Unity", "Harken", "Runtime", "Generated",
                 "HarkenTypes.g.cs"),
    os.path.join(REPO_ROOT, "HarkenSDK", "Unity", "Harken", "Runtime", "Generated",
                 "HarkenAbi.g.cs"),
]

DECL_RE = re.compile(r"^(?:VnPreview\w+|VnResult|uint32_t|void)\s+(VnPreview_[A-Za-z0-9_]+)\s*\(",
                     re.MULTILINE)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def parse_manifest(text):
    """Returns (exports, isolated_tokens) from the manifest's two sections."""
    sections = {"EXPORTS": [], "ISOLATED-TOKENS": []}
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line in sections:
            current = line
            continue
        if current is None:
            raise ValueError("manifest entry before any section header: " + line)
        sections[current].append(line)
    return sections["EXPORTS"], sections["ISOLATED-TOKENS"]


def check_manifest_matches_header(exports, header_text, failures):
    declared = set(DECL_RE.findall(header_text))
    listed = set(exports)
    if len(listed) != len(exports):
        failures.append("DevelopmentPreview.exports lists a name twice")
    for name in sorted(declared - listed):
        failures.append(
            "%s is declared in the preview headers but missing from "
            "DevelopmentPreview.exports" % name)
    for name in sorted(listed - declared):
        failures.append(
            "%s is listed in DevelopmentPreview.exports but not declared in "
            "the preview headers" % name)
    if exports != sorted(exports):
        failures.append(
            "DevelopmentPreview.exports EXPORTS is not sorted, so adding an "
            "entry would not produce a one-line diff")


def check_gameplay_face(tokens, failures):
    for path in GAMEPLAY_FILES:
        if not os.path.exists(path):
            failures.append("gameplay face file is missing: %s" %
                            os.path.relpath(path, REPO_ROOT))
            continue
        relative = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        for number, line in enumerate(read(path).splitlines(), start=1):
            for token in tokens:
                if token in line:
                    failures.append(
                        "%s:%d names the preview token %r: the development "
                        "preview surface must not reach the gameplay face" %
                        (relative, number, token))


def main():
    failures = []
    exports, tokens = parse_manifest(read(MANIFEST))
    if not exports:
        failures.append("DevelopmentPreview.exports declares no entry points")
    if not tokens:
        failures.append("DevelopmentPreview.exports declares no isolated tokens")
    check_manifest_matches_header(
        exports, read(PREVIEW_HEADER) + "\n" + read(PREVIEW_PROJECT_HEADER), failures)
    check_gameplay_face(tokens, failures)

    if failures:
        sys.stderr.write("preview isolation check failed:\n")
        for failure in failures:
            sys.stderr.write("  %s\n" % failure)
        return 1
    print("preview isolation check: %d entry points, %d isolated tokens, "
          "%d gameplay files clean" %
          (len(exports), len(tokens), len(GAMEPLAY_FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
