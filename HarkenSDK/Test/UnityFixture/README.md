# Unity SDK Test Fixtures

These are real Pack publications created through Harken Studio ProjectManager
and `ExportAllPacks`. They are test input, not hand-authored Pack schemas.
The original and renamed publications come from the same project; the other
project publication intentionally has different project and object identities.

| Directory | Purpose |
| --- | --- |
| `U4Original` | `Play_U4_A`, `Play_U4_B`, and Caller requiring B; independent Content owners |
| `U4Renamed` | Same project after A becomes `Play_U4_Renamed`; the persisted old name is unknown and requires an explicit edit |
| `U4OtherProject` | Same Action names in a different project for explicit project rebinding |
| `NameFirstPacks` | Paired English and Japanese language publications |
| `PlatformContractPacks` | Two Android platform instances for exact ID/name selection and ambiguity |
| `SpatialPacks` | Authored 3D Actions with a volume attenuation profile for rendered spatial tests |

The six-mode sample is reused directly from
`HarkenSDK/Unity/Harken/Samples~/PlayableDemo/PackSource`; its binary content is not
duplicated under this directory. Preparation copies it to both `GeneratedPacks`
(normal sample/lifecycle tests) and `U4Sample` (sample-specific tests).
`EditorWaveformGolden` is the portable production-exported waveform fixture from
`HarkenSDK/Test/Fixtures/EditorMetadata/1/golden`. Its Pack files are selected from
`HarkenPackManifest.json`, including `Init.pack` and the UUID-named Action Pack.
`EditorProjectPackExporterManifest.ProductionWaveformGoldenIsRepeatable` verifies
independent exports, canonical activation, and the exact three-frame stereo PCM.
The retained v1 publication is a managed reader compatibility fixture; it is
not used to start the current native Runtime. The v2 metadata fixtures below
are regenerated together with the current Pack body vintage; they exercise
metadata parsing and waveform construction, not full Runtime activation.

`EditorPreviewDirect` and `EditorPreviewRandom` use the v2 canonical fixtures in
`HarkenSDK/Test/Fixtures/EditorMetadata/2`. The Random fixture has three
occurrences sharing two envelopes across a dependency Pack.

## Prepare a project

Use a dedicated existing Unity test project with a matching Harken SDK and native
`HarkenCore` build. The native binary must support the same generated ABI, Pack
schema, gameplay catalog, and Variable contract as this repository revision.
Build/install native plugins separately in the SDK's documented platform
layout. No native binaries are included here. Windows real-device tests also
require an available output device; automated tests mute the output where
appropriate. Null tests do not replace real-device fade or listening evidence.

From the repository root:

```powershell
py -3 HarkenSDK/Test/UnityFixture/prepare.py --project F:/UnityTests/HarkenSdkTests
py -3 HarkenSDK/Test/UnityFixture/prepare.py --project F:/UnityTests/HarkenSdkTests --check
```

The script copies only the eleven named fixture trees beneath
`Assets/StreamingAssets/Audio`, verifies each file's SHA-256, and preserves
existing Unity `.meta` files. It overwrites differing fixture files and leaves
other assets alone. `--check` performs the same verification without writing.
It does not install an SDK, native library, or sample scripts; start Unity after
preparation to import the data. Enable the SDK test assemblies using your Unity
Test Framework configuration, then run the package EditMode and PlayMode tests.
Import the PlayableDemo separately when regenerating its scene and Prefabs.

## Prepare the serialized Player source

After installing the Pack data and opening Unity, explicitly run
**Harken > SDK Tests > Prepare Player TextAssets**, or invoke the static method
`Harken.Unity.Tests.HarkenUnityFixtureAssets.Prepare` with Unity's `-executeMethod`.
This requires the SDK EditMode test assembly to be enabled. The entry point
imports original `.bytes` TextAssets and uses AssetDatabase serialization to
create `Assets/HarkenSdkFixtureAssets/Resources/HarkenSdkFixture/Source.asset`.
Existing asset GUIDs are retained when preparation is repeated.
Preparation also ensures the Harken project settings resource exists so Player
communication tests can exercise and restore its serialized configuration.
It selects the installed fixture exports in that resource, so Player startup
uses the normal export-selection contract. Run it after installing all fixtures.
It regenerates the isolated test project's `HarkenVariables.g.cs` from the
current `U4Sample` manifest so the typed Variable guard and sample publication
use the same execution catalog. Allow Unity to finish recompiling before tests.

Run this preparation before both Editor PlayMode tests and building the
Windows Player tests. `SerializedTextAssetSourceStartsTheRuntimeInEditorAndPlayer`
loads that asset through `Resources.Load`, captures its bytes, and starts the
Runtime through the same source. It fails explicitly when the prepared asset is
missing; it never depends on another test having run first. These dedicated
fixture assets are intentionally retained for Player builds and may be removed
from a test project after verification.

