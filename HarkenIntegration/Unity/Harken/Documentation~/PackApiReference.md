# Unity API reference: Packs and publications

This chapter extends the [Unity API reference](UnityApiReference.md).
Pack operations run on the session's logic/game thread and can perform I/O and
allocate memory. Prepare content before a timing-sensitive trigger.

## Export identity and selection

Supply `Init.bytes` / `Init.json` and the requested Action's `.bytes` / `.json`
pair from the same export. JSON `schemaVersion` must match its binary header;
the native runtime validates support against the current Pack schema manifest.
Optional `metadata` and `waveforms` are not playback dependencies.

Select a platform directory explicitly, or select its direct child by exact
platform name. There is no platform index, legacy manifest, `.pack` path
resolution, or `.pack.bytes` wrapper fallback. Re-export old Packs with the
current Studio and update the Integration Package and native libraries together.
Pack migration is unsupported; Studio project migration remains a separate authoring workflow.

`PackManifest` has been removed. `GameplayPublication` and `HarkenPreparedPacks`
remain only as signature dependencies for suspended Editor APIs. Their capture,
read and validation entry points reject legacy publications with
`VN_PACK_INCOMPATIBLE`; they do not read files or invoke supplied readers.
Use `HarkenPackSource.CaptureCompanion` for TextAssets.

## Companion sources and asynchronous preparation

`PackSource` selects one platform source containing `Init.bytes` / `Init.json`
and `<Action>.bytes` / `<Action>.json`. It does not require an index, manifest,
or `GameplayPublication`. Startup reads Init only; preparing an Action reads
only its pair. Its transitive executable closure is already in that Pack.

| Source | Entry point |
| --- | --- |
| Filesystem directory | `PackSource.FromDirectory(platformDirectory, language)` |
| Synchronous custom reader | `PackSource.FromReader(readBytes, language, revision)` |
| Asynchronous custom reader | `PackSource.FromAsyncReader(readBytesAsync, language, revision)` |
| Unity TextAssets | `HarkenPackSource.CaptureCompanion(initJson, out source, out error, language)` |
| Packaged or HTTP URI | `HarkenStreamingPackSource.Create(platformRoot, language)` |

Reader paths are relative, use `/`, and name exactly the requested files.
Return `null` only for Missing; throw for read errors. A localized Action falls
back to General only when both localized files are Missing. A half pair, read
error, invalid JSON or mismatched Pack fails preparation without publication.
Reader arrays are copied when returned; keep each array stable until then.

Without `revision`, the source must remain immutable for its whole lifetime.
A mutable source supplies a monotonic, never-reused `Func<ulong>`: advance it
**before** changing bytes, throw while an update is in progress, and make the
new value readable only after the complete publication is available. The SDK
checks before and after capture and again before committing. This callback is
not a file watcher or an automatic refresh service. Use `RefreshAction` /
`RefreshActionAsync` for an intentional change to a discovered Action.

TextAssets freeze references from the existing `initPack` and `content` fields;
include both Action files in `content` with their relative paths. `initJson` is
passed explicitly. Content bytes are accessed only when requested. Keep these
assets (and their AssetBundle) alive while the source can read them. Destroyed
assets are errors, not Missing. Capture and reads occur on the Unity thread.

The streaming adapter takes an explicitly selected platform URI, including an
Android `jar:file://...!/assets/...` URI. Filesystem paths and `file://` URIs
reuse the directory reader. URI reads use UnityWebRequest: HTTP 404 is Missing;
network, permission and other HTTP failures are errors. It does not choose a
platform, enumerate content or download the whole project. Android directory auto-start remains suspended; configure an explicit companion URI at
the project's resource bootstrap.

```csharp
PackSource source = HarkenStreamingPackSource.Create(platformUri, language);
VnResult started = await runtime.StartRuntimeAsync(source, cancellation);
if (started != VnResult.VN_OK) return;

ActionPreparationResult ready = await runtime.Session.PrepareActionsAsync(
    new[] { "Play_Footstep" }, runtime.Session.ProjectId, cancellation);
if (ready.Result != VnResult.VN_OK) return;
// Existing synchronous PlayAction / HarkenAction calls can now reuse this resource.
runtime.PlayAction("Play_Footstep", runtime.Session.ProjectId, out ActionFiring firing);
// Retain the preparation while wanted; retry Release if unloading is refused.
VnResult released = ready.Preparation.Release();
```

