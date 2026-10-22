"""Capture build-resolved components, source references, and package receipts."""

from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import collect_legal_texts

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPONENT_INPUTS = (
    {"id": "miniaudio", "notice": "miniaudio", "path": "HarkenEngine/ThirdParty/miniaudio",
     "url": "https://github.com/mackron/miniaudio",
     "evidence": ["HarkenEngine/ThirdParty/miniaudio/miniaudio.h"]},
    {"id": "miniaudio-studio", "notice": "miniaudio", "path": "HarkenStudio/ThirdParty/miniaudio",
     "url": "https://github.com/mackron/miniaudio",
     "evidence": ["HarkenStudio/ThirdParty/miniaudio/miniaudio.h"]},
    {"id": "qtads", "notice": "Qt Advanced Docking System", "path": "HarkenStudio/ThirdParty/QtADS",
     "url": "https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System",
     "legalFiles": ["QtADS-LICENSE.txt", "QtADS-LGPL-2.1.txt"]},
    {"id": "ogg", "notice": "libogg", "path": "HarkenStudio/ThirdParty/libogg",
     "url": "https://xiph.org/ogg/", "legalFiles": ["libogg-COPYING.txt"]},
    {"id": "vorbis", "notice": "libvorbis / vorbisenc", "path": "HarkenStudio/ThirdParty/libvorbis",
     "url": "https://xiph.org/vorbis/", "legalFiles": ["libvorbis-COPYING.txt"]},
    {"id": "r8brain", "notice": "r8brain-free-src", "path": "HarkenStudio/ThirdParty/r8brain-free-src",
     "url": "https://github.com/avaneev/r8brain-free-src",
     "legalFiles": ["r8brain-LICENSE.txt"]},
    {"id": "ooura", "notice": "Ooura FFT (fft4g), embedded in r8brain-free-src", "parent": "r8brain",
     "url": "https://www.kurims.kyoto-u.ac.jp/~ooura/fft.html",
     "legalFiles": ["OouraFFT-NOTICE.txt"]},
    {"id": "pffft", "notice": "PFFFT, embedded in r8brain-free-src", "parent": "r8brain", "scope": "source-only",
     "url": "https://github.com/avaneev/r8brain-free-src",
     "evidence": ["HarkenStudio/ThirdParty/r8brain-free-src/fft/pffft.h"]},
    {"id": "lmdb", "notice": "LMDB", "path": "HarkenStudio/ThirdParty/lmdb",
     "url": "https://www.openldap.org/software/repo.html", "legalFiles": ["LMDB-LICENSE.txt"]},
    {"id": "dr-wav", "notice": "dr_wav (dr_libs)", "path": "HarkenStudio/ThirdParty/dr_wav", "products": ["studio"],
     "url": "https://github.com/mackron/dr_libs", "evidence": ["HarkenStudio/ThirdParty/dr_wav/dr_wav.h"]},
    {"id": "qwindowkit", "notice": "QWindowKit", "sourceDirectory": "qwindowkit-src",
     "url": "https://github.com/stdware/qwindowkit", "legalFiles": ["QWindowKit-LICENSE.txt"]},
    {"id": "qmsetup", "notice": "qmsetup", "parent": "qwindowkit", "sourceParent": "qwindowkit",
     "submodule": "qmsetup", "scope": "build-tool", "url": "https://github.com/stdware/qmsetup", "evidence": []},
    {"id": "syscmdline", "notice": "syscmdline", "parent": "qmsetup", "sourceParent": "qwindowkit",
     "submodule": "qmsetup/src/syscmdline", "scope": "build-tool",
     "url": "https://github.com/SineStriker/syscmdline", "evidence": []},
    {"id": "minizip-ng", "notice": "minizip-ng", "sourceDirectory": "minizip-ng-src",
     "url": "https://github.com/zlib-ng/minizip-ng", "legalFiles": ["minizip-ng-LICENSE.txt"]},
    {"id": "ed25519", "notice": "ed25519 (orlp)", "sourceDirectory": "ed25519-src",
     "url": "https://github.com/orlp/ed25519", "legalFiles": ["ed25519-LICENSE.txt"]},
    {"id": "zlib-ng", "notice": "zlib (or zlib-ng)", "sourceDirectory": "zlib",
     "url": "https://github.com/zlib-ng/zlib-ng", "legalFiles": ["zlib-LICENSE.txt"]},
    {"id": "apple-libcompression", "notice": "Apple libcompression", "scope": "system-library-not-redistributed",
     "url": "https://developer.apple.com/documentation/compression", "evidence": []},
    {"id": "heroicons", "notice": "Heroicons", "products": ["hub"],
     "url": "https://github.com/tailwindlabs/heroicons", "legalFiles": ["Heroicons-LICENSE.txt"]},
)
ROOT_TARGETS = {"studio": ("harken_editor", "harken_packc"), "hub": ("harken_hub",),
                "runtime": ("harken_core_shared",)}


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True,
                                   encoding="utf-8").strip()


