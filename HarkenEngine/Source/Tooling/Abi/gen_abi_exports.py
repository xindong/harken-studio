#!/usr/bin/env python3
"""Windows export allowlist generator for the HarkenCore shared library.

The shared library's export surface is an ABI contract: every named export is a
symbol a shipped game may bind to, and one that cannot be withdrawn without
breaking it. Letting the linker decide that set (CMake's
WINDOWS_EXPORT_ALL_SYMBOLS) publishes every non-static symbol in the runtime and
its vendored dependencies -- C++ mangled internals, allocator templates, and the
whole miniaudio surface -- none of which is a promise the project intends to
keep. This generator derives the export set from the one declaration site that
IS the public contract, so the DLL exports exactly the documented C API:

  <repo>/HarkenEngine/Source/Runtime/api/include/harken/Harken.h
        The public C API header. Every public entry point is declared at file
        scope inside its extern "C" block as `VnResult Vn_<Name>(...)`.

  <repo>/HarkenEngine/Source/Runtime/api/abi/HarkenCore.def
        The emitted module-definition file. Listed in the harken_core_shared
        target's sources on Windows, where CMake forwards it to the linker as
        /DEF: -- making it both the linker's instruction and the reviewable
        allowlist. Names are emitted sorted so adding an entry point produces a
        one-line diff.

Following the effect-registry and binding-pin precedent (gen_effect_registry.py,
gen_binding.py), the generator lives under Tooling/ and writes into the tree of
its consumer: the .def belongs to the api module whose header it mirrors and
whose shared-library target consumes it.

A module-definition file was chosen over annotating each declaration with an
export macro. Harken.h is a platform-conditional-free C header that three
generators parse as their source of truth (this one, gen_binding.py, and
gen_effect_registry.py); threading a dllexport/dllimport/visibility macro
through all of its declarations would change every line those parsers read, to
express a set this file already states directly and reviewably.

The .def is checked in so no consumer build depends on Python. Regenerate after
adding or removing a public entry point:

    py -3 HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py

Verify the checked-in .def still matches the header without rewriting it (also
wired as the ctest abi_export_allowlist_check):

    py -3 HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py --check

Verify a linked DLL's actual PE export directory equals the allowlist -- the
end-to-end proof that the build honored the .def, rather than trust that it did
(also wired as the ctest abi_export_surface_check):

    py -3 HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py --verify-dll <path>
"""

import argparse
import os
import re
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))
HARKEN_H = os.path.join(
    REPO_ROOT, "HarkenEngine", "Source", "Runtime", "api", "include", "harken",
    "Harken.h")
OUT_PATH = os.path.join(
    REPO_ROOT, "HarkenEngine", "Source", "Runtime", "api", "abi",
    "HarkenCore.def")

HEADER = (
    "; generated, do not edit.\n"
    "; Source: HarkenEngine/Source/Runtime/api/include/harken/Harken.h\n"
    "; Regenerate: py -3 HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py\n"
    ";\n"
    "; The Windows export allowlist for the HarkenCore shared library. Every\n"
    "; name here is a public ABI commitment; nothing outside this list is\n"
    "; exported. No LIBRARY statement -- the output name comes from the build.\n"
    "EXPORTS\n")

# A public entry point at file scope: the return type must be exactly VnResult
# with nothing else between it and the line start. Whitespace before the argument
# list may span lines, so an ordinary reformat stays a recognized declaration.
DECL_RE = re.compile(r"^VnResult\s+(Vn_[A-Za-z0-9_]+)\s*\(", re.MULTILINE)

# Every Vn_-prefixed identifier, wherever it appears. The audit works from this
# rather than from anything call-shaped: requiring an adjacent "(" let a macro-
# wrapped declaration -- HARKEN_API_NAME(Vn_Name)(void), where the name is
# followed by ")" -- and a name separated from its argument list by a newline
# slip past unseen, which is exactly the silent omission the audit exists to
# prevent. Both are legal C, and both would have shipped a DLL missing that
# entry point while every check stayed green.
IDENT_RE = re.compile(r"\bVn_[A-Za-z0-9_]+\b")