For a single cold play use `await runtime.PlayActionAsync(name, projectId,
cancellation)` (or the overload with a spatial source). Its
`HarkenActionPlayResult` contains `Result` and `Firing`; the same managed firing
ownership as `PlayAction` applies. The task completes after preparation and Post
acceptance, not after audible completion. The existing synchronous methods
remain available; a cold async-only source returns `VN_CAPABILITY_UNAVAILABLE`
there. Preloading once allows existing game playback calls to stay unchanged.

`PackBootstrap.Start(source, ref config, out session)` serves synchronous hosts;
`StartAsync(source, config, cancellation)` returns `PackStartupResult` with
`Result` and `Session`. `ActionPreparation.PrepareAsync` retries an existing
scope. Each successful preparation owns one lease, even when requests share
one in-flight read. Cancelling one waiter does not cancel the others; the last
waiter cancels the provider. Failure is not permanently cached. Cancellation
before commit throws `OperationCanceledException`; a successful commit always
returns its lease, even if cancellation races with its completion.

Call async APIs on the Unity main thread or a custom host's owner thread with
a SynchronizationContext that returns continuations to that thread. A missing
or default context is refused for cold async work. Native mutations never move
to the thread pool. Shutdown invalidates pending reads only after it succeeds;
component Disable also cancels a pending startup. Late completions cannot
initialize or publish into a later session. Configure auto-start with
`ConfigureCompanionSourceBeforeStartup(source)` before the component's Awake.

SDK requests finish cancellation even when a reader ignores its token. Such a
reader still owns its underlying I/O and should cooperate with cancellation.
A reader and its asynchronous continuations cannot prepare on the same session
(`VN_IN_USE`); unrelated callers can still share the pending read.

## Session-scoped ownership

`PackBootstrapSession` owns Init and explicitly acquired Action content requests. Its `InitPack`, cloned `StartupPacks` array, `Inspection`,
`IsActive`, `ContainmentRoot`, `Language`, and `ProjectId` describe that
session. `Publication` is null and `StartupPacks` is empty for companion sources. `StartupResult`, `FailedStartupPackStableId`, and
`IsStartupComplete` report startup preparation separately from engine
initialization. A component-owned `HarkenAudioSystem.Session` must be shut
down through `HarkenAudioSystem.ShutdownRuntime()`, not disposed by callers.

| Signature | Contract |
| --- | --- |
| `VnResult AcquirePack(ulong packStableId, out Pack pack)` | Acquire one request for previously discovered Content; discover Actions by name with `PrepareActions`. Repeated requests share a native Pack but have separate ownership counts. Init cannot be acquired through this call. |
| `VnResult ReleasePack(ulong packStableId, Pack pack)` | Release exactly one matching request; an unmatched handle returns `VN_STALE_HANDLE`. Keep ownership if unload fails. |
| `VnResult ReplacePack(ulong packStableId, Pack pack)` | Reload the same stable ID from the current content source and atomically replace the published set. The Pack must belong to this session. |
| `VnResult PrepareActions(string[] names, string projectId, out ActionPreparation preparation)` | Acquire the executable Pack closure for exact Action names. A non-null result scope may retain resources even when preparation failed; release it. |
| `VnResult PrepareStartupPacks()` | Return the Init-only startup state; use `PrepareActions` to acquire content. |
| `VnResult ReleaseStartupPacks()` | Release startup requests; companion startup has no Content requests. |
| `VnResult Shutdown()`, `void Dispose()` | End a caller-owned session. Failed native termination retains ownership; `Dispose` invokes `Shutdown` only while active. |

`ActionPreparation` exposes `IsPrepared`, `IsReleased`,
`LastReleaseResult`, `Prepare()`, and `Release()`. Its original dependency
plan can be reused on the same live session after release. `Release()`
unwinds the closure in reverse order; if an unload fails, retain the scope
and finish cleanup later. Keep any non-null scope returned from a failed
`PrepareActions` attempt. A successful direct `Actions.Post` keeps the scope
until its caller-owned firing reaches terminal state and is released. The
component and `HarkenAudioSystem.PlayAction` paths manage these scopes for
their callers.

`Harken.Unity.HarkenPack` provides one authored Pack request in a scene. Its
private serialized fields are `runtime`, `packStableId`, and `loadOnReady` (default
true). `PackStableId`, `Handle`, `IsLoaded`, `LoadOnReady`, and `LastResult`
are read-only; `Load()` and `Unload()` acquire/release this component's
request. Disabling the component releases it, transferring failed cleanup to
the Runtime owner. `ShutdownOrder` and `BeforeRuntimeShutdown()` are its
participant hooks.


