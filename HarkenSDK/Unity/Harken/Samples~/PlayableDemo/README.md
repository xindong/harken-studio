# Playable Audio Demo

Install the SDK, then copy this sample directory to
`Assets/Samples/Harken/PlayableDemo`. Open `Scenes/HarkenPlayableDemo.unity`
and enter Play Mode. The sample installer
copies the whole PackSource tree containing the `Windows/` platform folder to
`Assets/StreamingAssets/Audio/GeneratedPacks` so Editor and Player use the same
path.

For a Windows Player build, select `Windows` in Harken Project Settings' included
exports before building.

The scene contains one Runtime prefab, one Emitter prefab, a Listener on the
camera, six name-bound Action components, and a Variable component. The Runtime
starts the selected directory's Init pair. The demo controller prepares all six
Actions by name before enabling the Play buttons and releases its preparation
when disabled. Action components retain their normal firing ownership. Use each Play
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
and 1. The variable UUID and project identity are resolved from the published
Init through the native Runtime. No Shared Pack is required. The current
fixtures are produced by
`EditorProjectPackExporterManifest.ExportsPlayableDemoSampleFixtures` through
the production exporter. Two independent exports are compared byte-for-byte.
The sample explicitly selects its `Windows` directory.

## Rebuild the assets

Run **Harken > Playable Demo > Rebuild Scene and Prefabs** outside Play Mode.
The builder opens the Init and Action companion pairs in a temporary Null-device
session, resolves the six names and `Demo_Choice`, then closes that session before
writing the scene. Rebuilding requires the matching native library and no active
Runtime session. Missing or renamed Actions are refused before replacing assets.
`HarkenDemoSceneBuilder.RebuildAtPath(sampleRoot)` accepts an imported sample
path. Unity serializes the scene and Prefabs, retaining existing asset GUIDs.

Replace all binary and JSON pairs together when re-exporting. Rebuild the sample
assets to bind an intentionally replaced publication. `READY` means all six
Actions have been prepared; failed preparation keeps Play disabled.

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
