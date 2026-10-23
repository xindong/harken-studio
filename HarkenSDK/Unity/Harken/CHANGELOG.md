# Changelog

All notable changes to the Harken Core Unity package are documented here.

## [0.3.5] - unreleased

- Prepare Pack Schema 4 companion publications for Studio 0.9.0. The companion
  reader version is generated from the selected Pack schema independently of
  the legacy manifest reader. Rebuild publications and update managed bindings
  and native libraries together when adopting the new generation.
- Support directory and asynchronous companion sources with atomic Action
  preparation, explicit release, cancellation, and version validation before
  activation.
- Re-export PlayableDemo and companion regression data with the new generation.
  Rebuild the sample scene from its Init and Action pairs and prepare Actions by
  name before enabling playback.

## [0.3.4] - 2026-10-19

- Bind gameplay catalogs separately from Init packs and support a shared Init
  across language publications for incremental exports. Update managed bindings
  and native binaries together.
- Preserve emitter and listener transform publication when result observers
  change component state, and ignore stale runtime Ready callbacks.
- Size runtime command capacity for the configured maximum voice count.

## [0.3.3] - 2026-10-15

- Allow the project settings asset to move between parent directories while
  preserving its Resources lookup path and duplicate-resource validation.
- Reuse Editor-only settings after moving or renaming them, and reject
  duplicate Editor settings assets.

## [0.3.2] - 2026-10-14

- Remember Player application pause state during startup and apply it once the
  runtime session is active, without reporting uninitialized operations.
- Defer the initial Ready and BecameReady notifications until a paused startup
  session resumes successfully. Later resumes only raise BecameReady.
- Guard application pause handling during startup cancellation and shutdown.

## [0.3.1]

- Require Pack Schema 3 publications exported from new Harken Studio 0.5.0
  projects. Pack Schema 1 and 2 are no longer accepted. Replace existing Pack
  publications and their waveform companions together with the SDK update.
- Rename native artifacts on every supported platform to `HarkenCore.dll`,
  `libHarkenCore.so`, `libHarkenCore.dylib`, and `libHarkenCore.a`.
  Update managed bindings and native binaries together; remove the old native
  library while retaining its Unity plugin metadata. Windows import linkage
  uses `HarkenCoreImport.lib`; iOS P/Invoke remains `__Internal`.
  Native `Vn_*` ABI, exported symbols, and audio behavior are unchanged.
- Defer settings migration until asset import completes so a cold SDK import
  cannot prevent Editor shutdown and domain-reload cleanup registration.

## [0.3.0]

Initial C# binding + UPM package.

### Added

- Managed spatial sources for AudioSystem and Editor preview scopes, including
  source-instance identity, transform following, frozen tails after recycling,
  and explicit ownership of the shared preview listener.
- Thin `Voice` forwarding for native pan, width, spatialization mix, distance
  scale, independent output volume, and send slot controls.
- Action closure preparation for managed and component Play, shared per-firing
  resource ownership, retryable failed cleanup, and reusable warm preparation.
- Optional synchronous demand Pack providers with explicit snapshot precedence.
- Offline Editor source waveform companions, strict publication identity checks,
  per-channel extrema, and separately proven nominal Action durations.

- Explicit Harken spatial contract and allocation-free Unity coordinate
  conversion helpers.
- A `Harken.Unity` adapter assembly with listener and emitter transform helpers
  that accept Unity world-space values and perform the handedness conversion.
- Thin, generated P/Invoke projection of the whole Harken Core C ABI under
  `Runtime/Generated/`, machine-generated from the same single
  description table the engine's remote-wire mirror is generated from
  (`harken_wire_table.json`) — header/binding drift is structurally impossible.
- Blittable `[StructLayout(Sequential)]` mirrors of every public struct, the
  `VnResult` enum, every public enum and `#define` constant, and the callback
  delegate types.
- Generated ABI-fidelity check (`HarkenAbi.CheckLayout`) asserting the managed
  `Marshal.SizeOf`/`Marshal.OffsetOf` equal the C header's, plus C++
  `static_assert` pins of every generated value against `Harken.h`.
- SDK-facing ergonomic wrappers (`Listener`, `Emitter`, `Voice`/`Playback`,
  `Actions.Post` by stable id, `Values.SetValue`, `Packs.LoadPack`
  / `UnloadPack`) with a documented zero managed-heap-allocation hot path.
- Per-firing `ActionFiring` handles with independent stop/query/release control,
  nested and delayed work ownership, plus atomic `Playback.StopAll`.
- AOT-safe callback bridge (`HarkenCallbacks`): static thunks, cached function
  pointers, `userData`-keyed registry — no runtime code generation.
- `Runtime/Plugins/` layout + per-platform import-settings documentation; native
  library name baked per platform (`HarkenCore`, `__Internal` on iOS).
- A single `HarkenAudioSystem` owner and attachable Pack, Listener, Emitter, Action,
  and Variable components with observable lifecycle results.
- Unity and Unreal source roots separated so release builds can assemble each
  SDK with its platform binaries without committing generated artifacts.
- Unity-generated native plugin importers are normalized to the matching
  platform and CPU by SDK Editor code.

### Requirements

- Unity **2022.3 LTS** minimum (declared in `package.json`).
- IL2CPP/AOT compatible: no runtime code generation, no `Reflection.Emit`.
