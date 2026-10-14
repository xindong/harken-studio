# Binding gates (managed) — ABI fidelity + coordinates + zero-GC

Standalone .NET runner for the C# binding's machine gates (spec M5 §5.2 / §5.4,
acceptance §11.6). It compiles the **same** generated + hand-written SDK sources
the `HarkenSDK` UPM package ships (single source, no copy) and exercises them.

It has **no NuGet dependencies**, so it builds offline from the .NET SDK alone,
and its `LangVersion` is pinned to **9.0** (the Unity 2022.3 LTS C# level), so a
successful build also proves the generated + SDK sources are Unity-2022.3
language-compatible.

## Run modes

For local ABI work, native validation remains optional:

```
dotnet run --project HarkenSDK/Test/ContractTest
```

In this mode, an unavailable native library reports `Zero-GC hot : SKIP`; the
process still exits 0 when the ABI and coordinate gates pass.

For Batch-0 acceptance, require the renamed native library and a real exported
function call. From the repository root in PowerShell, run:

```powershell
$env:HARKEN_NATIVE_DIR = "build-nc/Bin/Debug"
dotnet run --project HarkenSDK/Test/ContractTest -- --require-native
```

This mode exits nonzero when native validation is skipped or fails. It also
loads the production-exported Unity fixtures and PlayableDemo publications through
the native Runtime. Hand-built waveform fixtures remain metadata reader contracts.
When `HARKEN_NATIVE_DIR` is explicitly set with `--require-native`, that directory is
authoritative: the runner does not probe repository build folders or fall back
to the operating system library search. This makes missing-library acceptance
checks deterministic.

To run the Pack bootstrap gate against the shared F3 fixture, pass its directory
with `--pack-root`:

```powershell
$env:HARKEN_NATIVE_DIR = (Resolve-Path "build-nc/bin/Debug").Path
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- `
  --require-native --pack-root ".temp/f3-catalog-resident-window"
```

The directory must contain `Init.pack` and `Content.pack`. The focused Runtime
test `PackFoundationQualification.CatalogMayExceedResidentWindow` creates this
fixture. The managed gate inspects Init before engine creation, applies its
capacity recommendations, starts the null-device Runtime, loads content Pack
`22`, posts one loaded Action, verifies known-unloaded result `-220`, and shuts
the Runtime down explicitly.

To validate two Android platform instances, first run the Editor fixture writer:

```powershell
ctest --test-dir build-nc -C RelWithDebInfo -R '^ProjectPlatformBuild.WritesTwoAndroidInstancesForNativeSdkReader$' --output-on-failure
$env:HARKEN_NATIVE_DIR = (Resolve-Path "build-nc/bin/RelWithDebInfo").Path
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- `
  --require-native --platform-root ".temp/p6-sdk-delivery"
```

This gate rejects automatic selection of the two Android candidates, resolves
each explicit instance ID, verifies its sidecars and file hashes, and loads its
Init and content Packs through the native null-device Runtime. `Platform native`
reports this gate separately from the `--pack-root` fixture. Both options can be
used in the same run.

For Unity PlayMode or Android Player tests, copy that complete delivery tree to
`Assets/StreamingAssets/Audio/PlatformContractPacks` in an isolated test project
with the current SDK and matching native Runtime installed. Run
`HarkenPlatformDeliveryTests.TwoAndroidInstancesLoadByIdAndName`. It reads the
packaged index and manifests, checks ambiguity, and starts each instance through
the actual `HarkenAudioSystem` component using a null audio device. Android Player
execution also exercises StreamingAssets extraction and its persistent cache;
running the test again exercises cache reuse. A missing fixture is a test failure.
Unity Editor execution does not substitute for Android Player or IL2CPP validation.

For the Variable write-scope contract, first build and run the native fixture test:

```powershell
ctest --test-dir build-nc -C RelWithDebInfo -R '^PackComposition.VariableSdkSetValuePreservesWriteScopeAndPublishesInitFixture$' --output-on-failure
$env:HARKEN_NATIVE_DIR = (Resolve-Path "build-nc/bin/RelWithDebInfo").Path
$env:HARKEN_VARIABLE_INIT = (Resolve-Path ".temp/variable-sdk-init.pack").Path
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- --require-native
```

The fixture uses the generated Variable catalog and canonical Init activation.
The managed gate calls typed, raw and deprecated setters through the real native
library, checks immediate acceptance and pending write order, rejects stale
Emitters, and measures successful SetValue calls for zero managed allocations.
Final applied values and override flags are checked by the C++ fixture test:
global 1 gives A/B 1/1, A-local 2 gives 2/1, then global 3 gives 3/3 with no local
overrides (the fixture scales these numbers to its declared 0..1 range).
The separate Layer/Bus runtime regression checks their actual parameter outputs.

## What each gate checks

The remote communication gate runs through the actual generated P/Invoke and
handwritten SDK wrapper. It checks default-off initialization, bounded string
and port inputs, a real occupied TCP port, preserved audio after startup refusal,
explicit retry, the reported process/endpoint, a TCP connection, explicit stop,
ephemeral restart, and automatic closure on engine termination. `Remote native`
reports missing native libraries as SKIP; `--require-native` makes that a failure.
This gate covers SDK lifecycle and socket startup, while the native remote
integration tests cover protocol negotiation and remote command results.

The gameplay publication/provider gate uses production exports and is enabled
with `--gameplay-root <original> --renamed-gameplay-root <renamed>` together with
`--language-root <multilingual>`. The original fixture contains `Play_U4_A`,
`Play_U4_B`, and `Play_U4_Caller`; its renamed publication removes `Play_U4_A`
and publishes `Play_U4_Renamed` on the same Action identity, without adding a
retired-name record or alias. The multilingual
fixture contains English and ja publications. Pack identities are read from
their manifests. This gate verifies JSON/Init pairing, tampered active/retired
rows, exact large integer revisions, byte-provider inspection/release, retained
partial startup, explicit correction of missing/corrupt content, and preservation
of the first engine owner when a second startup is rejected. It requires the
native library and fails when any required fixture is unavailable.

- **ABI fidelity** — `HarkenAbi.CheckLayout()` asserts every mirrored struct's
  managed `Marshal.SizeOf` / `Marshal.OffsetOf` equals the value the layout
  model computed (the same model the C++ side is pinned against). Pure managed
  reflection: it runs with **no native library**. Combined with the generated
  C++ `static_assert` pins (`harken_binding_abi_pin.g.h`, compiled into
  `harken_runtime_tests`, proving model == `Harken.h`), this establishes managed == C ABI.

- **Host coordinate conversion** — verifies the Unity axis mapping against the
  fixed Harken right-handed spatial contract. The regression also reconstructs
  the listener right vector and proves that a Unity-left source produces a
  negative pan value. Pure managed; no native library is required.

- **Pack bootstrap** — verifies recommendation mapping without a native library.
  With `--pack-root`, it also drives the real `PackBootstrap.Start` lifecycle and
  shared native fixture. Pack dependencies and catalog routing remain native
  Runtime responsibilities; the managed host supplies only the root and startup
  Pack identities.

- **Zero-GC hot path** — drives `Vn_SetValue`, the `Vn_SetVoice*` setters,
  `Vn_PostAction` (by stable id), and `Vn_Update` in a measured loop and
  asserts `GC.GetAllocatedBytesForCurrentThread()` did not change across 4096
  iterations of these synchronous calls. Before measurement, the runner warms
  the counter and requires a positive allocation control on the same thread.
  The measured region excludes that control and console output. The threshold
  remains exactly zero; unrelated process threads cannot contribute to it.
  This gate covers the calling-thread managed allocations of the exercised SDK
  paths, not allocations by user callbacks, other threads, or the native heap.
  This needs the native `HarkenCore` library:
  - build it (`HARKEN_BUILD_SHARED`, default ON) and either set
    `HARKEN_NATIVE_DIR` to the directory holding `HarkenCore.dll`, or leave it —
    the runner also probes `build-nc/bin/{Debug,Release}` and
    `build-vs/bin/{Debug,Release}`;
  - if the library is not loadable the gate is reported **SKIP** (never silently
  green); optional mode still exits 0 when the ABI and coordinate gates passed, while
    `--require-native` reports required native validation as **FAIL** and exits
    nonzero.

Exit code is nonzero when the ABI or coordinate gate fails, when the zero-GC
gate fails, or when the zero-GC gate skips under `--require-native`.

## Not covered here (honest dependencies)

- The **IL2CPP/AOT** build of the binding for macOS/Android/iOS is a Unity +
  device toolchain step; this runner proves C# language compatibility, not the
  AOT compile.
- The **Unity sample scene**'s audible behavior is an editor/device listen.