# Vn_-prefixed identifiers that are deliberately not exported entry points. Empty
# today: every such identifier in the header is a public function. A future
# non-exported use (a callback typedef, say) belongs here with a reason, so the
# exemption is reviewed once rather than the audit being loosened for everything.
NON_EXPORT_IDENTIFIERS = frozenset()


def strip_comments(text):
    """Blank out comments and string-literal contents, preserving line structure.

    Line count is preserved so diagnostics can cite real header line numbers.
    String literals are recognized so a comment token inside one is not mistaken
    for a comment, and their contents are then blanked: prose in a literal is not
    a declaration, and an identifier mentioned there must not be audited as a
    missing export.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(quote)
                    i += 1
                    break
                # Blank the content, keeping newlines so line numbers hold.
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end == -1:
                end = n
            else:
                end += 2
            out.append("\n" * text.count("\n", i, end))
            i = end
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end == -1:
                i = n
            else:
                i = end
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def read_header(header_path):
    """Return the header text, or fail loudly if it cannot be read.

    An unreadable header must never be mistaken for a header that declares
    nothing: silently proceeding would emit (or approve) an empty allowlist.
    """
    try:
        with open(header_path, "r", encoding="utf-8") as f:
            return f.read()
    except OSError as error:
        raise SystemExit("cannot read the public API header %s: %s"
                         % (header_path, error))


def find_public_exports(header_path):
    """Return the sorted public entry-point names declared in Harken.h.

    Fail-closed on the identifier, not on a syntax guess: every Vn_-prefixed
    identifier must be either a recognized declaration or an explicitly listed
    non-export, otherwise it is a hard error. Auditing anything narrower means
    the shapes the pattern does not model -- a macro-wrapped declaration, a name
    separated from its argument list by a newline -- pass unseen and ship a DLL
    missing that entry point with every check still green.
    """
    text = strip_comments(read_header(header_path))

    names = DECL_RE.findall(text)
    duplicates = sorted({n for n in names if names.count(n) > 1})
    if duplicates:
        raise SystemExit(
            "%s: entry point declared more than once: %s"
            % (header_path, ", ".join(duplicates)))

    declared_spans = {m.start(1) for m in DECL_RE.finditer(text)}
    unexplained = []
    for m in IDENT_RE.finditer(text):
        if m.start() in declared_spans:
            continue
        if m.group() in NON_EXPORT_IDENTIFIERS:
            continue
        line = text.count("\n", 0, m.start()) + 1
        unexplained.append((line, m.group()))
    if unexplained:
        detail = ", ".join("%s (line %d)" % (name, line)
                           for line, name in unexplained)
        raise SystemExit(
            "%s: Vn_-prefixed identifier that is not a recognized "
            "`VnResult Vn_Name(...)` file-scope declaration: %s. A public entry "
            "point must be declared in that shape so it reaches the export "
            "allowlist; anything else must be listed in NON_EXPORT_IDENTIFIERS "
            "with a reason. It must not be omitted from the DLL by accident."
            % (header_path, detail))

    if not names:
        raise SystemExit(
            "%s: no public entry points found; refusing to emit an empty "
            "export allowlist" % header_path)
    return sorted(names)


def emit(names):
    body = "".join("    %s\n" % name for name in names)
    return HEADER + body


def parse_def(path):
    """Return the sorted export names listed in a module-definition file."""
    names = []
    in_exports = False
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            if not line:
                continue
            if line.upper() == "EXPORTS":
                in_exports = True
                continue
            if in_exports:
                names.append(line.split()[0])
    return sorted(names)


def read_pe_export_names(path):
    """Return the sorted named exports in a PE image's export directory.

    Parsed here rather than shelled out to dumpbin / llvm-readobj so the CI
    check needs nothing on PATH beyond the Python the other generator checks
    already require.
    """
    with open(path, "rb") as f:
        image = f.read()

    if image[:2] != b"MZ":
        raise SystemExit("%s: not a PE image (missing MZ signature)" % path)
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit("%s: not a PE image (missing PE signature)" % path)

    coff = pe_offset + 4
    number_of_sections = struct.unpack_from("<H", image, coff + 2)[0]
    size_of_optional_header = struct.unpack_from("<H", image, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", image, optional)[0]
    if magic == 0x20B:      # PE32+
        data_directory = optional + 112
    elif magic == 0x10B:    # PE32
        data_directory = optional + 96
    else:
        raise SystemExit("%s: unknown optional header magic 0x%X" % (path, magic))

    export_rva, export_size = struct.unpack_from("<II", image, data_directory)
    if export_rva == 0 or export_size == 0:
        return []

    sections = []
    section_table = optional + size_of_optional_header
    for index in range(number_of_sections):
        entry = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", image, entry + 8)
        sections.append((virtual_address, max(virtual_size, raw_size),
                         raw_pointer))

    def to_offset(rva):
        for virtual_address, span, raw_pointer in sections:
            if virtual_address <= rva < virtual_address + span:
                return raw_pointer + (rva - virtual_address)
        raise SystemExit("%s: RVA 0x%X is outside every section" % (path, rva))

    directory = to_offset(export_rva)
    number_of_names = struct.unpack_from("<I", image, directory + 24)[0]
    address_of_names = struct.unpack_from("<I", image, directory + 32)[0]
    if number_of_names == 0:
        return []

    names_table = to_offset(address_of_names)
    names = []
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", image, names_table + index * 4)[0]
        start = to_offset(name_rva)
        end = image.index(b"\0", start)
        names.append(image[start:end].decode("ascii"))
    return sorted(names)


def describe_difference(expected, actual):
    """Print the two-way name difference; return True when the sets differ."""
    missing = [n for n in expected if n not in set(actual)]
    extra = [n for n in actual if n not in set(expected)]
    if not missing and not extra:
        return False
    for name in missing:
        print("  missing (declared, not exported): %s" % name)
    for name in extra:
        print("  unexpected (exported, not declared): %s" % name)
    print("  %d missing, %d unexpected" % (len(missing), len(extra)))
    return True


def parse_args(argv):
    """Parse the command line, rejecting anything not understood.

    Strictness is the point. A permissive parser that treated a misspelled
    --check as "no mode requested" would turn this gate into a no-op that also
    rewrites the checked-in allowlist -- green forever, and hiding the very
    drift it exists to catch. argparse exits non-zero on an unknown option, a
    missing --verify-dll operand, and any positional argument.
    """
    parser = argparse.ArgumentParser(
        prog="gen_abi_exports.py",
        description="Generate or verify the Windows export allowlist for the "
                    "HarkenCore shared library.",
        allow_abbrev=False)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check", action="store_true",
        help="verify the checked-in .def matches Harken.h without rewriting it")
    mode.add_argument(
        "--verify-dll", metavar="PATH",
        help="verify a linked DLL's PE export directory equals the allowlist")
    args = parser.parse_args(argv)
    return args


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    check = args.check
    verify_dll = args.verify_dll

    names = find_public_exports(HARKEN_H)

    if verify_dll:
        if not os.path.isfile(verify_dll):
            print("MISMATCH: %s does not exist; build harken_core_shared before "
                  "verifying its export table" % verify_dll)
            return 1
        if not os.path.isfile(OUT_PATH):
            print("MISMATCH: the export allowlist %s is missing; run the "
                  "generator" % OUT_PATH)
            return 1
        # The allowlist must agree with the header first: verifying a DLL
        # against a .def that has already drifted proves nothing.
        if parse_def(OUT_PATH) != names:
            print("MISMATCH: %s has drifted from Harken.h; run the generator"
                  % OUT_PATH)
            return 1
        exported = read_pe_export_names(verify_dll)
        if exported != names:
            print("MISMATCH: %s" % verify_dll)
            describe_difference(names, exported)
            return 1
        print("ok: %s exports exactly the %d allowlisted entry points"
              % (verify_dll, len(names)))
        return 0

    content = emit(names)
    if check:
        try:
            with open(OUT_PATH, "r", encoding="utf-8", newline="") as f:
                existing = f.read()
        except OSError:
            existing = None
        if existing != content:
            print("MISMATCH: %s" % OUT_PATH)
            if existing is None:
                print("  the allowlist is missing; run the generator")
            elif not describe_difference(names, parse_def(OUT_PATH)):
                print("  the entry points agree but the file text has drifted; "
                      "run the generator")
            return 1
        print("ok: %s (%d entry points)" % (OUT_PATH, len(names)))
        return 0

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("wrote: %s (%d entry points)" % (OUT_PATH, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
