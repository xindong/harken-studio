# Playable Audio Demo

Install the SDK, then copy this sample directory to
`Assets/Samples/Harken/PlayableDemo`. Open `Scenes/HarkenPlayableDemo.unity`
and enter Play Mode. The sample installer
copies the whole PackSource tree (root `HarkenPackIndex.json` plus the
`Windows/` platform folder) to
`Assets/StreamingAssets/Audio/GeneratedPacks` so Editor and Player use the same
path.

The scene contains one Runtime prefab, one Emitter prefab, a Listener on the
camera, six name-bound Action components, and a Variable component. The Runtime
startup list explicitly prepares all six Content Packs before the Play buttons
become available. An Action trigger only submits its checked name; it does not
load, unload, switch Packs, or retry a refused submission. Use each Play
button individually and use Set 0/Set 1 while the Variable Control Action
plays.

The content was exported by Harken Studio for Windows without localized media.
Init carries global definitions; each Action Pack carries its own programs and
media. The exact authored names are:

- `Play_Random_Draw`
- `Play_Shuffle`
- `Play_Random_Queue`
- `Play_Sequence`
- `Play_Sequence_Queue`
- `Play_Variable_Control`

The Variable component writes the global `Demo_Choice` variable with values 0
and 1. The variable UUID is read from the current publication
`variableCatalog`; the retired numeric ID 3 is not used. No Shared Pack is
required. The current fixtures are produced by
`EditorProjectPackExporterManifest.ExportsPlayableDemoSampleFixtures` through
the production exporter. The same project is exported into two independent
directories and compared byte-for-byte. Project and platform instance
identities come from the paired manifest and root index; historical
publication IDs are not hard-coded. The platform name and base platform are
both `Windows`. Automatic selection works on a
Windows host because this is the only matching instance. When replacing the
sample output with multiple instances of one base platform, configure the
Runtime's platform ID or exact instance name before startup.

## Rebuild the assets

Run **Harken > Playable Demo > Rebuild Scene and Prefabs** outside Play Mode.
The builder validates the paired catalog and Pack hashes in `PackSource`, looks
up the six exact names, and writes their project association and explicit
startup dependency order into the Unity assets. It refuses missing or renamed
Actions before replacing the scene. It does not infer names from numeric IDs.
The same builder is available as `HarkenDemoSceneBuilder.RebuildAtPath(sampleRoot)`
for a caller with a specific imported sample directory. Unity serializes the
scene and Prefabs; existing asset GUIDs are retained.

Replace the root index, selected manifest, and Pack files together when
re-exporting. Rebuild the sample assets to bind an intentionally replaced
publication. Ordinary Action configuration is never silently rebound to a
different project. `READY` means explicit startup preparation has completed;
missing Content reports `PREPARATION FAILED` and keeps Play disabled.

## Human listening check

1. Confirm Windows is using the intended default output device.
2. Copy the sample into Assets, then run **Harken > Playable Demo > Install Packs** if the
   Pack tree is not already under `Assets/StreamingAssets/Audio/GeneratedPacks`.
3. Open the Sample scene, enter Play Mode, and wait for `READY`.
4. Click each Play button once, allowing each sound to finish before clicking
   the next button.
5. While **Variable Control** is playing, click **Set 1** and then **Set 0**.
6. Record the Unity version, output device, and whether each sound was heard.

Automated capture energy is useful diagnostic evidence, but it does not replace
this listening check.
