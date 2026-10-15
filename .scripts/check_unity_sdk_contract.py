#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
DEFAULT_SDK = REPOSITORY / "HarkenSDK" / "Unity" / "Harken"
FORBIDDEN_UNITY_ROOT_SUFFIXES = {".cpp", ".h", ".uplugin"}
NATIVE_SUFFIXES = {".a", ".dll", ".dylib", ".lib", ".so"}
NATIVE_PLUGIN_PATHS = {
    "Runtime/Plugins/x86_64/HarkenCore.dll",
    "Runtime/Plugins/x86_64/libHarkenCore.so",
    "Runtime/Plugins/macOS/libHarkenCore.dylib",
    "Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so",
    "Runtime/Plugins/iOS/libHarkenCore.a",
}
NATIVE_SOURCE_CONTRACT = {
    "target": "harken_core_shared",
    "workflow": ".github/workflows/core-artifact.yml",
    "windows_x86_64": "Runtime/Plugins/x86_64/HarkenCore.dll",
    "linux_x86_64": "Runtime/Plugins/x86_64/libHarkenCore.so",
    "macos_universal": "Runtime/Plugins/macOS/libHarkenCore.dylib",
    "android_arm64": "Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so",
    "ios_arm64": "Runtime/Plugins/iOS/libHarkenCore.a",
}
VOICE_WRAPPER_CONTRACT = {
    "SetVolume": (
        "public VnResult SetVolume(float volumeDb, uint rampMs)",
        "HarkenNative.Vn_SetVoiceVolume(Handle, volumeDb, rampMs)",
    ),
    "SetFilter": (
        "public VnResult SetFilter(uint filterKind, float cutoffHz, float q, "
        "uint enable01, uint rampMs)",
        "HarkenNative.Vn_SetVoiceFilter(Handle, filterKind, cutoffHz, q, "
        "enable01, rampMs)",
    ),
    "SetPitch": (
        "public VnResult SetPitch(float ratio)",
        "HarkenNative.Vn_SetVoicePitch(Handle, ratio)",
    ),
    "SetPosition": (
        "public VnResult SetPosition(VnVec3 position)",
        "HarkenNative.Vn_SetVoicePosition(Handle, position)",
    ),
    "SetOrientation": (
        "public VnResult SetOrientation(VnVec3 forward)",
        "HarkenNative.Vn_SetVoiceOrientation(Handle, forward)",
    ),
    "SetPan": (
        "public VnResult SetPan(float pan)",
        "HarkenNative.Vn_SetVoicePan(Handle, pan)",
    ),
    "SetWidth": (
        "public VnResult SetWidth(float width)",
        "HarkenNative.Vn_SetVoiceWidth(Handle, width)",
    ),
    "SetSpatializationMix": (
        "public VnResult SetSpatializationMix(float mix)",
        "HarkenNative.Vn_SetVoiceSpatializationMix(Handle, mix)",
    ),
    "SetDistanceScale": (
        "public VnResult SetDistanceScale(float scale)",
        "HarkenNative.Vn_SetVoiceDistanceScale(Handle, scale)",
    ),
    "SetOutputVolume": (
        "public VnResult SetOutputVolume(float volumeDb, uint rampMs)",
        "HarkenNative.Vn_SetVoiceOutputVolume(Handle, volumeDb, rampMs)",
    ),
    "SetSend": (
        "public VnResult SetSend(uint sendSlot, VnObjectId toBus, float levelDb)",
        "HarkenNative.Vn_SetVoiceSend(Handle, sendSlot, toBus, levelDb)",
    ),
    "SetSendLevel": (
        "public VnResult SetSendLevel(uint sendSlot, float levelDb, uint rampMs)",
        "HarkenNative.Vn_SetVoiceSendLevel(Handle, sendSlot, levelDb, rampMs)",
    ),
    "ClearSend": (
        "public VnResult ClearSend(uint sendSlot)",
        "HarkenNative.Vn_ClearVoiceSend(Handle, sendSlot)",
    ),
    "Stop": (
        "public VnResult Stop(uint fadeMs)",
        "HarkenNative.Vn_Stop(Handle, fadeMs)",
    ),
}


