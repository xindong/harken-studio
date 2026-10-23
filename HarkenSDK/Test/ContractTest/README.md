# Managed and native SDK contracts

The .NET 9 runner compiles the shipped generated and hand-written SDK sources
with C# 9 and no NuGet dependencies. It checks ABI layout, coordinate conversion,
Variable symbol rules, bootstrap capacity recommendations, byte providers,
zero-allocation calls, Remote communication and companion lifecycle behavior.

```powershell
dotnet run --project HarkenSDK/Test/ContractTest
$env:HARKEN_NATIVE_DIR = (Resolve-Path "build-nc/bin/RelWithDebInfo").Path
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- --require-native
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
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- `
  --require-native --companion-root "path/to/Windows"
```

This focused mode requires `Init.bytes` with `Init.json` and Action pairs,
including any language directories. It starts the native Runtime, prepares and
posts each Action, releases preparation and shuts down. Its version marker comes
from the validated binary. `.scripts/studio_schema_release_audit.py` uses this
entry point for development validation; shipping builds do not depend on it.

The separate native foundation fixture remains available through `--pack-root`:

```powershell
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- `
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
dotnet run --project HarkenSDK/Test/ContractTest --configuration Release -- --variables-only
```

`--current-world <path>` adds the existing current-world input check.
`--state-only-unavailable` verifies capability refusal against an older native
library. Unity compilation, IL2CPP, device behavior and audible output require
their corresponding environments; this runner covers managed/native contracts.
