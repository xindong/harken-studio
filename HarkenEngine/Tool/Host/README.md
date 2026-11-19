# Native Pack playback

Select the Init Pack and one content Pack explicitly and post the Action by ID:

```sh
harken_host pack-play --init-pack /path/to/Init.bytes --content-pack /path/to/Content.bytes --action-id <guid> --seconds 1 --repeats 2
```

The content Pack already contains its nested Action dependencies. The Host reads
only the two selected files; it does not read Studio metadata, require a
manifest or scan a directory. `--action-id` must name the authored root Action
of the content Pack. Use the Host, native runtime and Packs from the same release.

The attenuation probe uses the same selection:

```sh
harken_host att04-nearfar --init-pack /path/to/Init.bytes --content-pack /path/to/Content.bytes --action-id <guid> --distance 1 --null
```

The Host reports the selected Sound under the posted firing, including a Sound reached by nested
Actions. Concurrently starting several Sounds remains ambiguous for the
single-selection diagnostic. Recovery reuses the captured Pack bytes.

Add `--null` for an offline smoke run. It checks rendered PCM and playback
records; it does not validate a physical device or replace listening acceptance.
