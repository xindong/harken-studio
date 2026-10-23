# Unity API reference: startup, Actions, and Emitters

This reference covers the Harken Unity package's game-facing startup, Action,
and Emitter APIs.
The package namespaces are `Harken.Unity` for Unity components and adapters,
and `Harken` for managed Runtime wrappers and value types. All signatures below
are C# signatures; `VnResult.VN_OK` means the call succeeded. Check every other
result rather than treating a valid handle as proof of success.

The main declarations are in
[`HarkenAudioSystem.cs`](../Runtime/Unity/HarkenAudioSystem.cs),
[`HarkenAction.cs`](../Runtime/Unity/HarkenAction.cs),
[`HarkenEmitter.cs`](../Runtime/Unity/HarkenEmitter.cs), and
[`Actions.cs`](../Runtime/Sdk/Actions.cs). Generated `VnResult` and firing
state definitions are in
[`HarkenTypes.g.cs`](../Runtime/Generated/HarkenTypes.g.cs).

The companion chapters cover [Variables and Labels](VariableApiReference.md),
[Packs](PackApiReference.md), [Voice and buses](PlaybackMixApiReference.md),
[notifications and remote communication](DiagnosticsRemoteApiReference.md),
[Editor tools](EditorApiReference.md), and
[custom hosts and public types](HostAndTypesReference.md). Generated
`HarkenNative` is internal; use the public wrappers and the session owner.

## Minimal scene setup

1. Install `com.harken.core` through Hub. In **Project Settings > Harken >
   Project**, select the exported platform directory containing `Init.bytes`
   and `Init.json`, or its parent with an exact **Platform Name**. Keep every
   `.bytes`/`.json` pair together. For a built desktop Player, include the selected
   export in the Player exports list and deploy it through the project's
   build/content pipeline. The SDK does not copy it. Android uses an explicit
   companion source as described below.
2. Add one `HarkenAudioSystem` to the scene. **Auto Start** is enabled by
   default; in the Editor and desktop Player, its `Awake` reads the directory
   from Project Settings. Add a
   `HarkenListener` to the camera or player Transform for spatial playback.
3. Add a `HarkenEmitter` to an object whose Transform should drive a sound.
   Add `HarkenAction`, assign that emitter, and enter **Action Name** and
   **Project Id** manually. Use the exact authored name and the audio project's
   GUID; a running companion session exposes the validated GUID through
   `Session.ProjectId`. Automatic catalog binding, preview and waveform controls
   are temporarily unavailable. The component follows the runtime selected in its `runtime` field,
   or the singleton `HarkenAudioSystem.Instance` when that field is empty.
4. Wait for `HarkenAudioSystem.IsPreparationComplete` before an explicit
   gameplay trigger. `IsReady` only says that an engine session exists;
   startup Content preparation can still be pending or failed.

Android's default directory Auto Start is temporarily unavailable and returns
`VN_CAPABILITY_UNAVAILABLE` before reading the old index/manifest. Disable Auto
Start before `Awake`, create a source with
`HarkenStreamingPackSource.Create(platformUri, language)`, then await
`StartRuntimeAsync(source)`. Android jar URIs are supported. Alternatively,
configure a companion source before startup with
`ConfigureCompanionSourceBeforeStartup(source)`. See the
[companion source reference](PackApiReference.md) for asynchronous reads and
playback. The PlayableDemo is suspended and is not a current setup example.

For a component trigger, the following script can be placed on any GameObject
and connected to a button or gameplay event:

```csharp
using Harken;
using Harken.Unity;
using UnityEngine;

public sealed class FootstepTrigger : MonoBehaviour
{
    [SerializeField] private HarkenAction action;

    public void Trigger()
    {
        if (action == null) return;
        VnResult result = action.Play();
        if (result != VnResult.VN_OK) Debug.LogWarning(result, this);
    }
}
```

The `HarkenAction` component prepares its Action's Pack closure, posts once,
then owns its firing and resource cleanup. `playOnEnable` is a separate
Inspector option: it attempts one Play after startup preparation and emitter
creation; a refused attempt is not replayed automatically. Invoke `Play()`
again for an explicit retry after fixing the cause.

## Shared conventions and ownership

| Entry point | Resource and handle ownership | Content behavior |
| --- | --- | --- |
| `HarkenAction.Play()` | Component owns the firing, Pack scope, and cleanup. Its `Firing` is for observation and stopping; do not release it. | Prepares the Action's declared Pack closure before posting. |
| `HarkenAudioSystem.PlayAction(...)` | SDK owns the returned firing and releases it after completion. Caller may query or stop, but must not release it. | Synchronously prepares the closure. Preload for a latency-sensitive trigger. |
| `Actions.Post(...)` | Caller owns the returned `ActionFiring`; stop/query/release it and manage any prepared Pack scope. | No loading or automatic preparation. |
| `HarkenEmitter` | Component owns one native emitter and follows its Transform. Do not destroy its `Handle` yourself. | No Action content loading. |
| `Emitter.Create(...)` | Caller owns the native emitter handle and destroys it after its users finish. | No Action content loading. |
| `HarkenSpatialSource` | `HarkenAudioSystem` owns the native emitter; caller ends following when the object identity ends. | A `PlayAction` using the source still prepares Action content. |

