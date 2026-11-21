# Harken for Unity

**English** | [简体中文](README.zh-CN.md)

`com.harken.core` integrates the Harken game-audio engine into Unity.

Documentation: ONLINE_DOCUMENTATION_URL_PENDING

## What It Is

Harken is a game-audio middleware. Sound designers author sounds and playback logic in Harken Studio and export them as Packs; this package loads those Packs in a Unity game and plays them through the Harken engine, so game code triggers sounds by name instead of handling audio files directly.

## Features

- Start the engine from Project Settings with a selected Pack directory, or from code with directory, byte or custom sources.
- Post Actions by name, stop them, and follow their playback state.
- Write Variables by name to drive authored parameters.
- Position sounds in 3D with emitter and listener components that follow Unity transforms.
- Load and release Packs explicitly, with per-language and per-platform Pack directories.
- Preview Actions in the Unity Editor.
- Connect Harken Studio to a running game for live inspection.

## Requirements

| Item | Requirement |
| --- | --- |
| Unity | 2022.3 or later |
| Platforms | Windows x64, macOS, Linux x64, Android arm64-v8a, iOS |
| Unity built-in modules | Audio, JSONSerialize, UnityWebRequest (declared as package dependencies) |
| Content | Packs exported by Harken Studio |

## Versions and Compatibility

- The package version is the `version` field of `package.json`. It is independent of the Harken Studio and Harken engine versions.
- The package ships two native library editions: Release (`HarkenEngine`) and Development (`HarkenEngineDev`). **Project Settings > Harken > Project > Native Library** selects the edition: *Automatic* uses Development in the Editor and in Development Builds and Release otherwise; *Release* and *Development* force one edition everywhere. Restart Unity after changing it.
- APIs that exist only in the Development edition are compiled only when `HARKEN_ENGINE_DEV` is defined, so Release builds never reference them.
- Startup refuses a native library whose ABI major version differs from the managed bindings.
- Each package version reads one Pack metadata version (currently 4). Export Packs with a Harken Studio release that writes that version. The engine checks the Pack binary version itself.

## License

See [LICENSE.md](LICENSE.md) and [NOTICE.md](NOTICE.md).

### Compact Unity Pack releases

With Python 3.9+ and the Studio CLI installed, run the SDK publishing tool on the saved project and a target platform UUID. Choose a new output directory for each release.

```sh
python Tools~/publish_packs.py --studio-cli /path/to/HarkenStudioCLI --project GameAudio.hsp --platform-id <uuid> --output release-042
```

Use the resulting platform directory with the normal Unity Pack startup, directory, or reader source. The tool performs a fresh Studio export and copies the complete publication. Each canonical Pack of up to 16 MiB is stored as a single independent gzip member if that saves space. Larger or incompressible Packs, companion JSON and auxiliary files keep their original bytes. The SDK retains the normal names and validates the expanded canonical Pack before loading it. A directory load streams the compressed member into one bounded output array on the load path. A reader supplying bytes also retains its input until expansion ends. The native loader and gameplay use the canonical bytes.

For direct native file loads or earlier Unity SDKs, use the ordinary Studio CLI build directory. When another delivery layer already compresses files, compare its final archive sizes before enabling compact publication.

Add `--format zip` to publish a standard archive at `<output>/<platform>/Packs.zip`. Archive entries start at the platform root and retain the exact Studio Pack, companion JSON and auxiliary file contents. Entries up to 16 MiB use DEFLATE when it reduces their size. Larger entries and entries that would grow use ZIP storage without compression. A standard ZIP extractor can restore the platform directory for the ordinary file loader.
