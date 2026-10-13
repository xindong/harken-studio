# Unity API reference: Variables and Labels

This chapter extends the [startup, Action, and Emitter reference](UnityApiReference.md).
`Harken.Values` is the game-code API;
`Harken.Unity.HarkenVariable` is the component API. Calls must run on the Unity
game thread. The current Init Pack supplies the live Variable and Label names.
Variable names are case-sensitive ASCII identifiers matching
`[A-Za-z][A-Za-z0-9_]{0,127}`. Label names are UTF-8
strings without an embedded NUL. A generated GUID symbol survives a
rename of the same object; a string name must be updated after a rename.

## Write scope and lifetime

A Variable definition has no fixed global or local scope. Omitting the optional
`Emitter? emitter` writes globally. A global write also removes **all** local
overrides for that Variable, including when the global number is unchanged.
Passing a valid emitter writes its local value for every consumer on that
emitter, across Actions, firings, and Layers. A local write never changes a
different emitter or the global value. Bus bindings always read the global
value; routing does not transfer emitter identity. An explicitly supplied
`Emitter.None`, `default(Emitter)`, or stale emitter is refused. The parameter
is nullable: omitting it, passing `null`, `default(Emitter?)`, or a bare
`default` in `Values.SetValue("Speed", 1f, default)` means a **global write**,
including removal of local overrides. Destroy an emitter and create a new one
when pooling gives a GameObject a new identity. A retained emitter retains its
local values.

`SetValue` accepts a number, current Label name, or stable Label GUID. A Step
Variable requires a finite exact integer within both its authored range and
`[-2^24, +2^24]`. A Smooth Variable requires a finite number within its authored
range. Invalid numeric values return `VN_INVALID_ARG`; they are not clamped.
Label writes resolve to the Label's authored anchor value. An unknown, deleted, or
out-of-range Label fails without falling back to a numeric write. All writes
submit synchronously in call order; use the returned `VnResult` for admission,
and query separately when the applied value matters. There is no public local
Clear or ResetVariables API. Write the Variable's authored default when a
reset-like value is required; that is still a normal write.

```csharp
using Harken;

VnResult global = Values.SetValue("Surface", "Stone");
VnResult local = Values.SetValue("Surface", "Wood", emitter);
VnResult speed = Values.SetValue("Speed", 0.5f, emitter);
```

The example assumes `Speed` is a Smooth Variable whose range includes `0.5`,
and the named Labels exist on `Surface`. Check each result. A project-qualified
name call additionally verifies the publication's project identity; use the
nonempty project GUID in `D` format.

## `Harken.Values` signatures

Each write below returns `VnResult`. The optional `Emitter? emitter = null`
has the same scope meaning for every overload. The first three signatures
require `projectId`; the remaining name/ID forms do not. Generated
`HarkenVariableId` forms check the generated execution catalog against the
live one. Raw GUID and current-name forms resolve against loaded content
without that generated-catalog gate.

| Variable selection | Number | Label name | Label GUID |
| --- | --- | --- | --- |
| Current name + project | `SetValue(string variableName, string projectId, float value, Emitter? emitter = null)` | `SetValue(string variableName, string projectId, string labelName, Emitter? emitter = null)` | `SetValue(string variableName, string projectId, VnObjectId labelId, Emitter? emitter = null)` |
| Current name | `SetValue(string variableName, float value, Emitter? emitter = null)` | `SetValue(string variableName, string labelName, Emitter? emitter = null)` | `SetValue(string variableName, VnObjectId labelId, Emitter? emitter = null)` |
| Stable Variable GUID | `SetValue(VnObjectId variableId, float value, Emitter? emitter = null)` | `SetValue(VnObjectId variableId, string name, Emitter? emitter = null)` | `SetValue(VnObjectId variableId, VnObjectId labelId, Emitter? emitter = null)` |
| Generated typed Variable | `SetValue(HarkenVariableId variable, float value, Emitter? emitter = null)` | `SetValue(HarkenVariableId variable, string name, Emitter? emitter = null)` | `SetValue(HarkenVariableId variable, VnObjectId labelId, Emitter? emitter = null)` |

`Resolve(string variableName, out VnObjectId variableId)` resolves an exact
current name to its stable ID. Failed resolution leaves `variableId` at its
default. Variable names must match `[A-Za-z][A-Za-z0-9_]{0,127}`; spaces,
hyphens, and leading digits or underscores are invalid. Label-name calls
accept nonempty UTF-8 text without an embedded NUL;
project-qualified calls require a valid nonempty project GUID. A typed
Variable with no assigned stable ID returns `VN_INVALID_ARG`.

`QueryVariable(HarkenVariableId variable, out VnVariableQueryResultV1 result)`
queries a typed Variable after the catalog check. The lower-level
`QueryVariable(in VnVariableQueryDescV1 desc, ref VnVariableQueryResultV1 result)`
and `QueryVariableTicket(in VnVariableTicketQueryDescV1 desc, ref
VnVariableTicketQueryResultV1 result)` forward caller-prepared ABI structures.
Set their `structSize` fields as required by the generated ABI. The legacy
batch setters and global reset wrappers are internal; they are not public
game-code APIs.