Call Unity components and `PackBootstrapSession` methods on the Unity main
thread. The session also requires the thread that created it. Values in
`UnitySpatial` use Unity world space; raw `Emitter` and `Listener` values use
Harken's right-handed coordinates (`+X` right, `+Y` up, `-Z` forward, meters).
A `VnResult` reports the immediate operation. It is not a guarantee that a
firing has finished or that the user heard audio.

## Runtime owner: `Harken.Unity.HarkenAudioSystem`

Only one active component can own a Runtime session. A duplicate is disabled
with `VN_ALREADY_INITIALIZED`. `Awake` claims ownership, and Auto Start starts
from the selected project Pack directory on supported filesystem hosts. Android
requires a configured companion source or explicit startup while its default
directory path is suspended. With Auto Start disabled before
`Awake`, call a startup overload explicitly after `Awake`; failed requests are
not retried by the component. These serialized options are private Inspector
fields, not public C# setters. Adding the component to an active GameObject can
run `Awake` immediately; a subsequent `Configure*BeforeStartup` call cannot undo
that startup. Use a preconfigured scene/prefab for explicit startup.

Player application pause callbacks may arrive before initialization completes.
The component remembers the latest application pause state without reporting an
uninitialized operation, then applies it when the session is created. A session
created while paused stays `Suspended`; its first Ready/BecameReady notifications
wait for a successful resume. Later resumes raise only `BecameReady`. Application
pause callbacks are ignored in the Editor. Explicit `SetSuspended` calls still
require an active session and report native failures normally.

