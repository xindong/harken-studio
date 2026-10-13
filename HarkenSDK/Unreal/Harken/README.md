# Harken for Unreal Engine

Harken is a runtime Unreal Engine plugin backed by the public HarkenCore C ABI.
The plugin is self-contained after packaging and can be copied directly to a
project's `Plugins/Harken` directory.

## Configured targets

- Windows x64
- macOS universal
- Linux x64
- Android arm64-v8a
- iOS arm64 device

The Win64 plugin is compiled and tested with Unreal Engine 5.8.1. The other
four targets have build and packaging rules, but still require compilation on
their native CI machines or devices.

Linux currently supports HarkenCore's Null device mode only. Real audio-device
output remains unsupported, matching the existing Unity SDK behavior.

The source plugin does not commit generated native binaries. Run the repository
packaging script before building the plugin so the public header and the native
library for the selected platform are staged into `Source/ThirdParty` and
`Binaries/ThirdParty`.

## Runtime content

Place the exported Pack files (new exports use `.bytes`) under the project's
`Content/Harken` directory. Existing `.pack` paths remain supported; the loader
opens the exact configured or manifest path without interpreting its extension.
A `CurrentWorld.vnworld` preview envelope is optional Studio/debug
output and is not required at startup. Paths configured in Project Settings >
Plugins > Harken are relative to the project Content directory unless they are
absolute. Files under `Content/Harken` are staged as NonUFS files so HarkenCore
can open them by path.

Use the raw exported files for Unreal; Unity AssetBundles are not Unreal content.

`HarkenContent.json` identifies one deployed content bundle using `platformId`,
`platformName`, and `basePlatform`. Each ordered Pack carries the same instance
ID and remains inside that bundle directory. Manifest startup currently accepts
the `Windows` base platform; binary packaging target names such as `Win64`
remain separate from content instance identity. Deploy the chosen instance's
complete bundle to switch between two Windows content configurations.

## Quick start

1. Copy the packaged `Harken` directory to `<Project>/Plugins/Harken`.
2. Enable the plugin and restart the editor.
3. Configure startup content under Project Settings > Plugins > Harken.
4. Add one `Harken Listener Component` to the player or camera.
5. Add `Harken Emitter Component` to actors that trigger spatial actions.

The initial integration uses HarkenCore's native device output, matching the
existing Unity SDK. Unreal Audio Mixer output is a separate future transport.

## Action and Variable Names

C++ supports both names and generated GUID symbols:

```cpp
Subsystem->PostAction(TEXT("Play_Footstep"), Emitter, 0, Firing);
Subsystem->PostAction(HarkenActions::Play_Footstep, Emitter, 0, Firing);
Subsystem->SetValue(TEXT("Speed"), 0.5f, Emitter);
Subsystem->SetValue(HarkenVariables::Speed, 0.5f, Emitter);
Subsystem->ResolveVariable(TEXT("Speed"), SpeedId);
```

Blueprint uses `PostActionByName`, `PostActionById`, `SetValueByName`, and
`SetValueById`; the existing `SetValue` Variable-reference node remains available.
`ResolveVariable` returns a GUID that can connect directly to `SetValueById`.
Name lookup uses the loaded Init Pack and follows publication
replacement and unload. Names are case-sensitive ASCII identifiers. Only current
names are exported; after a rename, callers must update the string or use the
object's GUID. Historical aliases and retired-name storage are deferred.

Generate `Generated/HarkenActions.g.h` and `Generated/HarkenVariables.g.h` from
the exported manifest, selecting explicit output paths for your project:

```text
py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py --publication HarkenPackManifest.json --action-unreal Generated/HarkenActions.g.h --unreal Generated/HarkenVariables.g.h
```

Keywords are escaped with `A_` or `V_`; existing prefixes receive another prefix
to avoid collisions. These symbols retain the complete GUID. Native C++ can use
the same generator with `--action-native` and `--native` output paths.

## Variable writes

`UHarkenEngineSubsystem::SetValue(Variable, Value, Emitter = nullptr)` is the
recommended Blueprint and C++ entry point. The optional Blueprint input is an
Emitter Component. C++ supports typed Variables, raw GUIDs, or current Variable
names with an `FHarkenEmitterHandle`. The value can be a number, a stable Label
GUID, or the current Label name. A specified component must already own a live emitter;
invalid components and stale handles fail without issuing a global write.

