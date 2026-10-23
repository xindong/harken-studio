# Runtime Remote Communication

Remote is part of the same runtime sources used by the static Engine and
`HarkenCore` shared library. It is available in Debug, RelWithDebInfo and Release.
The library runs the service inside its loading process. No separate Engine
host executable is required.

## Lifecycle

Communication is off after `Vn_Init`. On the Engine logic thread, call
`Vn_RemoteStart` with a size-initialized `VnRemoteConfig` to opt in. Continue
calling `Vn_Update` on that thread to execute remote requests. Query
`Vn_RemoteGetStatus` for the actual listening port, process and instance identity,
protocol, Engine version, build ID and separate communication result.
`Vn_RemoteStop` stops communication; `Vn_Term` also closes its sockets, worker and
capture session. Stopping communication does not stop game playback.

```c
VnRemoteConfig remote = {0};
remote.structSize = sizeof(remote);
remote.flags = VN_REMOTE_DISCOVERY_ENABLED;
remote.applicationName = "Example Game";
remote.projectName = "Example Project";
VnResult remoteResult = Vn_RemoteStart(&remote);
```

Call this only after successful Engine initialization. A failed start leaves
audio running. Report `remoteResult` separately, correct the configuration or
port conflict, and retry. Explicitly stop an existing service before changing
its configuration. The Unity and Unreal integrations perform this startup
automatically. Their Harken project settings pages expose `Enable Communication
in Builds`, enabled by default. It controls packaged games; Editor sessions
remain enabled. SDK retries honor that policy and report communication errors
separately from audio initialization. Raw C ABI users own the lifecycle explicitly.

| Setting | Default | Meaning |
| --- | --- | --- |
| `bindAddress` | `127.0.0.1` | Fixed loopback listener; null, empty, or `127.0.0.1` are accepted; other addresses return `VN_INVALID_ARG` |
| `tcpPort` | `0` | System-assigned port, reported in status and discovery |
| `discoveryPort` | `37840` | Shared UDP discovery port |
| `flags` | `0` | Set `VN_REMOTE_DISCOVERY_ENABLED` for discovery |
| `queueBytes` | `1048576` | Bound for each network request/response queue |
| `maxCommandsPerUpdate` | `32` | Maximum queued requests dispatched per update |
| `requestTimeoutMs` | `30000` | Peer inactivity, incomplete-frame and stalled-send timeout |

Unity/Unreal enable communication and discovery by default, restricted to this
machine. Their address setting is not editable in the project settings UI.
Listening, a connected Studio, and active capture are separate states. With
communication off there is no communication socket or worker. Monitor capture
buffers and its worker are initialized only when a consumer requests them.
After first use, Monitor retains those buffers until Engine termination and its
idle worker waits for publication. Network operations never run on the audio
callback; the network worker exchanges bounded queues with the logic thread.

## Connection Contract

`include/harken_remote/Protocol.h` is the shared Engine/Studio binary contract.
It uses explicit little-endian fixed-width fields and length-prefixed UTF-8
strings, with a 16-byte header and a maximum 256 KiB payload. It does not expose
C++ object layouts, addresses or the legacy tooling pipe protocol.

Discovery sends a query to multicast group `239.255.37.84` and receives unicast
announcements correlated by request ID. Each application instance has its own
identity and TCP port. The current Studio build permits only local discovery
and connections. `RemoteConnection::kLocalConnectionsOnly` is a compile-time
constant fixed to `true`; LAN support remains in source and requires a source
change and rebuild to restore. There is no runtime setting to enable it.
Discovery binds to loopback and uses multicast TTL `0` so multiple local
processes sharing the discovery port remain discoverable without LAN queries.
Manual connections also require a loopback address; `localhost` maps directly
to loopback without DNS. Engine TCP and UDP listeners also bind to `127.0.0.1`.
The previous wildcard binding code is retained as comments for future work.
An Engine admits one active Studio connection at a time.

Every TCP connection starts with Hello/Welcome. Protocol major versions must
match; minor version and capability bits are negotiated. Engine product
versions may differ if the protocol and required capabilities agree. Engine
version alone is never evidence of compatibility. Requests carry increasing
nonzero IDs. Responses correlate to those IDs; unsolicited capture data uses
zero. Connection epochs invalidate queued requests after disconnection.
Mutations are never automatically replayed on reconnect or timeout, because a
lost response does not prove the command was not executed.

Supported operations include paged runtime catalogs, real snapshots, posting
Actions, stopping playback, and changing global variables or bus/voice volume.
Control results report Engine API results. Catalog generations fence control
against an outdated runtime world. Lifecycle, arbitrary file operations and
resource/project hot reload are not remote commands.

Monitor records use the existing packed Monitor envelope. This protocol version
fixes that payload contract; an incompatible payload change requires a protocol
version change. Local instance tokens are cleared before transmission. Capture
uses an explicit remote Monitor session and capture-control lease. Capture
failure or lost records is reported as unavailable/stopped rather than valid
zero data; basic snapshots can continue independently. Studio local audition
and remote runtime state are separate instances.

## Network Boundary

This version supports only same-machine communication over IPv4 TCP/UDP.
Both listeners bind to `127.0.0.1`, and discovery joins only the loopback
interface. LAN discovery and connections are unavailable. Explicit non-loopback
configuration is rejected before sockets are created; existing configurations
are not migrated. The transport does not authenticate or encrypt peers. A local
peer that can reach the port can control the opted-in runtime.
No cloud relay, telemetry upload or persistent remote data store is involved.
Future authenticated transports must establish per-session permissions before
dispatch and preserve the same logic-thread and capability boundaries. Legacy
Monitor test endpoint state is not production connection or authentication
evidence; its unimplemented LAN authentication mode returns unsupported.

## Verification

`harken_remote_runtime_tests` exercises real sockets, protocol rejection,
default-off behavior, failure isolation, replay fencing and Monitor lifecycle.
`RemoteConnection.*` covers the asynchronous Studio controller and dialog using
isolated Qt peers. `RemoteConnectionProcess.*` starts `harken_remote_test_game`,
which links the actual shared library and calls only public Engine APIs. That
acceptance test requires a working audio output device and exercises real Pack
playback, multiple-instance discovery, capture, control, reconnect and shutdown.
It does not use the historical host tool. The managed SDK contract harness runs
with `--require-native` so a missing DLL cannot turn into a passing skip.