def csharp_code_only(source: str) -> str:
    """Mask comments and literals so contract checks inspect active C# code."""
    output: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]

        if state == "code":
            if pair == "//":
                output.extend("  ")
                index += 2
                state = "line_comment"
                continue
            if pair == "/*":
                output.extend("  ")
                index += 2
                state = "block_comment"
                continue
            if source.startswith(("$@\"", "@$\""), index):
                raise ValueError("must not contain interpolated strings")
            if source.startswith("@\"", index):
                output.extend("  ")
                index += 2
                state = "verbatim_string"
                continue
            if source.startswith("$\"", index):
                raise ValueError("must not contain interpolated strings")
            if char == "\"":
                output.append(" ")
                index += 1
                state = "string"
                continue
            if char == "'":
                output.append(" ")
                index += 1
                state = "char"
                continue
            output.append(char)
            index += 1
            continue

        if state == "line_comment":
            output.append(char if char in "\r\n" else " ")
            index += 1
            if char in "\r\n":
                state = "code"
            continue

        if state == "block_comment":
            if pair == "*/":
                output.extend("  ")
                index += 2
                state = "code"
            else:
                output.append(char if char in "\r\n" else " ")
                index += 1
            continue

        if state in {"string", "char"}:
            output.append(char if char in "\r\n" else " ")
            index += 1
            if char == "\\" and index < len(source):
                escaped = source[index]
                output.append(escaped if escaped in "\r\n" else " ")
                index += 1
            elif (state == "string" and char == "\"") or (
                state == "char" and char == "'"
            ):
                state = "code"
            continue

        output.append(char if char in "\r\n" else " ")
        index += 1
        if char == "\"":
            if index < len(source) and source[index] == "\"":
                output.append(" ")
                index += 1
            else:
                state = "code"

    return "".join(output)


def verify_layout(
    sdk: Path, require_binaries: bool, errors: list[str]
) -> None:
    for forbidden in ("Unreal",):
        if (sdk / forbidden).exists():
            errors.append(f"HarkenSDK must not contain {forbidden}")

    native_files = set()
    for path in sdk.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(sdk).as_posix()
        suffix = path.suffix.lower()
        if suffix in FORBIDDEN_UNITY_ROOT_SUFFIXES:
            errors.append(f"Non-Unity source remains in Harken SDK: {relative}")
        if suffix == ".meta":
            normalized = relative.replace("\\", "/")
            forbidden_meta = {
                "README.md.meta",
                "CHANGELOG.md.meta",
                "LICENSE.md.meta",
                "VERSION.meta",
                "package.json.meta",
                "harken-integration.json.meta",
                "Samples~.meta",
            }
            allowed = (
                normalized not in forbidden_meta
                and (normalized.startswith("Runtime/Plugins/")
                     or normalized.startswith("Samples~/")
                     or normalized.startswith("Tests/")
                     or normalized.endswith((".unity.meta", ".prefab.meta"))
                     or normalized.endswith((".cs.meta", ".asmdef.meta")))
            )
            if not allowed:
                errors.append(f"Invalid Unity .meta file in SDK: {relative}")
        elif suffix in {".cs", ".asmdef"}:
            if not path.with_name(path.name + ".meta").is_file():
                errors.append(f"Unity source is missing metadata: {relative}.meta")
        elif suffix in NATIVE_SUFFIXES:
            native_files.add(relative)

    if require_binaries:
        missing = NATIVE_PLUGIN_PATHS - native_files
        extra = native_files - NATIVE_PLUGIN_PATHS
        if missing:
            errors.append(f"Missing native plugin binaries: {sorted(missing)}")
        if extra:
            errors.append(f"Unexpected native plugin binaries: {sorted(extra)}")
        for relative in sorted(NATIVE_PLUGIN_PATHS & native_files):
            if (sdk / relative).stat().st_size == 0:
                errors.append(f"Native plugin binary is empty: {relative}")
    elif native_files:
        errors.append(
            "Repository SDK must not track native binaries: "
            f"{sorted(native_files)}"
        )


