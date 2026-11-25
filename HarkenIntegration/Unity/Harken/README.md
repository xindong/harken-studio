# Harken for Unity

**English** | [简体中文](README.zh-CN.md)

`com.harken.core` integrates the Harken game-audio engine into Unity.

Documentation: https://harkenstudio.xd.com/document/0.10.0/en/unity-integration.html

## What It Is

Harken is a game-audio middleware. Sound designers author sounds and playback logic in Harken Studio and export them as Packs; this package loads those Packs in a Unity game and plays them through the Harken engine, so game code triggers sounds by name instead of handling audio files directly.

## Prepare first-level audio on Windows

Games that own the audio session can call `await PackBootstrap.StartLevelAsync(source, config, actionNames, cancellation)` on the Unity main thread. Create the source from the platform Pack directory and pass the Action names needed for the first level. Supported Windows runtimes open the output while preparing these sounds. To play an opening cue immediately, create its emitter and post the Action in `onPrepared`, returning the post result. Playback starts at the beginning of the cue on the real output. On `VN_OK` both the level and the device are ready. Call `Preparation.Release()` when leaving the level and `Session.Shutdown()` when ending the session. Scenes using the default audio component let that component own startup and shutdown.

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
- Remote communication works only with the Development edition; with the Release edition its methods return `VN_CAPABILITY_UNAVAILABLE`.
- Startup refuses a native library whose ABI major version differs from the managed bindings.
- Each package version reads one Pack metadata version (currently 4). Export Packs with a Harken Studio release that writes that version. The engine checks the Pack binary version itself.

## Runtime Faults

If an operation returns `VN_RUNTIME_RECREATE_REQUIRED`, subscribe to `HarkenAudioSystem.RuntimeRecreationRequired` or call `HarkenEngine.QueryRuntimeFault(out RuntimeFaultInfo info)` for the first fault. Stop posting work and shut down and initialize a new session. A `VN_OUT_OF_MEMORY` result alone permits the current session to continue.

## License

See [LICENSE.md](LICENSE.md) and [NOTICE.md](NOTICE.md).
