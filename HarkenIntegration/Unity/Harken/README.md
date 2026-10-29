# Harken Unity Integration Package

`com.harken.core` is the Unity package for the Harken Core game-audio runtime.
The package root contains Unity content only. The `version` field in this
package's `package.json` is the sole source of the Integration Package version.
The package combines a managed SDK (C# bindings and gameplay/host APIs) with
Unity runtime components, Editor tools and native runtime libraries. In this
manual, SDK refers to the managed API layer; Integration Package refers to the
complete Unity deliverable.

Editor catalog binding, preview, waveform browsing and symbol generation are
temporarily unavailable while they are adapted to current-schema Pack companions.
Configure **Action Name** and **Project Id** manually in the Action Inspector, or
use the runtime APIs by name. Android's default directory auto-start and the
PlayableDemo are also suspended; explicit companion sources and runtime playback
remain available. See the entry points below and the
[Editor API status](Documentation~/EditorApiReference.md).

## API reference manual

The manual describes the public SDK APIs and Unity components, including their
parameters, resource ownership, lifecycle and failure behavior.

- [Startup, Actions, Emitters and listeners](Documentation~/UnityApiReference.md)
- [Variables and Labels](Documentation~/VariableApiReference.md)
- [Pack loading, publications and ownership](Documentation~/PackApiReference.md)
- [Voice and Bus mixing](Documentation~/PlaybackMixApiReference.md)
- [Notifications, diagnostics and Remote](Documentation~/DiagnosticsRemoteApiReference.md)
- [Editor catalog, preview and symbol generation](Documentation~/EditorApiReference.md)
- [Custom hosts and public types](Documentation~/HostAndTypesReference.md)

Each chapter covers signatures, parameters, lifecycle, ownership and failure
behavior. For symbol-generation availability and existing IDE completion, see
[Generate symbols and use code completion](#generate-symbols-and-use-code-completion).

## Public API

Game code uses the public gameplay and host APIs in `Harken.Core` and the Unity
components in `Harken.Unity`. Editor integrations use the separate
`Harken.Unity.Editor` assembly.

| Responsibility | Public entry points |
| --- | --- |
| Actions, Variables and spatial state | `Actions`, `ActionFiring`, `ActionSeekTicket`, `Values`, `Emitter`, `Listener`; Unity components are described below. |
| Session and content ownership | `HarkenAudioSystem` for Unity-managed sessions; `HarkenEngine`, `PackBootstrap`, `PackBootstrapSession`, `Packs` and `PackByteProvider` for custom hosts. |
| Voice and Bus runtime controls | `Voice`, including `Voice.SetFilter`; `HarkenEngine.SetBusVolume`, `SetBusOutputVolume` and `SetBusMute`. |
| Optional Player communication | `HarkenEngine.StartRemoteCommunication`, `StopRemoteCommunication` and `QueryRemoteCommunication`; Unity settings and component retry/status methods are described under Remote communication. |
| Editor preview, waveform and symbol generation | Temporarily suspended. `HarkenEditorPreview`, `HarkenEditorCatalog` and `HarkenVariableCatalogGenerator` retain their declarations but refuse preparation, refresh and generation with `VN_CAPABILITY_UNAVAILABLE`. |

The complete generated P/Invoke facade (`HarkenNative`), ABI inspection helper
(`HarkenAbi`), Snapshot/Monitor DTOs, Bus topology/solo/filter operations, and
raw media/Effect/Binding mutation are internal implementation or tool APIs.
The direct `RemoteCommunication` bridge, catalog emitters, callback pointer
fields and provider pin counters are internal too. Generated types and constants
needed by supported public signatures remain public; access is classified per
declaration. These managed access restrictions do not remove native exports or
change native layouts.

`Voice.SetSend` and `Voice.ClearSend` remain public structural exceptions: they
create, replace or remove a Send route. `SetSendLevel` changes an existing route's
level. The public API therefore still includes these routing operations.

### Calling thread, results and units

Call Unity components and gameplay APIs on the Unity main thread. Custom hosts
serialize engine calls on the logic thread that owns `Init` and `Update`; call
`HarkenEngine.Update()` regularly to service callbacks and retirement. These
control APIs are not audio-render callbacks. A zero-allocation wrapper does not
make an operation safe to call from an audio thread. Resource preparation may
allocate and perform synchronous I/O.

Check every `VnResult`. `VN_OK` means that a control request was accepted; audio
application or stop completion may follow on a later block. Capacity refusals
such as `VN_BACKPRESSURE` are not queued for an automatic SDK retry. Correct the
cause or wait for capacity, then explicitly retry when appropriate. Handle
`IsValid` properties only check for a nonzero value; use the corresponding query
to determine whether an object is still live.

| Parameter | Unit and accepted values |
| --- | --- |
| `HarkenAudioSystem.SetMasterVolume` | Linear amplitude: 0 is silence, 1 is unity; finite inputs clamp to `[0, 1]`. |
| Voice/Bus volume, output volume and Send level | dB: 0 is unity. Native validation governs finite gain and routing constraints. Main-output volume does not change Send levels. |
| `Voice.SetPitch` | Playback-rate ratio in `(0, 64]`; 1 preserves the authored rate. |
| `Voice.SetPan` | `[-1, 1]`: left to right; 0 is center. |
| `Voice.SetWidth`, `SetSpatializationMix` | `[0, 1]`. Spatialized mono and stereo use the shared spatial matrix. Mix 0 preserves the authored 2D image; Mix 1 uses the point/spread image. Width spreads stereo input columns along the front arc and blends mono toward center. Authored 2D voices store both values without changing their image. |
| `Voice.SetDistanceScale` | `[0.01, 100]`; attenuation uses distance divided by this scale. |
| `Voice.SetFilter` | `filterKind`: 0 lowpass / 1 highpass; cutoff in Hz, finite Q, positive Q when enabled; `enable01`: 0 bypass / 1 enabled. Native cutoff limits also determine bypass. |
| `rampMs`, `fadeMs`, Seek `positionMs` | Milliseconds. Seek uses source time; relative mode accepts signed offsets. |
| Unity Transform positions | Meters; Unity helpers perform the shared handedness conversion. Raw `VnVec3` calls use engine coordinates. |

Voice setters preserve the native validation and smoothing rules. In particular,
`Voice.SetPosition` returns `VN_EMITTER_ATTACHED` while a live emitter owns its pose;
move the emitter instead. The complete per-operation contract is in the native
[`Harken.h`](../../../HarkenEngine/Source/Runtime/api/include/harken/Harken.h).

Centered mono preserves the original sample level in each stereo output channel
in both 2D and 3D. Its equal-power pan law has unity gain per side at center;
native mono mix stays unity. Stereo media keeps separate channels through source processing; spatial
mixing then preserves its natural correlation and phase cancellation. Wider than
stereo sources are rejected for 3D. Live pan, width and Mix changes share a 10 ms
matrix transition; initial starts and park returns snap to the current image.

## Install

Use **Hub > Engine Integrations**. Select the Integration Package source
directory and an installation location inside the Unity project. The default is
`Packages/com.harken.core`; a regular `Assets` subdirectory such as
`Assets/ThirdParty/HarkenIntegration` is also supported. Editor-only folders,
Resources, StreamingAssets, hidden folders, linked directories and external
destinations are not supported. Older Integration Package sources without
custom-location support remain limited to `Packages/com.harken.core` and
`Assets/HarkenSDK`.

`Assets/HarkenSDK` remains a supported installation convention. Old `HarkenSDK`
source layouts and `HARKEN_UNITY_SDK_PATH` are legacy aliases; use
`HarkenIntegration` and `HARKEN_UNITY_INTEGRATION_PATH` for new source locations.
The package identity, assembly names and asset GUIDs remain unchanged. Legacy
backup names (`SDK`, `SDK.meta`, `ReintegrationSDK`) and persisted `sdk_*` fields
remain unchanged for compatibility and describe the Integration Package.

Hub checks for existing Integration Packages and package references before
installation or updates. A single existing installation determines the location;
conflicting copies or unidentifiable Integration Package remnants must be resolved
first. Use **Migrate...** beside the installed location to move the current
Integration Package without unlinking projects.
Migration preserves modified files, additional files, hidden records, empty
directories and all `.meta` files, including the Integration Package directory's
own metadata. It does not reinstall files from the Integration Package source or
reset ownership hashes.
Close Unity first and select an empty destination without existing `.meta` data.
Hub verifies a complete backup outside the Unity project before moving anything.
Conflicting package references and unsupported Integration Package locations block
migration. Project code that hard-codes old paths must be updated separately.

Integration Package discovery also inspects local packages referenced under other
names. Paths containing the entire Unity project are rejected. Each discovery shares a limit
of 100,000 inspection steps, 64 directory levels, 1 MiB per JSON file and 16 MiB
of JSON in total, and checks a two-second deadline between filesystem operations.
Repeated or overlapping package references reuse visited directories. If a limit
is reached, Hub blocks Integration Package changes until the package references
or scanned content are corrected; incomplete discovery is never treated as a safe installation.
Malformed unrelated package or assembly identity files do not block Integration
Package changes, but malformed project UPM manifests and recognizable Integration
Package remnants still do.
If no Integration Package has been identified at `Packages/com.harken.core` or
`Assets/HarkenSDK`, an existing `package.json` there must be
readable and identify a package. An Integration Package identified by its
ownership record can still be repaired at its existing location after confirming
file changes. An empty directory or a valid unrelated package at those paths is
not treated as an Integration Package.

An interrupted migration blocks installation and removal until **Recover...**
restores the previous location. Its record is stored at
`Library/HarkenIntegration/pending-migration.json`; the verified backup contains
`SDK`, its root `SDK.meta` when present, and `migration.json` with paths and hashes.
Do not remove the pending record to bypass recovery. If files have changed since
the interruption, Hub refuses to overwrite them; preserve both locations and
restore the verified backup manually.

Unrelated nonempty directories are not overwritten;
move any project files preserved by an uninstall before reusing that directory.

Each game project's last verified relative location is stored in Hub's local
`engine-targets.db`. The installed Integration Package retains its version,
managed file list and hashes in `.harken-integration-state.json` at its actual
root. Hub can rediscover a moved Integration Package when this record is valid.
If Hub's database is lost, add the game project again; the Harken project association must be selected
again separately. Restore a missing or invalid Integration Package ownership
record from a backup before updating or using managed removal.

For source development, add this repository package to `Packages/manifest.json`:

```json
"com.harken.core": "file:../path/to/harken-studio/HarkenIntegration/Unity/Harken"
```

The same `HarkenIntegration/Unity/Harken` source is used for package and Assets installs.
Copy the package contents directly into the selected root. The Editor hook
locates the installed Integration Package by its script GUID and package
identity, then configures its native plugins after imports, moves and deletions.
Runtime Pack and audio data stay outside the Integration Package. Filesystem auto-start uses the directory selected under Project Settings > Harken > Project in the Editor and desktop Player. Select the exported platform directory containing `Init.bytes` and `Init.json`, or select its parent and set the exact Platform Name. The project team's build/deployment pipeline is responsible for including the Packs; the Unity integration does not copy them or choose a different directory.

For the default filesystem loader, relative paths start at the Unity project root in the Editor. In a desktop Player, they start at the parent of `Application.dataPath` (the executable directory on Windows). For example, `GameContent/GeneratedPacks` resolves to that same relative layout under either root. Editor auto-start through Project Settings requires a directory inside the Unity project; a desktop Player can use an external absolute directory as configured. Unity's `Assets/StreamingAssets/...` paths map to the actual `Application.streamingAssetsPath/...` in the Player, so moving the installed game does not retain the Editor path. Missing directories fail startup without searching another location.

For AssetBundles or another project-owned resource system, disable Auto Start
before `Awake` and supply a `PackSource` through `StartRuntime` or
`StartRuntimeAsync`. Use `PackSource.FromReader` for synchronous bytes,
`HarkenPackSource.CaptureCompanion` for TextAssets, or
`HarkenStreamingPackSource.Create(platformRoot, language)` for packaged/HTTP
URIs, including Android jar URIs. The reader supplies paired `.bytes` and `.json`
files by their logical relative paths. Android's default directory auto-start
returns `VN_CAPABILITY_UNAVAILABLE`; configure a companion source before startup
or call `StartRuntimeAsync` explicitly. See the
[companion source reference](Documentation~/PackApiReference.md).

The repository deliberately does not track generated native binaries. Studio
builds and Integration Package release artifacts inject binaries produced by the
`harken_core_shared` target. `Runtime/Plugins/README.md` documents the exact
platform and CPU layout. Only reviewed Unity identity and importer metadata is
kept beside assets that need stable GUIDs (Scenes, Prefabs, referenced
scripts/asmdefs, Samples, Tests, and native plugin importer settings).
Documentation and package identity files do not need `.meta` files. The Unity
Editor hook narrows each native-plugin importer to its actual platform and CPU.

## Variable writes

Variable definitions have no scope. Public writes accept the authored variable name, a generated GUID symbol, or a typed Variable reference. Name calls can include an audio project id to check project ownership. Omitting the emitter writes globally and removes every emitter override for that Variable, even when the global number is unchanged. Passing an emitter writes only its local value. An explicitly supplied empty or stale emitter fails; it never becomes a global write.

```csharp
Values.SetValue("Wood", projectId, 1);             // A = 1, B = 1
Values.SetValue("Wood", projectId, 2, emitterA);   // A = 2, B = 1
Values.SetValue("Wood", projectId, 3);             // A = 3, B = 3; no local overrides
```

`Values.SetValue` accepts a numeric value, a Label name, or a Label GUID. All three forms support a Variable name, generated GUID, or typed Variable reference and the same optional emitter. Variable-name calls also have project-qualified overloads. Label writes use the Label's authored anchor value.

```csharp
Values.SetValue("Surface", "Wood");
Values.SetValue("Surface", "Stone", emitterA);
Values.SetValue("Surface", projectId, "Wood", emitterA);
Values.SetValue("Surface", labelId, emitterA);
```

Local values belong to the emitter and are shared by all of its consumers,
including multiple plays, different Actions, and sounds under different Layers.
Layer bindings evaluate in each descendant playback's emitter context. Bus
bindings always read the global value; routing does not transfer emitter context.
See the [Game Modulate contract](../../../.docs/philosophy/game-modulate-system.md).

Writes are submitted synchronously in call order. Use `Values.SetValue` for global and emitter writes. Legacy emitter setters, batch setters, and global reset wrappers are not public. Local Clear and ResetVariables are unavailable.
For object pooling, destroy the old emitter and create a new one when assigning
a new game-object identity. A retained emitter retains its local values.

`HarkenVariable` supports a name plus project ID, or a typed `Variable` binding.
Assigning its public `Variable` property or selecting the typed field in the
Inspector explicitly switches to typed mode and clears the name/project pair.
Existing serialized components may contain both: the name remains authoritative,
but its resolved ID must match the typed ID or the write is refused with
`VN_INVALID_ARG`. Neither an incomplete name/project pair nor conflicting IDs
silently falls back to a different Variable. Typed writes use the existing
generated-catalog guard.

## GameObject components

- `HarkenAudioSystem` is the single global owner. Filesystem auto-start reads the selected companion platform directory and starts with `Init.bytes` plus `Init.json`. It does not fall back to StreamingAssets/Audio/GeneratedPacks. On a built desktop Player, the platform directory must also be included in the selected player exports. Android requires an explicit companion source while its default directory startup is suspended. The owner
  pumps `Vn_Update`, observes suspend/resume results, shuts participants down
  in dependency order, terminates the native session, then clears managed
  ownership and callback registrations at the safe completion boundary.
  Keep every exported `.bytes` file together with its matching `.json` companion.
- `HarkenPack` is hidden from Add Component, and its Load On Ready control is
  hidden. Companion sessions disable automatic cold-ID loading. After content
  is discovered with `Session.PrepareActions`, explicit `Load()` and `Unload()`
  can still acquire and release a lease for that known Pack ID.
- `HarkenListener` publishes a Unity Transform as the listener.
- `HarkenEmitter` owns one native emitter and follows its Transform.
- `HarkenAction` persists an exact authored `actionName` and validated `projectId`.
  It posts through the checked name API, then queries, stops, and releases its
  own firing and its Action resource scope. Cold Play synchronously prepares
  the named Action's declared Pack closure before one checked native Post. `playOnEnable`
  attempts once after preparation; a refused attempt is not retried in Update.
- `HarkenVariable` writes a global or emitter-scoped authored Variable.

Every component exposes its last `VnResult`. `HarkenAudioSystem` additionally exposes
`State`, `LastOperation`, `LastResult`, `BecameReady`, `OperationCompleted`, and
Inspector-bindable Ready/Failed/Result events. Duplicate runtime owners fail
instead of silently taking over global engine state.

For overlapping global one-shots, call
`runtime.PlayAction(actionName, projectId, out firing)`. Optional preload or
`Session.PrepareActions` moves cold I/O before a timing-sensitive trigger.
The optional `callerSeed` overload preserves a caller-selected seed.
`HarkenAudioSystem` owns these firings and releases each after natural completion; the returned
handle can be queried or stopped, but must not be released by the caller.
Managed slot capacity refusal is `VN_NO_ACTION` before any provider call. Native
Post refusal releases the newly acquired resource scope; failed cleanup retains
ownership for maintenance. A request never stops another firing or replays later.
Native `Actions.Post` remains a no-load primitive.

`Voice` remains a non-owning handle. Its pan, width, spatialization mix,
distance scale, output volume, and send methods each forward once to the
matching `Vn_*` operation and return that `VnResult` unchanged. Validation,
ramping, rendering, pending/parked behavior, and failure semantics remain native;
the managed SDK does not cache values or retry `VN_BACKPRESSURE`.

`ActionPreparation` owns shared Pack requests. Release unwinds the closure in
reverse order and retains remaining ownership if an unload fails. Always retain
the returned scope, including a failed preparation, until cleanup succeeds.
After release, `Prepare()` reuses its plan on the original live session without
warm managed allocation. Component Play also reuses its released plan.

Before startup, `ConfigureLifecycleBeforeStartup(true, true)` opts into pausing
and muting Unity audio while the prepared Runtime owns the session, and shutdown
when its component or GameObject is disabled. Both options default to false.
The Unity integration restores the captured Unity pause and volume values only
after the session ends. `RuntimeEnded` reports the final state, operation, and result after
ownership is cleared; runtime failure retains its original cause. Failed native
termination retains ownership and does not send an ended event. A completion
listener may explicitly start another session on an active, enabled host.
Disabling, destroying, or quitting a host prevents restart during that lifecycle
transition. After a runtime failure disables the component, explicitly enable it
before starting again. For shutdown inside a native callback, completion follows
the outermost `HarkenEngine.Update()` return from native code, including when the
host has been disabled or destroyed. Custom managed hosts must pump through
`HarkenEngine.Update()` to preserve this completion boundary. The generated
P/Invoke layer is internal and is not a game integration entry point.

Companion startup uses an explicitly selected platform directory. Platform Name
can select a direct child of the configured root; when the root is already the
platform directory, that name must match its basename. The legacy Platform Id
control is hidden because companions have no index inventory. If a platform ID
was previously saved, use **Clear legacy platform selection** in **Project
Settings > Harken > Project**. Code can override it with
`ConfigurePlatformBeforeStartup(null, platformName)` before startup. Keep startup
Pack IDs empty and preload Actions by name.

## Lifecycle

`Auto Initialize` defaults to true. Disable it on an active scene component
when game resources must be prepared first. Awake still claims singleton
ownership and applies the background/persistence settings, but leaves `State`
at `Stopped` without reading Packs or scheduling startup. After Awake, configure
the component and call `StartRuntime(source)` or `StartRuntimeAsync(source)` with
a companion `PackSource`. Failed
requests are returned directly and never retried automatically. Repeated starts
and duplicate components return `VN_ALREADY_INITIALIZED` without another session.

Runtime initialization transitions through `Starting` to `Ready` or `Failed`.
`Ready` means the engine session exists; use `IsPreparationComplete` before
enabling an automatic gameplay trigger. Companion sessions start with Init only
and discover Actions by name. For retained legacy startup-ID sources, a missing
startup Content Pack leaves
the valid session available and exposes `Session.StartupResult` and
`Session.FailedStartupPackStableId`. After supplying the missing Content, call
`Session.PrepareStartupPacks()` explicitly. Existing firing requests are not
replayed. `HarkenPack` and startup preparation share explicit Pack requests, so
releasing one component's request does not unload another owner's request.
Pack, Emitter, Action, and Variable components wait for Ready. During shutdown,
participants first stop owned firings and emitters and cancel pending requests.
`PackBootstrapSession.Shutdown()` terminates the whole native session; it does
not individually unload every Pack. Once termination completes, the host clears
managed ownership and callback registrations. When shutdown starts inside a
native callback, that completion waits until the outermost `HarkenEngine.Update`
returns. Suspend and Resume failures remain
visible through the same result contract. Unity lifecycle callbacks call
`SetSuspended`; hosts may call the same method explicitly when managing their
own application lifecycle.

## Pack filenames and existing publications

Current companion exports use `Init.bytes` with `Init.json` and
`ActionName.bytes` with `ActionName.json`, including any selected language
directory. Keep each pair from the same export; do not rename files or rewrite
their metadata. No separate legacy manifest is required for companion playback.

Legacy Pack manifests, indexes, `.pack` path resolution and `.pack.bytes`
wrappers are unsupported. Re-export old Packs with the current Studio and update
the Integration Package and native libraries together. Unity imports the exact
`.bytes` and `.json` files as TextAssets. The default Init path is `Init.bytes`; explicitly configured
paths must match it. Studio project migration is unaffected.

## Unity Editor and prepared bytes

The existing Action Inspector selection, binding and independent preview consume
current Init/Action companion pairs. `HarkenEditorCatalog.Refresh` supports a
platform directory, PackSource reader or paired TextAssets. `GetDuration` reports
Native exact/bounded/unknown execution time independently of optional VKD data.
`GetSourceWaveforms` collects parent-Pack media; `Scope.GetPlayingWaveform` follows
the actual direct Sound/single-layer Random selection. Resource copying preserves
published relative paths. See the [Editor API reference](Documentation~/EditorApiReference.md)
and [VKD resource contract](Documentation~/EditorWaveformFormat.md).

`HarkenPackSource.CaptureCompanion(initJson, out source, out error, language)`
captures TextAsset references on the main thread. The resulting PackSource also
works with Runtime startup and preparation. Do not decode binary bytes as text.
Catalog preparation uses the existing session/Pack owners and validates recaptured
execution identity. Keep all script/assembly `.meta` files when moving assets.

Legacy manifest readers, symbol generation and `Samples~/PlayableDemo` legacy
installation remain unavailable. Android directory auto-start still requires its
own packaged-reader integration; explicit streaming sources remain supported.

## Remote communication

`Edit > Project Settings > Harken` contains `Enable Communication in Builds`,
enabled by default. It controls all packaged Players, including Development
Builds. Clear it before building to keep communication stopped in that Player.
Editor Play Mode starts communication with its audio session. Editor preview
preparation is temporarily unavailable. The same native library supports communication in
Release builds; no Development Build or separate host process is required.

The page initially creates its project-wide configuration at
`Assets/Harken/Resources/HarkenProjectSettings.asset`. The parent directories
can be changed, for example to `Assets/Audio/Resources/HarkenProjectSettings.asset`;
keep the `Resources/HarkenProjectSettings.asset` resource path and a single copy.
The Editor reuses the existing resource at its current location. Commit this asset
and its `.meta` with the game project. Unity includes it as a Resources asset in the
Player; it is independent of scene and prefab settings and survives Integration
Package updates.
Without a settings asset, communication and discovery use their enabled defaults.
Conflicting resources with the same settings name fail build validation.

Editor-only audio project settings are initially created at
`Assets/Harken/Editor/HarkenEditorProjectSettings.asset` and are found by type.
They can be moved or renamed to match the game project's directory layout,
but must stay outside `Resources` so the audio project directory is not shipped.
Keep a single Editor settings asset; duplicates fail settings lookup and build validation.

The Communication page contains the connection settings.
`Bind Address` is fixed to `127.0.0.1`. TCP connections and UDP discovery are
restricted to this machine; LAN communication is unavailable. The native service
rejects other configured addresses with `VN_INVALID_ARG`; existing settings are
not migrated.
`Remote Port` zero requests an available TCP port, allowing multiple game
processes to run together. `Enable Remote Discovery` defaults to true, and
`Remote Discovery Port` zero selects UDP port `37840`. Studio discovery reports
the actual TCP port; `QueryRemoteCommunication` exposes it for manual connections.
An empty application name uses `Application.productName`. Engine version and
build identity come from the native library.

`HarkenAudioSystem` automatically applies the project policy after audio startup.
It has no per-component communication switch or configuration override.
`RetryRemoteCommunication()` retries a failed startup using project settings;
it cannot enable communication in a Player whose project setting disables it.
`ShutdownRuntime()` closes communication through native engine termination,
including during component lifecycle cleanup.

Call component methods on the Unity main thread. Network changes apply at the
next audio session or failed-start retry; retrying an active service returns
`VN_IN_USE`. Changing the build setting does not disable Editor communication.

A communication startup failure preserves the audio session and its Ready
result. Inspect `LastRemoteResult` and `LastRemoteError` separately, correct the
project configuration, then retry explicitly. `QueryRemoteCommunication(out status)`
returns native listener/session state and its last service result; it does not
clear the component's last operation failure. There is no automatic retry or
per-frame status polling. Existing `Vn_Update` calls pump remote commands.

Custom hosts use `HarkenEngine.StartRemoteCommunication`,
`StopRemoteCommunication`, and `QueryRemoteCommunication` after `HarkenEngine.Init`
or a successful `PackBootstrap.Start`. Remote remains off until explicitly started;
`PackBootstrap` does not opt in. A failed start leaves audio running and requires
an explicit retry. Engine termination stops the listener.
While the Engine is initialized, these calls must run on
the Engine logic thread that owns `Init` and `Update`.
The transport is restricted to the local machine and does not authenticate peers
or encrypt traffic. Disabling discovery does not disable manual local connections
to the listener.

## Managed Spatial Playback

`HarkenAudioSystem.CreateSpatialSource(transform, out source)` creates a
source owned by `HarkenAudioSystem` for one live object instance. Repeated creation
for the same live Transform returns the same source, preserving emitter-local identity.
Pass that source to `PlayAction(name, projectId, source, out firing)`; overlapping
firings keep their own resource ownership. The original source-free overloads
remain available. Native authored 3D and attenuation settings determine the
sound; supplying a source does not force 2D content into 3D.

Call `source.EndFollow()` before recycling or resetting that object. The Unity
integration freezes its last pose and retains its emitter until the existing firings have
ended. Create a new source when the visual object is reused. An ended source
cannot start another Action. Ending follow is not a Stop command: loops still
require explicit firing Stop or runtime shutdown. Native handles and resource
release remain owned by `HarkenAudioSystem`. Sources cannot cross runtime sessions.

Edit Mode spatial preview is temporarily unavailable: both parent and scope
`Prepare` calls return `VN_CAPABILITY_UNAVAILABLE`. Runtime spatial playback
remains available. The retained scope contracts are documented in the
[Editor API reference](Documentation~/EditorApiReference.md).

## Action and Variable Calls

After loading a publication, names and generated GUID symbols address the same objects:

```csharp
Actions.Post("Play_Footstep", emitter, out firing);
Actions.Post(Harken.Generated.HarkenActions.Play_Footstep, emitter, out firing);
Values.SetValue("Speed", 0.5f, emitter);
Values.SetValue(Harken.Generated.HarkenVariables.Speed, 0.5f, emitter);
Values.Resolve("Speed", out VnObjectId speed);
```

Direct `Actions.Post` does not load Packs. Its caller owns the returned root
firing, retains the required content, queries completion, and calls `Release`
after `VN_ACTION_FIRING_TERMINAL_RETAINED`. An accepted `Stop` is asynchronous;
`Release` returns `VN_IN_USE` while the firing is still active. Retain ownership
when cleanup fails. This differs from firings returned by `HarkenAudioSystem`,
`HarkenAction` or Editor preview, whose release remains owned by the component
or preview scope.

`ActionFiring.Seek(positionMs, mode, out ticket)` forwards a source-time seek.
On `VN_OK`, retain the `ActionSeekTicket` and `Query` it: `ACCEPTED` is pending,
`APPLIED` or `CANCELLED` is terminal. `Release` refuses a pending ticket with
`VN_IN_USE`; release the terminal ticket explicitly, including when the firing
itself is owned by a component or preview scope. No automatic retry or release is
added. Relative seek is supported for resident sources; V1 streamed seek is absolute and non-looping.
A firing tree containing any deferred Action step returns
`VN_CAPABILITY_UNAVAILABLE` without partially seeking other targets. V1 requires
already-started, active Sound targets; Container/Queue/nested-service targets
are not supported. A just-posted firing may still be awaiting its audio-start
acknowledgement, so successful Post alone does not establish Seek readiness.

For advanced pinned calls, `ActionReference.ProjectGuid` is the 16 UUID bytes
in canonical text order. Ordinary `Guid.ToByteArray()` uses a different order
for its first fields and must not be copied directly into this member. The
name/project overloads of `Actions.Resolve` handle that conversion internally.

Omitting the Variable emitter writes globally and clears that Variable's emitter
overrides. Explicit invalid emitters are refused. Names are case-sensitive ASCII
identifiers and are resolved from the current Init Pack. After a rename, callers
must update the string or use the object's GUID. An Action publication can also
contain explicit retired-name records, exposed by `GameplayPublication.RetiredActions`;
those names return `VN_GAMEPLAY_NAME_RETIRED`. Variable lookup uses current names.
Name lookup follows Pack replacement and unload without a separate SDK name-table
publication step. Old names are not automatically aliased to current names.

### Generate symbols and use code completion

Symbol generation is temporarily unavailable. The **Harken > Generate SDK
Symbols From Publication** menu is hidden; `GeneratePublication`, `Generate`
and `ValidatePublication` return `VN_CAPABILITY_UNAVAILABLE` without reading or
writing assets. Use authored Action and Variable names with the intended project
ID while this workflow is suspended.

Existing generated GUID fields and the corresponding runtime overloads remain
available when they identify the loaded content. IDE completion for existing
fields still comes from Visual Studio or Rider. It does not discover newly
exported Actions or Variables, and a stale symbol file must not be treated as
proof that the loaded content contains those objects.

Restore generation only after the generator consumes current-schema Pack
companions and its output has been validated against those exports. The retained
Editor declarations and their current results are listed in the
[Editor API reference](Documentation~/EditorApiReference.md).

## Generated ABI

ABI files under `Runtime/Generated` are produced from
`HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json`. Regenerate and
check them with:

```text
py -3 HarkenEngine/Source/Tooling/Binding/gen_binding.py
py -3 HarkenEngine/Source/Tooling/Binding/gen_binding.py --check
```

The binding is IL2CPP/AOT compatible. Main-thread component lifecycle calls do
not change the Runtime audio-thread contract.