def project_source_sha256(relative: str) -> str:
    content = subprocess.check_output(["git", "-C", str(REPO_ROOT), "show", f"HEAD:{relative}"])
    return hashlib.sha256(content).hexdigest()


def checkout_dirty(root: Path, commit: str) -> bool:
    if git(root, "rev-parse", "HEAD") != commit:
        raise ValueError("source checkout does not match the configured build commit")
    return bool(git(root, "status", "--porcelain", "--untracked-files=normal"))


def validate_channel(channel: str) -> None:
    if channel not in {"stable", "beta", "dev", "nightly"}:
        raise ValueError(f"unknown build channel: {channel}")


def check_source(identity: dict) -> dict:
    validate_channel(identity["channel"])
    return {"configuredDirty": identity["gitDirty"],
            "checkoutDirty": checkout_dirty(REPO_ROOT, identity["gitCommit"])}


@contextmanager
def package_output(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".packaging-", dir=output) as temporary:
        staging = Path(temporary)
        yield staging
        paths = list(staging.iterdir())
        for path in paths:
            if (output / path.name).exists() or (output / path.name).is_symlink():
                raise ValueError(f"package output already exists: {output / path.name}")
        for path in paths:
            path.rename(output / path.name)


def file_record(path: Path, root: Path) -> dict:
    name = path.relative_to(root).as_posix()
    if path.is_symlink():
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"payload symlink escapes its root: {name}")
        return {"path": name, "symlink": os.readlink(path)}
    return {"path": name, "size": path.stat().st_size, "sha256": sha256(path)}


def inventory(root: Path, excluded: Path | None = None) -> list[dict]:
    return [file_record(path, root) for path in sorted(root.rglob("*"))
            if (path.is_file() or path.is_symlink()) and not (excluded and path.is_relative_to(excluded))]


def payload_state(root: Path, excluded: Path) -> list[dict]:
    return [{"path": path.relative_to(root).as_posix(), "size": path.stat().st_size,
             "mtimeNs": path.stat().st_mtime_ns,
             "symlink": os.readlink(path) if path.is_symlink() else None}
            for path in sorted(root.rglob("*"))
            if (path.is_file() or path.is_symlink()) and not path.is_relative_to(excluded)]


