# Unity API reference: Packs and publications

This chapter extends the [Unity API reference](UnityApiReference.md).
Pack operations run on the session's logic/game
thread and can perform file I/O, decode metadata, invoke a byte provider, and
allocate managed memory. Prepare content before a timing-sensitive trigger.

## Export identity and selection

For the companion entry (`PackBootstrap.StartDirectory` or `PackSource`), supply
`Init.bytes` / `Init.json` and the requested Action's `.bytes` / `.json` pair.
Both JSON files require top-level numeric `schemaVersion: 4`. Exact integer
spellings such as `4.0` and `4e0` are valid; missing, mistyped, fractional or
unsupported versions return `VN_BAD_PACK`. Re-export incompatible resources
with the matching Studio release. Optional `metadata` and `waveforms` are not
playback dependencies. A bad localized version cannot fall back to General.
A refused Init leaves no session; a refused Action refresh preserves the
accepted names, playback and leases, and can be retried after correcting the
resource. The manifest APIs below retain their existing publication contract.

A legacy manifest publication has a root `HarkenPackIndex.json`, one selected platform
directory, its `HarkenPackManifest.json`, and the Pack files named by that
manifest. If the index is absent, directory selection tries the supplied path
as a platform directory. There is no legacy index fallback; a malformed
`HarkenPackIndex.json` fails selection. Match the
project, platform instance, language, Init Pack, manifest, and Pack bytes from
one export. A path locates bytes; the manifest and Pack identities decide
whether those bytes belong to the publication. `HarkenAudioSystem` Auto Start
uses **Project Settings > Harken > Project > GeneratedPacks directory** and
selects the platform from the root index. Its `IsReady` can be true with
missing startup Content; other startup failures can leave no active session.
Check the startup result and `Session?.IsActive`, then `Session.StartupResult`
and `FailedStartupPackStableId` before enabling gameplay. See the
[Editor/Player path rules](UnityApiReference.md#inspector-and-project-settings).

`PackIndexEntry` has `Id`, `Name`, `BasePlatform`, `Path`, and `IsValid`.
`PackManifestEntry` has `Name`, `PackStableId`, `Path`, `PlatformId`,
`ContentHash`, `Dependencies`, `VariantKey`, and `IsValid`.
`GameplayPublication` exposes the selected `Platform`, `Language`, `ProjectId`,
`InitPackStableId`, `InitContentRevision`, `Digest`, `Actions`, `RetiredActions`,
`Packs`, and `InitPack`. `TryFindAction(name/id, out action)` and
`TryFindPack(id, out pack)` query that snapshot.
`GetActionPreparationOrder(string[] names, string projectId, out ulong[] ids)`
returns dependency-first Pack IDs for exact Action names. It checks project
identity, retired names, and dependency cycles. These `GameplayPublication`
method signatures build and verify the publication/Init pairing:

```csharp
static VnResult Parse(string inventoryJson,
    string selectedJson, string language,
    out GameplayPublication publication, out string error);
static VnResult Read(string platformDirectory,
    string language, out GameplayPublication publication, out string error);
VnResult ValidateInit(byte[] bytes, out string error);
```

`selectedJson` is the selected language manifest when the inventory redirects
to one; otherwise it can be null. `Read` reads that selection from disk, and
refuses an in-progress publication marker with `VN_PACK_STALE_REVISION`.
Check the returned result and error before using the output publication.
`GameplayAction` exposes the authored name, stable
ID, name binding revision, callable fingerprint, owning Pack ID, and provider
closure fingerprint. `RetiredGameplayAction` holds a former name, stable ID,
last binding revision, and reason.

`PackManifest` exposes parsing, platform selection, language manifest
selection, contained-path resolution, and byte/hash verification helpers.
`IndexFileName`, `ManifestFileName`, and `SchemaVersion` are its public
constants. Never infer a Pack's identity from its filename alone.

All following helpers are static and return `bool`; check success before using
the output values. A `language` parameter selects a language key, with null
selecting the inventory default. IDs and names are exact publication values.

| Signature (return type `bool`) | Purpose |
| --- | --- |
| `IsPlatformId(string id)` | Validate a nonempty canonical lowercase GUID in `D` format. |
| `IsPlatformName(string name)` | Validate a platform folder name and its length/character limits. |
| `IsBasePlatform(string value)` | Accept Windows, macOS, Linux, Android or iOS. |
| `TryParseIndex(string json, out PackIndexEntry[] platforms)` | Parse index JSON content. |
| `TryParseManifest(string json, out PackManifestEntry[] packs)` | Parse manifest JSON content. |
| `TryParseManifest(string json, out PackIndexEntry platform, out PackManifestEntry[] packs)` | Also return platform identity. |
| `TrySelectPlatform(PackIndexEntry[] platforms, string platformId, string platformName, string basePlatform, out PackIndexEntry selected, out string error)` | Select by ID or name, or one unambiguous base-platform match. |
| `MatchesPlatform(PackIndexEntry expected, PackIndexEntry actual)` | Compare all platform identity fields. |
| `TryResolvePlatformDirectory(string exportRoot, string platformName, out string platformDirectory)` | Select a directory and verify all selected Pack files. |
| `TryResolvePlatformDirectory(string exportRoot, string platformId, string platformName, string basePlatform, out string platformDirectory, out string error)` | Full selector with default language and file verification. |
| `TryResolvePlatformDirectory(string exportRoot, string platformId, string platformName, string basePlatform, string language, out string platformDirectory, out string error)` | Select a language and verify all selected Pack files. |
| `TryReadPlatformDirectory(string exportRoot, string platformId, string platformName, string basePlatform, string language, out string platformDirectory, out string error)` | Select/read metadata without requiring every Content file. |
| `TryResolvePackPath(string platformDirectory, ulong packStableId, out string packPath)` | Resolve and verify a file using the default language. |
| `TryResolvePackPath(string platformDirectory, ulong packStableId, string language, out string packPath)` | Resolve and verify a file; missing files and hash mismatch both return false. |
| `TryLoadManifest(string platformDirectory, out PackManifestEntry[] packs)` | Read metadata and verify all Pack files. |
| `TryLoadManifest(string platformDirectory, out PackIndexEntry platform, out PackManifestEntry[] packs)` | Also return platform identity. |
| `TryLoadManifest(string platformDirectory, string language, out PackIndexEntry platform, out PackManifestEntry[] packs)` | Select language and verify all files. |
| `TryReadManifest(string platformDirectory, string language, out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage)` | Read selected metadata without verifying all Content files. |
| `TryReadManifest(string platformDirectory, string language, out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage, out ulong initPackStableId)` | Also return the selected Init ID. |
| `TrySelectManifest(string inventoryJson, string selectedJson, string language, out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage, out ulong initPackStableId)` | Select/validate an inventory and language manifest supplied as JSON content. |
| `TryGetSelectedManifestPath(string inventoryJson, string language, out string relativePath)` | Resolve the language manifest path; null means the inventory itself. |
| `VerifyPackBytes(byte[] bytes, PackManifestEntry pack)` | Validate bytes against the manifest entry and content hash. |
| `VerifyPackFile(string platformDirectory, PackManifestEntry pack)` | Validate a contained file and its content hash. |
| `IsSafeRelativePath(string value)` | Reject rooted paths, backslashes, empty or traversal segments and invalid characters. |
| `TryJoinContainedPath(string root, string relativePath, out string resolved)` | Join a safe relative path under a root; does not load or verify file bytes. |

## Unity-managed sources

`HarkenPackSource` is a `ScriptableObject` with an inventory `TextAsset`,
optional selected-language manifest, Init `TextAsset`, and `ContentAsset[]`.
Each `ContentAsset` contains `relativePath` and a Pack `TextAsset`. Capture
them on the Unity main thread before native provider callbacks. Pack `.bytes`
must be supplied as bytes, not decoded as text. Inventory, selected manifest,
language, Init and Content are private serialized Inspector fields. Dynamic
code uses the public `HarkenPreparedPacks.Capture/Create` factories rather than
assigning those fields.

| Signature | Contract |
| --- | --- |
| `VnResult HarkenPackSource.Capture(out HarkenPreparedPacks prepared, out string error, string selectedLanguage = null)` | Snapshot selected publication and available Pack assets. Error text explains failure. |
| `VnResult HarkenPackSource.Capture(out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error, string selectedLanguage = null)` | Same capture with a snapshot of selected manifest bytes. |
| `static VnResult HarkenPreparedPacks.Capture(string manifestPath, Func<string, byte[]> readBytes, out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error, string language = null)` | Read the inventory, selected manifest, and Init through a synchronous logical-path reader. The reader remains the demand source for uncaptured Content. |
| `static VnResult HarkenPreparedPacks.Create(GameplayPublication publication, byte[] init, out HarkenPreparedPacks prepared, out string error)` | Validate and snapshot a caller-supplied publication/Init pair. |
| `VnResult HarkenPreparedPacks.SupplyPack(ulong id, byte[] bytes)` | Verify Content ID and hash, then retain a snapshot. Explicitly supplied bytes win over the demand provider. |
| `VnResult HarkenPreparedPacks.SetDemandProvider(PackByteProvider.Handler provider, object state = null)` | Use a synchronous provider for missing Content; demand bytes are borrowed for that load, not kept as an unbounded cache. |
| `VnResult HarkenPreparedPacks.Start(ulong[] startupIds, ref VnConfig config, out PackBootstrapSession session)` | Start a caller-owned session. Use a `HarkenAudioSystem.StartRuntime(...)` overload instead when that component owns lifecycle. |

`HarkenPreparedPacks.Publication` identifies the captured publication.
`SelectedManifestPath` is set by reader `Capture`; it can be null after `Create`
or `HarkenPackSource.Capture`. `SupplyPack`, `SetDemandProvider` and `Start`
require the thread that created the prepared object.

The reader's `manifestPath` is a safe relative path to a platform inventory
manifest, not the root index. Selected manifests, Init and Content paths are
resolved relative to that inventory's directory. The reader must not append
the language directory again. Capture rereads inventory/selected metadata to
check consistency, so the reader must support repeated reads and remain
available for later Content demand. Missing/null/empty data returns
`VN_CONTENT_NOT_LOADED`; reader exceptions become `VN_PACK_PROVIDER_FAILED`,
invalid UTF-8 becomes `VN_BAD_PACK`, and changed metadata on reread becomes
`VN_PACK_STALE_REVISION`.

For Init/Content Pack payloads, Auto Start's default reader accepts a plain file
or the same path with `.bytes` appended; two copies at once cause provider
failure. Index and manifest metadata still require the normal `.json` filenames
used by directory selection. Explicit
`StartRuntime(string containmentRoot)` uses direct file reads and does not
perform that mapping. Custom readers implement their own storage mapping.

### Recovering missing content

Always inspect the startup result and `session?.IsActive`. When an active
session survives a missing Content result:

1. If the caller retained `HarkenPreparedPacks`, call `SupplyPack` on its creating
   thread, then `Session.PrepareStartupPacks()`. For filesystem/reader startup,
   restore the original file or reader response before preparation.
2. For a failed Action scope, release any retained resources as needed, then
   retry `PrepareActions` or `ActionPreparation.Prepare()`. A scope with partial
   ownership must finish `Release()` before `Prepare()` can reacquire it.
   Explicitly trigger Play/Post after successful preparation; it is not replayed.
3. For a failed `HarkenPack.Load()`, repair the source and call `Load()` again.
   `Update` does not retry that failed request every frame. Such a component can
   keep `IsPreparationComplete` false even after startup Packs recover.

If no session exists, fix the failure and start again. Direct filesystem
bootstrap can retain an active session after a missing startup Content result;
check the returned session before selecting the recovery path.

## Session-scoped ownership

`PackBootstrapSession` owns Init, startup requests, and explicitly acquired
Content requests. Its `InitPack`, cloned `StartupPacks` array, `Inspection`,
`IsActive`, `ContainmentRoot`, `Language`, and `Publication` describe that
session. `StartupResult`, `FailedStartupPackStableId`, and
`IsStartupComplete` report startup preparation separately from engine
initialization. A component-owned `HarkenAudioSystem.Session` must be shut
down through `HarkenAudioSystem.ShutdownRuntime()`, not disposed by callers.

| Signature | Contract |
| --- | --- |
| `VnResult AcquirePack(ulong packStableId, out Pack pack)` | Acquire one request for Content. Repeated requests share a native Pack but have separate ownership counts. Init cannot be acquired through this call. |
| `VnResult ReleasePack(ulong packStableId, Pack pack)` | Release exactly one matching request; an unmatched handle returns `VN_STALE_HANDLE`. Keep ownership if unload fails. |
| `VnResult ReplacePack(ulong packStableId, Pack pack)` | Reload the same stable ID from the current content source and atomically replace the published set. The Pack must belong to this session. |
| `VnResult PrepareActions(string[] names, string projectId, out ActionPreparation preparation)` | Compute and acquire dependency closure for exact Action names. A non-null result scope may retain resources even when preparation failed; release it. |
| `VnResult PrepareStartupPacks()` | Retry only missing startup Pack requests after supplying/fixing Content; update `StartupResult` and failed ID. |
| `VnResult ReleaseStartupPacks()` | Release startup requests in reverse order, returning the first failure while retaining failed ownership. |
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
| `LoadPack(ulong packStableId, string utf8ContainmentRoot, out Pack pack)` | Resolve an ID using the default language, then use the compatibility single-Pack entry. Current canonical Init/Content is refused with `VN_PACK_INIT_LIFECYCLE`. |
| `LoadPack(ulong packStableId, string utf8ContainmentRoot, string language, out Pack pack)` | Same compatibility load with explicit language selection. |
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
All three public `PackBootstrap.Start` overloads return `VnResult`:

```csharp
static VnResult Start(string initPackPath, string containmentRoot,
    ulong[] startupPackStableIds, ref VnConfig config,
    out PackBootstrapSession session);
static VnResult Start(string initPackPath, string containmentRoot,
    ulong[] startupPackStableIds, string language, ref VnConfig config,
    out PackBootstrapSession session);
static VnResult Start(GameplayPublication publication, byte[] initBytes,
    ulong[] startupPackStableIds, PackByteProvider.Handler contentProvider,
    object providerState, ref VnConfig config, out PackBootstrapSession session);
```

`containmentRoot` is the selected platform directory; `initPackPath` must match
the selected manifest's Init path. `startupPackStableIds` cannot be null and
contains requested Content IDs. The provider overload requires a valid
publication, matching Init bytes and non-null synchronous Content provider;
`providerState` is passed back to that provider. `config` receives recommended
unset capacities. Check both result and output session: `VN_OK` means this
startup request completed, while `VN_CONTENT_NOT_LOADED` can retain an active
session needing explicit recovery. Direct Pack set mutation is for hosts that own the whole
session and publication revision; it is not a second loading path to mix
with `HarkenAudioSystem` component requests.

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

`VN_CONTENT_NOT_LOADED` means a required file/byte array is absent;
`VN_PACK_HASH_MISMATCH` means the supplied bytes do not match the manifest;
`VN_PACK_STALE_REVISION` means publication/manifest revision pairing failed;
`VN_PACK_PATH_ESCAPE` rejects content outside the selected root;
`VN_IN_USE` reports a concurrent mutation or ownership conflict. Preserve
the failed `ActionPreparation`, `Pack`, or session object until its actual
cleanup succeeds. `HarkenAudioSystem.IsReady` alone does not imply complete
startup Content or scene Pack preparation; use `IsPreparationComplete` and
the individual failure results. A `VN_OK` startup return does mean that specific
startup Pack request completed successfully.
# Companion sources and asynchronous preparation

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
platform, enumerate content or download the whole project. The legacy Android
index-based auto-start remains a separate entry; wire a new companion URI at
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
