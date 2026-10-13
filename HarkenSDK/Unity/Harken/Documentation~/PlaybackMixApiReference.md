# Unity API reference: Voice, playback, and buses

This chapter describes the handwritten `Harken.Voice`, `Harken.Playback`, and
`Harken.HarkenEngine` mix controls. These are
direct Runtime calls on the host's externally synchronized logic thread. They
return an immediate `VnResult`; an accepted live change reaches rendering at a
subsequent audio block. `VN_BACKPRESSURE` means the change was refused without
changing state. It is distinct from `VN_NO_VOICE` (voice capacity).

## Voice handle and scope

`new Voice(ulong handle)` wraps a generational native voice handle. `Handle`
is the raw value, `IsValid` only checks that it is nonzero, and equality and
hashing compare that value. This struct owns no voice or engine state. A stale
handle returns `VN_STALE_HANDLE`; it does not authorize access to a new voice
that reused a slot. This compatibility wrapper requires a real native voice
handle from its owning integration. The public `Playback` facade only exposes
`StopAll`; it does not create raw voices. Neither `ActionFiring.Handle` nor an
arbitrary `producedPlayableInstance` is a voice handle. Control Action playback
through `Actions` or its owning `HarkenAction`.

| `Voice` signature | Value and effect |
| --- | --- |
| `VnResult SetVolume(float volumeDb, uint rampMs)` | API contribution to authored voice volume, in dB; composes with Variable contributions. A positive ramp interpolates linear amplitude; zero snaps at the next block. |
| `VnResult SetOutputVolume(float volumeDb, uint rampMs)` | Independent main-output gain in dB. Voice sends bypass it. Pending/parked voices stage a value without ramping. |
| `VnResult SetFilter(uint filterKind, float cutoffHz, float q, uint enable01, uint rampMs)` | Tone-stage lowpass (`0`) or highpass (`1`). `enable01` is `0`/`1`; enabled Q must be positive, cutoff and Q finite. It composes with Variable contributions. |
| `VnResult SetPitch(float ratio)` | Positive, finite playback-rate ratio at most `64`; preserves playback position. |
| `VnResult SetPosition(VnVec3 position)` | Finite right-handed world position in meters. A live attached Emitter owns position and causes `VN_EMITTER_ATTACHED`. |
| `VnResult SetOrientation(VnVec3 forward)` | Nonzero forward vector with finite components; Runtime normalizes it. Default direction is `(0, 0, -1)`. Invalid vectors return `VN_INVALID_ARG`; a live attached Emitter owns orientation and causes `VN_EMITTER_ATTACHED`. |
| `VnResult SetPan(float pan)` | Finite `[-1, 1]`: left, center, right; contributes to the final pan, including 3D geometry. |
| `VnResult SetWidth(float width)` | Finite `[0, 1]`. Spatialized mono blends toward the front-center image; stereo spreads its two input columns along the front arc. Contributions combine by taking the widest, so `0` cannot narrow a wider contribution from another source. Stored without an image change on authored 2D voices. |
| `VnResult SetSpatializationMix(float mix)` | Finite `[0, 1]`; linearly blends the original 2D matrix at `0` with the point/spread matrix at `1` for spatialized mono or stereo. Stored without an image change on authored 2D voices. Sources wider than stereo are rejected for 3D. |
| `VnResult SetDistanceScale(float scale)` | Finite `[0.01, 100]`; distance attenuation uses pair distance divided by scale. |
| `VnResult SetSend(uint sendSlot, VnObjectId toBus, float levelDb)` | Set or replace a zero-based voice send slot. Sends carry the post-spatial-matrix image in the mix layout, before the independent main-output gain. Slot must be below configured `voiceSendSlots`; target bus must exist. |
| `VnResult SetSendLevel(uint sendSlot, float levelDb, uint rampMs)` | Change an occupied send's level in dB. Empty or out-of-range slots return `VN_INVALID_ARG`. |
| `VnResult ClearSend(uint sendSlot)` | Clear a slot; an already empty valid slot succeeds. |
| `VnResult Stop(uint fadeMs)` | Request a fade to stop this voice. A successful request does not mean the voice is already terminal. |

`volumeDb` and send levels must be finite. Filter kinds outside `0`/`1`,
`enable01` outside `0`/`1`, and invalid ranges return `VN_INVALID_ARG`.
`rampMs` and `fadeMs` are milliseconds. Per-voice position/orientation is for
unattached voices; use `HarkenEmitter` or `Emitter` for an attached voice.
Unity `Transform` values should pass through `UnitySpatial` rather than be
copied into raw `VnVec3` coordinates.

On stereo output, centered mono preserves its original sample level in each
channel in both 2D and 3D. In non-mono mix layouts its equal-power pan law has
total squared gain `2`: stereo center is `(1, 1)`, hard pan or a surround center
speaker can reach `sqrt(2)`. Native mono mix remains unity. Allow headroom for
these gains; no automatic normalization is applied. Stereo channels remain independent through source processing and voice
effects. At Width `0`, the point image combines them with a `1/sqrt(2)` weight
per input before equal-power panning. Correlation and phase cancellation remain
audible; there is no content-dependent loudness normalization. At Width `1`,
the two stereo columns reach the front-left/right endpoints with those same
weights. Use Mix `0` to restore the original stereo image. Mix does not bypass
distance attenuation, the cone or filtering.

Live pan, Width and Mix updates share one 10 ms matrix transition. Initial
starts and park returns install the current image immediately. Retargeting
begins at the last rendered image; a source that produces no frames does not
advance that matrix transition.

`Playback.StopAll(uint fadeMs)` requests an atomic stop for all voices and
Action firings. Authored state and registered content stay loaded. The call
preflights capacity and can return `VN_BACKPRESSURE`; it does not release a
caller-owned `ActionFiring`, Pack request, or component.

`Voice.SetFilter` remains public. `SetSend` and `ClearSend` also remain public
compatibility operations that change a voice's send routing; `SetSendLevel`
only adjusts an existing send. These exceptions do not expose Bus topology
construction to game code.

## Runtime mixing of existing buses

The `HarkenEngine` calls below require an initialized engine. Bus IDs are
`VnObjectId`; the master is `VnConstants.VN_BUS_MASTER` in the generated ABI.
These controls adjust existing buses in the loaded publication. Authored Bus
topology and filters are not public Unity game-code mutation APIs.

| Signature | Contract |
| --- | --- |
| `VnResult SetBusVolume(VnObjectId busId, float volumeDb, uint rampMs)` | Mix-wide gain in dB; affects direct output and post-fader sends. The first volume assignment snaps regardless of `rampMs`. |
| `VnResult SetBusOutputVolume(VnObjectId busId, float volumeDb, uint rampMs)` | Independent gain on the main output toward the parent/final mix; sourced sends are unchanged. |
| `VnResult SetBusMute(VnObjectId busId, uint mute01, uint fadeMs)` | Mute/unmute without replacing the bus's authored or API volume. Uses the ordinary Bus Volume ramp rules, including the first-assignment snap. |

Volumes must be finite; `mute01` is `0` or `1`. Invalid values return
`VN_INVALID_ARG`, and an unknown Bus ID returns `VN_UNKNOWN_ID`. Gain changes
begin at a block boundary. Ordinary
Bus Volume's first assignment snaps even for a positive ramp; later positive
ramps interpolate in linear amplitude. Bus Output Volume is independent and
honors a positive ramp on its first assignment too. Internal Bus creation,
topology, filter, solo, and raw-handle filter helpers are not consumer entry
points. See the SDK [public API policy](../../../API_SURFACE.md).
