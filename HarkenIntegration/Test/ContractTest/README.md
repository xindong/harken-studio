# Managed and native API contracts

The .NET 9 runner compiles the generated and hand-written managed API sources
shipped in the Unity Integration Package with C# 9 and no NuGet dependencies. It checks ABI layout, coordinate conversion,
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

Pack metadata version rules are checked with hand-written JSON. Behavior that
needs real Studio exports (startup, preparation, language and platform selection,
renaming, ownership, audible playback) is covered by the cross-product
verification, which builds and exports its projects on every run.

To validate a real exported selected-platform directory independently:

```powershell
dotnet run --project HarkenIntegration/Test/ContractTest --configuration Release -- `
  --require-native --companion-root "path/to/Windows"
```

This focused mode requires `Init.bytes` with `Init.json` and Action pairs,
including any language directories. It starts the native Runtime, prepares and
posts each Action, releases preparation and shuts down. Its version marker is
the Pack metadata version this package accepts (`packMetadataVersion`).
`.scripts/schema_release_audit.py` uses this entry point for development
validation; shipping builds do not depend on it.

For the attenuation descriptor boundary, run `--attenuation-only` with
`HARKEN_NATIVE_DIR` pointing to the matching `HarkenEngineDev` library. The
runner is built for the development edition; the attenuation entry points are
Core ABI and identical in both editions, and the shipping export table is
checked by the Engine export checks. It calls the generated Register/Replace
bindings with the 608/612-byte prefixes and the new descriptor, including an
enabled 1000 Hz high-pass point, a zero-Hz bypass point and invalid tail values.
This proves native argument transfer and validation; it does not measure the
resulting sound. It does not skip a missing library.

Unity compilation, IL2CPP, device behavior and audible output require their
corresponding environments; this runner covers managed/native contracts.
