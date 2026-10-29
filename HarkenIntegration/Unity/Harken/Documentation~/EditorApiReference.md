# Unity Editor catalog, duration and preview

`Harken.Unity.Editor` consumes current Studio `Init.json`/`Init.bytes` and
Action `.json`/`.bytes` pairs. It does not require ProjectData, metadata, a
legacy manifest or symbol generation. Use these APIs on the Unity main thread.

## Catalog and existing selection controls

```csharp
var catalog = new HarkenEditorCatalog();
VnResult result = catalog.Refresh(platformDirectory, language);
result = catalog.GetDuration(actionName, out HarkenActionDuration duration, out string diagnostic);
```

`Refresh(PackSource, actionNames[, HarkenEditorResources])` supports callbacks;
`Refresh(HarkenPackSource, language, initJson[, HarkenEditorResources])` supports
Unity TextAssets. Reader callers supply candidate names; Init contains no Action
inventory. Catalog validates actual pairs sequentially and keeps descriptors,
JSON and digests instead of all media bytes. Missing/invalid candidates fail the
refresh and clear published names, duration and waveform validity. An Init-only
catalog is valid. `GetPack` returns actual paths, bytes, stable ID and content hash.

The existing Inspector source, Action selection, project binding and preview
controls are restored. The TextAsset companion field is **Init JSON**.
`CheckReference` validates name/project; `SelectAction` records Undo and updates
the existing serialized reference. `GetPreparationOrder` returns the parent
Pack ID; that Pack already owns embedded Action dependencies.

`Publication` is a compatibility projection of inspected facts. Callable/name
binding revisions unavailable before installation are zero; resolve Actions
through the running session. `PreparedPacks.Start(startupIds, ...)` and the first
`session.AcquirePack(id, ...)` use the validated name locator and existing owner
ledger. Preparation recaptures each pair and refuses changed execution bytes or
required names with `VN_PACK_STALE_REVISION`.

## Duration without waveform data

`GetDuration` is independent of VKD and returns `Unknown`, `Exact` or `Bounded`,
with Action ID, content hash, catalog revision and an explanatory reason. Only
Native inspection duration flags are authoritative. Never substitute source
trim duration. `ExactSeconds` is null for unknown/range results.

For SAGA2 editor caches, key by source, Action and catalog revision; clear
cached automatic duration after refresh failure, unknown/range or Action change.
Preserve user-authored values under the game's existing edit rules. An Editor API
unknown result does not imply every timeline supports manual length input.

## Independent Editor playback

```csharp
using (var preview = new HarkenEditorPreview())
using (var scope = preview.CreateScope())
{
    scope.Prepare(catalog.PreparedPacks, new[] { actionName });
    scope.Play(actionName, out ActionFiring firing);
    // Editor update pumps the session; Tick is also available explicitly.
    scope.GetPlayingWaveform(firing, catalog, out var waveform, out var diagnostic);
    scope.Stop(firing, 0);
}
```

Check each result before using outputs. Preview defaults to the real device and
works outside Play Mode. Multiple scopes share the engine owner and hold their
own preparations/firings. Dispose scopes; do not separately dispose `Session`.
Stop, natural retirement, shutdown, assembly reload and Play Mode transitions
release the work owned by the preview scopes. A source replacement needs
controlled shutdown and explicit preparation. Display-only refresh preserves execution/session leases
while invalidating old playing-waveform associations.

`GetPlayingWaveform` uses the actual Native started/retired stream, correlated
with the firing, project, root, selected ordinal, occurrence fingerprint and
leased media checksum. Direct Sound and one-layer Random Sound are supported.
Loss, incomplete identity, unsupported paths or ambiguous/multiple voices return
unavailable; the first Random candidate is never a playback substitute. Native
Monitor V3 access is generated and internal to the managed API layer; V1/V2
consumers retain their existing ABI and default negotiation.

## Optional waveform resources

See [waveform resources](EditorWaveformFormat.md) for resource-root setup,
copying and source envelopes. `GetSourceWaveforms` collects all parent-Pack media,
including embedded Actions; it is not a rendered mix. `GetActionPreview` returns
supported candidates, and `GetActionWaveform` only returns an unambiguous direct
Sound. `GetWaveform` keeps independent exact execution duration even when optional
waveform data is unavailable.

## Still-retired tooling

Legacy manifest `Capture/Create/Refresh` overloads remain rejected. The standalone
`HarkenWaveformPublication.Parse`, variable/symbol generation and sample's legacy
publication installation remain unavailable. Existing generated names may still
serve as ID aliases. Plugin importers, Project Settings and runtime components
retain their existing responsibilities.
