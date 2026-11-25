# Changelog

All notable changes to Harken for Unity are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this package adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.10.0] - 2026-11-25

Paired with Harken Studio 0.10.0. Packs must be exported again with Studio 0.10.0 and shipped together with this package: it does not read Packs from earlier versions, and earlier packages do not read Packs from Studio 0.10.0.

### Added

- The package ships two native library editions: Release (`HarkenEngine`) and Development (`HarkenEngineDev`).
- **Project Settings > Harken > Project > Native Library** selects the edition. *Automatic* uses Development in the Editor and in Development Builds, and Release otherwise; *Release* and *Development* force one edition everywhere. Restart Unity after changing it.
- The Game view **Mute Audio** toggle now also mutes Harken output, with a short fade.
- Startup refuses a native library whose ABI major version does not match the managed bindings.

### Changed

- Remote communication, snapshot and Monitor features are available only with the Development edition (`HARKEN_ENGINE_DEV`). With the Release edition, the public remote communication methods return `VN_CAPABILITY_UNAVAILABLE`.
- The package declares its Unity built-in module dependencies (Audio, JSONSerialize, UnityWebRequest), so they cannot be disabled while the package is installed.
- New settings assets are created at `Assets/Resources/HarkenProjectSettings.asset` and `Assets/Editor/HarkenEditorProjectSettings.asset`. Existing settings assets keep working wherever they are.
- `HarkenAudioSystem.ConfigurePlatformBeforeStartup` has an overload that takes only the platform name.
- Renamed to match the current Pack metadata terms. The previous names remain as obsolete forwarding members:
  - `HarkenAudioSystem.ConfigureCompanionSourceBeforeStartup` is now `ConfigurePackSourceBeforeStartup`, and `HarkenPackSource.CaptureCompanion` is now `CapturePackSource`.
  - `HarkenStreamingPackSource` is now `HarkenStreamingAssetsPackSource`.
  - `ActionResolved.PublicationGeneration` is now `NameTableGeneration`.
  - `HarkenPreparedPacks` is now `HarkenPreviewContent`, and `HarkenEditorCatalog.PreparedPacks` is now `PreviewContent`.
  - `VN_ACTION_TARGET_PIECE` is now `VN_ACTION_TARGET_SEGMENT`.
- The Editor catalog is rebuilt from the Pack metadata. `GameplayPublication` is replaced by `ActionCatalog` (`ProjectId`, `Language`, `InitPackStableId`, `Actions`, `TryFindAction`, `GetPreparationOrder`), and `GameplayAction` by `ActionCatalogEntry` (`Name`, `ActionId`, `PackStableId`). `HarkenEditorCatalog.Catalog` and `HarkenPreviewContent.Catalog` expose it. `GameplayPublication`, `GameplayAction`, `PackManifestEntry`, `PackIndexEntry`, `RetiredGameplayAction` and the `Publication` properties remain as obsolete views of the same catalog; fields that never carried data keep their empty values. `HarkenActionPreview.Action` and `HarkenActionWaveform.Action` return `ActionCatalogEntry`, which converts to `GameplayAction`.
- The Action inspector says Pack directory, Refresh, Save binding and Action instead of publication terms. Bindings saved by earlier versions are not read; save them again.
- Reading a Pack directory no longer compares file identities through platform system calls or rejects file names that differ only in letter case. It does not detect a Studio export in progress; a partially written Pack is rejected as invalid, so retry after the export finishes.

### Deprecated

- `StreamSources`, `StreamByteSource`, `StreamReadCancellation` and `StreamSourceRequest`. Studio does not export streaming resources.
- `CurrentWorld` and `CurrentWorldLoadReport`. No product produces current-world envelopes; load Packs through `PackBootstrap`.
- The low-level Pack API: `Packs.LoadPack`, `LoadInitPack`, `InspectInitPack`, `ActivateCanonicalPackSet` and `UnloadCanonicalPackSet`, `CanonicalPackMember`, `PackByteProvider.LoadPack` and `PackByteProvider.InspectInitPack`. Use `PackBootstrap`.
- The renamed members listed above, `HarkenAudioSystem.ConfigurePlatformBeforeStartup(string, string)`, `HarkenEditorCatalog.RefreshPlatform`, `HarkenProjectSettings.PlatformId`, and the `HarkenProjectSettings` capacity properties and constants, which no longer have any effect.

### Removed

