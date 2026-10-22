# Unity SDK Test Fixtures

These are real Pack publications exported by Harken Studio from the ordinary
author projects in `Projects`. SDK tests consume the publications; regeneration
uses the Studio application and does not require an editor test executable.
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
| `CompanionPacks` | Init plus one root containing a three-Action closure; General and English variants from the same author project |

`CompanionPacks` is consumed by the standalone native-backed SDK contract runner.
Its ordinary author project is `Projects/Companion/CompanionFixture.hsp`. It has
`Play_Head -> Play_Tail -> Play_Nested -> Sound`, with a 100 ms mono tone.
To regenerate, export the Windows platform with the Sound's language distinction
disabled for the General pair, then enabled for the English pair. Retain only
the Init pair, the General `Play_Head` pair and `English/Play_Head` pair. The
missing child files are intentional. No manifest or binary rewriting is used.

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
Regenerate the publications in `Data` from their source projects as described
below. Do not hand-edit manifests, indexes, or Pack binaries.
Use each publication's Init path when configuring test startup; current exports
use original `.bytes` files rather than the older `.pack` fixture names.

## Regenerate Publications

Use a current Studio build with its matching `packc` executable. Copy the complete
source project directory, including its `AudioSources`, to a new temporary
directory before opening the `.hsp` in Studio. Keep the checked-in projects as
authoritative inputs; save and export from the copies. Existing author projects
can be reexported the same way, preserving their project, Action, Pack and
Variable identities.

In the Publisher, select the platforms below, include all Actions and language
rows, and use **Export All**. The projects use `BuildPacks` as the output directory;
Studio writes each platform under `BuildPacks/<platformName>` and the platform
index at `BuildPacks/HarkenPackIndex.json`.

| Source project under `Projects` | Export selection | Publication to copy | Destination |
| --- | --- | --- | --- |
| `Playback/Playback.hsp` | Windows | `BuildPacks/Windows` | `Data/U4Original` |
| Same Playback copy after renaming A | Windows | `BuildPacks/Windows` | `Data/U4Renamed` |
| `OtherProject/OtherProject.hsp` | Windows | `BuildPacks/Windows` | `Data/U4OtherProject` |
| `Spatial/Spatial.hsp` | Windows | `BuildPacks/Windows` | `Data/SpatialPacks` |
| `Languages/Languages.hsp` | Windows, all languages | `BuildPacks/Windows`, including `English` and `ja` | `Data/NameFirstPacks` |
| `Platforms/Platforms.hsp` | Android_Low and Android_High | Complete `BuildPacks`, including its index | `Data/PlatformContractPacks` |
| `VariableNames/VariableNames.hsp` | Windows | `BuildPacks/Windows` | Unity project's `Assets/StreamingAssets/Audio/VariableNames` |

For Playback, copy the original publication before changing the project. Then
rename the existing `Play_U4_A` Action to `Play_U4_Renamed` in Studio, save and
move the generated `BuildPacks` directory aside before exporting again. Copy the
new publication to U4Renamed. Starting from an empty output directory keeps files
from the previous Action name out of the renamed fixture. Do not delete and
recreate the Action or create another project for this step: both publications
must retain the same project and Action identities. `OtherProject` already has distinct
identities and must remain a separate source project.

Replace each destination's old generated files with the corresponding complete
publication. Copy only published Pack files, manifests, waveform companions and,
for PlatformContractPacks, the platform index; exclude source projects, audio and
caches. Omit `HarkenEditorWaveforms.json` from U4Original, U4Renamed and
U4OtherProject because their tests cover publications without waveform metadata.
Retain both language directories in NameFirstPacks and both Android platform
directories in PlatformContractPacks.

The playback projects contain four-second tones so stop tests can observe
independent firings before natural completion. Spatial adds a distance profile
with volume points at 0 metres/0 dB and 100 metres/-60 dB, plus a 100 ms delayed
Action. VariableNames declares the Smooth variable `Sdk_Value` with range 0..1
and default 0. Keep these authored properties when updating the source projects.

The existing `.scripts/studio/harken_ipc.ps1` client can perform the same operations
against a running Studio instance. For example, copy Playback to
`.temp/sdk-fixtures/Playback` and open that copy's `.hsp` in Studio. From the
repository root, verify the connected instance before saving or exporting:

```powershell
$projectPath = (Resolve-Path ".temp/sdk-fixtures/Playback/Playback.hsp").Path
$state = & .scripts/studio/harken_ipc.ps1 get_app_state -AsObject
if ($state.result.state -ne "project_loaded" -or
    [IO.Path]::GetFullPath($state.result.project_path) -ne $projectPath) {
    throw "The connected Studio instance has not loaded the copied project."
}
& .scripts/studio/harken_ipc.ps1 save_project
& .scripts/studio/harken_ipc.ps1 export_pack
$build = (& .scripts/studio/harken_ipc.ps1 get_pack_build_status -AsObject).result
```

`export_pack` starts asynchronously. Repeat the last command until
`$build.in_progress` is false, then require `$build.result.status` to be `ok` and
`$build.result.result.success` to be true before copying files or issuing another
build. It uses the Publisher's current
selection; select all Actions, languages and the required platforms first.
`get_project_info` lists platform IDs, and `set_pack_export_selected_platforms`
accepts them in its `platform_ids` array. After copying U4Original and moving
`BuildPacks` aside, rename through Studio's IPC, then save and export:

```powershell
& .scripts/studio/harken_ipc.ps1 rename_item '{"path":"Play_U4_A","new_name":"Play_U4_Renamed"}'
& .scripts/studio/harken_ipc.ps1 save_project
& .scripts/studio/harken_ipc.ps1 export_pack
$build = (& .scripts/studio/harken_ipc.ps1 get_pack_build_status -AsObject).result
```

The client connects to the local Studio IPC port 9527 and requests normal
Studio operations; it does not
construct project internals or rewrite publication schemas.

## Deterministic Native Rendering Tests

The named-variable startup regression reuses the checked-in PlayableDemo
Windows publication installed by `prepare.py` in
`Assets/StreamingAssets/Audio/U4Sample/Windows`. Invoke
`Harken.Unity.Tests.HarkenUnityFixtureAssets.PrepareVariableSource`.
With the test-native bridge and `HARKEN_SDK_TEST_NULL_PUMP` enabled as below,
run `HarkenPreparedRuntimeTests.TextAssetAndPublicationStartupResolveCurrentVariableNames`.
It starts both the serialized TextAsset and parsed-publication paths, checks the
initial value, pumps accepted named writes to completion, and verifies that a
wrong project cannot change the result. It uses the sample's `Demo_Choice`
discrete variable, whose default is zero and accepted values are 0 and 1,
without a separate local export.

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