## Component: `Harken.Unity.HarkenVariable`

The component serializes a Runtime, optional `HarkenEmitter`, typed Variable,
authored `variableName`, `projectId`, numeric `value`, and `setOnEnable`.
Inspector publication binding supplies the exact name/project pair. When
`setOnEnable` is true, the component attempts to apply after the Runtime
becomes ready. It retries only `VN_NOT_INITIALIZED` for at most 120 frames;
other failures stop that automatic attempt. Invoke `Apply()` explicitly after
correcting a failure. With an emitter reference, it waits for a created live
emitter and writes locally; without one, it writes globally. A destroyed
referenced emitter returns `VN_STALE_HANDLE`, not a global write.

| Member | Contract |
| --- | --- |
| `VnObjectId VariableStableId`, `string VariableName`, `string ProjectId` | Serialized identity readouts. `VariableStableId` comes from the typed field. |
| `HarkenVariableId Variable { get; set; }`, `float Value { get; }` | Typed reference and last configured number. Setting `Variable` clears the name/project pair and selects typed-only binding; it does not submit a write. |
| `VnResult LastResult { get; }` | Last component write result. |
| `VnResult SetValue(float nextValue)` | Store the number and immediately call `Apply()`; cancels the pending automatic attempt. |
| `VnResult Apply()` | Write the stored number through the selected binding and optional emitter. Returns the underlying `Values.SetValue` result. |

An empty name and project select the typed field; only
one of that pair being present returns `VN_INVALID_ARG`. When both are present,
the component resolves the project-qualified name and rejects a different
assigned typed ID rather than writing the wrong Variable. Inspector typed
selection also clears the old name/project pair. Typed binding checks catalog
fingerprints; it does not contain an independent project GUID.

The serialized Runtime, emitter, name/project and enable fields are Inspector
configuration, not public writable fields. Programmatic typed writes also use
the public `Values.SetValue(HarkenVariableId, ...)` overload.

`HarkenVariableReference` is a `ScriptableObject` exposing one read-only
`HarkenVariableId Variable`. `HarkenVariableId` contains public `stableId`
and `codeName` fields, `IsAssigned`, and
`FromGenerated(string name, VnObjectId id)`. Equality compares both fields;
`GetHashCode()` uses the stable ID. A generated symbol is a reference, not a
runtime registration operation.

## Generated symbols and catalog pairing

Use **Harken > Generate SDK Symbols From Publication** with the exported
`HarkenPackManifest.json` to regenerate `HarkenActions.g.cs` and
`HarkenVariables.g.cs` as one publication pair. The generated
`Harken.Generated.HarkenVariables` fields are raw `VnObjectId` GUIDs, not typed
Variable references. `Values.SetValue(HarkenVariables.Speed, ...)` therefore
uses the raw GUID overload. To request the typed catalog check, construct a
reference explicitly (assuming the publication defines `Speed`):

```csharp
HarkenVariableId speed = HarkenVariableId.FromGenerated(
    "Speed", Harken.Generated.HarkenVariables.Speed);
VnResult result = Values.SetValue(speed, 0.5f, emitter);
```

Regenerate after an export changes the published catalog. The SDK's internal
guard blocks a typed write with
`VN_VARIABLE_CATALOG_MISMATCH` when the execution fingerprint differs. A
display-only catalog fingerprint difference warns and records a mismatch but
allows the write. Raw ID and current-name calls use the loaded publication.
The guard and its test hooks are internal.

The public `HarkenVariableCatalogGenerator.GeneratePublication(...)` and
`Generate(...)` Editor methods write generated C# from a publication JSON
path. `ValidatePublication(string json, string sourceLabel, out string error)`
validates only the Variable catalog in JSON content; it does not verify Action
data, Pack bytes or manifest/Init pairing. The emitter implementation classes
are internal. Keep generated
files paired with the Pack export; editing their contents by hand loses that
pairing. See [Editor APIs](EditorApiReference.md) for signatures and output
ownership.

## Failure results to preserve

| Result | Meaning for this API |
| --- | --- |
| `VN_INVALID_ARG` | Invalid numeric value/range, empty/malformed name or project, wrong game thread, unassigned typed Variable, or explicit invalid emitter. |
| `VN_UNKNOWN_ID`, `VN_NO_VARIABLE`, `VN_NO_VARIABLE_LABEL` | Current name/ID or Label is unavailable in the loaded publication. |
| `VN_GAMEPLAY_WRONG_PROJECT` | Project binding does not match the loaded publication. A missing or renamed Variable string returns `VN_NO_VARIABLE`; Variable name lookup does not use Action retired-name tracking. |
| `VN_VARIABLE_CATALOG_MISMATCH` | Generated typed execution catalog differs from the live one; regenerate from the paired export. |
| `VN_VARIABLE_EMITTER_SLOT_EXHAUSTED`, `VN_BACKPRESSURE` | Different resource/admission refusals; preserve the exact result for diagnosis. |
