# Playable Audio Demo

This sample is temporarily unavailable. Its checked-in Pack publication and
scene builder still depend on a legacy index/manifest workflow that has not been
adapted to the current Pack companion contract.

Importing the sample no longer installs Packs automatically. The **Install
Packs** and **Rebuild Scene and Prefabs** menus are hidden. An explicit
`InstallIfImported` call reports the unavailable status without copying files;
`Rebuild` and `RebuildAtPath` refuse before changing scene or prefab assets.
An already imported scene disables its sample Runtime before startup and shows
an unavailable message instead of playback controls.

For current integration, follow the [Unity API setup](../../Documentation~/UnityApiReference.md).
Use a directory or explicit `PackSource` containing paired `Init.bytes` and
`Init.json` plus the requested Action pairs. Configure Action Name and Project Id
manually, and play by name through `HarkenAction` or `HarkenAudioSystem`.
Android can use `HarkenStreamingPackSource` with `StartRuntimeAsync`; its default
legacy directory Auto Start is suspended.

## Retained sample content

The scene and prefabs retain the six authored Action names and the global
`Demo_Choice` Variable for restoration work:

- `Play_Random_Draw`
- `Play_Shuffle`
- `Play_Random_Queue`
- `Play_Sequence`
- `Play_Sequence_Queue`
- `Play_Variable_Control`

The current assets are historical sample content, not a supported publication
for the next release. Re-enabling a menu or supplying a legacy manifest does not
complete the adaptation. Preserve scene, prefab and script asset identities
while updating the sample.

## Restoration requirements

Restore `HarkenPlayableDemoController.IsAvailable`, installation and rebuild
entry points only after all of these conditions hold:

1. The exported sample uses the current schema's paired Pack files, without a
   separate legacy manifest or fixed schema number defined by the integration.
2. The builder obtains project and Variable identities from current supported
   data, binds the exact Action names, and prepares Actions through the current
   runtime path instead of an inventory-derived startup ID list.
3. Installation, scene startup and all six Actions work with the matching Integration Package
   and native library, including the `Demo_Choice` controls.
4. Scene/prefab identity checks and an actual listening check pass. Record the
   Unity version, output device, each audible Action, and both Variable values.

Keep the suspension independent of schema numbers so a later schema release
does not require editing these entry-point gates.