def verify_manifests(sdk: Path, errors: list[str]) -> None:
    package = json.loads((sdk / "package.json").read_text(encoding="utf-8"))
    integration = json.loads(
        (sdk / "harken-integration.json").read_text(encoding="utf-8")
    )
    if package.get("name") != "com.harken.core":
        errors.append("package.json name must be com.harken.core")
    if package.get("version") != "0.3.3":
        errors.append("package.json version must be 0.3.3")
    if "samples" in package:
        errors.append("package.json must not declare repository-owned Samples")

    for relative in integration.get("required_files", []):
        path = sdk / relative
        if not path.is_file() or path.stat().st_size == 0:
            errors.append(f"Integration required file is missing: {relative}")

    alternatives = {
        alternative
        for plugin in integration.get("native_plugins", [])
        for alternative in plugin.get("alternatives", [])
    }
    if alternatives != NATIVE_PLUGIN_PATHS:
        errors.append(
            "harken-integration.json native plugins differ from the release "
            f"layout: missing={sorted(NATIVE_PLUGIN_PATHS - alternatives)}, "
            f"extra={sorted(alternatives - NATIVE_PLUGIN_PATHS)}"
        )
    source = integration.get("native_binary_source", {})
    if source != NATIVE_SOURCE_CONTRACT:
        errors.append("harken-integration.json native binary source is invalid")


def verify_sample_scripts(sdk: Path, errors: list[str]) -> None:
    definitions: dict[str, list[Path]] = {}
    for meta in sdk.rglob("*.cs.meta"):
        if not meta.with_suffix("").is_file():
            continue
        match = re.search(r"^guid: ([0-9a-f]{32})$", meta.read_text(encoding="utf-8"), re.MULTILINE)
        if match:
            definitions.setdefault(match.group(1), []).append(meta)

    # Unity serializes MonoScript references as one flow-mapping line in scene/prefab YAML.
    reference = re.compile(r"^\s*m_Script: \{[^\n}]*\bguid: ([0-9a-f]{32})\b[^\n}]*\}", re.MULTILINE)
    for asset in (sdk / "Samples~").rglob("*"):
        if asset.suffix not in {".unity", ".prefab"}:
            continue
        for guid in set(reference.findall(asset.read_text(encoding="utf-8"))):
            matches = definitions.get(guid, [])
            if len(matches) != 1:
                errors.append(
                    f"Sample script GUID must resolve to exactly one SDK source: "
                    f"{asset.relative_to(sdk).as_posix()}: {guid} has {len(matches)} definitions"
                )


def verify_voice_wrappers(sdk: Path, errors: list[str]) -> None:
    source = (sdk / "Runtime/Sdk/Voice.cs").read_text(encoding="utf-8")
    try:
        code = csharp_code_only(source)
    except ValueError as error:
        errors.append(f"Voice.cs {error}")
        return
    if re.search(r"(?m)^[ \t]*#", code):
        errors.append("Voice.cs must not contain preprocessor directives")
        return
    compact = re.sub(r"\s+", "", code)
    for name, (signature, native_call) in VOICE_WRAPPER_CONTRACT.items():
        expected = re.sub(r"\s+", "", signature + "=>" + native_call + ";")
        if compact.count(expected) != 1:
            errors.append(
                f"Voice.{name} must be one exact native forwarding call: "
                f"{signature} => {native_call};"
            )


def verify(sdk: Path, require_binaries: bool) -> None:
    errors: list[str] = []
    candidates = [sdk, sdk / "Unity" / "Harken", sdk / "Harken"]
    sdk = next(
        (candidate for candidate in candidates
         if (candidate / "package.json").is_file()),
        sdk,
    )
    if not sdk.is_dir():
        raise SystemExit(f"Unity SDK root does not exist: {sdk}")
    verify_layout(sdk, require_binaries, errors)
    verify_manifests(sdk, errors)
    verify_sample_scripts(sdk, errors)
    verify_voice_wrappers(sdk, errors)
    if errors:
        raise SystemExit("\n".join(errors))
    content = "source-only" if not require_binaries else "assembled"
    print(
        f"Verified {content} Unity SDK layout, manifests, and native "
        "binary provenance, including thin Voice forwarding."
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk-root", type=Path, default=DEFAULT_SDK)
    parser.add_argument("--require-binaries", action="store_true")
    arguments = parser.parse_args()
    verify(arguments.sdk_root.resolve(), arguments.require_binaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
