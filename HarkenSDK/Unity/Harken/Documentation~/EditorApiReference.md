# Unity API reference: Editor catalog, preview, and tools

In SDK 0.3.5, the catalog, Action Inspector's automatic list, preview, waveform
tools, and symbol-generation menu described here require legacy manifest
publications. They do not read Studio 0.9.0 Pack Schema 4 companion pairs or its
optional metadata/waveform exports. For new publications, use exact authored
names with [runtime preparation](UnityApiReference.md#prepared-content-and-preload)
and validate playback in Play mode or a Player. Legacy catalog parsing alone
does not establish compatibility with the current native runtime.

The `Harken.Unity.Editor` assembly is available only in the Unity Editor. It
validates a selected publication for inspectors and provides a preview session;
Player code must use the Runtime and Unity component APIs. Calls involving
Unity objects, `AssetDatabase`, or preview lifecycle run on the Editor main
thread.

## `HarkenEditorCatalog`

Create a catalog and refresh it when the selected export, platform, language,
or source changes. On success, `Publication` is the validated catalog.
`PreparedPacks` is populated by TextAsset/reader capture paths; an ordinary
filesystem directory refresh succeeds with `PreparedPacks == null` and sets
`DirectoryPath` instead. Check the property before passing it to a prepared-byte
API. `LastResult` and
`Diagnostic` describe the last catalog refresh; failed refresh clears the
selected publication and increments `Revision`. `WaveformResult` and
`WaveformDiagnostic` are separate because a valid gameplay publication can
lack an Editor waveform companion.

| Signature | Purpose |
| --- | --- |
| `VnResult RefreshPlatform(string root, string platformId, string platformName, string basePlatform, string language = null)` | Select a platform directory from the export index, then refresh. |
| `VnResult Refresh(string directory, string language = null)` | Read and pair a directory publication, including Unity asset capture where required. |
| `VnResult Refresh(HarkenPackSource source, string language = null)` | Capture a `TextAsset` publication. |
| `VnResult Refresh(HarkenPackSource source, string language, TextAsset companion)` | Capture gameplay plus an optional waveform companion. |
| `VnResult Refresh(string manifestPath, Func<string, byte[]> readBytes, string language = null)` | Read a logical-path publication through a byte callback. |
| `VnResult CheckReference(string name, string projectId, out string diagnostic)` | Validate an exact Action name and bound project; distinguishes unknown, retired, and wrong-project names. |
| `VnResult SelectAction(HarkenAction action, string name, bool rebindProject, out string diagnostic)` | Validate and write the component's serialized Action/project binding. Explicit `rebindProject` accepts the current publication's project. |
| `VnResult GetPreparationOrder(string name, out ulong[] ids, out string diagnostic)` | Return dependency-first Pack IDs for one Action. |
| `VnResult GetWaveform(string name, out float[] samples, out double? durationSeconds, out string diagnostic)` | Return merged alternating min/max extrema and optional known one-shot duration. |
| `VnResult GetActionWaveform(string name, out HarkenActionWaveform waveform, out string diagnostic)` | Return detailed source envelope and diagnostic. |
| `VnResult GetActionPreview(string name, out HarkenActionPreview preview, out string diagnostic)` | Return direct Sound or complete Random candidates, a cached overview and independently proven duration bounds. |

The optional `HarkenEditorWaveforms.json` companion is Editor data, paired to
the selected manifest and capped at 16 MiB.
`HarkenWaveformPublication.Parse(byte[] companionBytes, byte[] manifestBytes,
GameplayPublication publication, out HarkenWaveformPublication waveforms,
out string diagnostic)` validates it. Missing companion bytes return
`VN_CAPABILITY_UNAVAILABLE`; malformed companion data returns `VN_BAD_PACK`.
`TryFindAction(string name, out HarkenActionWaveform waveform)` looks up an
Action by exact name. A
`HarkenActionWaveform` exposes `Action`, `Source`, `OneShotDurationKnown`,
`OneShotDurationSeconds`, and unavailable reasons. Its `HarkenSourceEnvelope`
exposes stable sound ID, property fingerprint, content revision, frame count,
sample rate, channel/bucket counts, source duration, and
`GetChannelMinima(int channel)` / `GetChannelMaxima(int channel)`, which return
cloned arrays for `0 <= channel < ChannelCount` (an invalid index throws).
`GetMergedMinMax()` returns alternating extrema across all channels.
Waveform failure does not invalidate a
valid gameplay catalog. See [waveform format](EditorWaveformFormat.md).

`GetActionPreview` reads v1 and v2 companions through the same validated cache.
Its `Kind` distinguishes `DirectSound`, `RandomCandidates` and `Unavailable`.
`Candidates` preserves every occurrence and shares immutable source envelopes.
`Overview.MinMax` is a cached read-only sequence of alternating extrema;
`Overview.DurationSeconds` is the source-time axis, not a playback deadline.
`DurationKind` is `Exact`, `Bounded` or `Unknown`; only proven bounds populate
`MinimumDurationSeconds` and `MaximumDurationSeconds`. Unknown duration retains
valid waveform data and supplies `DurationUnavailableReason`.
`TryFindPreview` provides the same model from a parsed publication.
The legacy waveform methods still succeed only for direct Sound and return
capability-unavailable for Random; they never return its upper bound as an exact
duration. Refresh clears old preview data even when the new companion is missing
or invalid. No waveform result is a prerequisite for playback.

## `HarkenEditorPreview` and scopes

An Editor preview owns its native session. `CreateScope()` gives a disposable
`HarkenEditorPreview.Scope` for a selection or Inspector; dispose it when that
owner stops previewing. `Session`, `IsPrepared`, `LastResult`, `Diagnostic`,
`LastRemoteResult`, and `FiringCount` expose preview state. Do not dispose the
underlying `Session` separately. `Prepare` refuses with
`VN_ALREADY_INITIALIZED` during Play Mode, a Play Mode transition, or while
another Editor preview owns a session. Successful preparation registers an
Editor update pump and may start configured remote communication.

Directory `Prepare` reuses `catalog.PreparedPacks` when the catalog captured
bytes, including a Unity `.pack.bytes` source. The prepared-byte overload also
accepts a `HarkenPreparedPacks` object directly.

| Preview call | Contract |
| --- | --- |
| `Prepare(string directory, string language, ulong[] startupIds, bool nullDevice = false)` | Build preview from a publication directory. |
| `Prepare(HarkenPackSource source, ulong[] startupIds, bool nullDevice = false)` | Capture a Unity source and prepare. |
| `Prepare(HarkenPreparedPacks source, ulong[] startupIds, bool nullDevice = false)` | Prepare previously captured bytes. |
| `PrepareStartupPacks()` | Explicitly retry missing startup content after it is supplied. |
| `Play(string name, string projectId, out ActionFiring firing)` | Play an exact Action from the preview publication. |
| `Stop(ActionFiring firing, uint fadeMs = 50)` | Stop that preview-owned firing. If no undisposed scope remains, also arrange session shutdown after all firings finish. |
| `Stop(uint fadeMs = 50)` | Stop all preview firings, cancel pending scope preparations, and begin shutting down the entire preview session. |
| `Tick()` | Explicitly pump preview state and pending cleanup; the prepared preview also registers an Editor update pump. |
| `Shutdown()` / `Dispose()` | Release preview ownership; check `Shutdown()` result when cleanup matters. |
| `static ShutdownOwnedSession()` | End the currently owned Editor preview session. |

There are two entry paths. For standalone preview, call a parent
`preview.Prepare(...)`, then `preview.Play(...)` with content already prepared.
For selection-scoped preview, call `CreateScope()` followed directly by
`Scope.Prepare(HarkenPreparedPacks source, string[] actionNames)`; its first
nonempty preparation starts the session. Calling parent `Prepare` first and
then scope `Prepare` returns `VN_PACK_STALE_REVISION`. All scopes sharing a
session must use the same `HarkenPreparedPacks` object instance; newly captured
bytes with equivalent contents do not satisfy that identity requirement.

Typical scope calling order:

1. Capture a `HarkenPreparedPacks` source once and check its result. A successful
   directory catalog refresh alone may not provide these bytes.
2. Create a preview and scope, then call `scope.Prepare(prepared, actionNames)`.
   Inspect `IsPrepared`, `LastResult`, and `Diagnostic` before playing.
3. For spatial preview, call `scope.SetListener(Transform target)` after
   preparation, then `scope.CreateSpatialSource(Transform target, out
   HarkenSpatialSource source)`. One scope owns the shared listener at a time;
   conflicting listener/source operations return `VN_IN_USE`.
4. Call `scope.Play(string name, out ActionFiring firing)` or
   `scope.Play(string name, HarkenSpatialSource source, out ActionFiring firing)`.
   Only names in that scope's prepared list can play. The preview owns and
   releases the returned firing; do not call `firing.Release()` yourself.
5. Use `scope.Stop(ActionFiring firing, uint fadeMs = 50)` or
   `scope.StopAll(uint fadeMs = 50)` to stop that scope's playback. Dispose the
   scope when its Inspector/selection closes. The preview's update pump
   continues owned firing and Pack cleanup, including failed-release retries.

Changing the prepared names while the scope is playing requests stops and
stores the latest preparation request, returning `VN_IN_USE` until the old
firings finish. The Editor update pump completes that transition; observe
`IsPrepared` and `LastResult` instead of replaying the same request every frame.
Other `VN_IN_USE` cases, such as a reentrant call during a tick, may refuse
before storing a request. The Inspector should not start its own gameplay
`HarkenAudioSystem` session.

## Generated symbols and project settings

`HarkenVariableCatalogGenerator` exposes these Editor-only entry points:

| Signature | Purpose |
| --- | --- |
| `static void GenerateFromMenu()` | **Harken > Generate SDK Symbols From Publication**; opens the publication picker. |
| `static VnResult GeneratePublication(string jsonPath, string variablePath, out string error)` | Read a publication file, write Variable symbols to `variablePath` and Action symbols beside them, then import both. |
| `static VnResult Generate(string jsonPath, string csharpPath, out string error)` | Write and import only Variable symbols. |
| `static VnResult ValidatePublication(string json, string sourceLabel, out string error)` | Validate Variable catalog JSON content without writing files. This is not a filename parameter. |
| `static string ResolveOutputPath()` | Reuse a discovered generated asset, otherwise choose the package location or `Assets/Harken/Generated`. |

Generation reads the selected publication; generated names are aliases for
stable IDs, not a replacement for validating the runtime publication.
`ValidatePublication` does not validate the Action catalog, Pack files or
manifest/Init pairing; a missing Variable list can be a valid empty catalog.
Its `VN_OK` is not proof that the full export is playable. The underlying
`ActionCatalogEmitter`, `VariableCatalogEmitter` and guard are internal.
For generation, IDE completion and output ownership, follow the
[menu walkthrough](../README.md#generate-symbols-and-use-code-completion).
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
