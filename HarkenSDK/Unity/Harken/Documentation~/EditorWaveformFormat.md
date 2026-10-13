# Editor Waveform Companion v2

`HarkenEditorWaveforms.json` is optional Editor metadata for one selected playable
publication. It describes final exported-source PCM, not processed playback or
the candidate selected by a Random container. Runtime Pack bytes, loading and
playback do not depend on it. Each selected language manifest has its own
companion, including common dependency Pack media.

The offline `harken-editor-metadata` tool accepts `--manifest`, `--pack-root`,
optional `--pack-suffix .bytes`, and `--output`. It reads the existing publication
without initializing audio, rendering a graph or recompiling a world. Canonical
export uses the same producer inside its complete publication transaction.
Runtime content bundles preserve companions. Suffixes do not change identity.

## Identity and compatibility

New exports use `schemaVersion: 2` and
`envelopeKind: "exported-source-pcm-minmax"`. The SDK reads v1 and v2. Install the
producer and SDK together: an older v1-only SDK rejects v2 metadata. The new SDK
reports an explicit unsupported-version diagnostic for future versions.
Metadata failure does not block playback.

`manifestByteLength` and decimal-string `manifestFnv1a64` bind the exact raw
selected manifest. The header binds project, platform, base platform, language,
Init stable ID/hash and gameplay catalog digest. Every active Action appears
once with its name, stable ID, name binding revision, callable fingerprint, owner
Pack ID and provider closure fingerprint. Callable fingerprints identify name
bindings, not Action program semantics. The producer separately verifies program
identity and final PCM content revision against canonical Pack data.

64-bit IDs, hashes, revisions and frame counts are decimal strings; object IDs
are canonical GUID strings. Counts, sample rates and schema version are JSON
integers. Unknown/duplicate keys or rows, nonfinite numbers, invalid envelopes
and mismatched identities reject the whole companion.

## Sources and occurrences

The top-level `sources` table stores each envelope once. Its `key` joins Sound
GUID, property fingerprint, PCM revision, actual media owner Pack ID and owner
hash with colons. Each row contains those identities, frame/rate/channel/bucket
counts, and per-channel `min`/`max` arrays. The media owner must belong to the
Action owner's validated dependency closure; it need not be the Action Pack.

An Action has `kind: directSound`, `randomCandidates`, or `unavailable`, plus an
ordered `candidates` array of `{ occurrence, sourceKey }`. Direct occurrences use
`Action GUID:0`; Random occurrences use `container GUID:stable child ordinal`.
Repeated members keep distinct occurrence identities while sharing a source.
Static members are ordered by stable child ordinal, never filtered by random
history. Duplicate occurrences/keys, dangling references, unreferenced sources
and ownership/hash mismatches are rejected.

Supported shapes are one Play targeting a direct Sound or a Random with direct
Sound candidates. Any unsupported/unresolved member makes the entire Action
unavailable with a nonempty `sourceReason`, no candidates and unknown duration.
Nested containers, Sequence, Queue and multi-step Actions never publish partial
candidate overviews.

Bucket b covers `[floor(b*frames/buckets), floor((b+1)*frames/buckets))`.
All channels and the final frame contribute. The cached SDK overview aligns
sources at zero on real time axes, merges extrema without averaging, and adds
silence after shorter sources end. Rational boundary projection avoids rounding
peaks into adjacent buckets. Opposite-phase and right-only audio do not cancel.
Overview duration is the longest source duration, independent of Action duration.

## Action duration

The independent `duration` object has `kind: exact`, `bounded`, or `unknown`.
Known durations contain nonnegative `minSeconds` and `maxSeconds`, equal when
exact. Unknown duration has a nonempty `reason` and no numerical bounds.

Frames/rate prove bounds only after checking Action timing, target container,
occurrence overrides, leaf Sound, authored ancestors and output closure: zero
delay/fade/cooldown, immediate quantization, no pitch or loop, nonlooping media,
and no effects, sends or dynamic bindings that invalidate the proof. Any
uncertain candidate makes duration unknown while preserving valid envelopes.
Bounds cover every proven candidate. Source duration must not substitute for
unknown Action duration or become an automatic stop deadline.

## Budgets and publication

Limits are 16 MiB per companion, 4096 Actions, 256 occurrences per Action,
32 channels and `min(frameCount, 2048)` buckets per source channel. The producer
preflights allocation costs and checks final serialized bytes. Producer and reader
also share a separate 16 MiB decoded-source budget: each unique source costs
`channelCount * bucketCount * 8 + 1024` bytes. Reused sources are charged once;
new sources are charged only when their entire Action is accepted. An over-budget
Action becomes wholly unavailable with `metadata-budget`. It publishes no
partial references and releases uncommitted source budget, allowing later small
Actions to succeed. Accepted Actions may share committed sources. Export uses
the existing atomic manifest/companion publication transaction.

Bundle semantic identity excludes only the two raw manifest binding fields,
after validating them against the sibling manifest. This preserves the existing
display-name rename rule. Files stay intact; reuse rejects mismatched pairs.

## SDK use and cache

```csharp
VnResult result = catalog.GetActionPreview(actionName, out var preview, out var diagnostic);
if (result == VnResult.VN_OK)
{
    HarkenCandidateOverview overview = preview.Overview;
    IReadOnlyList<float> extrema = overview.MinMax;
    // Draw extrema on overview.DurationSeconds, without copying each frame.
    // Use Action bounds only when DurationKind is Exact or Bounded.
}
```

`Candidates` and `MinMax` are read-only. `Overview` is built once per parsed
Action. `GetMergedMinMax()` explicitly returns a copy. New and legacy queries
share one validated publication cache. Refresh and publication changes clear
positive and negative cached results; missing or malformed companions never
retain an earlier waveform.

`GetActionWaveform` and `GetWaveform` retain their direct Sound contract: success
has a non-null Source, and nominal duration is returned only when proven exact.
Random returns capability-unavailable with `random-requires-action-preview`, no
Source and no nominal duration. v1 direct Sound data keeps its old behavior and
projects into the new model without inventing Random candidates.

## Shared verification data

`HarkenSDK/Test/Fixtures/EditorMetadata/1/` remains the v1 compatibility fixture.
`2/golden/` and `2/random/` are native canonical Pack fixtures consumed unchanged
by managed contract and Unity tests. Random has three occurrences, two sources
of different rates/lengths and shared dependency media. Native tests compare
exact generated JSON; managed tests verify analytical envelopes and invalid
input rejection. Production export tests check final PCM and bundle behavior.


The native `DecodedSourceBudgetIsAtomicAcrossActionsAndSharedWithReader` test
writes a boundary publication to `.temp/editor-waveform-v2-budget`. Set
`HARKEN_EDITOR_WAVEFORM_BUDGET_FIXTURE` to that absolute directory when running
`HarkenAbiFidelity --waveform-only` to feed the same generated publication to the
SDK reader and verify atomic refusal, subsequent admission and source reuse.

New Studio publications already name their Packs `*.bytes` in the manifest. Run the metadata CLI without `--pack-suffix` for these publications. The optional `--pack-suffix .bytes` flag is only for legacy manifests naming `*.pack` whose physical files were wrapped as `*.pack.bytes`; it does not rewrite paths or publication identity.
