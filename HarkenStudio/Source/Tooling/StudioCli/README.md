# Harken Studio CLI

`harken-studio` authors and exports Harken projects without opening a window or
connecting to a running Studio. It uses `QCoreApplication`, the editor command
registry, the project save pipeline, and the existing pack builder. An agent only
needs terminal access and the executable's path. No provider-specific plugin,
account, MCP client, or embedded language model is required.

## Installation and discovery

Build the `harken_studio_cli` CMake target, or build `harken_editor` to include it
with Studio. On Windows, `harken-studio.exe` is beside `HarkenStudio.exe`; on
macOS it is in `HarkenStudio.app/Contents/MacOS`. Keep the accompanying libraries,
`schema/`, and `Harkenizer` in place. Add that directory to your shell's `PATH`,
or use the absolute executable path.

```sh
harken-studio --help
harken-studio --version --json
harken-studio commands --json
harken-studio commands create_item --json
harken-studio commands set_item_property --json
harken-studio commands export_pack --json
```

Command discovery reads the live editor registry. Each entry includes an
`inputSchema`, the original `parameters`, and `requiresDesktop`. `schemaSource`
distinguishes declared JSON Schema from legacy descriptions and undocumented
parameters. A legacy description can omit required arguments or constraints;
execution still uses the editor's validation. Do not infer that an undocumented
command accepts no arguments. `readOnly` follows the editor's existing dispatch
classification, not a guarantee about filesystem side effects.

For any terminal-capable agent, supply the executable path and ask it to start
with `--help` and `commands --json`. Installation alone cannot cause every agent
to discover the program automatically.

## Project operations

```sh
harken-studio init ./GameAudio --json
harken-studio exec list_items --project ./GameAudio/GameAudio.hsp --json
harken-studio exec import_audio --project ./GameAudio/GameAudio.hsp --input import.json --save --json
harken-studio run --project ./GameAudio/GameAudio.hsp --input workflow.json --save --json
harken-studio exec export_pack --project ./GameAudio/GameAudio.hsp --input export.json --json
```

`import.json`:

```json
{"path":"/absolute/path/to/footstep.wav"}
```

`workflow.json` is an ordered array. For example, after importing the sound:

```json
[
  {"cmd":"create_action","args":{"name":"PlayFootstep"}},
  {"cmd":"set_item_property","args":{"path":"PlayFootstep","property":"steps","value":[{"type":"Play","targetName":"footstep"}]}},
  {"cmd":"list_items","args":{}}
]
```

Use the returned object identifiers and `commands set_item_property --json` to
configure the action's `steps`, sound properties, routing, containers, and other
authoring data. Inspect `get_action_definition` to read action steps. Runtime
short IDs and editor authoring handles are different identifiers; use the field
requested by the command's parameters.

`export.json`:

```json
{"output_path":"/absolute/path/to/game/audio-output"}
```

`export_pack` exports the project's platforms by default; supply a `platform_id`
from `list_project_platforms` to select one. Its result describes completed platform
outputs, not merely a queued build. Outputs use the same format and build
services as Studio and Harkenizer, for consumption by Harken Engine and its
integrations.

## Process contract

- `exec <name>` takes a JSON object through `--args`, `--input file`, or
  `--input -` for stdin. File/stdin input avoids shell-specific quoting.
- `run` takes an array of `{ "cmd": "...", "args": { ... } }`. All request
  shapes are checked first. Commands then execute sequentially and stop at the
  first error. Results include their command and zero-based index.
- Project edits are saved only with `--save` or an explicit `save_project`.
  Without saving, in-memory edits are discarded when the process exits.
  Creation, export, and other external operations write their own outputs.
  A batch is not a filesystem transaction; failure does not undo earlier saves
  or exported files. Do not use omission of `--save` as a dry-run guarantee.
- Opening a project uses Studio's exclusive project lock. A project already
  open in another process returns `PROJECT_IN_USE`; the CLI never activates a
  desktop window to handle the request. Schema upgrades are not automatic.
- `--json` produces one UTF-8 JSON document on stdout, with `protocolVersion: 1`
  and `status: "ok" | "error"`. Diagnostics go to stderr. Help remains text.
  Preserve unknown response fields when forwarding results to another agent.
- Exit codes: `0` success, `1` execution failure, `2` invalid usage or input,
  `124` asynchronous wait timeout, `130` interruption.
- `--timeout` limits each asynchronous wait, defaulting to 300 seconds.
  Interruption or timeout requests cancellation when the operation supports it.
  Shutdown waits for workers to release files safely; it is not a hard process
  deadline. Synchronous editor operations are not forcibly interrupted.
- This host does not initialize an audio device. Runtime audition and monitoring
  require an initialized runtime; desktop inspection and layout commands return
  `DESKTOP_REQUIRED`.

## Extension boundary

`StudioCli/Main.cpp` owns shell syntax and process I/O. `StudioSession` owns the
headless project lifetime and asynchronous waits. `EditorCommandRegistration`
and `EditorCommandCatalog` are shared by the desktop and CLI. Audio configuration,
validation, saving, and export remain in the existing editor services.

Register new command groups through `registerEditorCommands`, put parameter
metadata beside the existing action definition/catalog, and mark actions that
require widgets with `requiresDesktop` (legacy handlers use
`registerDesktopHandler`). New registered operations then appear without a CLI
command switch or a second schema. Asynchronous commands return an `operation`
with `status_command`, `status_args`, and optional `cancel_command`; the status
response contains `in_progress` and the final response in `result`.

New authoring behavior must be exposed through shared commands to be available
headlessly. Behavior implemented only in a widget cannot be discovered or
translated automatically. Some existing blueprint gestures remain desktop
commands; use explicit authoring operations such as `set_output_bus_routing`,
`set_send`, and `move_item` for their project data. The current shared editor
library still links Qt Widgets; this host creates no widgets and requires no
display platform plugin. Separating that build dependency is a further core
library refactor, not a second authoring implementation in this CLI.

The external interface follows the [Command Line Interface Guidelines](https://clig.dev/).
