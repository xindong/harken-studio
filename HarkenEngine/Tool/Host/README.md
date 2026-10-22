# Native Pack playback

For companion exports, select the platform directory explicitly and post by
Action name:

```sh
harken_host pack-play --root /path/to/Windows --action-name Play_Head --seconds 1 --repeats 2
```

The directory contains `Init.bytes`, `Init.json`, `Play_Head.bytes`, and
`Play_Head.json`. The Action pack already contains its nested Action dependencies.
The Host reads only these pairs and uses their authored IDs; it does not require
a manifest or scan the directory. Use the SDK and packs from the same release.

Add `--language English` to request `English/Play_Head.bytes` and its JSON.
Only when both localized files are absent does the Host use the General pair at
the root. A partial pair, malformed metadata, redirected file, or active
`.harken-pack-publishing` marker fails the command. Init is always read at the
root. Keep the publisher's marker present for the complete publication.

Do not combine `--action-name` with `--action`, `--content`, `--platform-id`, or
`--platform-name`. The original manifest/index entry remains available:

```sh
harken_host pack-play --root /path/to/publication --action <action-guid> --content <pack-name>
```

Both entries use the same playback and recovery path. The Host reports the
selected Sound under the posted firing, including a Sound reached by nested
Actions. Concurrently starting several Sounds remains ambiguous for the
single-selection diagnostic. Recovery reuses the captured companion bytes.

Add `--null` for an offline smoke run. It checks rendered PCM and playback
records; it does not validate a physical device or replace listening acceptance.
