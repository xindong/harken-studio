# SDK API audience contract

The SDK exposes game intent and host integration separately from authoring and
engine tooling. The complete native ABI remains the implementation authority.
`HarkenEngine/Source/Tooling/Binding/sdk_api_surface.json` classifies every observed
SDK declaration; it does not redefine native signatures, layouts, exports, or
wire opcodes. Unity now enforces this classification through managed visibility.
The C projection has not been changed by the Unity visibility work.

## Audiences and dispositions

| Audience | Contract |
| --- | --- |
| `gameplay` | Post, query, seek, stop and release authored Actions; set declared Variables; update Emitter/Listener state. |
| `host-integration` | Own the session, prepare and load published content, supply resource bytes, maintain callbacks and optional Player communication. |
| `tooling` | Authoring structure, raw content registration, effects/bindings/topology mutation, deep inspection, and explicit Editor tools. |
| `internal` | Implementation helpers, full native projection, binding context, or retired ABI entries. |

`supported` preserves an explicit contract. `move-internal` is enforced for Unity;
the C projection does not enforce that access boundary. `compatibility` is a named
exception with its consumer contract and a retirement condition. `tombstone`
keeps an already retired native entry retired. An audience is a target contract,
not a statement about today's C# `public` keyword or DLL export table.

The exact declaration list includes public types and members, protected extension
points, callbacks, enum values, properties/accessors, overloads, generic arity,
and declaration attributes. Imports, friend assemblies, assembly definitions
and source locations are also frozen so
a type cannot silently acquire a different binding or move into another assembly.
Partial type accessibility is merged within its assembly. Declaration identities
include their source file; imports include their namespace scope.
Type entries also freeze the ordered field and enum declarations (including
private layout fields and C# properties that may synthesize storage).
Reordering an implicit enum or a struct cannot silently
change its values or layout. Inventory output includes the ordered declarations
behind each fingerprint; this conservative source check is not an ABI calculator.
Enclosing conditional directives and branch identity are part of each declaration.
Type fingerprints retain conditional access changes, and local compilation symbols
are frozen too. Moving an existing API behind an Editor-only condition requires
review just as adding or removing an API does.
File context entries preserve the order of symbol definitions/undefinitions
and other preprocessing directives.
An unchanged set of directives is insufficient when their order changes the
compiled surface. Inventory output exposes the sequence behind its fingerprint.
That sequence includes declaration anchors, so moving a type across an existing
nullable directive is detected. Source offsets are used only for ordering; ordinary
comments and method-body edits do not become declaration identities.
Body-local conditional branches are excluded unless they contain preprocessing
state (such as definitions or pragmas) that can affect subsequent declarations.
Both branches of platform conditionals are inspected. Parse failures fail closed
until their public surface is explicitly supported.

## Current API contract and repository consumers

The consumers referenced here are repository integrations, samples and SDK tests.
They are not evidence of deployed production projects or a production migration
requirement.

| Capability | Decision and consumer evidence |
| --- | --- |
| Actions and Variables | Preserve existing checked names, IDs, results, query/release and Emitter scope. `Actions.cs`, `Values.cs` and Unity components are direct consumers. |
| Pack preparation and bytes | Preserve companion lifecycle, explicit file/byte sources and canonical set ownership. Legacy manifest/index readers and ID-to-path resolution are removed. `PackBootstrap.cs`, `Packs.cs`, `PackByteProvider.cs` and the engine integrations consume them. No implicit loading is added. |
| Existing Bus gain/mute | Keep volume, output volume and mute as runtime controls of an existing mix. They cannot create or reroute a Bus. Unity's `HarkenAudioSystem.SetMasterVolume` uses output volume; it must remain functional. |
| Existing Voice parameters | Preserve the implemented volume, pitch, filter, pan, width, spatial, output gain, Send level and stop contract. `.scripts/check_unity_sdk_contract.py::VOICE_WRAPPER_CONTRACT` checks these wrappers. No equivalent Variable replacement is assumed. |
| Voice SetSend/ClearSend | Both remain public and are classified as structural compatibility exceptions. They create/replace/remove a route, unlike `SetSendLevel`; their continued presence must not be described as a fully removed structural API surface. This classification records current SDK behavior, not a claim that production projects depend on these calls. |
| Bus topology/solo/filter and Effect/Binding/raw media mutation | Internal in the Unity managed SDK. Complete native ABI support remains for Studio, Preview, Remote and tests; the C projection is unchanged. |
| Remote communication | Preserve optional Player settings, status, retry and host lifecycle. The direct managed bridge is internal; public host methods and the Unity integration remain usable in Players. |
| SDK Editor services | Catalog discovery, waveform browsing, preview preparation and symbol generation are temporarily suspended until they consume current-schema Pack companions. Public declarations remain, but catalog refresh, preview preparation and generator validation/generation return `VN_CAPABILITY_UNAVAILABLE`. The Action Inspector supports manual Action name and project ID configuration. |
| Generated types | Supported public signature dependencies and their transitive fields, callback parameters, and semantic enum vocabulary (for example device mode and Action outcomes) remain public. Tool-only Unity DTOs, including Snapshot/Monitor types, are internal. Numeric field types do not justify hiding their enum vocabulary. |
| Helpers and probes | Full P/Invoke, generation helpers, callback pointer fields, mutable catalog/thread probes and byte-provider counters are implementation details. Existing internal consumers remain wired through precise SDK assembly access. |

Old Pack formats have no read or migration compatibility. `PackManifest`,
manifest-based ID loading and `Packs.SetGameplayCatalog` are removed. Public
publication models and prepared-publication declarations needed by suspended
Editor signatures remain inert: capture/read/validation return
`VN_PACK_INCOMPATIBLE`, and waveform parsing returns `VN_CAPABILITY_UNAVAILABLE`.
Current callers use `PackSource`, `StartDirectory` and Action preparation.

Constants are classified individually: master Bus, Pack and Remote discovery
semantics remain supported, while Monitor/Snapshot formats and limits, curve and
effect capacities, staged Voice installation and internal variable-batch capacity
belong to tooling. Keeping `VnConstants` public does not approve all its members.

`RemoteCommunication` is consumed directly by `HarkenProjectSettings` and
`HarkenAudioSystem`; `PackBootstrapSession` does not start it. Custom managed hosts
use `HarkenEngine.StartRemoteCommunication`, `StopRemoteCommunication`, and
`QueryRemoteCommunication`. The integrated Player settings/retry path is unchanged.

## Unity managed API

Unity exposes gameplay, host integration and explicit Editor services through
public wrappers. The full native projection and tool implementation details are
internal. Changing managed visibility changes which declarations game code can
compile against; native signatures, layouts and exports remain unchanged.
See the package [Public API overview](Unity/Harken/README.md#public-api) and its
usage sections for the available entry points and ownership rules.

| API area | Current Unity contract |
| --- | --- |
| `HarkenNative` and `HarkenAbi` | Internal. Public game and host code uses Actions, Values, Emitter/Listener, Voice, Packs and lifecycle wrappers. |
| Action Seek | `ActionFiring.Seek` returns an explicitly owned `ActionSeekTicket` with `Query`/`Release`. Native admission, asynchronous completion and capability limits are preserved. |
| Remote communication | Public `HarkenEngine.StartRemoteCommunication`, `StopRemoteCommunication` and `QueryRemoteCommunication` wrap the internal bridge with explicit opt-in and retry semantics. |
| Voice filter | `Voice.SetFilter` is public; the raw-handle `HarkenEngine.SetVoiceFilter` helper is internal. |
| Variable/Action catalog generation | Temporarily unavailable. The menu is hidden; `GeneratePublication`, `Generate` and `ValidatePublication` return `VN_CAPABILITY_UNAVAILABLE` before reading or writing assets. Existing GUID/name runtime calls remain available. The underlying emitters are internal. |
| Bus topology/solo/filter, Snapshot/Monitor DTOs and raw media/effect/binding DTOs | Internal tool APIs. Public Bus volume/output volume/mute controls remain available. |
| Callbacks and byte providers | Registrations and resource-provider APIs remain public. Callback pointer fields and pin counters are internal implementation details. |

Generated symbol audiences live in `managed_generation`, including internal
symbols absent from the public inventory. Generation rejects missing or stale
entries. Native declarations and wire flags never determine managed visibility.
Only the named Unity integration, Editor and two SDK test assemblies receive
internal access. Imported game samples and external consumers have no friend grant.
`HarkenLateActionService` keeps its script identity, filename and metadata GUID.

Repository source checks, independent consumer compilation and SDK tests verify
this boundary. They establish which APIs are accessible and how supported calls
behave; they do not establish compatibility with an installed production project.

Temporary capability gates do not change schema or product versions. Restore
Editor publication services only after all exposed workflows consume the current
Pack companion contract without a separate legacy manifest or version source.
Android's default manifest-based directory auto-start and the PlayableDemo are
also suspended. Explicit companion sources, including
`HarkenStreamingPackSource` with `StartRuntimeAsync`, and runtime playback by
Action name remain supported. `HarkenPack` automatic cold-ID loading is disabled
for companion sessions; discover content with `Session.PrepareActions` before
using its explicit lease API.

## Check and change the surface

The source parsers are development dependencies installed as PyPI wheels,
with versions pinned in `HarkenEngine/Source/Tooling/Binding/surface-requirements.txt`.
They are not vendored or included in product distributions. Their upstream
license files remain in the installed wheels.

| Package | License | Upstream |
| --- | --- | --- |
| tree-sitter | MIT | https://github.com/tree-sitter/py-tree-sitter |
| tree-sitter-c-sharp | MIT | https://github.com/tree-sitter/tree-sitter-c-sharp |

Use Python 3.12 and the pinned source-parser requirements:

```text
python -m pip install --only-binary=:all: -r HarkenEngine/Source/Tooling/Binding/surface-requirements.txt
python HarkenEngine/Source/Tooling/Binding/sdk_surface.py
python .scripts/tests/test_sdk_surface.py -v
python .scripts/tests/test_managed_generation_policy.py -v
```

The inventory mode reports observed declarations, source locations and line
numbers to a caller-selected file. It never assigns an audience or updates the
approved policy:

```text
python HarkenEngine/Source/Tooling/Binding/sdk_surface.py --inventory .temp/sdk-surface.json
```

For an intentional API change, review each added/changed/removed declaration,
assign it to the justified group, and update the exact policy entry. A public
type does not approve future members automatically. A member belongs to exactly
one group. Compatibility groups require a retirement condition. Never change
the wire table's `public`, opcode, or policy to express an SDK audience.

The source check complements compilation. Run
`.scripts/check_unity_sdk_boundary.py` separately to stage the
SDK, compile a separate library and non-friend consumer, check every generated
symbol and test inaccessible APIs independently. Its `--sdk-root` option accepts
an assembled SDK package; see `HarkenSDK/Test/ExternalConsumer/README.md` for
native-host and Unity validation. The managed generation policy tests and
external-consumer checks are standalone validations, not workflow steps.
These checks do not replace the
installed C/C++ consumer, Unity, final package, or real-device acceptance
required when accessibility and distribution actually change.