def build_model(build_dir: Path, product: str, configuration: str, extra_targets: tuple[str, ...] = ()) -> dict:
    identity = read_json(build_dir / "generated/release-identity.json")
    source_state = check_source(identity)
    indexes = list((build_dir / ".cmake/api/v1/reply").glob("index-*.json"))
    if not indexes:
        raise ValueError("CMake File API reply is missing; reconfigure this build directory")
    index = read_json(max(indexes, key=lambda path: path.stat().st_mtime_ns))
    reply = max(indexes, key=lambda path: path.stat().st_mtime_ns).parent
    objects = {item["kind"]: read_json(reply / item["jsonFile"]) for item in index["objects"]}
    model = objects["codemodel"]
    if Path(model["paths"]["source"]).resolve() != REPO_ROOT.resolve():
        raise ValueError("CMake build belongs to a different source checkout")
    configs = [config for config in model["configurations"] if config["name"] == configuration]
    if len(configs) != 1:
        raise ValueError(f"CMake configuration is missing: {configuration}")
    all_targets = {target["id"]: read_json(reply / target["jsonFile"])
                   for target in configs[0]["targets"]}
    by_name = {target["name"]: target for target in all_targets.values()}
    roots = list(ROOT_TARGETS[product]) + list(extra_targets)
    if product == "hub" and "harken_hub_update_bridge" in by_name:
        roots.append("harken_hub_update_bridge")
    if product == "runtime" and "harken_core_shared" not in by_name:
        roots = ["harken_core"]
    if any(name not in by_name for name in roots):
        raise ValueError(f"build is missing product targets: {roots}")
    targets = {}
    pending = [by_name[name] for name in roots]
    while pending:
        target = pending.pop()
        if target["id"] in targets:
            continue
        targets[target["id"]] = target
        pending.extend(all_targets[dep["id"]] for dep in target.get("dependencies", [])
                       if dep["id"] in all_targets)
    paths = set()
    generated_roots = {(build_dir / target["paths"]["build"]).resolve()
                       for target in targets.values() if target.get("paths", {}).get("build") not in {None, "."}}
    generated_roots.update((build_dir / directory["build"]).resolve()
                           for directory in configs[0].get("directories", []) if directory["build"] != ".")
    for target in targets.values():
        for source in target.get("sources", []):
            if not source.get("isGenerated"):
                paths.add((REPO_ROOT / source["path"]).resolve())
        for group in target.get("compileGroups", []):
            paths.update(Path(include["path"]).resolve() for include in group.get("includes", []))
    paths = {path for path in paths if not any(path.is_relative_to(root) for root in generated_roots)}
    artifacts = []
    for target in targets.values():
        for artifact in target.get("artifacts", []):
            path = (build_dir / artifact["path"]).resolve()
            if path.is_file():
                artifacts.append({"target": target["name"], "type": target["type"],
                                  "name": path.name, "sha256": sha256(path),
                                  "path": path.relative_to(build_dir).as_posix()})
    for name in roots:
        if not any(item["target"] == name for item in artifacts):
            raise ValueError(f"product target has not been built: {name}")
    cache = {entry["name"]: entry["value"] for entry in objects["cache"]["entries"]}
    option_names = {"CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_MSVC_RUNTIME_LIBRARY",
                    "ANDROID_ABI", "ANDROID_PLATFORM", "ANDROID_STL", "HARKEN_DISABLE_LMDB",
                    "BUILD_SHARED_LIBS", "BUILD_STATIC", "MZ_LIBCOMP", "MZ_ZLIB_FLAVOR"}
    return {"identity": identity, "sourceState": source_state, "paths": paths, "artifacts": artifacts,
            "targets": sorted(target["name"] for target in targets.values()), "cache": cache,
            "linkInputs": {target["name"]: target["link"]["commandFragments"]
                           for target in targets.values() if target.get("link", {}).get("commandFragments")},
            "buildOptions": {key: value for key, value in cache.items()
                             if key in option_names or key.startswith("HARKEN_BUILD_")},
            "toolchains": [{"language": entry["language"],
                            "compiler": {key: entry.get("compiler", {}).get(key)
                                         for key in ("id", "version", "target")}}
                           for entry in objects.get("toolchains", {}).get("toolchains", [])],
            "cmakeVersion": index["cmake"]["version"]["string"]}


def notice_components() -> list[dict]:
    lines = (REPO_ROOT / "NOTICE").read_text(encoding="utf-8").splitlines()
    rows = {}
    for cells in csv.reader((line for line in lines if line.startswith("|")), delimiter="|",
                            quoting=csv.QUOTE_NONE, escapechar="\\"):
        if len(cells) != 7:
            raise ValueError("NOTICE dependency tables must have five columns; escape literal pipes")
        name, version, license_name, _, _ = (cell.strip() for cell in cells[1:-1])
        if name == "Name" or not name.strip("-: "):
            continue
        if not all((name, version, license_name)):
            raise ValueError("NOTICE dependency name, version and license must not be empty")
        values = (version, license_name)
        if rows.setdefault(name, values) != values:
            raise ValueError(f"conflicting dependency entries in NOTICE: {name}")
    entries = []
    for entry in COMPONENT_INPUTS:
        if entry["notice"] not in rows:
            raise ValueError(f"dependency is absent from NOTICE: {entry['id']}")
        version, license_name = rows[entry["notice"]]
        commit = re.search(r"\b[0-9a-f]{40}\b", version)
        if ("sourceDirectory" in entry or "submodule" in entry) and not commit:
            raise ValueError(f"NOTICE must pin a full source commit: {entry['id']}")
        entries.append({**entry, "name": entry["notice"], "version": commit[0] if commit else version,
                        "licenseName": license_name})
    return entries