## Lower-level Pack API

`Pack` is a readonly native handle (`Handle`, `IsValid`, `Unload()`). Calling
`Unload()` directly is for a caller-owned raw Pack, not a handle owned by a
session or component. `Packs.ChangeVersion` is an SDK cache invalidation
stamp; it is not a readiness or publication identity check.

Current canonical Init/Content exports must be activated as a complete Pack
set. Use `PackBootstrap.Start` and session `AcquirePack`, or
`ActivateCanonicalPackSet` when the custom host owns the entire publication.
The single-Pack compatibility load methods below do not provide that lifecycle.

| `Harken.Packs` method | Purpose |
| --- | --- |
| `InspectInitPack(string utf8Path, out InitPackInspection inspection)` | Inspect Init requirements without loading it. The report includes required capability and recommended capacities. |
| `LoadInitPack(string utf8Path, string utf8ContainmentRoot, out Pack pack)` | Compatibility single-Pack entry; current canonical Init is refused with `VN_PACK_INIT_LIFECYCLE`. Use bootstrap/set activation. |
| `LoadPack(string utf8Path, out Pack pack)` | Legacy path-based load entry, not a startup path for current canonical exports. |
| `UnloadPack(Pack pack)` | Unload a caller-owned raw handle. |
| `ActivateCanonicalPackSet(CanonicalPackMember[] members, ulong setRevision, out VnCanonicalPackSetActivationReportV1 report, out VnCanonicalPackSetMemberResultV1[] results, Pack expectedCurrentInit = default, ulong baseSetRevision = 0)` | Atomically activate one ordered set; Init first, Content members sorted by stable ID. Check report and row results. |
| `UnloadCanonicalPackSet(Pack initPack, ulong baseSetRevision, ulong unloadRevision)` | Unload a canonical set with revision guards. |
| `ReplacePack(Pack pack, string utf8Path, string utf8ContainmentRoot)` | This raw compatibility entry currently refuses valid calls with `VN_PACK_INIT_LIFECYCLE`; use session `ReplacePack` for a live session. |

`CanonicalPackMember` supplies `StableId`, `Kind`, `Expected`, `Path`,
`ContainmentRoot`, `Provider`, and `ProviderState` to atomic activation.
`PackBootstrap.ApplyRecommendations(ref VnConfig config, in
InitPackInspection inspection, int startupPackCount)` fills inspected
capacity recommendations only where supported fields are unset. It rejects
explicitly insufficient capacities; it does not silently enlarge every limit.
The path-based `PackBootstrap.Start` overloads require `initPackPath` to be the
selected directory's `Init.bytes` and `startupPackStableIds` to be empty. They
delegate to companion directory startup. Prefer `StartDirectory` or `Start(PackSource, ...)`.
The `GameplayPublication` startup overload and manifest ID-to-path load overloads
have been removed. Callers must re-export and prepare Actions by name.

`PackByteProvider.Handler(ulong packStableId, string relativeLocatorUtf8,
uint variantKey, ulong expectedContentHash, out byte[] payload, object state)`
returns Pack bytes synchronously.
`PackByteProvider.LoadPack(ulong packStableId, Handler handler, object state,
out Pack pack)` uses the same single-Pack compatibility path and refuses
current canonical Init/Content with `VN_PACK_INIT_LIFECYCLE`.
`PackByteProvider.InspectInitPack(ulong packStableId, Handler handler, object
state, out InitPackInspection inspection)` inspects bytes without loading a
session and remains usable for current Init Packs. The adapter pins
bytes until the native release callback. Provider pin/call counters are
internal diagnostics, not public lifetime controls. Do not throw from a provider or
retain a native pointer to its temporary byte span. The SDK translates
provider failures into `VnResult` values.

## Common failures

`VN_CONTENT_NOT_LOADED` means a required pair is absent. `VN_BAD_PACK` rejects
incomplete or inconsistent pairs. `VN_PACK_INCOMPATIBLE` rejects unsupported
formats; re-export with the matching Studio. `VN_PACK_STALE_REVISION` rejects a
source changed during capture. `VN_IN_USE` reports a concurrent mutation or
ownership conflict. Preserve failed preparation scopes and session ownership
until cleanup succeeds; explicitly retry preparation after repairing the source.
