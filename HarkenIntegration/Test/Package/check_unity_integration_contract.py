#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_INTEGRATION = REPOSITORY / "HarkenIntegration" / "Unity" / "Harken"
PREVIEW_TOKENS = ("VnPreview", "VN_PREVIEW_")
FORBIDDEN_UNITY_ROOT_SUFFIXES = {".cpp", ".h", ".uplugin"}
NATIVE_SUFFIXES = {".a", ".dll", ".dylib", ".lib", ".so"}
NATIVE_PLUGIN_PATHS = {
    "Runtime/Plugins/x86_64/HarkenEngine.dll",
    "Runtime/Plugins/x86_64/HarkenEngineDev.dll",
    "Runtime/Plugins/x86_64/libHarkenEngine.so",
    "Runtime/Plugins/x86_64/libHarkenEngineDev.so",
    "Runtime/Plugins/macOS/libHarkenEngine.dylib",
    "Runtime/Plugins/macOS/libHarkenEngineDev.dylib",
    "Runtime/Plugins/Android/arm64-v8a/libHarkenEngine.so",
    "Runtime/Plugins/Android/arm64-v8a/libHarkenEngineDev.so",
    "Runtime/Plugins/iOS/libHarkenEngine.a",
    "Runtime/Plugins/iOS/libHarkenEngineDev.a",
}
REQUIRED_FILES = (
    "package.json",
    "README.md",
    "LICENSE.md",
    "NOTICE.md",
    "Runtime/Harken.Core.asmdef",
    "Runtime/Unity/Harken.Unity.asmdef",
    "Editor/Harken.Unity.Editor.asmdef",
    "Runtime/Generated/HarkenAbi.cs",
    "Runtime/Generated/HarkenNative.cs",
    "Runtime/Generated/HarkenTypes.cs",
)
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
    integration: Path, require_binaries: bool, errors: list[str]
) -> None:
    for forbidden in ("Unreal",):
        if (integration / forbidden).exists():
            errors.append(f"HarkenIntegration must not contain {forbidden}")

    native_files = set()
    for path in integration.rglob("*"):
        relative = path.relative_to(integration).as_posix()
        suffix = path.suffix.lower()
        asset = path.with_name(path.name[:-len(".meta")]) if suffix == ".meta" else path
        if any(part.endswith("~") or part.startswith(".")
               for part in asset.relative_to(integration).parts):
            if suffix == ".meta":
                errors.append(f"Unity ignores this asset; remove its .meta file: {relative}")
            continue
        if path.is_file() and suffix in FORBIDDEN_UNITY_ROOT_SUFFIXES:
            errors.append(f"Non-Unity source remains in Harken Integration Package: {relative}")
        if path.is_file() and suffix in NATIVE_SUFFIXES:
            native_files.add(relative)
        if suffix == ".meta":
            if not asset.exists():
                errors.append(f"Unity .meta file has no matching asset: {relative}")
        elif (relative != "Runtime/Plugins" and not relative.startswith("Runtime/Plugins/")
              and not path.with_name(path.name + ".meta").is_file()):
            errors.append(f"Unity asset is missing metadata: {relative}.meta")

    if require_binaries:
        missing = NATIVE_PLUGIN_PATHS - native_files
        extra = native_files - NATIVE_PLUGIN_PATHS
        if missing:
            errors.append(f"Missing native plugin binaries: {sorted(missing)}")
        if extra:
            errors.append(f"Unexpected native plugin binaries: {sorted(extra)}")
        for relative in sorted(NATIVE_PLUGIN_PATHS & native_files):
            if (integration / relative).stat().st_size == 0:
                errors.append(f"Native plugin binary is empty: {relative}")
    elif native_files:
        errors.append(
            "Repository Integration Package must not track native binaries: "
            f"{sorted(native_files)}"
        )


def verify_package(integration: Path, errors: list[str]) -> None:
    package = json.loads((integration / "package.json").read_text(encoding="utf-8"))
    if package.get("name") != "com.harken.core":
        errors.append("package.json name must be com.harken.core")
    expected_version = json.loads(
        (DEFAULT_INTEGRATION / "package.json").read_text(encoding="utf-8")
    ).get("version")
    if package.get("version") != expected_version:
        errors.append(f"package.json version must match the source checkout ({expected_version})")
    if "samples" in package:
        errors.append("package.json must not declare repository-owned Samples")

    for relative in REQUIRED_FILES:
        path = integration / relative
        if not path.is_file() or path.stat().st_size == 0:
            errors.append(f"Integration required file is missing: {relative}")


def verify_generated_binding(integration: Path, errors: list[str]) -> None:
    for path in sorted((integration / "Runtime/Generated").glob("*.cs")):
        text = path.read_text(encoding="utf-8")
        for token in PREVIEW_TOKENS:
            if token in text:
                errors.append(
                    f"Generated binding projects the development preview surface: "
                    f"{path.relative_to(integration).as_posix()} names {token}"
                )


def verify_voice_wrappers(integration: Path, errors: list[str]) -> None:
    source = (integration / "Runtime/API/Voice.cs").read_text(encoding="utf-8")
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


def verify(integration: Path, require_binaries: bool) -> None:
    errors: list[str] = []
    candidates = [integration, integration / "Unity" / "Harken", integration / "Harken"]
    integration = next(
        (candidate for candidate in candidates
         if (candidate / "package.json").is_file()),
        integration,
    )
    if not integration.is_dir():
        raise SystemExit(f"Unity Integration Package root does not exist: {integration}")
    verify_layout(integration, require_binaries, errors)
    verify_package(integration, errors)
    verify_generated_binding(integration, errors)
    verify_voice_wrappers(integration, errors)
    if errors:
        raise SystemExit("\n".join(errors))
    content = "source-only" if not require_binaries else "assembled"
    print(
        f"Verified {content} Unity Integration Package layout, package identity, "
        "required files, generated binding isolation, and thin Voice forwarding."
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--integration-root", "--sdk-root", dest="integration_root", type=Path, default=DEFAULT_INTEGRATION)
    parser.add_argument("--require-binaries", action="store_true")
    arguments = parser.parse_args()
    verify(arguments.integration_root.resolve(), arguments.require_binaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