def resolve_components(model: dict, product: str) -> tuple[list[dict], dict[str, Path]]:
    registry = notice_components()
    selected = {}
    external_sources = {}
    covered = set()
    for entry in registry:
        hits = set()
        if "path" in entry:
            root = REPO_ROOT / entry["path"]
            hits = {path for path in model["paths"] if path.is_relative_to(root)}
        elif "sourceDirectory" in entry:
            for path in model["paths"]:
                for root in (path, *path.parents):
                    expected = entry["sourceDirectory"].lower()
                    if root.name.lower() in {expected, expected + "-src"} and (root / ".git").exists():
                        hits.add(path)
                        previous = external_sources.setdefault(entry["id"], root)
                        if previous != root:
                            raise ValueError(f"multiple sources selected for {entry['id']}")
                        break
        if hits or product in entry.get("products", []):
            covered.update(hits)
            selected[entry["id"]] = entry
    for entry in registry:
        if entry.get("parent") in selected:
            selected[entry["id"]] = entry
            if "submodule" in entry:
                root = external_sources[entry["sourceParent"]] / entry["submodule"]
                if git(root, "rev-parse", "HEAD") != entry["version"] or not (root / "LICENSE").is_file():
                    raise ValueError(f"submodule source or license differs from the review: {entry['id']}")
    for path in model["paths"] - covered:
        parts = [part.lower() for part in path.parts]
        if "thirdparty" in parts or "_deps" in parts:
            if path.name.lower() != "thirdparty":
                raise ValueError(f"unregistered build dependency: {path}")
    if "minizip-ng" in selected and "zlib-ng" not in selected:
        if any(path.name == "mz_strm_libcomp.c" for path in model["paths"]):
            selected["apple-libcompression"] = next(entry for entry in registry if entry["id"] == "apple-libcompression")
        else:
            raise ValueError("minizip-ng uses an unreviewed compression supplier; use the pinned zlib-ng source")
    for name, root in external_sources.items():
        selected[name]["sourceDirty"] = checkout_dirty(root, selected[name]["version"])
    legal_files = {name: source for source, name in collect_legal_texts.THIRD_PARTY_FILES.get(product, ())}
    for entry in selected.values():
        if "evidence" not in entry:
            entry["evidence"] = [legal_files[name].relative_to(REPO_ROOT).as_posix()
                                 for name in entry.get("legalFiles", [])]
        for name in entry["evidence"]:
            if not (REPO_ROOT / name).is_file() or not (REPO_ROOT / name).stat().st_size:
                raise ValueError(f"dependency license evidence is missing: {name}")
    return list(selected.values()), external_sources


def component(entry: dict) -> dict:
    license_value = {"id": entry["license"]} if "license" in entry and " AND " not in entry["license"] else None
    licenses = ([{"license": license_value}] if license_value else
                [{"expression": entry["license"]}] if "license" in entry else
                [{"license": {"name": entry["licenseName"]}}])
    result = {"type": "library", "bom-ref": entry["id"], "name": entry["name"],
              "version": entry["version"], "licenses": licenses,
              "externalReferences": [{"type": "vcs", "url": entry["url"]}],
              "properties": [{"name": "release:scope", "value": entry.get("scope", "build-input")} ]}
    if "sourceDirty" in entry:
        result["properties"].append({"name": "release:source-dirty", "value": json.dumps(entry["sourceDirty"])})
    if entry.get("evidence"):
        result["properties"].append({"name": "release:license-evidence", "value": json.dumps(
            [{"path": name, "sha256": sha256(REPO_ROOT / name)} for name in entry["evidence"]])})
    return result


def qt_components(model: dict, payload: Path, product: str) -> tuple[list[dict], Path | None]:
    qt_dir = model["cache"].get("Qt6_DIR")
    if not qt_dir:
        return [], None
    qt_root = Path(qt_dir).parents[2]
    qmake = qt_root / "bin" / ("qmake.exe" if os.name == "nt" else "qmake")
    version = subprocess.check_output([str(qmake), "-query", "QT_VERSION"], text=True).strip()
    reviewed = read_json(collect_legal_texts.LEGAL_SOURCE_DIRS[product] / "Qt6-SOURCES.json")
    if version != reviewed["qt_version"]:
        raise ValueError(f"Qt {version} has no reviewed component inventory")
    names = {path.name for path in payload.rglob("*") if path.is_file()}
    modules = {name.removeprefix("Qt6").removesuffix(".dll").lower()
               for name in names if name.startswith("Qt6") and name.endswith(".dll")}
    modules.update(name[2:-10].lower() for name in names if name.startswith("Qt") and name.endswith(".framework"))
    modules.update(path.name[2:-10].lower() for path in payload.rglob("Qt*.framework"))
    if not modules:
        raise ValueError("desktop package has no deployed Qt modules")
    supported = {entry["module"].removeprefix("qt") for entry in reviewed["components"]}
    supported.update({"widgets", "concurrent", "opengl", "openglwidgets"})
    if modules - supported:
        raise ValueError(f"unreviewed Qt modules: {sorted(modules - supported)}")
    qt = {"id": "qt", "name": "Qt", "version": version, "license": "LGPL-3.0-only",
          "url": "https://code.qt.io/qt/", "scope": "dynamically-linked"}
    entries = [component(qt)]
    for entry in reviewed["components"]:
        if entry["module"].removeprefix("qt") not in modules:
            continue
        item = {"id": "qt:" + entry["id"], "name": entry["name"], "version": entry["version"],
                "licenseName": entry["license"], "url": entry["attribution"], "scope": "qt-supplier-inventory"}
        entries.append(component(item))
    return entries, qt_root


