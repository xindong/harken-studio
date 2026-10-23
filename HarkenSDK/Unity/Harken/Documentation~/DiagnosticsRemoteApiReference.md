# Unity API reference: notifications, diagnostics, and remote communication

This chapter covers `HarkenCallbacks`, public `HarkenEngine` remote controls,
and the diagnostics exposed by `HarkenAudioSystem`.
Native callbacks run during `HarkenEngine.Update()`
on the pumping thread, never on the audio thread. Unity's runtime owner pumps
that call in its `Update` method. Do not arbitrarily mutate Packs or reenter
lifecycle from callbacks. The native contract supports callback-side `Term`
with deferred teardown; use the public owner shutdown method and let the
outermost `Update` return finish cleanup. Handlers must not propagate exceptions
across the native callback boundary.

## Callback registration

`HarkenCallbacks.InstallNotification(ref VnConfig config,
NotificationHandler handler, object state = null)` and `InstallLog(ref VnConfig
config, LogHandler handler, object state = null)` set the corresponding
callback function pointer and user-data key before `HarkenEngine.Init`.
`NotificationHandler(in VnNotification notification, object state)` receives
native notification fields; `LogHandler(uint level, uint code, string text,
object state)` receives decoded UTF-8 text. A null handler throws
`ArgumentNullException`.

Each install returns a `HarkenCallbacks.Registration`. Hold it for the engine
session and `Dispose()` after callbacks have stopped. `IsActive` reflects its
registry entry; disposal is idempotent. `HarkenCallbacks.NotificationFnPtr`
and `LogFnPtr` are internal; use the registration methods. The raw callback and
user-data fields in `VnConfig` and the generated callback delegates remain
public for host interop; this does not expose those internal thunk pointers.
Registration and disposal follow the externally synchronized host thread
contract. If `HarkenAudioSystem` owns the session, subscribe to its
`NotificationReceived` event instead of changing its native config.

| `HarkenAudioSystem` member | Use |
| --- | --- |
| `event HarkenCallbacks.NotificationHandler NotificationReceived` | Observe notifications forwarded by the owner during its update. The supplied `state` is the owner's registered callback state. |
| `LastResult`, `LastOperation`, `OperationCompleted` | Inspect immediate component-operation results. |
| `LastStartupError`, `Session.StartupResult`, `Session.FailedStartupPackStableId` | Distinguish engine startup from missing or failed startup Content. |
| `LastRemoteResult`, `LastRemoteError` | Inspect remote startup separately from game-audio readiness. |
| `VnResult RetryRemoteCommunication()` | Retry configured communication on an existing session. A call outside the gameplay thread returns `VN_INVALID_ARG`. A non-current owner, missing/inactive session, shutdown, or host exit returns `VN_NOT_INITIALIZED`. |
| `VnResult QueryRemoteCommunication(out VnRemoteStatus status)` | Read native communication state. `status.structSize` is initialized by the wrapper. |

Snapshot/Monitor types and `HarkenEngine.Capture` are internal tooling APIs.
Game code observes public operation results, notifications, and owned firing
queries. `HarkenEngine.Update()` drains notifications, invokes callbacks, and
flushes commits; custom hosts call the public wrapper so managed cleanup can
observe the outermost Update return boundary.

## Remote communication

```csharp
VnResult HarkenEngine.StartRemoteCommunication(string applicationName,
    string bindAddress = "127.0.0.1", int port = 0,
    bool enableDiscovery = true, int discoveryPort = 0,
    string projectName = null);
VnResult HarkenEngine.QueryRemoteCommunication(out VnRemoteStatus status);
VnResult HarkenEngine.StopRemoteCommunication();
```

`StartRemoteCommunication` passes a bind address, application and optional project name to
the native service. TCP and UDP bind only to `127.0.0.1`; null, empty, or
`127.0.0.1` are accepted, and other addresses return `VN_INVALID_ARG`. LAN
communication is unavailable. Port values must be in `[0, 65535]`; embedded NUL and
UTF-8 text that does not fit the native fixed buffers return `VN_INVALID_ARG`.
The wrapper uses 64-byte address and 128-byte application/project buffers,
including terminators. `port == 0` and `discoveryPort == 0` defer selection to
the native service. `StopRemoteCommunication` ends that service;
`QueryRemoteCommunication` reports its current
state. Remote communication is optional for game audio; a remote failure is
reported through the owner's remote result and does not by itself undo the
engine session. Project Settings controls automatic startup for the owner.
The underlying `RemoteCommunication` bridge is internal;
custom hosts use the public methods above. For a session owned
by `HarkenAudioSystem`, use its settings, status and retry methods so lifecycle
ownership stays with that component.

`PackBootstrapSession` does not automatically start remote communication.

## DirectWorld host/debug utility

`CurrentWorld.Load(string path, out CurrentWorldLoadReport report)` loads a
Studio/debug DirectWorld envelope. Games load published `.pack` content through
the Pack APIs. The report exposes `Status`, `Result`, prior/result generation,
runtime semantic fingerprint, capability mask, body byte count, and checksum.
Check the return result and report before treating the new world as active.
This call can allocate for a long UTF-8 path and is not a per-frame API.

## Result lookup and ownership of failures

The complete enum and numeric values are generated in
[`HarkenTypes.g.cs`](../Runtime/Generated/HarkenTypes.g.cs). Common results:

| Result | Meaning and first place to inspect |
| --- | --- |
| `VN_NOT_INITIALIZED` (`-303`) | No usable engine session; check owner state and lifecycle. |
| `VN_ALREADY_INITIALIZED` (`-304`) | A second owner or init request; check singleton ownership. |
| `VN_NO_VOICE` (`-300`) | Voice capacity exhausted. |
| `VN_BACKPRESSURE` (`-301`) | A command or plan could not be admitted now; the refused call did not change state. Diagnose admission and lifecycle before retry policy. |
| `VN_IN_USE` (`-302`) | Conflicting ownership or concurrent mutation; inspect the resource scope. |
| `VN_UNKNOWN_ID` (`-200`) / `VN_STALE_HANDLE` (`-201`) | Unknown authored identity versus an expired generational handle. |
| `VN_CONTENT_NOT_LOADED` (`-220`) | Required Pack content is absent or not prepared. |
| `VN_PACK_HASH_MISMATCH` (`-412`) / `VN_PACK_STALE_REVISION` (`-222`) | Supplied bytes or publication revision do not match the selected manifest. |
| `VN_GAMEPLAY_WRONG_PROJECT` / `VN_GAMEPLAY_NAME_RETIRED` | Wrong-project binding, or a retired Action name. Variable lookup has no retired-name tracking: a missing/renamed Variable name returns `VN_NO_VARIABLE`. |
| `VN_DEVICE_ERROR` (`-400`) | Device/backend failure; inspect logs and platform state. |

The result of a Post or Set call is its immediate admission result. Firing
state, notification delivery, audio-device output, and human listening are
separate observations. Preserve failed release ownership until cleanup really
succeeds; a logged error alone is not a reason to discard its handle.
