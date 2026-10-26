# Unity API reference: Editor catalog, preview, and tools

The `Harken.Unity.Editor` assembly is available only in the Unity Editor.
Publication tools are temporarily suspended while their readers are adapted to
current-schema Pack companions. Player and Play Mode playback use the Runtime
and Unity component APIs.

The Action Inspector accepts **Action Name** and **Project Id** manually. Enter
the exact authored name and the audio project's GUID; a running companion
session exposes the validated identity through `Session.ProjectId`. Automatic
catalog binding, waveform browsing and preview controls are hidden.
`HarkenEditorCatalog.Refresh*`, all preview `Prepare` entry points including
`Scope.Prepare`, and generator validation/generation return
`VN_CAPABILITY_UNAVAILABLE` before reading publication files or starting audio.
The symbol-generation menu is unregistered. Plugin importers, Project Settings,
and runtime components remain available; the legacy Platform Id setting is hidden.
When an ID was previously saved, **Project Settings > Harken > Project** shows
**Clear legacy platform selection** so it can be removed before companion startup.

Historical Pack, gameplay-publication and waveform parsers have been removed.
Data declarations remain only for suspended API signatures. Restore each
workflow only after it consumes current-schema companions without requiring a
separate legacy manifest or fixed schema number, and validate it before removing
the capability gate. Calls involving Unity objects or `AssetDatabase` run on
the Editor main thread.

## `HarkenEditorCatalog`

All `Refresh` overloads and `RefreshPlatform` return
`VN_CAPABILITY_UNAVAILABLE` without reading a directory, TextAsset or callback.
They clear `Publication`, `PreparedPacks` and `DirectoryPath`, increment
`Revision`, and update `LastResult`, `Diagnostic`, `WaveformResult` and
`WaveformDiagnostic`.

`CheckReference`, `SelectAction`, `GetPreparationOrder`, `GetWaveform`,
`GetActionWaveform` and `GetActionPreview` return the same unavailable result.
They do not validate bindings, write serialized components, traverse
publication dependencies, or generate previews.

`HarkenWaveformPublication.Parse` returns `VN_CAPABILITY_UNAVAILABLE` without
parsing legacy metadata. Its result models remain available for the suspended
Editor API signatures. Current companion metadata needs a separate tooling
integration before waveform browsing and preview are restored.

## `HarkenEditorPreview` and scopes

All `Prepare` overloads and `Scope.Prepare` return
`VN_CAPABILITY_UNAVAILABLE` before starting a native session or reading a
publication. The old directory and prepared-publication startup workflows are
unsupported; there is no manifest or wrapper-path fallback.

The preview and scope ownership declarations remain available. A preview owns
its `Session`; callers must not dispose that session separately. Scopes are
disposable owners for a selection or Inspector. Existing `Stop`, `StopAll`,
`Tick`, `Shutdown`, `ShutdownOwnedSession` and `Dispose` cleanup entry points
remain in place. `Session`, `IsPrepared`, `LastResult`, `Diagnostic`,
`LastRemoteResult` and `FiringCount` expose the preview state. These declarations
do not make playback available while preparation is suspended.

## Generated symbols and project settings

`HarkenVariableCatalogGenerator` retains these Editor-only declarations:

| Signature | Purpose |
| --- | --- |
| `static void GenerateFromMenu()` | No registered menu. An explicit call displays the temporary-unavailability diagnostic without opening a publication picker. |
| `static VnResult GeneratePublication(string jsonPath, string variablePath, out string error)` | Returns `VN_CAPABILITY_UNAVAILABLE`; no files are read, written or imported. |
| `static VnResult Generate(string jsonPath, string csharpPath, out string error)` | Returns `VN_CAPABILITY_UNAVAILABLE`; no files are read, written or imported. |
| `static VnResult ValidatePublication(string json, string sourceLabel, out string error)` | Returns `VN_CAPABILITY_UNAVAILABLE`; no publication validation occurs. |
| `static string ResolveOutputPath()` | Reuse a discovered generated asset, otherwise choose the package location or `Assets/Harken/Generated`. |

Existing generated names remain aliases for stable IDs; they do not replace
validation of the loaded runtime content. The underlying
`ActionCatalogEmitter`, `VariableCatalogEmitter` and guard remain internal.
Use [name-based calls](../README.md#action-and-variable-calls) while generation
is unavailable.
Installation and migration use the current [package instructions](../README.md);
the removed legacy script/settings migration classes are not callable APIs.

`HarkenEditorProjectSettings` holds Editor-only audio project location at
`Assets/Harken/Editor/HarkenEditorProjectSettings.asset`; `Load()`,
`GetOrCreate()`, and `ValidateNotInResources()` manage it. The
`HarkenProjectSettingsProvider` exposes the **Project Settings > Harken**
pages, `AssetPath`, `GetOrCreateSettings()`, and `ValidateSettingsAssets()`;
the build preprocessor validates the project settings asset before a Player
build. These settings providers are Editor infrastructure and are not callable
from a Player assembly.

The custom inspectors, settings provider, plugin importer, and lifecycle hooks
are Editor integration infrastructure. Their public types are discoverable in
[`Editor/`](../Editor/) but are not Player entry points.
