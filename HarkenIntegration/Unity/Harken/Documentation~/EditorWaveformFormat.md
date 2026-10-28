# Published Editor waveform resources

Current Action JSON may include optional `waveforms` rows:

```json
{"soundGuid":"<Sound GUID>","file":"../Waveforms/<sha256>.vkd","trimStartFrame":0,"trimEndFrame":44100}
```

SDK playback and duration do not require this data. `metadata` and ProjectData
are not prerequisites. Legacy standalone waveform publications remain rejected.

## Paths and copying

Each file reference is relative to its owning Action JSON. Declare the presentation
root separately from executable Pack paths. Legal `../` stays within that root;
absolute paths, escapes and filesystem reparse redirects are refused.
Directory catalog refresh uses the parent of the selected platform directory:

```text
ExportRoot/
  Windows/Init.json + Init.bytes
  Windows/Play_Action.json + Play_Action.bytes
  Waveforms/<sha256>.vkd
```

For other layouts pass `HarkenEditorResources.FromDirectory(resourceRoot,
platformDirectory)` to the PackSource refresh overload. `FromReader(platformPath,
reader)` receives normalized root-relative paths; `FromAssets(platformPath, assets)`
uses the same names and reads binary TextAsset bytes on the Unity thread. Import
VKD as `.bytes` where Unity requires it, retaining the published `.vkd` name as the
asset mapping's `relativePath`. Never decode binary Packs/VKD as text.

`catalog.CopyResources(destinationRoot, actionNames, out copiedPaths, out diagnostic)`
recaptures validated Init/selected Action pairs and copies their referenced VKDs,
preserving paths from the declared presentation root. It does not delete unrelated
or shared resources. On failure, `copiedPaths` identifies completed writes; retry
after correcting the source. Missing optional VKDs can fail this copy/display
operation without invalidating executable playback.

## Envelope and duration semantics

The decoder accepts VKD v2, validates SHA256 filename, section bounds/overlap,
WAVE dimensions/rate and finite ordered extrema. SHA256 identifies the VKD itself;
it is not the converted media checksum. Trim uses original source frames, the
source sample rate, samples-per-peak and actual last-bucket length. Read boundaries
with `HarkenSourceEnvelope.GetBucketFrameBoundary`; a partial trim bucket retains
the source bucket's extrema. Native occurrence gain is applied once without
clamping or normalization. Envelopes are source displays, not final DSP output.

`GetSourceWaveforms` returns one row per parent-Pack media Sound, including
embedded Action media and per-row optional-data diagnostics. Candidate views
carry their fixed gain. Only `Scope.GetPlayingWaveform` associates a Random view
with the actual started choice. Source duration and Native execution duration
are separate; unknown/range execution time never falls back to source duration.

Base and derived caches have bounded count/byte budgets. Refresh clears display
caches and failed reads, enabling missing/corrupt resources to be restored.
Application-held returned envelopes remain the caller's responsibility.
