# Native plugin placement

The C# binding is a thin P/Invoke projection; it needs the native `HarkenCore`
library present for each target platform. Unity resolves native libraries from
the `Plugins/<platform>/` layout and creates the corresponding importer metadata
when the assembled package is imported.

The repository source package does not check generated binaries into Git. The
Harken distribution pipeline should assemble every runtime listed for the current
editor host in `harken-integration.json` before shipping Harken Studio. Integration
blocks only when the current editor runtime or required Integration Package content is missing. Other
missing Unity targets are reported with guidance while available platforms continue
to integrate; when bundled, every declared runtime is validated and installed.

The binding resolves the library by the name baked at compile time in
`HarkenNative.LibraryName` (`Generated/HarkenNative.g.cs`):

- Desktop / macOS / Android: `HarkenCore` (a shared library — `.dll` / `.dylib`
  / `.so`).
- iOS / visionOS / tvOS: `__Internal` — the runtime is **statically linked**
  into the app; iOS forbids dynamic loading. Ship the static library (`.a`) and
  let the linker fold it in; do **not** rename it to `HarkenCore`.

## Expected layout

Android runtime support is limited to ARM64 (`arm64-v8a`).

```
Runtime/Plugins/
├── x86_64/         HarkenCore.dll        (Windows editor + standalone; desktop dev/test)
│                   libHarkenCore.so      (Linux editor + standalone, if used)
├── macOS/          libHarkenCore.dylib   — Apple silicon + Intel
├── Android/
│   └── arm64-v8a/  libHarkenCore.so
└── iOS/            libHarkenCore.a        (static; linked as __Internal)
```

## Per-platform import settings

The Unity integration editor importer locates its installation using the GUID in
`Editor/HarkenPluginImporterSetup.cs.meta` and verifies the package identity.
It configures the libraries below `Runtime/Plugins` in UPM installations and
custom Assets locations, including `Assets/Audio/HarkenIntegration`. Keep the Integration Package directory
structure and its `.meta` files when importing or moving it. Importing or moving
Integration Package assets schedules a configuration check; correct settings are left unchanged.
Moving a native library outside the Integration Package stops this automatic configuration.
Restart Unity after moving a loaded native library.

The distribution directory can be copied into Assets, embedded under Packages,
or used as a local directory package. Before publishing an immutable UPM archive
or registry package, include complete Unity `.meta` files with the native platform
settings already configured. Unity ignores assets without metadata in immutable
packages; the editor importer cannot configure a library that Unity did not import.

Select each binary in the Unity Project window and set:

- **x86_64/HarkenCore.dll** — Platform: *Editor* + *Standalone*; CPU x86_64; OS
  Windows. (Enables desktop-editor play-mode and the standalone player.)
- **x86_64/libHarkenCore.so** — Platform: *Editor* + *Standalone*; CPU x86_64;
  OS Linux.
- **macOS/libHarkenCore.dylib** — Platform: *Editor* + *Standalone*; CPU
  *AnyCPU* for the required x86_64 + arm64 universal library.
- **Android/arm64-v8a/libHarkenCore.so** — Platform: *Android*; CPU *ARM64*.
- **iOS/libHarkenCore.a** — Platform: *iOS*; add to the generated Xcode project;
  the binding's `__Internal` import name links it statically. No CPU scoping —
  the static lib carries its slices.

Set exactly one enabled library per (platform, CPU) so Unity does not report a
"multiple plugins with the same name" conflict. The `x86_64` desktop library is
what the standalone dotnet ABI / zero-GC harness under `HarkenIntegration/Test/ContractTest` loads
as well (see that project's README).

## Building the native library

`HarkenCore` is built from the engine sources. The repository provides an
optional shared-library CMake target (`HARKEN_BUILD_SHARED`, default OFF) that
produces `HarkenCore.dll` / `libHarkenCore.{so,dylib}` from the same sources as
the static `harken_core`. The macOS/Android/iOS device backends and their
real-hardware validation are the device-backend segment's concern; this package
consumes whatever libraries that build produces.

`.github/workflows/core-artifact.yml` builds the native libraries. Desktop
builds use `windows-release`, `macos-release`, and `linux-release`; Android and
iOS provide their toolchain and architecture parameters directly. All build
options default OFF. Shared-library builds enable `HARKEN_BUILD_CORE` and
`HARKEN_BUILD_SHARED`; iOS enables only `HARKEN_BUILD_CORE` and builds the static
`harken_core` target.

`.github/workflows/integration-artifact.yml` assembles these libraries with the Unity
Integration Package sources and validates the package layout. Integration contract tests belong
to `.github/workflows/pr-gate-integration.yml`, including the Windows native binding
test. The artifact workflows do not run the Integration Package in Unity.