- Offline documentation (`Documentation~`) and the suspended PlayableDemo sample. Documentation is now online; see `README.md`.
- The project-specific `HarkenActions.g.cs` and `HarkenVariables.g.cs` symbol files are no longer shipped with the package.
- The `harken-integration.json` manifest. The package is identified by `package.json` only.
- Generated Variable symbols: `HarkenVariableId`, the `HarkenVariableReference` asset, `Values.RegisterGeneratedCatalog`, the `HarkenVariableId` overloads of `Values.SetValue` and `QueryVariable` and of `Emitter.QueryVariable`, and the Variable ID picker. `HarkenVariable` now identifies its Variable by name and project only; use Variable names or `VnObjectId` in code.
- Entry points of the retired Pack manifest workflow, which only returned an unsupported result: `GameplayPublication.Parse`, `Read` and `ValidateInit`; `Packs.ReplacePack`; `PackBootstrapSession.Publication`; `HarkenPackSource.Capture`; `HarkenPreparedPacks.Capture`, `Create`, `SupplyPack`, `SetDemandProvider` and `SelectedManifestPath`; `HarkenAudioSystem.ConfigurePreparedSourceBeforeStartup`, `ConfigurePreparedBytesBeforeStartup`, `StartRuntime(HarkenPreparedPacks)` and `StartRuntime(string, Func<string, byte[]>, string)`; `HarkenEditorPreview.Prepare(HarkenPackSource, ulong[], bool)`; `HarkenEditorCatalog.Refresh(string, Func<string, byte[]>, string)`; and `HarkenWaveformPublication`.
- The Platform Id project setting and the hidden Pack source and platform ID fields of `HarkenAudioSystem`. Select a platform directory by name.

### Fixed

- Remote communication failed to start when the project settings asset contained a legacy bind address.
- `HarkenPack` could act on a runtime Ready notification after it was disabled, or after its runtime stopped, during the same notification.
- A failed spatial source creation could leave a partially registered source behind.
- An inactive startup session kept its captured Pack bytes and sources alive.

## [0.9.4] - 2026-11-02

Paired with Harken Studio 0.9.4. These changes come mainly from the included audio runtime.

### Changed

- Sound playback preparation data is reused and variable lookups are faster, reducing repeated preparation work.
- Playback progress tracking and storage for voices awaiting playback need less traversal and allocation.

### Fixed

- Stopping at the natural end, the end of the current loop or an exit marker after a seek now uses the actual playback progress, including seeks performed while paused.
- Exit-marker stops were delayed or missed when source and output sample rates differed and pitch adjustment was applied.
- Seek result queries were inaccurate, seeks could change the duration of finite loops, and seeks could reactivate sounds in their effect tail.
- Pack updates and unloading could release audio data still needed by playing sounds or by other Packs, and failed loading or activation did not clean up prepared resources.
- Content updates did not retain compatible global and emitter variables, and emitter reuse could restore stale values.
- Send gains that overflow are now rejected, and invalid output from bypassed effects no longer contaminates the original signal.
- A failed nested playback could leave content partially started, leave inconsistent state, or report an inaccurate failure reason.

## [0.9.3] - 2026-10-29

Paired with Harken Studio 0.9.3. The package version now follows the Studio version.

### Added

- Action catalog browsing, selection, project binding and playback preview in the Unity Editor are restored, including preview outside Play Mode.
- Action duration queries report an exact duration, a bounded range or an unknown duration, and do not require waveform files.
- Source waveforms are displayed when waveform resources are available. Direct Sounds and single-layer Random Sounds show the waveform of the actual playback selection.

### Changed

- Display-only refreshes keep the current preview session instead of interrupting playback.
- Replaced TextAsset resources can be read again after a refresh.
- Copying Packs and waveform resources keeps the required companion files and their relative directory structure.

### Fixed

- Waveforms of trimmed audio and partial final waveform buckets lost their range or trailing data.
- Stale error messages remained visible on waveform cache hits after resources recovered.
- Startup with an explicit `PackSource` was blocked by a leftover `Init.pack` path setting. Synchronous and asynchronous startup now use the supplied source.

## [0.3.7] - 2026-10-28

First public release, paired with Harken Studio 0.9.2.

### Added

- Unity components: `HarkenAudioSystem`, `HarkenAction`, `HarkenVariable`, `HarkenEmitter` and `HarkenListener`.
- Action playback through components or C# APIs: global, on a specified emitter, or following a game object; state queries, stop with fades, and Seek for supported content.
- Variable writes through `HarkenVariable` or the `Values` API, using numeric values, Label names or Label IDs, globally or per emitter.
- 3D positioning through emitters and the listener, plus Voice controls (volume, pitch, pan, width, spatialization mix, distance scale, filters and sends) and Bus volume, output volume and mute.
- Loading of Init and Action Packs exported by Studio as paired `.bytes` and `.json` files, from directories, TextAssets or custom synchronous and asynchronous readers, with explicit preparation, preload and release.
- Operation results, playback state, notifications and logging. With communication enabled, Studio on the same computer can connect through Remote Connect and Monitor.

### Known issues

- Editor Action browsing, selection and preview are unavailable; enter Action Name and Project Id manually in the Inspector.

[Unreleased]: https://github.com/xindong/harken-studio/compare/HarkenStudio-0.10.0...HEAD
[0.10.0]: https://github.com/xindong/harken-studio/releases/tag/HarkenStudio-0.10.0
[0.9.4]: https://github.com/xindong/harken-studio/releases/tag/HarkenStudio-0.9.4
[0.9.3]: https://github.com/xindong/harken-studio/releases/tag/HarkenStudio-0.9.3
[0.3.7]: https://github.com/xindong/harken-studio/releases/tag/HarkenStudio-0.9.2