For retained legacy startup-ID sources, missing startup Content
(`VN_CONTENT_NOT_LOADED`) can leave an active session
and raise Ready/BecameReady. Check the startup return value and
`Session?.IsActive` before inspecting `Session.StartupResult` and
`Session.FailedStartupPackStableId`. Other preparation failures can occur before
session creation or shut it down. Recover missing content as described in the
[Pack reference](PackApiReference.md#recovering-missing-content). Merely waiting
for `IsPreparationComplete` does not retry failed requests.

### Inspector and project settings

| Setting | Meaning |
| --- | --- |
| **Auto Start** (`autoInitialize`, default `true`) | Start from configured Pack content during `Awake`. Android's default directory path is suspended; configure a companion source first or disable Auto Start and start explicitly. |
| **Keep Across Scenes** (`persistAcrossScenes`, default `true`) | Apply `DontDestroyOnLoad` to the owner GameObject. |
| **Shut Down When Disabled** (`shutdownWhenDisabled`, default `false`) | End the session when the component or GameObject is disabled. |
| **Ready / Failed / Result Observed** | Inspector-bindable Unity events; Failed and Result Observed pass a string. |
| **GeneratedPacks directory** | The single directory selection used by Auto Start. Editor and Player resolve it according to the rules below. There is no implicit `StreamingAssets/Audio/GeneratedPacks` fallback. |
| **Platform Name** | Select a direct child of the configured root, or match the selected platform directory's basename. The legacy Platform Id control is hidden; when an ID was saved, use **Clear legacy platform selection** on this page before companion directory startup. |
| **Language / Init Pack** | Select the publication language and its matching Init Pack. |
| **Max Simultaneous Playbacks** | Limits SDK-managed `PlayAction` slots as well as native firing capacity. An exhausted managed slot refuses with `VN_NO_ACTION`. |

In the Editor, relative paths start at the Unity project root and the selected
directory must be inside that project. Desktop Players resolve relative paths
from the parent of `Application.dataPath` and also accept external absolute
directories. `Assets/StreamingAssets/...` maps explicitly to
`Application.streamingAssetsPath/...`. Android packaged companions use the
explicit streaming source instead of the suspended directory Auto Start path.
The project team deploys Packs to the configured
location. The SDK installation folder does not determine the Pack location.
The old serialized `packRoot` value does not select Auto Start content.

Project settings are stored in
`Assets/Harken/Resources/HarkenProjectSettings.asset`. The component's
`Configure*BeforeStartup` calls can override the corresponding startup choices
before the next session. See the package README for the full Player-export and
Android packaging contract. `HarkenProjectSettings.Load()` returns the project
asset when present; its relevant read-only properties are `PackDirectory`,
`PlayerExports`, `PlatformId`, `PlatformName`, `Language`, `InitPack`,
`SuppressUnityAudio`, `MaxVoices`, `MaxPacks`, `MaxActions`,
`MaxActionFirings`, `MaxEmitters`, and `EmitterVariableSlots`.
`HarkenProjectSettings` also exposes the default capacity constants. Its
`Set*` methods and `ActiveForStartup` are internal, not game-code setters.

### Status and events

| Member | Meaning |
| --- | --- |
| `static HarkenAudioSystem Instance { get; }` | Current singleton component. It can exist while `State` is `Stopped`. |
| `HarkenRuntimeState State { get; }` | `Stopped`, `Starting`, `Ready`, `Suspended`, `Failed`, or `ShuttingDown`. |
| `bool IsReady { get; }` | True only in `Ready`; does not imply all startup Content is prepared. |
| `bool IsPreparationComplete { get; }` | True when the session is ready, startup Packs are complete, and queued or active load-on-ready `HarkenPack` components are loaded. |
| `PackBootstrapSession Session { get; }` | Current session; null before startup or after complete shutdown. Do not dispose an owner component's session directly. |
| `VnResult LastResult { get; }`, `string LastOperation { get; }` | Last observed component operation and its immediate result. |
| `string LastStartupError { get; }`, `string PackRootPath { get; }` | Startup diagnostic text and selected filesystem platform directory; the path can be empty for prepared-byte startup. |
| `string Language { get; }`, `int PendingPackLoadCount { get; }` | Selected language and queued component Pack loads. |
| `event Action<HarkenAudioSystem> BecameReady` | Raised after session readiness and again after a successful resume. Check `IsPreparationComplete` for gameplay triggers. |
| `event Action<HarkenAudioSystem, string, VnResult> OperationCompleted` | Reports an observed operation name and result. |
| `event Action<HarkenAudioSystem, HarkenRuntimeState, string, VnResult> RuntimeEnded` | Final state, operation, and result after ownership is cleared. Failed native termination retains ownership and does not raise this event. |

`NotificationReceived`, `LastRemoteResult`, `LastRemoteError`,
`RetryRemoteCommunication()`, and `QueryRemoteCommunication(...)` concern
remote/notification integration; see the
[diagnostics reference](DiagnosticsRemoteApiReference.md).
`HarkenRuntimeResultEvent` is the serializable `UnityEvent<string>` type for
the Inspector's Failed and Result Observed callbacks.

### Startup and lifecycle calls

| Signature | Parameters, timing, and result |
| --- | --- |
| `VnResult ConfigureLifecycleBeforeStartup(bool suppressAudio, bool stopWhenDisabled)` | Before startup: optionally pause/mute Unity audio while Runtime owns the session, and end the session on disable. Both default to false. Returns `VN_INVALID_ARG` during a session or transition. |
| `VnResult ConfigureBeforeStartup(string relativePackRoot, string relativeInitPack, ulong[] startupPacks, bool useNullDevice, bool keepAcrossScenes)` | Before startup: validate and store relative Pack/Init paths, startup Pack IDs, Null device, and scene persistence. `startupPacks` cannot be null; invalid input or an active/starting session returns `VN_INVALID_ARG`. The stored `relativePackRoot` does not select Auto Start content: Project Settings supplies that directory, or pass an explicit root to `StartRuntime(string)`. Scene persistence is applied in `Awake`, so changing `keepAcrossScenes` after `Awake` does not reapply `DontDestroyOnLoad` to that GameObject. |
| `VnResult ConfigurePlatformBeforeStartup(string platformId, string platformName = null)` | Before startup: pass either a legacy platform instance ID or exact directory name, not both. For companion directories, pass a null/empty ID and an optional exact name. Invalid selection format or an active/starting session returns `VN_INVALID_ARG`. |
| `VnResult ConfigureLanguageBeforeStartup(string selectedLanguage)` | Before startup: select a language key (empty means publication default). Invalid path-like input or an active/starting session returns `VN_INVALID_ARG`. |
| `VnResult ConfigureCompanionSourceBeforeStartup(PackSource source)` | Configure an explicit companion source before startup; Auto Start can use it on Android as well as filesystem hosts. |
| `VnResult StartRuntime(PackSource source)` | Start from a synchronous companion source and initialize its Init pair. |
| `Task<VnResult> StartRuntimeAsync(PackSource source, CancellationToken cancellation = default)` | Await companion Init readiness, including asynchronous packaged/HTTP reads. Start on the Unity main thread. |
| `VnResult ConfigurePreparedSourceBeforeStartup(HarkenPackSource source)` | Retained legacy manifest source configuration. For current TextAssets use `CaptureCompanion` and a `PackSource` startup overload. |
| `VnResult ConfigurePreparedBytesBeforeStartup(HarkenPreparedPacks source)` | Retained legacy prepared-byte configuration. Null or an existing/starting session returns `VN_INVALID_ARG`. |
| `VnResult StartRuntime(string containmentRoot)` | Start from a selected companion platform directory, or its parent with an exact configured platform name. Legacy indexed roots remain accepted by the retained reader. A previously configured legacy prepared source/bytes takes precedence. `null` is suitable only when such a source is configured. |
| `VnResult StartRuntime(HarkenPreparedPacks source)` | Retained legacy prepared-byte startup; source must be non-null and match configured language/platform. |
| `VnResult StartRuntime(string manifestPath, Func<string, byte[]> readBytes, string selectedLanguage = null)` | Retained legacy manifest-reader startup. Current companion readers use `PackSource` startup overloads. |
| `VnResult SetSuspended(bool suspended)` | Suspend or resume the active session. Without one returns `VN_NOT_INITIALIZED`; resume success moves state to `Ready` and raises `BecameReady`. |
| `VnResult ShutdownRuntime()` | Stop owned firings and participants, release Packs and emitters, then terminate the Runtime. Check its result. A failed native termination retains the session for cleanup. |
| `VnResult SetMasterVolume(float volume)` | Set the master bus output volume from a finite linear value clamped to `[0, 1]`; invalid values return `VN_INVALID_ARG`, no ready session returns `VN_NOT_INITIALIZED`. |

`static string CurrentPackBasePlatform()` reports the host's base platform
name. `RegisterParticipant(IHarkenRuntimeParticipant)` and
`UnregisterParticipant(IHarkenRuntimeParticipant)` are for custom session-aware
components. Such a participant supplies `int ShutdownOrder` and
`void BeforeRuntimeShutdown()`; a larger order runs first. Built-in Action,
Emitter, and Pack components register themselves. `ObserveResult(string,
VnResult)` publishes an operation result for this owner.

### Prepared content and preload

For a directory of companion pairs, `PackBootstrap.StartDirectory(string
platformDirectory, string language, ref VnConfig config, out
PackBootstrapSession session)` starts with `Init.bytes` and `Init.json` only.
`HarkenAudioSystem.StartRuntime(string)` also accepts this selected platform
directory. Set an explicitly configured Init path to `Init.bytes` and leave
startup Pack IDs empty. Clear a legacy platform ID: companion directories have
no platform inventory to verify it. An explicit platform name must match the
directory basename, or select that direct child of the supplied root.
`Session.ProjectId` is the actual Init identity;
`Session.Publication` is null for this source.

`Session.PrepareActions` reads only the requested Action pairs on first use.
Embedded Actions need no additional files. Loaded requests share their owner
and do not read the directory again. A selected language falls back to the
platform root only when both language files are missing. A partial pair or
invalid input fails preparation without changing the active set.
`HarkenAction.Play()` and `HarkenAudioSystem.PlayAction(...)` prepare before
resolving the name. Low-level `Actions.Resolve/Post` never perform file I/O.

`Session.RefreshAction(string name, string projectId, out ActionPreparation
preparation)` explicitly captures a previously discovered Action again,
including a renamed or unloaded Action, and returns one resource lease.
Release it as with a normal preparation. Existing scopes can still be released
after a rename or language variant replacement. Failed refresh preserves the
active content. Acquire-by-ID can retain or reload a previously discovered
owner using its accepted name; changed physical IDs are rejected. Unknown IDs
have no filename mapping in this source, so discover new content by name.

For Unity TextAssets, configure `HarkenPackSource`'s Init Pack, Content and
Language fields, then call `CaptureCompanion(initJson, out source, out error,
language)` on the Unity main thread. Supply the paired Init JSON TextAsset
explicitly and start the resulting `PackSource`. The old inventory and selected
manifest fields are hidden. See [Pack sources](PackApiReference.md) for streaming
and reader-backed alternatives.

The following `HarkenPreparedPacks` details describe retained legacy APIs.
They do not provide current companion discovery or restore suspended Editor tools.

`HarkenPackSource` is a `ScriptableObject` containing the selected publication
and Pack `TextAsset`s. `Capture(out HarkenPreparedPacks prepared, out string
error, string selectedLanguage = null)` and its overload with `out byte[]
manifestBytes` snapshot them on the Unity main thread. `HarkenPreparedPacks`
can also be obtained with `Capture(string manifestPath,
Func<string, byte[]> readBytes, out HarkenPreparedPacks prepared, out byte[]
manifestBytes, out string error, string language = null)` or
`Create(GameplayPublication publication, byte[] init, out HarkenPreparedPacks
prepared, out string error)`. Its `SupplyPack(ulong id, byte[] bytes)` verifies
and snapshots a Content Pack; `SetDemandProvider(PackByteProvider.Handler
provider, object state = null)` supplies missing Content synchronously.
`Start(ulong[] startupIds, ref VnConfig config, out PackBootstrapSession
session)` is the lower-level owner path; use a `HarkenAudioSystem` startup
overload when that component owns the Runtime. `Publication` and
`SelectedManifestPath` expose the captured identity and manifest location.
The source's legacy inventory and selected manifest fields remain serialized
but are hidden from the Inspector. Dynamic legacy code uses the public
`HarkenPreparedPacks.Capture/Create` factories. `SelectedManifestPath` is set by
reader `Capture`; it may be null for `Create` or `HarkenPackSource.Capture`.

`Session.PrepareActions(string[] names, string projectId, out
ActionPreparation preparation)` computes and acquires the declared Pack
closure for exact Action names. It may perform cold I/O. Keep every non-null
scope even after a failed preparation: `ActionPreparation.Release()` must
finish cleanup, and a failed release retains its remaining ownership.
`IsPrepared`, `IsReleased`, and `LastReleaseResult` expose its state;
`Prepare()` reacquires a released scope on its original live session. Use
`Session.PrepareStartupPacks()` to recover startup Packs after supplying missing
Content. `Session.StartupResult`, `FailedStartupPackStableId`,
`IsStartupComplete`, and `IsActive` distinguish those states. `HarkenPack`
offers a scene component for one authored Pack ID: `PackStableId`, `IsLoaded`,
`LoadOnReady`, `Handle`, and `LastResult` report it; `Load()` and `Unload()`
acquire and release that component's request.

The `HarkenPack` Add Component entry and Load On Ready control are hidden while
cold-ID preloading is adapted. In companion sessions, `LoadOnReady` evaluates
to false even for an existing serialized component, so it neither queues a cold
load nor blocks `IsPreparationComplete`. Explicit `Load()` can lease a Pack
after `Session.PrepareActions` has discovered its ID; `Unload()` still releases it.

## Action component: `Harken.Unity.HarkenAction`

The Inspector serializes private `runtime`, `emitter`, `actionName`, `projectId`,
`callerSeed`, `playOnEnable`, and `stopFadeMs` (default 50 ms). Enter Action Name
and Project Id manually while automatic catalog binding is suspended. Names are exact, case-sensitive
authored identifiers matching `[A-Za-z][A-Za-z0-9_]{0,127}`. A valid emitter
component is required for `Play()`.
The component handles one firing at a time and increments its stored
`callerSeed` after each native Post attempt.

| Member | Contract |
| --- | --- |
| `string ActionName { get; }`, `string ProjectId { get; }` | Current serialized Action identity. |
| `ActionFiring Firing { get; }` | Current handle for query/stop. The component owns its release. |
| `bool IsPlaying { get; }` | True while the component retains a valid firing handle; inspect `LastFiringInfo.state` for observed native state. |
| `VnResult LastResult { get; }`, `VnActionFiringInfo LastFiringInfo { get; }` | Last component operation and last successful firing query. |
| `VnResult Play()` | Requires an active enabled component, ready Runtime, created emitter, and no current firing/preparation. Resolves the project-bound name, prepares content, and posts once. Invalid component state returns `VN_INVALID_ARG`; name, content, and native admission errors are returned unchanged. |
| `VnResult Stop()` | Requests stop with serialized `stopFadeMs`; if no firing remains, returns `VN_OK`. Cleanup continues after a terminal query. |

`ShutdownOrder` and `BeforeRuntimeShutdown()` implement
`IHarkenRuntimeParticipant` for owner shutdown; game code need not call them.
Disabling the component transfers outstanding firing and Pack cleanup to the
runtime owner. A `Stop()` success is a stop request, not a terminal state.

## Programmatic Action playback

### Owner-managed global or spatial playback

`HarkenAudioSystem` exposes these overloads:

```csharp
VnResult PlayAction(string actionName, string projectId, out ActionFiring firing);
VnResult PlayAction(string actionName, string projectId,
                    ulong callerSeed, out ActionFiring firing);
VnResult PlayAction(string actionName, string projectId,
                    HarkenSpatialSource source, out ActionFiring firing);
VnResult CreateSpatialSource(Transform target, out HarkenSpatialSource source);
```

`actionName` is the current authored name; `projectId` is its publication
project GUID in `D` format. The source-free overload uses no emitter. The
`callerSeed` overload chooses a deterministic caller seed; the others assign
one from the owner. The spatial overload requires a live source created by the
same owner and session. `PlayAction` requires `IsPreparationComplete`, reserves
one managed firing slot, resolves the name, prepares its content, then posts.
It returns `VN_CONTENT_NOT_LOADED` while startup preparation is incomplete and
`VN_NO_ACTION` when no managed slot is available. Query or stop the returned
firing if needed; **do not call `Release()` on an owner-managed firing**.

```csharp
using Harken;
using Harken.Unity;
using UnityEngine;

public sealed class OneShotPlayer : MonoBehaviour
{
    [SerializeField] private HarkenAudioSystem audioSystem;
    [SerializeField] private string actionName;
    [SerializeField] private string projectId;

    public void Trigger()
    {
        if (audioSystem == null || !audioSystem.IsPreparationComplete) return;
        VnResult result = audioSystem.PlayAction(actionName, projectId,
                                                 out ActionFiring firing);
        if (result != VnResult.VN_OK) Debug.LogWarning(result, this);
        // The SDK owns firing. Keep it only if this caller needs Query or Stop.
    }
}
```

For a moving object, call `CreateSpatialSource(target, out source)` once per
live object identity, use `PlayAction(name, projectId, source, out firing)`,
and call `source.EndFollow()` before pooling or reassigning the object. The
source freezes its last pose and stays alive until its current firings end.
`EndFollow()` does not stop a loop. Repeated creation for the same live
Transform and session returns the same source; an ended source cannot be used
for another Play. `HarkenSpatialSource.IsFollowing` and `LastResult` report its
state. `Dispose()` calls `EndFollow()`.

`source.SetDistanceScale(float scale)` sets a positive finite range multiplier
for that followed identity. It requires the original live session and a source
that is still following; an ended source returns `VN_INVALID_ARG`, and a released
or obsolete source returns `VN_STALE_HANDLE`. A new source starts at 1. The value
is stored by the native emitter, with no serialized Unity copy.

### Low-level `Harken.Actions`

These calls submit directly to the active native session. They do not acquire
Pack content or own the returned firing. Prepare the required content first,
or use the owner-managed API above.

| Signature | Contract |
| --- | --- |
| `VnResult Resolve(string actionName, out ActionResolved resolved)` | Resolve a current exact name without project qualification. `resolved` is default on failure. |
| `VnResult Resolve(string actionName, string projectId, out ActionResolved resolved)` | Resolve with a nonempty project GUID in `D` format; malformed project ID returns `VN_INVALID_ARG`. |
| `VnResult Post(VnObjectId actionId, Emitter emitter, out ActionFiring firing)` | Post by stable ID with caller seed 0. |
| `VnResult Post(VnObjectId actionId, Emitter emitter, ulong callerSeed, out ActionFiring firing)` | Post by stable ID and seed. |
| `VnResult Post(string actionName, Emitter emitter, out ActionFiring firing)` | Post by current exact name with seed 0. |
| `VnResult Post(string actionName, Emitter emitter, ulong callerSeed, out ActionFiring firing)` | Post by current exact name and seed. |
| `VnResult Post(in ActionReference pinned, Emitter emitter, ulong callerSeed, out ActionFiring firing)` | Post a pinned project/name/ID contract. Invalid fields return `VN_INVALID_ARG`. |
| `VnResult Post(in ActionResolved resolved, Emitter emitter, ulong callerSeed, out ActionFiring firing)` | Post a previously resolved identity. A changed publication may return `VN_STALE_HANDLE`; resolve again before a new request. |
| `string Describe(VnResult result, string actionName)` | Format a human-readable diagnostic for a Post result. |

Pass `Emitter.None` to use the engine session's stable default state. Random,
Sequence, supported Queue and other Action content keep their authored state
rules without requiring a game object. Default callers share Emitter-scoped
history, variables and cooldowns; authored Global state remains shared with
explicit emitters. The default has no pose and does not change authored spatial
behavior. Use a caller-owned emitter for independent state or spatial following.
A nonzero stale emitter is refused instead of falling back to the default. `ActionReference` contains `ProjectGuid` (16 bytes), `ActionId`,
`ExpectedName`, `NameBindingRevision`, and `CallableFingerprint`.
`ActionResolved` exposes `ActionId`, `NameBindingRevision`,
`CallableFingerprint`, and `PublicationGeneration`. Resolved values belong to
the publication state in which they were obtained.

`ActionFiring` is a readonly handle struct. Its `Handle` and `IsValid` only
describe handle presence. `Stop(uint fadeMs)` requests a fade in milliseconds;
`Query(out VnActionFiringInfo info)` returns the current state;
`QueryStep(uint authoredEventIndex, out VnActionStepResult result)` returns a
step's result; and `Release()` releases a caller-owned terminal firing.
`VnActionFiringInfo.state` is `VN_ACTION_FIRING_ACTIVE`,
`VN_ACTION_FIRING_STOPPING`, or `VN_ACTION_FIRING_TERMINAL_RETAINED`. For a
direct `Actions.Post`, retain the firing until terminal, release it, then
release its `ActionPreparation`; inspect both release results. A stopped
firing can remain retained until completion. `ActionFiring(ulong handle)`,
`Equals(ActionFiring)`, `Equals(object)`, and `GetHashCode()` provide handle
identity, not ownership transfer.

### Seeking an Action firing

| Signature | Contract |
| --- | --- |
| `VnResult ActionFiring.Seek(double positionMs, VnActionSeekMode mode, out ActionSeekTicket ticket)` | Submit a seek for the whole firing. `VN_OK` admits the request and returns a caller-owned ticket; it does not prove the seek has reached rendering. |
| `VnResult ActionSeekTicket.Query(out VnActionSeekInfoV1 info)` | `VN_OK` means the query succeeded; inspect `info.state` and `(VnResult)info.result` for seek status and outcome. The wrapper initializes `structSize`. |
| `VnResult ActionSeekTicket.Release()` | Release after applied/cancelled. An accepted, unfinished ticket returns `VN_IN_USE`; retain it and query again later. |

`positionMs` is finite source time in milliseconds. Absolute mode requires a
nonnegative position; relative mode can move backwards. Resident playback
supports relative seeks; streamed V1 seeks support absolute, non-looping
playback only. Deferred Action steps or unsupported playable trees reject the
whole operation without partially seeking supported targets. Read the exact
`VnActionSeekMode` and `VnActionSeekState` members in the generated types.
Seek requires an active root firing: a child firing returns `VN_IN_USE`, and
a non-active firing returns `VN_STALE_HANDLE`. Release the previous seek ticket
before submitting another seek on the same firing, even if that ticket is
already applied or cancelled.

`ActionSeekTicket(ulong handle)`, `Handle`, `IsValid`, equality and hashing
describe identity, not ownership transfer. The Seek caller owns its ticket
even when `HarkenAction` or `HarkenAudioSystem` owns the firing. Poll until
applied/cancelled, then release the ticket; do not release that owner's firing.
Keep a failed-release ticket only while its original session remains active.
After confirmed termination of that session, clear its local tickets and never
retry them in a new session. Query/Release can report `VN_NOT_INITIALIZED` with
no engine, or `VN_STALE_HANDLE` after a restart; these are not pending-seek
states. Normal engine updates are required for accepted work and cleanup to
progress while the session is active.

## Emitter and listener APIs

### Unity components

`HarkenEmitter` serializes `runtime` and `stopFadeMs` (default 50 ms). It
creates and registers one native emitter when its Runtime becomes ready. The
Runtime publishes its Transform when changed. The component transfers cleanup
to the Runtime when disabled. A retained emitter keeps emitter-local Variable values;
destroy and recreate it when pooling assigns a new object identity.

| Member | Contract |
| --- | --- |
| `Emitter Handle { get; }`, `bool IsCreated { get; }` | Live native handle and creation state. An unavailable handle is `default`. |
| `VnResult LastResult { get; }` | Last component operation, including transform publication. |
| `VnResult Create()` | Create/register after a live session exists. Pose publication happens at the shared frame boundary or immediately before an SDK Post. Repeated creation of this component's current emitter returns `VN_OK`; no session returns `VN_NOT_INITIALIZED`. |
| `VnResult Stop()` | Stop the emitter's current playback using `stopFadeMs`; no created emitter returns `VN_OK`. |
| `VnResult SetDistanceScale(float scale)` | Set a positive finite range multiplier on this enabled component's current identity. An unavailable/obsolete identity returns `VN_STALE_HANDLE`; an active spatial publication returns `VN_IN_USE`. Destroy/recreate resets the multiplier to 1. |
| `VnResult DestroyEmitter()` | Stop, then destroy the owned emitter. Check the result; failed cleanup retains ownership. |

`ShutdownOrder` and `BeforeRuntimeShutdown()` serve owner cleanup. Do not
destroy the component's `Handle` through the low-level wrapper.

`HarkenListener` serializes an optional `runtime` reference and exposes
`VnResult LastResult { get; }`. Attach it to the camera or player whose pose
should drive spatial audio. Enabled listeners register as candidates. The
first candidate that successfully publishes its pose becomes the selected
listener; later candidates remain registered with `LastResult = VN_IN_USE`
and cannot overwrite the selected pose.

| AudioSystem member | Contract |
| --- | --- |
| `HarkenListener SelectedListener { get; }` | Current listener component, or `null` when none is selected. |
| `VnResult SelectListener(HarkenListener listener)` | Validate and publish an enabled candidate belonging to this session, then select it. Failure preserves the previous selection. Selecting the current owner republishes its pose without creating another identity. |
| `VnResult SetDistanceReference(HarkenEmitter emitter)` | Use this enabled component's current emitter for distance evaluation. The component must belong to this runtime and session. |
| `VnResult SetDistanceReference(HarkenSpatialSource source)` | Use an existing source still following in this runtime and session. This does not create an emitter or keep the source alive. |
| `VnResult ClearDistanceReference()` | Restore distance evaluation from the actual listener position. |

Disabling or destroying the selected listener clears the selection. Existing
candidates do not automatically take over; select one explicitly. With no
selected component, native audio retains its default or last accepted
listener pose. Restarting the Runtime clears session ownership; still-enabled
components register again and retransmit even an unchanged Transform.

```csharp
VnResult selected = runtime.SelectListener(cutsceneListener);
if (selected != VnResult.VN_OK) Debug.LogWarning(selected);
```

### Pose sampling and playback timing

Each AudioSystem owns one internal coordinator with
`DefaultExecutionOrder(+10000)`. Its `LateUpdate` captures the selected
listener and all active component/Transform sources, publishes the captured
values, then performs the normal Runtime update. Place camera and gameplay
Transform writes before this boundary. A script with the same or later order,
a project execution-order override, or a custom PlayerLoop must explicitly
arrange that ordering. Writes after the boundary enter the next submission.
The AudioSystem keeps its early initialization order of `-10000`.

`HarkenAction.Play` and spatial `HarkenAudioSystem.PlayAction` capture and
publish the selected listener and that request's source immediately before
the actual Post, after resource preparation. `PlayActionAsync` waits for
preparation, revalidates the session/source, and captures the current pose at
that point. A cancelled or obsolete request cannot post into a new session.
The existing installation/ACK service can still be required before the first
audible block; these calls do not promise synchronous sound. A later normal
frame submission can legitimately accept a newer pose. Game code does not
need to add an extra `HarkenEngine.Update` or position setter.

Pose publication contains no user callbacks or maintenance pumps. Publication
results are notified after firing/resource ownership is registered, or after
the normal native update returns. Play/Stop/Destroy in those safe callbacks
retain their existing ownership rules; callbacks cannot recursively pump the
Runtime. Invalid listener input preserves the last accepted snapshot. Invalid
source input preserves that source's last accepted pose and reports the result.

Emitter distance scale multiplies the existing authored/Voice range scale.
Distance contributions use physical distance divided by the combined scale;
pan and cone direction still use physical geometry. Set the scale before Play
to affect its initial spatial state. An asynchronous Play uses the current
native scale when it actually posts after preparation. Live changes use the
existing target smoothing. Component and Transform-source scale setters reject
reentry during spatial publication with `VN_IN_USE`, without invoking result
callbacks inside that boundary. Native destruction freezes each attached
playback's last accepted source pose and scale; a new emitter identity starts at 1.

An explicit distance reference changes only distance contributions (volume,
low-pass and high-pass filtering, width and proximity); pan and cone continue to use the actual Listener.
The runtime keeps one selection across same-session Listener changes and clears
it on shutdown. New selections validate their pose before changing the choice.
Selection and clear reject publication reentry with `VN_IN_USE`.

An already-selected source that ends following, or component that disables,
uses its original native identity's last published position while that identity
remains alive. It does not follow a recycled Transform or a new emitter generation.
Actual native destruction falls back to the Listener; selecting an ended source
anew is rejected. Before each Post or frame submission, the runtime validates
the complete native identity before reading the reference Transform. A live
reference with an invalid pose rejects that spatial submission and preserves the
previously accepted spatial state. Async Play captures the current selection
after content preparation. Callbacks that replace the selection cannot be
overwritten by an older publication or cleanup result.

Distance high-pass filtering comes from the attenuation profile in published
content. A disabled curve or a value of 0 Hz contributes no high-pass filtering.
Game code consumes that content through the existing playback APIs; profile
descriptors and curve construction remain internal tooling types. Use the SDK,
native library and published content from a matching delivery. The managed ABI
layout check does not detect native high-pass support. An older native library
rejects the new attenuation descriptor with `VN_INVALID_ARG`, including when
its high-pass curve is disabled; the SDK does not shorten it or discard the curve.

For pooled Transform sources, call `EndFollow()` before reassigning the object
and create a new source for the new identity; existing tails keep the frozen
pose. Raw `Emitter` handles have no Unity Transform association: their caller
must explicitly submit the listener/source pose before low-level `Actions.Post`.

### Raw `Harken.Emitter` and coordinate adapters

| Member | Contract |
| --- | --- |
| `static readonly Emitter None`, `Emitter(ulong handle)`, `ulong Handle`, `bool IsValid` | Empty or native emitter handle. Constructing a handle does not create an emitter. |
| `static VnResult Create(out Emitter emitter)` | Create a caller-owned native emitter; check result before use. |
| `static VnResult CreateStateOnly(out Emitter emitter)` | Create independent state without a pose; uses the same public emitter quota. |
| `VnResult SetPosition(VnVec3 position)` | Set a position in Harken coordinates, meters. StateOnly returns `VN_CAPABILITY_UNAVAILABLE`. |
| `VnResult SetOrientation(VnVec3 forward)` | Set its forward direction in Harken coordinates. |
| `VnResult SetDistanceScale(float scale)` | Set a positive finite range multiplier, default 1. Nonpositive/NaN/infinite values return `VN_INVALID_ARG`, StateOnly returns `VN_CAPABILITY_UNAVAILABLE`, and stale handles return `VN_STALE_HANDLE`; refused writes preserve the previous value. |
| `VnResult Stop(uint fadeMs)` | Request stop for playback on this emitter; fade is in milliseconds. |
| `VnResult Destroy()` | Destroy this caller-owned native emitter after its users finish. |
| `VnResult QueryVariable(HarkenVariableId variable, out VnVariableQueryResultV1 result)` | Query this emitter's value after a typed catalog check; see Variable documentation for that separate feature. |

`Emitter.Equals(Emitter)`, `Equals(object)`, and `GetHashCode()` compare
handles. The wrapper itself holds no managed ownership state.

`Harken.Unity.UnitySpatial.ToHarken(Vector3 value)` converts Unity coordinates
to `VnVec3`. `SetEmitterTransform(Emitter, Transform)` and
`SetEmitterTransform(Emitter, Vector3 position, Vector3 forward)` publish
world-space pose; the latter rejects nonfinite positions or invalid forward
vectors with `VN_INVALID_ARG`. `SetListener(Transform)` and
`SetListener(Vector3 position, Vector3 forward, Vector3 up)` publish a listener
pose. Raw `Harken.Listener.SetDistanceReference(Emitter emitter)` selects a live
spatial emitter; `Emitter.None` clears it. StateOnly returns
`VN_CAPABILITY_UNAVAILABLE`, a stale handle returns `VN_STALE_HANDLE`, and refused
selections preserve the current choice. Raw `Harken.Listener.Set(VnVec3 position, VnVec3 forward, VnVec3 up)`
accepts Harken coordinates. All setter calls return a `VnResult`.

## Common immediate failures

| Result | Practical interpretation for this reference |
| --- | --- |
| `VN_INVALID_ARG` | Invalid argument or component state; for example, an unbound `HarkenAction`, malformed project ID, or invalid spatial pose. |
| `VN_NOT_INITIALIZED` / `VN_ALREADY_INITIALIZED` | No live session, or a second/duplicate owner or startup request. |
| `VN_CONTENT_NOT_LOADED` | Required startup or Action content has not been prepared or supplied. |
| `VN_UNKNOWN_ID` / `VN_GAMEPLAY_NAME_RETIRED` / `VN_GAMEPLAY_WRONG_PROJECT` | Name/ID unavailable in the current publication, renamed, or bound to another project. Check the exported publication and component binding. |
| `VN_STALE_HANDLE` / `VN_PACK_STALE_REVISION` / `VN_PACK_HASH_MISMATCH` | A handle or publication/Pack pairing is stale or mismatched. Refresh from one coherent export. |
| `VN_NO_ACTION` / `VN_NO_EMITTER` / `VN_NO_VOICE` / `VN_BACKPRESSURE` | Different admission/capacity refusals. Preserve the exact code when diagnosing; a refused Post does not replay itself. |
| `VN_IN_USE` | Operation conflicts with current resource ownership or an in-progress mutation. |
| `VN_CAPABILITY_UNAVAILABLE` | A capability is temporarily suspended or unavailable for the selected source, including Android's default directory Auto Start and Editor publication tools. |

For the full result table, inspect the generated `VnResult` enum in
[`HarkenTypes.g.cs`](../Runtime/Generated/HarkenTypes.g.cs) and the
[diagnostics reference](DiagnosticsRemoteApiReference.md). Do not treat
`VN_BACKPRESSURE` as `VN_NO_VOICE`.

Editor preview preparation is temporarily unavailable. The retained scope
contract owns separate StateOnly emitters and reuses them across
Prepare/Play/StopAll in the same session. Disposal drains owned firings before
releasing the scope identity; session teardown invalidates all of its handles.
Ordinary runtime callers continue to pass `Emitter.None`. The existing Studio
preview emitter and variable rehearsal behavior are unchanged.
