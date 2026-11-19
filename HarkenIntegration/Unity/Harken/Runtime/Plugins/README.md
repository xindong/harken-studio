# Native plugin placement

The C# binding is a thin P/Invoke projection; it needs both native Harken Engine
editions present for each target platform: the release library `HarkenEngine`
and the development library `HarkenEngineDev`. Unity resolves native libraries from
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

- Desktop / macOS / Android: `HarkenEngineDev` when `HARKEN_ENGINE_DEV` is
  defined, otherwise `HarkenEngine` (shared libraries — `.dll` / `.dylib` / `.so`).
- iOS / visionOS / tvOS: `__Internal` — the runtime is **statically linked**
  into the app; iOS forbids dynamic loading. Ship the static libraries (`.a`)
  and let the linker fold the selected one in.

`HARKEN_ENGINE_DEV` is defined in the Editor and in Development Builds. The
development-only managed API (remote communication, snapshots, Monitor
observation) exists only when it is defined. **Project Settings > Harken >
Project > Native Library** overrides the choice: *Automatic* follows the build
type, *Release* and *Development* force one edition for the Editor and every
build by adding `HARKEN_NATIVE_RELEASE` or `HARKEN_NATIVE_DEVELOPMENT` to the
scripting define symbols. Restart Unity after changing it so the Editor loads
the selected library. A pre-build hook enables exactly one edition per
platform for each build.

## Expected layout

Android runtime support is limited to ARM64 (`arm64-v8a`).

```
Runtime/Plugins/
├── x86_64/         HarkenEngine.dll, HarkenEngineDev.dll          (Windows editor + standalone)
│                   libHarkenEngine.so, libHarkenEngineDev.so      (Linux editor + standalone)
├── macOS/          libHarkenEngine.dylib, libHarkenEngineDev.dylib — Apple silicon + Intel
├── Android/
│   └── arm64-v8a/  libHarkenEngine.so, libHarkenEngineDev.so
└── iOS/            libHarkenEngine.a, libHarkenEngineDev.a        (static; linked as __Internal)
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

The importer keeps the Editor on the development library (unless the override
selects *Release*) and enables exactly one edition per platform for players:

- **x86_64 Windows** — CPU x86_64; OS Windows.
- **x86_64 Linux** — CPU x86_64; OS Linux.
- **macOS** — CPU *AnyCPU* for the required x86_64 + arm64 universal library.
- **Android/arm64-v8a** — CPU *ARM64*.
- **iOS** — added to the generated Xcode project; the binding's `__Internal`
  import name links it statically. No CPU scoping — the static lib carries its slices.

The standalone dotnet ABI / zero-GC harness under `HarkenIntegration/Test/ContractTest`
defines `HARKEN_NATIVE_DEVELOPMENT` and loads `HarkenEngineDev` (see that project's README).

## Building the native library

Both editions are built from the Engine sources: `HARKEN_BUILD_ENGINE` produces
`HarkenEngine` (Core only) and `HARKEN_BUILD_ENGINE_DEV` produces `HarkenEngineDev`
(Core + Development). `cmake --install` publishes the libraries, public headers,
Pack format headers and the ABI manifest. `.github/workflows/core-artifact.yml`
builds and installs both editions for every platform; iOS builds both as static libraries.

`.github/workflows/integration-artifact.yml` assembles these libraries with the Unity
Integration Package sources and validates the package layout. Integration contract tests belong
to `.github/workflows/pr-gate-integration.yml`, including the Windows native binding
test. The artifact workflows do not run the Integration Package in Unity.
