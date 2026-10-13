#!/usr/bin/env python3
"""Checks the preview allocation table against the header it documents.

The allocation table (PreviewAbiAllocation.md) is where the whole status space is
readable at once, including the codes that are allocated but not yet produced. A
table that drifts from the header is worse than no table: it reads as a
specification while describing something the build does not do, and the drift is
silent -- a code added to the header and not to the table breaks nothing that
compiles.

So the two are compared in both directions:

  * every `#define VN_PREVIEW_<NAME> <value>` status macro in the header appears in
    the table with the same value, and vice versa;
  * the space is dense from zero with no gap, because a gap is a number nobody can
    account for later;
  * every row states whether this vintage produces the code, so an allocated code
    that nothing returns is visible as such rather than implied to work.

Run:

    py -3 HarkenEngine/Source/Tooling/Preview/abi/check_preview_allocation.py
"""

import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TABLE = os.path.join(SCRIPT_DIR, "PreviewAbiAllocation.md")
HEADER = os.path.join(SCRIPT_DIR, "..", "include", "harken_preview",
                      "PreviewAbi.h")

# A status macro in the header. Only the plain decimal status space is matched:
# the bit-flag and enumerated-field macros in the same header are cast
# expressions or belong to other axes, and they are documented in their own
# sections of the table rather than in the status table.
HEADER_STATUS_RE = re.compile(
    r"^#define\s+(VN_PREVIEW_[A-Z0-9_]+)\s+(\d+)\s*$", re.MULTILINE)

# A status row of the table: | value | `NAME` | produced | meaning |
TABLE_ROW_RE = re.compile(
    r"^\|\s*(\d+)\s*\|\s*`(VN_PREVIEW_[A-Z0-9_]+)`\s*\|\s*(yes|no)\s*\|")

# Macros that are decimal but are not status codes: they name positions on other
# enumerated axes, each documented in its own table section. Listed explicitly so
# a new status macro cannot be quietly absorbed into this exemption.
NON_STATUS_PREFIXES = (
    "VN_PREVIEW_PLAYABLE_KIND_",
    "VN_PREVIEW_LIFECYCLE_",
    "VN_PREVIEW_REASON_",
)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def header_statuses(text):
    found = {}
    for name, value in HEADER_STATUS_RE.findall(text):
        if name.startswith(NON_STATUS_PREFIXES):
            continue
        found[name] = int(value)
    return found


def table_statuses(text, failures):
    found = {}
    for line in text.splitlines():
        match = TABLE_ROW_RE.match(line)
        if match is None:
            continue
        value, name, _produced = match.group(1), match.group(2), match.group(3)
        if name in found:
            failures.append("%s appears twice in the status table" % name)
        found[name] = int(value)
    return found


def main():
    failures = []
    header = header_statuses(read(HEADER))
    table = table_statuses(read(TABLE), failures)

    if not header:
        failures.append("no status macros found in PreviewAbi.h")
    if not table:
        failures.append("no status rows found in PreviewAbiAllocation.md")

    for name in sorted(set(header) - set(table)):
        failures.append(
            "%s = %d is defined in PreviewAbi.h but has no row in "
            "PreviewAbiAllocation.md" % (name, header[name]))
    for name in sorted(set(table) - set(header)):
        failures.append(
            "%s = %d has a row in PreviewAbiAllocation.md but is not defined in "
            "PreviewAbi.h" % (name, table[name]))
    for name in sorted(set(header) & set(table)):
        if header[name] != table[name]:
            failures.append(
                "%s is %d in PreviewAbi.h and %d in PreviewAbiAllocation.md" %
                (name, header[name], table[name]))

    values = sorted(table.values())
    if values and values != list(range(len(values))):
        failures.append(
            "the documented status space is not dense from zero: %s" % values)

    if failures:
        sys.stderr.write("preview allocation table check failed:\n")
        for failure in failures:
            sys.stderr.write("  %s\n" % failure)
        return 1
    print("preview allocation table check: %d status codes agree with the "
          "header" % len(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