def native_file(path: Path) -> bool:
    with path.open("rb") as stream:
        magic = stream.read(4)
    return (path.suffix.lower() in {".dll", ".exe", ".so", ".dylib", ".a", ".lib"}
            or magic[:2] == b"MZ" or magic in {b"\x7fELF", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
                                                b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe"})


def macos_deployment_path(build_dir: Path, product: str, configuration: str) -> Path:
    return build_dir / "generated" / f"macos-deployment-{product}-{configuration}.json"


def read_macos_deployment(build_dir: Path, product: str, configuration: str, model: dict) -> dict:
    path = macos_deployment_path(build_dir, product, configuration)
    if not path.is_file():
        return {}
    report = read_json(path)
    if (report.get("schemaVersion") != 1 or report.get("identity") != model["identity"]
            or report.get("product") != product or report.get("configuration") != configuration
            or product != "studio" or not model["identity"]["platform"].startswith("macos-")):
        raise ValueError("macOS deployment receipt does not match the configured build")
    seen = set()
    for record in report["libraries"]:
        source = record["sourceArtifact"]
        if (source not in model["artifacts"] or source["type"] != "SHARED_LIBRARY"
                or not source["name"].endswith(".dylib")
                or record["path"] != f"HarkenStudio.app/Contents/Frameworks/{source['name']}"
                or record["path"] in seen):
            raise ValueError("macOS deployment source does not match a CMake target artifact")
        seen.add(record["path"])
    return report


def verify_payload_binaries(payload: Path, model: dict, qt_root: Path | None,
                            sdk_hashes: set[str], payload_records: list[dict] | None = None,
                            deployment: dict | None = None) -> list[dict]:
    artifact_hashes = {entry["sha256"] for entry in model["artifacts"]}
    deployed = {entry["path"]: entry for entry in (deployment or {}).get("libraries", [])}
    qt_files = {}
    if qt_root:
        for directory in ("bin", "lib", "plugins"):
            for path in (qt_root / directory).rglob("*"):
                if not path.is_file() or path.is_symlink():
                    continue
                is_module = path.name.startswith("Qt6") and path.suffix == ".dll"
                is_framework = any(part.startswith("Qt") and part.endswith(".framework") for part in path.parts)
                is_plugin = (directory == "plugins" and path.suffix in {".dll", ".dylib"}
                             and path.relative_to(qt_root / directory).parts[0] in {
                                 "generic", "iconengines", "imageformats", "networkinformation",
                                 "platforms", "sqldrivers", "styles", "tls"})
                if is_module or is_framework and native_file(path) or is_plugin:
                    qt_files.setdefault(path.name, []).append(path)
    records = []
    hashes = {item["path"]: item.get("sha256") for item in (payload_records or [])}
    for path in payload.rglob("*"):
        if path.is_symlink() or not path.is_file() or not native_file(path):
            continue
        digest = hashes.get(path.relative_to(payload).as_posix()) or sha256(path)
        record = {"path": path.relative_to(payload).as_posix(), "sha256": digest}
        if digest in artifact_hashes:
            records.append({**record, "origin": "CMake target artifact"})
            continue
        if digest in sdk_hashes:
            records.append({**record, "origin": "SDK Runtime build receipt"})
            continue
        if record["path"] in deployed and deployed[record["path"]]["sha256"] == digest:
            records.append({**record, "origin": "CMake target via macdeployqt",
                            "sourceArtifact": deployed[record["path"]]["sourceArtifact"]})
            continue
        if path.name in qt_files:
            candidates = [{"path": candidate.relative_to(qt_root).as_posix(), "sha256": sha256(candidate)}
                          for candidate in qt_files[path.name]]
            exact = any(candidate["sha256"] == digest for candidate in candidates)
            if not exact and "macos" not in model["identity"]["platform"]:
                raise ValueError(f"deployed Qt binary differs from the configured kit: {path.name}")
            records.append({**record, "origin": "Qt kit", "kitCandidates": candidates,
                            "deploymentModified": not exact})
            continue
        raise ValueError(f"payload binary has no matching build or supplier: {path.relative_to(payload)}")
    return records


def read_runtime_receipts(root: Path, commit: str, channel: str = "stable") -> list[dict]:
    validate_channel(channel)
    receipts = []
    for path in sorted(root.rglob("runtime-build.json")):
        receipt = read_json(path)
        if receipt["identity"]["gitCommit"] != commit or receipt["identity"]["channel"] != channel:
            raise ValueError("SDK Runtime source identity differs from the package source")
        sdk = next((parent.parent for parent in path.parents
                    if parent.name == "BuildEvidence" and parent.parent.name == "HarkenSDK"), None)
        directories = [
            sdk / "Unity/Harken/Legal",
            sdk / "Unreal/Harken/Legal",
        ] if sdk else [path.parent]
        for notice in receipt.get("notices", []):
            for directory in directories:
                source = directory / notice["path"]
                if (not source.resolve().is_relative_to(directory.resolve()) or not source.is_file()
                        or sha256(source) != notice["sha256"]):
                    prefix = "SDK plugin Runtime" if sdk else "Runtime"
                    raise ValueError(f"{prefix} license evidence is missing or changed")
        receipts.append(receipt)
    if not receipts:
        raise ValueError("SDK Runtime build receipts are missing")
    return receipts


def qt_metadata(model: dict, qt_root: Path) -> list[Path]:
    directories = {Path(value) for name, value in model["cache"].items()
                   if name.startswith("Qt6") and name.endswith("_DIR")
                   and Path(value).is_relative_to(qt_root)}
    files = {path for directory in directories for path in directory.rglob("*.cmake") if path.is_file()}
    files.update(path for name in ("qconfig.pri", "qmodule.pri")
                 if (path := qt_root / "mkspecs" / name).is_file())
    return sorted(files)


def prepare(product: str, payload: Path, build_dir: Path, output: Path,
            version: str, build_id: str, configuration: str = "Release",
            binary_identity: dict | None = None, model: dict | None = None,
            setup_stub: Path | None = None, payload_records: list[dict] | None = None,
            payload_manifest: dict | None = None) -> Path:
    model = model if model is not None else build_model(
        build_dir, product, configuration, ("harken_hub_setup",) if setup_stub else ())
    identity = model["identity"]
    source_state = model["sourceState"] if "sourceState" in model else check_source(identity)
    expected_version = identity["hubVersion"] if product == "hub" else identity["version"]
    if version != expected_version:
        raise ValueError("package version differs from its configured source")
    if binary_identity and any(binary_identity[key] != identity[key]
                               for key in ("gitCommit", "gitDirty", "buildId", "channel")):
        raise ValueError("binary identity differs from the source build")
    if setup_stub and not any(item["target"] == "harken_hub_setup" and item["sha256"] == sha256(setup_stub)
                              for item in model["artifacts"]):
        raise ValueError("setup stub differs from the source build")
    entries, external = resolve_components(model, product)
    if external:
        source_state = {**source_state, "dependencyDirty": any(entry.get("sourceDirty", False) for entry in entries)}
    components = [component(entry) for entry in entries]
    qt, qt_root = qt_components(model, payload, product)
    components += qt
    sdk_receipts = []
    if product in {"studio", "sdk"}:
        sdk_receipts = read_runtime_receipts(payload, identity["gitCommit"], identity["channel"])
        source_state = {**source_state, "runtimeDirty": any(receipt["identity"]["gitDirty"]
                        or receipt["sourceState"]["checkoutDirty"] for receipt in sdk_receipts)}
        known = {item["bom-ref"] for item in components}
        for receipt in sdk_receipts:
            for item in receipt["components"]:
                if item["bom-ref"] not in known:
                    components.append(item)
                    known.add(item["bom-ref"])
    sdk_hashes = {artifact["sha256"] for receipt in sdk_receipts for artifact in receipt["artifacts"]}
    evidence_dir = output / f"{product}-{version}-{build_id}-evidence"
    if evidence_dir.exists():
        raise ValueError(f"release evidence already exists: {evidence_dir}")
    state = payload_state(payload, evidence_dir)
    payload_records = payload_records if payload_records is not None else inventory(payload, evidence_dir)
    deployment = read_macos_deployment(build_dir, product, configuration, model)
    binary_origins = verify_payload_binaries(payload, model, qt_root, sdk_hashes, payload_records, deployment)
    qt_sources = read_json(collect_legal_texts.LEGAL_SOURCE_DIRS[product] / "Qt6-SOURCES.json")["source_archives"] if qt else []
    evidence_dir.mkdir(parents=True)
    try:
        if qt_root:
            metadata = evidence_dir / "qt-build-metadata.tar.gz"
            with tarfile.open(metadata, "w:gz", compresslevel=1) as archive:
                for path in qt_metadata(model, qt_root):
                    archive.add(path, arcname=path.relative_to(qt_root).as_posix())
        root_ref = f"{product}:{identity['gitCommit']}:{identity['platform']}"
        bom = {"bomFormat": "CycloneDX", "specVersion": "1.6", "serialNumber": "urn:uuid:" + str(uuid.uuid4()),
               "version": 1, "metadata": {"timestamp": datetime.now(timezone.utc).isoformat(),
               "component": {"type": "application", "bom-ref": root_ref, "name": product, "version": version}},
               "components": sorted(components, key=lambda entry: entry["bom-ref"]),
               "dependencies": [{"ref": root_ref, "dependsOn": [entry["bom-ref"] for entry in components]}]}
        write_json(evidence_dir / "sbom.cdx.json", bom)
        receipt = {"schemaVersion": 1, "product": product, "version": version, "packageBuildId": build_id,
                   "identity": identity, "sourceState": source_state,
                   "configuration": configuration, "cmakeVersion": model["cmakeVersion"],
                   "toolchains": model["toolchains"], "targets": model["targets"],
                   "buildOptions": model.get("buildOptions", {}),
                   "linkInputs": model.get("linkInputs", {}), "binarySelfTest": binary_identity,
                   "buildArtifacts": model["artifacts"],
                   "binaryOrigins": binary_origins, "qtSourceReferences": qt_sources, "_payloadState": state,
                   "checks": {"cleanMatchingSource": "dirty" if any(source_state.values()) else "passed",
                              "registeredBuildDependencies": "passed",
                              "payloadBinaryOrigins": "passed", "legalTexts": "passed"},
                   "releaseApproval": "pending", "artifacts": []}
        receipt["payloadManifest" if payload_manifest else "payload"] = payload_manifest or payload_records
        if deployment:
            receipt["macosDeployment"] = deployment
        write_json(evidence_dir / "receipt.json", receipt)
    except BaseException:
        shutil.rmtree(evidence_dir)
        raise
    return evidence_dir


def retain_packaging_inputs(evidence_dir: Path, inputs: dict[str, Path]) -> None:
    receipt = read_json(evidence_dir / "receipt.json")
    tracked = set(git(REPO_ROOT, "ls-files").splitlines())
    records = {}
    for name, source in inputs.items():
        relative = source.relative_to(REPO_ROOT).as_posix() if source.is_relative_to(REPO_ROOT) else None
        if relative in tracked:
            digest = sha256(source)
            if digest == project_source_sha256(relative):
                records[name] = {"projectSource": relative, "sha256": digest}
                continue
        destination = evidence_dir / "packaging-inputs" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source != destination:
            shutil.copyfile(source, destination)
        records[name] = file_record(destination, evidence_dir)
    receipt["packagingInputs"] = records
    write_json(evidence_dir / "receipt.json", receipt)


def finalize(evidence_dir: Path, artifacts: list[Path], payload: Path,
             intermediate_artifacts: tuple[Path, ...] = ()) -> list[dict]:
    receipt = read_json(evidence_dir / "receipt.json")
    if payload_state(payload, evidence_dir) != receipt.pop("_payloadState"):
        raise ValueError("payload changed while packaging")
    for artifact in artifacts:
        if not artifact.is_file() or not artifact.stat().st_size:
            raise ValueError(f"package artifact is missing or empty: {artifact.name}")
    records = [file_record(path, path.parent) for path in artifacts]
    receipt["artifacts"] = [record for path, record in zip(artifacts, records) if path not in intermediate_artifacts]
    if intermediate_artifacts:
        receipt["intermediateArtifacts"] = [record for path, record in zip(artifacts, records)
                                            if path in intermediate_artifacts]
    receipt["checks"]["payloadMetadataUnchangedDuringPackaging"] = "passed"
    receipt["checks"]["packageArtifactsPresent" if artifacts else "payloadReadyForArtifactUpload"] = "passed"
    receipt["evidenceFiles"] = [item for item in inventory(evidence_dir) if item["path"] != "receipt.json"]
    write_json(evidence_dir / "receipt.json", receipt)
    return records


def record_runtime(build_dir: Path, output: Path, configuration: str) -> None:
    model = build_model(build_dir, "runtime", configuration)
    entries, external = resolve_components(model, "runtime")
    if external:
        raise ValueError("Runtime has fetched dependencies requiring source-delivery review")
    components = [component(entry) for entry in entries]
    notices = []
    if "android" in model["identity"]["platform"]:
        ndk_path = model["cache"].get("CMAKE_ANDROID_NDK") or model["cache"].get("ANDROID_NDK")
        if not ndk_path:
            raise ValueError("Android build does not record its NDK path")
        ndk = Path(ndk_path)
        properties = configparser.ConfigParser()
        properties.read_string("[ndk]\n" + (ndk / "source.properties").read_text(encoding="utf-8"))
        revision = properties["ndk"]["Pkg.Revision"]
        output.parent.mkdir(parents=True, exist_ok=True)
        for name in ("NOTICE", "NOTICE.toolchain"):
            source = ndk / name
            if not source.is_file():
                raise ValueError(f"Android NDK license material is missing: {name}")
            target = output.parent / f"Android-NDK-{name}.txt"
            shutil.copyfile(source, target)
            notices.append(file_record(target, output.parent))
        components.append(component({"id": "android-ndk-runtime", "name": "Android NDK static C++ runtime",
            "version": revision, "licenseName": "See Android NDK NOTICE and NOTICE.toolchain",
            "url": "https://android.googlesource.com/toolchain/llvm-project/", "scope": "statically-linked"}))
    source_state = model["sourceState"] if "sourceState" in model else check_source(model["identity"])
    write_json(output, {"schemaVersion": 1, "identity": model["identity"], "sourceState": source_state,
                        "configuration": configuration,
                        "cmakeVersion": model["cmakeVersion"], "targets": model["targets"],
                        "buildOptions": model.get("buildOptions", {}),
                        "linkInputs": model.get("linkInputs", {}),
                        "toolchains": model["toolchains"], "artifacts": model["artifacts"],
                        "components": components, "notices": notices})


def prepare_sdk(payload: Path, receipts_root: Path, output: Path, channel: str = "dev") -> Path:
    commit = git(REPO_ROOT, "rev-parse", "HEAD")
    receipts = read_runtime_receipts(receipts_root, commit, channel)
    sdk = payload / "HarkenSDK"
    collect_legal_texts.verify_sdk(sdk)
    version = (sdk / "VERSION").read_text(encoding="utf-8").strip()
    if version != (REPO_ROOT / "HarkenSDK/VERSION").read_text(encoding="utf-8").strip():
        raise ValueError("SDK version differs from the source checkout")
    for path in receipts_root.rglob("runtime-build.json"):
        destination = sdk / "BuildEvidence" / path.relative_to(receipts_root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        for notice in read_json(path).get("notices", []):
            source = path.parent / notice["path"]
            for plugin_root in (sdk / "Unity/Harken", sdk / "Unreal/Harken"):
                target = plugin_root / "Legal" / notice["path"]
                if target.exists():
                    if sha256(target) != notice["sha256"]:
                        raise ValueError("SDK Runtime builds used incompatible toolchain notices")
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
    identity = {**receipts[0]["identity"], "platform": "multi-platform", "version": version,
                "channel": channel, "gitDirty": any(receipt["identity"]["gitDirty"]
                    or receipt["sourceState"]["checkoutDirty"] for receipt in receipts)}
    component_ids = {item["bom-ref"] for receipt in receipts for item in receipt["components"]}
    paths = {REPO_ROOT / entry["path"] for entry in notice_components()
             if entry["id"] in component_ids and "path" in entry}
    model = {"identity": identity, "paths": paths,
             "artifacts": [artifact for receipt in receipts for artifact in receipt["artifacts"]],
             "targets": sorted({target for receipt in receipts for target in receipt["targets"]}),
             "cache": {}, "toolchains": [item for receipt in receipts for item in receipt["toolchains"]],
             "cmakeVersion": sorted({receipt["cmakeVersion"] for receipt in receipts})}
    checksums = payload / "SHA256SUMS.txt"
    records = inventory(payload, checksums)
    checksum_roots = ("HarkenSDK/Unity/Harken/Runtime/Plugins/", "HarkenSDK/Unreal/Harken/")
    checksums.write_text("".join(f"{record['sha256']}  {record['path']}\n" for record in records
                        if "sha256" in record and record["path"].startswith(checksum_roots)
                        and Path(record["path"]).name != "README.md"), encoding="utf-8", newline="\n")
    records.append(file_record(checksums, payload))
    records.sort(key=lambda record: record["path"])
    return prepare("sdk", payload, Path(), output, version, commit[:12], model=model, payload_records=records)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--channel", choices=("stable", "beta", "dev", "nightly"),
                        default=os.environ.get("HARKEN_BUILD_CHANNEL", "dev"))
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--runtime-receipt")
    target.add_argument("--sdk-payload")
    parser.add_argument("--runtime-receipts")
    parser.add_argument("--output-dir")
    args = parser.parse_args(argv)
    if args.runtime_receipt:
        record_runtime(Path(args.build_dir).resolve(), Path(args.runtime_receipt).resolve(), args.configuration)
    else:
        if not args.runtime_receipts or not args.output_dir:
            parser.error("--sdk-payload requires --runtime-receipts and --output-dir")
        payload = Path(args.sdk_payload).resolve()
        with package_output(Path(args.output_dir).resolve()) as staging:
            evidence = prepare_sdk(payload, Path(args.runtime_receipts).resolve(), staging, args.channel)
            finalize(evidence, [], payload)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: release evidence: {error}") from error