```cpp
Subsystem->SetValue(X, 1.0f);             // A = 1, B = 1
Subsystem->SetValue(X, 2.0f, EmitterA);   // A = 2, B = 1
Subsystem->SetValue(X, 3.0f);             // A = 3, B = 3; no overrides
Subsystem->SetValue(TEXT("Surface"), TEXT("Grass"), EmitterA);
Subsystem->SetValue(SurfaceId, GrassLabelId, EmitterA);
```

Label values resolve through the currently loaded Pack mapping. Missing names,
deleted Labels, and Labels outside the Variable range fail without writing a
numeric fallback. Typed Variable calls retain the generated catalog check;
GUID and current-name Variable calls use the loaded content directly.

Blueprint Label inputs use `SetValueByLabelId` and `SetValueByLabelName`; both
nodes display as `Set Value` and accept a typed Variable and an optional Emitter
Component. Their distinct bridge names are required because reflected Unreal
functions cannot overload by value type. C++ callers use the `SetValue` overloads.
The plugin's `Config/DefaultHarken.ini` redirects existing Label write nodes to
these bridges when assets load; their original input pin names are preserved.
Open and resave affected Blueprints after updating the plugin.

Variable definitions have no scope. Global writes replace the global value and
remove all emitter overrides for that Variable, including repeated assignments
of the same number. Local writes affect all consumers on the chosen emitter,
across Actions, playback instances, and Layers. Layer bindings use each
descendant playback's emitter context; Bus bindings always read global values.
Routing never creates or merges emitter contexts. See the
[Game Modulate contract](../../../.docs/philosophy/game-modulate-system.md).

Setters submit synchronously in call order. Separate emitter setters and global
reset wrappers remain deprecated forwarding APIs. Local Clear and
`ResetVariables` are unavailable. Reusing a component for a new game-object
identity requires ending its old emitter and creating a new one.

## Remote communication

Project Settings > Plugins > Harken > Communication contains
`Enable Communication in Builds`, enabled by default. It controls packaged
games, including Development and Shipping builds. Clear it before packaging
to keep communication stopped in that game. The setting is stored in the game
project's `DefaultGame.ini` and is included by Unreal's configuration pipeline.
Editor builds always start communication with audio, including PIE, regardless
of this build setting. The shared Engine runs the service inside the Editor or
game process; Release builds support the same communication feature.

The default bind address is `0.0.0.0`. TCP port `0` requests an available port
for each process. Discovery is enabled by default when communication is enabled;
discovery port `0` selects UDP `37840`. Studio discovery and
`QueryRemoteCommunication` report the actual TCP endpoint. An empty application
name uses Unreal's project name; the native library supplies its engine version
and build identity.

The Engine Subsystem exposes Blueprint and C++ `RetryRemoteCommunication`,
`QueryRemoteCommunication`, and `GetRemoteCommunicationResult`. Retry reads the
project settings after a failed start and cannot bypass disabled communication
in a packaged game. Communication startup failure is logged separately and does
not fail audio initialization.
Normal engine shutdown closes the listener and active connection automatically.
There is no extra per-frame SDK polling; the existing engine update dispatches
remote requests.

Call the subsystem's communication lifecycle and query methods on Unreal's
game thread. Direct C API `Vn_RemoteStart`, `Vn_RemoteStop`, and
`Vn_RemoteGetStatus` calls must use the Engine logic thread while the Engine
is initialized. Project network changes apply at the next audio session or
failed-start retry; retrying an active service returns `VN_IN_USE`.

This transport is for trusted local networks; peer authentication and transport
encryption are not implemented. Bind to `127.0.0.1` for local-only access, or
disable the service when remote access is unnecessary.

## Repository validation

From the repository root on Windows:

```powershell
py -3 .scripts/unreal_sdk.py `
  --platform Win64 `
  --build-core `
  --build-plugin `
  --run-tests `
  --engine-root "C:\Program Files\Epic Games\UE_5.8"
```

This builds HarkenCore, stages the public header and native library, packages
the plugin, and runs the Unreal automation tests.
