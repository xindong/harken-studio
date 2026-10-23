# Unity SDK fixture inputs

These fixtures are real Studio exports used by current runtime tests. Each
selected platform contains `Init.bytes + Init.json` and same-name Action pairs.
Language directories contain Action pairs and share the platform Init. The
runtime does not read legacy index, manifest or standalone waveform publications.

| Data directory | Coverage |
| --- | --- |
| `U4Original` | Independent A/B roots and Caller with its embedded B closure |
| `U4Renamed` | Same project and Action identity after renaming A |
| `U4OtherProject` | Different project identity |
| `SpatialPacks` | Spatial emitter lifecycle, attenuation and delayed Action |
| `NameFirstPacks` | English and Japanese Action selection |
| `PlatformContractPacks` | Explicit Android_Low and Android_High platform directories |
| `CompanionPacks` | Three-Action embedded closure, General and English variants |
| `VariableNames` | Authored `Sdk_Value` Variable and typed/name binding |

PlayableDemo and Editor publication tooling remain suspended. Their old exports
are not copied into the active fixture project. Tests dedicated to retired
manifest/index parsing have been removed; active ownership, preparation,
callback and lifecycle tests consume companion inputs.

## Prepare an isolated Unity project

Use a dedicated Unity project with the current SDK package, the matching native
library and test assemblies enabled. Enable the built-in
`com.unity.modules.assetbundle` package for the AssetBundle case.
Copy `HarkenSDK/Test/ExternalConsumer/Unity`
to `Assets/ExternalConsumer` to include the unrelated public consumer assembly.

```powershell
python HarkenSDK/Test/UnityFixture/prepare.py --project <Unity-project>
python HarkenSDK/Test/UnityFixture/prepare.py --project <Unity-project> --check
```

The script synchronizes repository fixture files under
`Assets/StreamingAssets/Audio` and removes obsolete files within those fixture
directories. It does not install the SDK or replace native plugins.

Run `Harken.Unity.Tests.HarkenUnityFixtureAssets.Prepare` through Unity's
`-executeMethod` to create current TextAsset resources and the AssetBundle.
`PrepareVariableSource` prepares the Variable source independently. These assets
include both JSON and bytes; use `CaptureCompanion` to create runtime sources.

Run EditMode and PlayMode, including `ExternalConsumerTests`. Tests using
`HARKEN_SDK_TEST_NULL_PUMP` require the dedicated native test bridge and the same
define enabled for Standalone. The bridge is test-only and must not ship in the
SDK. Editor results do not establish Android, IL2CPP or audible device behavior.

## Regenerate publications

Copy the authored projects under `Projects` into a temporary working directory.
Open the copies in the current Studio, apply supported project migration and
save. Keep project/Action GUIDs and the authored audio unchanged. Select the
platforms and all Actions/languages in Publisher, then use **Export All**.

| Source project | Export selection | Destination |
| --- | --- | --- |
| `Playback/Playback.hsp` | Windows | `Data/U4Original` |
| Same Playback copy after renaming `Play_U4_A` to `Play_U4_Renamed` | Windows | `Data/U4Renamed` |
| `OtherProject/OtherProject.hsp` | Windows | `Data/U4OtherProject` |
| `Spatial/Spatial.hsp` | Windows | `Data/SpatialPacks` |
| `Languages/Languages.hsp` | Windows, all languages | `Data/NameFirstPacks` |
| `Platforms/Platforms.hsp` | Android_Low and Android_High | `Data/PlatformContractPacks/<platformName>` |
| `VariableNames/VariableNames.hsp` | Windows | `Data/VariableNames` |
| `Companion/CompanionFixture.hsp` | Windows, Play_Head, General and English | `Data/CompanionPacks` |

Copy only the complete produced `.bytes + .json` pairs and language directories,
replacing the previous destination files. There is no platform index to copy.
For the rename fixture, rename the existing Action through Studio and export to
a fresh output directory; do not recreate the Action or project. Playback audio
is four seconds so stop/seek tests observe live firings. Spatial retains its
0-metre/0-dB and 100-metre/-60-dB attenuation and 100-ms delayed Action.

The existing `.scripts/studio/harken_ipc.ps1` client can drive the same workflow.
Check `get_app_state` and its project path before modifying a copied project.
Use `save_project`, `export_pack` and `get_pack_build_status`; wait for
`in_progress=false` and a successful result before copying outputs or starting
another export. `get_project_info` provides platform IDs for
`set_pack_export_selected_platforms`.

Never modify a Pack binary header or JSON version to imitate re-export. Current
native fixtures must come from the producer being tested. These checked-in
exports provide the present baseline; automatic regeneration from candidate
builds remains separate preparation for a schema-only upgrade PR.
