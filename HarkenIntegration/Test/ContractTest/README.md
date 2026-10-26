# Managed and native SDK contracts

The .NET 9 runner compiles the shipped generated and hand-written SDK sources
with C# 9 and no NuGet dependencies. It checks ABI layout, coordinate conversion,
Variable symbol rules, bootstrap capacity recommendations, byte providers,
zero-allocation calls, Remote communication and companion lifecycle behavior.

```powershell
dotnet run --project HarkenIntegration/Test/ContractTest
$env:HARKEN_NATIVE_DIR = (Resolve-Path "build-nc/bin/RelWithDebInfo").Path
dotnet run --project HarkenIntegration/Test/ContractTest --configuration Release -- --require-native
```

Optional mode reports unavailable native checks as skipped. `--require-native`
fails if a required check cannot run. An explicit `HARKEN_NATIVE_DIR` is
authoritative and disables native-library search fallbacks.

The native suite uses current companion fixtures under `UnityFixture/Data`.
It covers Init-only startup, atomic Action preparation, language selection,
failed refresh preservation, renaming, shared leases, asynchronous reads,
cancellation, retries and restart isolation. Suspended PlayableDemo and retired
manifest/index publications are not validation inputs.

To validate a real exported selected-platform directory independently:

```powershell
dotnet run --project HarkenIntegration/Test/ContractTest --configuration Release -- `
  --require-native --companion-root "path/to/Windows"
```

This focused mode requires `Init.bytes` with `Init.json` and Action pairs,
including any language directories. It starts the native Runtime, prepares and
posts each Action, releases preparation and shuts down. Its version marker comes
from the validated binary. `.scripts/studio_schema_release_audit.py` uses this
entry point for development validation; shipping builds do not depend on it.

For the attenuation descriptor boundary, run `--attenuation-only` with
`HARKEN_NATIVE_DIR` pointing to the matching production library. It calls the
generated Register/Replace bindings with the 608/612-byte prefixes and the new
descriptor, including an enabled 1000 Hz high-pass point, a zero-Hz bypass point
and invalid tail values. This proves native argument transfer and validation;
it does not measure the resulting sound.

Run the same newly built runner in a separate process with
`--attenuation-old-native` and `HARKEN_NATIVE_DIR` pointing to the preserved
pre-high-pass production library. The old library must initialize and accept
the historical prefixes, then return `VN_INVALID_ARG` for both Register and
Replace with the new descriptor, including its disabled high-pass curve.
Neither mode skips a missing library or retries with a shortened descriptor.
`HarkenAbi.CheckLayout()` alone is a managed-layout check, not a native capability
probe; old gameplay calls can still work with the old library.

The separate native foundation fixture remains available through `--pack-root`:

```powershell
dotnet run --project HarkenIntegration/Test/ContractTest --configuration Release -- `
  --require-native --pack-root ".temp/f3-catalog-resident-window"
```

`PackFoundationQualification.CatalogMayExceedResidentWindow` produces its raw
`Init.pack` and `Content.pack` binaries. This low-level contract inspects Init,
applies capacity recommendations, activates the canonical set through byte
providers, posts loaded content, rejects unloaded content and unloads the set.
It does not discover or interpret legacy publication files.

For the focused Variable write-scope check, generate the native fixture and run:

```powershell
ctest --test-dir build-nc -C RelWithDebInfo -R '^PackComposition.VariableSdkSetValuePreservesWriteScopeAndPublishesInitFixture$' --output-on-failure
$env:HARKEN_VARIABLE_INIT = (Resolve-Path ".temp/variable-sdk-init.pack").Path
dotnet run --project HarkenIntegration/Test/ContractTest --configuration Release -- --variables-only
```

`--current-world <path>` adds the existing current-world input check.
`--state-only-unavailable` verifies capability refusal against an older native
library. Unity compilation, IL2CPP, device behavior and audible output require
their corresponding environments; this runner covers managed/native contracts.