`HarkenEditorLifecycleTests` additionally requires the imported PlayableDemo
scripts. Its three EditMode tests exercise actual script/domain reload and
Play Mode transitions, a saved scene reload after adopting renamed content,
and repeated sample generation followed by six checked Action submissions.
They restore Editor play-mode settings and remove their temporary assets.
Sample admission uses Null and is not listening or real-device fade evidence.

Fixture paths are derived from this repository and the `--project` argument;
the preparation process does not depend on a developer's local plan or worktree.
The committed JSON and Pack fixtures are regenerated through the current
production project/platform export chain. Set `HARKEN_UNITY_PLATFORM_FIXTURE_OUT`
when running the focused editor fixture test to retain its exported publication
for synchronization. Do not hand-edit manifests, indexes, or Pack binaries.
Use each publication's Init path when configuring test startup; current exports
use original `.bytes` files rather than the older `.pack` fixture names.

## Regenerate Playback Publications

Configure a Studio-enabled build with
`CMAKE_PROJECT_HarkenCore_INCLUDE` pointing to this directory's
`GeneratePublication.cmake`, then build `harken_unity_publication` and
`harken_packc`. Invoke the generator with a new output directory and the packc
executable path. It creates an author project and exports `original` and
`renamed` publications using four-second synthetic tones, so real-device stop
tests can observe independent firings before natural completion. Run again with a different new output directory for
`U4OtherProject`; pass `--spatial` for `SpatialPacks`. Copy the corresponding
exports to `Data`, removing superseded generated Pack files. Omit the optional
`HarkenEditorWaveforms.json` from the three U4 playback fixtures: their tests
explicitly cover publications without waveform metadata.
For an existing copied author project, append `--project <copy.hsp>` instead
of `--spatial`. This exports the platform directory and index while preserving
project, Action, Pack and Variable identities. The export saves the project;
always use a recoverable copy of its authoritative source.

Regenerate the language and platform fixtures with the existing Studio tests:
`EditorProjectPackExporterManifest.LanguageVariantsKeepDistinctPackIdentities`
uses `HARKEN_UNITY_FIXTURE_OUT`, and
`ProjectPlatformBuild.DeliveryIndexAccumulatesOnlyPublishedInstances` uses
`HARKEN_UNITY_PLATFORM_FIXTURE_OUT`. Set each output to a new directory and run
only those exact tests in `harken_editor_tests`. Copy the published Pack files,
manifests, waveform companions and platform index into `NameFirstPacks` and
`PlatformContractPacks`; exclude source audio and caches. Retain both
English/ja publications and both Android_Low/Android_High platform instances.

On headless Windows, set `QT_QPA_PLATFORM=offscreen` and point
`QT_QPA_PLATFORM_PLUGIN_PATH` to the matching Qt installation's `plugins/platforms`
directory. The generator uses the real ProjectManager and export pipeline;
it does not rewrite Pack schemas or binary payloads.

## Deterministic Native Rendering Tests

For the named-variable startup regression, generate a current publication with
`harken_unity_publication <new-output-directory> <packc-path> --variable`.
Copy its `original` publication into the dedicated Unity project's
`Assets/StreamingAssets/Audio/VariableNames`, then invoke
`Harken.Unity.Tests.HarkenUnityFixtureAssets.PrepareVariableSource`.
With the test-native bridge and `HARKEN_SDK_TEST_NULL_PUMP` enabled as below,
run `HarkenPreparedRuntimeTests.TextAssetAndPublicationStartupResolveCurrentVariableNames`.
It starts both the serialized TextAsset and parsed-publication paths, checks the
initial value, pumps accepted named writes to completion, and verifies that a
wrong project cannot change the result. This fixture is generated by the current
production exporter; it does not depend on the older PlayableDemo Pack binaries.

Use a separate Windows native build with `HARKEN_BUILD_CORE=ON`,
`HARKEN_BUILD_SHARED=ON`, and `CMAKE_PROJECT_HarkenCore_INCLUDE` pointing to
`NativeTestBridge.cmake`. Install that build's DLL only in the dedicated Unity
test project, and enable `HARKEN_SDK_TEST_NULL_PUMP` for its test assemblies.
The test-only exports pump fresh Null blocks and copy their output; pumping a
real-device session is rejected. Never distribute this DLL as the SDK runtime.
Normal builds do not include these exports.

The spatial tests exercise both managed facades, source movement, listener
rotation, authored distance gain, overlapping recycled sources, frozen tails,
and authored 2D compatibility. The profile's default width widens near sources
toward center, so direction assertions use a farther source. Volume-curve
interpolation is linear in amplitude after conversion from authored dB points.
These deterministic rendering checks do not establish device or listening
acceptance.
