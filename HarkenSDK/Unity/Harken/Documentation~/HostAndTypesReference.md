# Unity API reference: custom hosts and public types

Most Unity games use `HarkenAudioSystem`, which owns initialization, update,
callbacks, Packs, and shutdown. The APIs below are for a custom host that owns
the whole native session. Do not initialize a second `HarkenEngine` beside an
active `HarkenAudioSystem`.

## Engine lifecycle

| Signature | Contract |
| --- | --- |
| `VnConfig HarkenEngine.DefaultConfig()` | Return a zero-initialized config with the current mirror's `structSize`. Numeric zero selects native defaults; set the required capacity/device fields before startup. |
| `VnResult HarkenEngine.Init(in VnConfig config)` | Initialize one native engine session; successful init resets Variable gameplay session tracking. |
| `VnResult HarkenEngine.Update()` | Pump notifications/callbacks and flush commits on the host's externally synchronized calling thread. Call regularly, outside the audio callback. |
| `VnResult HarkenEngine.Suspend()` / `Resume()` | Release and rebuild the platform device while preserving engine state. The host synchronizes lifecycle calls; a failed resume leaves the engine suspended for an explicit retry. |
| `VnResult HarkenEngine.Term()` | Terminate after stopping/releasing dependent resources. Successful termination resets Variable session tracking. Check failures before dropping ownership. |

For a published game, use `PackBootstrap.Start(string initPackPath, string
containmentRoot, ulong[] startupPackStableIds, ref VnConfig config, out
PackBootstrapSession session)` or its language overload. The prepared-byte
overload accepts `GameplayPublication`, Init bytes, startup IDs, a synchronous
`PackByteProvider.Handler`, provider state, config, and an output session.
`PackBootstrap.ApplyRecommendations(ref VnConfig config, in
InitPackInspection inspection, int startupPackCount)` checks/fills Pack,
catalog, and resident-definition capacities before startup. A successful
`Start` transfers Init and startup Pack ownership to the session; use
`session.Shutdown()` on the creating thread. An engine may initialize even
when a startup Content Pack is missing. Check the return value and
`session?.IsActive` before accessing `session.StartupResult` and
`session.IsStartupComplete`; other failures may leave no session. Details are in the
[Pack reference](PackApiReference.md).

## Identities, handles, and coordinates

`VnObjectId` is a 128-bit value with `high` and `low` fields. It has a
two-`ulong` constructor, `IsNull`, `FromGuid(Guid)`, `ToGuid()`, `TryParse(string,
out VnObjectId)`, `Parse(string)`, value equality, comparison, and a `D`-format
`ToString()`. `TryParse` accepts GUID `D` or brace-wrapped `B`; `Parse`
throws `FormatException` on invalid text. A stable authored ID is distinct
from a native generational `ulong` Voice, Emitter, Pack, or Action firing
handle. `IsValid` on a wrapper usually means nonzero, not that the engine
still owns that handle. A stale generational handle returns `VN_STALE_HANDLE`.

Raw `VnVec3` and orientation data use Harken right-handed coordinates:
`+X` right, `+Y` up, `-Z` forward, meters. `UnitySpatial` converts Unity
Transforms and vectors for the components. `VnConstants` holds generated
constants such as `VN_BUS_MASTER`; use it rather than a copied numeric ID.

`UnityCoordinates.ToHarken(float x, float y, float z)` returns `VnVec3`
with Z negated. UTF-8 encoding helpers such as `Utf8` are internal; supported
public wrappers accept managed strings and perform their own native encoding.

Variable thread/session tracking and its mutable probes are internal SDK
implementation details. Custom hosts use `HarkenEngine.Init`, `Update`, and
`Term` on their owning thread; those wrappers maintain the managed session
tracking. `HarkenPackStartup` is an obsolete subclass retained
for serialized scene compatibility; use `HarkenAudioSystem` for new scenes.

## Generated ABI and symbols

The ABI mirrors `HarkenTypes.g.cs`, `HarkenNative.g.cs`, and `HarkenAbi.g.cs`
under [`Runtime/Generated`](../Runtime/Generated/) are generated from
`HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json` by
`HarkenEngine/Source/Tooling/Binding/gen_binding.py`.
The checked-in baseline `HarkenVariables.g.cs` instead comes from
`HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py` and its
`variable_catalog.json`. For a game's selected publication, use
`HarkenVariableCatalogGenerator.GeneratePublication` to generate the Action
and Variable symbol files together, as described in the
[Editor reference](EditorApiReference.md#generated-symbols-and-project-settings).
Running the ABI generator does not refresh those gameplay symbols.
Do not edit generated files by hand.

| File / type | What to use it for |
| --- | --- |
| `HarkenTypes.g.cs` | Public signature types such as `VnResult`, `VnConfig`, `VnNotification`, `VnRemoteStatus`, and their semantic enums. Tool-only Snapshot/Monitor and raw media/effect/binding types are internal. Only supported `VnConstants` members are public. |
| `HarkenNative.g.cs` | Internal P/Invoke projection. Game code and custom managed hosts use public wrappers; they cannot call this class directly. |
| `HarkenAbi.g.cs` | Internal layout checks used by the SDK and its tests, not a public custom-host entry point. |
| `HarkenActions.g.cs`, `HarkenVariables.g.cs` | Gameplay symbol files. The checked-in Action class is empty and the Variable file is a baseline catalog; generate both from the selected game publication before relying on them. |

For exact public enum values and struct fields, inspect the generated
declarations included in the SDK. A declaration being present in a generated
file does not make it public. The [API audience contract](../../../API_SURFACE.md)
separates gameplay, custom-host and internal tooling capabilities; native ABI
availability does not grant access from a game C# assembly.

Internally, the library name resolves to `HarkenCore` on dynamic-library
platforms and to `__Internal` on iOS, visionOS, and tvOS Players. The binding
uses static Cdecl declarations and AOT-safe callback thunks; it does not
generate interop code at runtime. The Unity package declares its minimum Unity
version and package version in [`package.json`](../package.json).
Native binaries must match the selected platform and generated ABI. Platform
installation and Player export requirements are in the package
[README](../README.md).

`AOT.MonoPInvokeCallbackAttribute` in the SDK is a compile-only shim outside
Unity; Unity builds use Unity's own AOT attribute. It is not a gameplay API.

## Finding the right layer

| Task | Preferred API |
| --- | --- |
| Scene lifecycle and ordinary gameplay playback | `HarkenAudioSystem`, `HarkenAction`, `HarkenEmitter` in the [startup/Action/Emitter reference](UnityApiReference.md) |
| Variable and Label writes | `Values` and `HarkenVariable` in the [Variable reference](VariableApiReference.md) |
| Publication and Pack requests | `HarkenPreparedPacks`, `PackBootstrapSession`, `HarkenPack` in the [Pack reference](PackApiReference.md) |
| Voice and bus mix changes | `Voice`, `Playback`, `HarkenEngine` in the [mix reference](PlaybackMixApiReference.md) |
| Editor catalog and preview | `Harken.Unity.Editor` in the [Editor reference](EditorApiReference.md) |
