# Harken Engine changelog

## Unreleased

### Deprecated APIs

- Use `Vn_ActivateCanonicalPackSetV2` for Pack set activation and member replacement. `Vn_ActivateCanonicalPackSetV1` and `Vn_ReplacePack` are deprecated. Use `Vn_UnloadCanonicalPackSetV1` to unload the set.
- The `VnPackDesc` vintage of `Vn_LoadPack` for legacy media Packs is deprecated. Use `VnPackLoadDesc` to load current Pack files.
- `Vn_LoadCurrentWorld` for DirectWorld envelopes is deprecated. Runtime applications can activate canonical Pack sets.
- In the development edition, use `VnPreview_PrepareProjectV4` in place of `VnPreview_PrepareProjectV2` or `VnPreview_PrepareProjectV3`.
- The monitor measurement names `VN_MONITOR_MEASURE_LUFS_SHORT_TERM` and `VN_MONITOR_MEASURE_LUFS_INTEGRATED` are deprecated. Their IDs 10 and 11 are permanently reserved.

These declarations enter their deprecation period in this release. Removal of an exported function requires an ABI major version change.

### Host CLI

- The streaming music, ambience and one-shot playback command has been renamed from `m2-scene` to `scene-mix`.
