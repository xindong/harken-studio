# Development Preview ABI allocation

The preview ABI is development-only and physically isolated from the production
runtime export, SDK, and ordinary Remote surfaces. Status values are append-only.

## Entry points

| Entry | Meaning |
|---|---|
| `VnPreview_PlayPlayable` | Starts a typed Sound or supported Container root from the current world. |
| `VnPreview_StopPlayable` | Stops the whole root with the Runtime parent-stop semantics. |
| `VnPreview_QueryPlayable` | Reads the fixed logic-owned lifecycle state of a root. |
| `VnPreview_CapturePlaybackPaths` | Reads the paths of currently rendering playable-owned voices, including Action and preview starts. |

The removed Segment/Advance surface has no compatibility aliases. Preview time
advances only through normal Runtime update and audio processing.

## Status space

| Value | Name | Produced | Meaning |
|---:|---|:---:|---|
| 0 | `VN_PREVIEW_OK` | yes | Success. |
| 1 | `VN_PREVIEW_INVALID_ARG` | yes | Invalid pointer, kind, flags, reserved field, stable id, or handle tag. |
| 2 | `VN_PREVIEW_STRUCT_SIZE_MISMATCH` | yes | A structure is not the exact current vintage. |
| 3 | `VN_PREVIEW_NOT_SUPPORTED` | yes | A known kind or mode is not executable in this unit. |
| 4 | `VN_PREVIEW_UNKNOWN_HANDLE` | yes | A tagged token does not name a Runtime root row. |
| 5 | `VN_PREVIEW_STALE_HANDLE` | yes | The Runtime root has retired. |
| 6 | `VN_PREVIEW_NO_PROGRAM` | no | Pinned legacy allocation. |
| 7 | `VN_PREVIEW_INSTANCE_CAPACITY` | no | Pinned legacy allocation. |
| 8 | `VN_PREVIEW_DEVELOPMENT_API_DISABLED` | yes | Development Preview is disabled. |
| 9 | `VN_PREVIEW_PROGRAM_MISMATCH` | no | Pinned legacy allocation. |
| 10 | `VN_PREVIEW_RESULT_CAPACITY` | yes | The playback path buffer is too small; requiredCount reports the current full count. |
| 11 | `VN_PREVIEW_HOST_ERROR` | yes | Runtime is unavailable or the exception firewall caught a failure. |
| 12 | `VN_PREVIEW_EFFECT_CAPACITY` | no | Pinned legacy allocation. |
| 13 | `VN_PREVIEW_RESULT_EXPIRED` | no | Pinned legacy allocation. |
| 14 | `VN_PREVIEW_UNKNOWN_ID` | yes | The current-world identity or required media is missing or mismatched. |
| 15 | `VN_PREVIEW_NO_VOICE` | yes | Playable, voice, effect, send, or related fixed capacity is exhausted. |
| 16 | `VN_PREVIEW_BACKPRESSURE` | yes | A retryable page, owner, command, or stop admission refused atomically. |
| 17 | `VN_PREVIEW_REVISION_CONFLICT` | yes | The current-world generation changed before preview start admission. |

## Frozen structures

`VnPreviewPlayPlayableDescV2` is 32 bytes with 8-byte alignment.
`VnPreviewPlayableStateV2` is 64 bytes with 8-byte alignment. The library owns
a complete literal offset table. Play clears a non-null output handle before
validation; Query writes the caller buffer only after a successful Runtime read.

`VnPreviewPlaybackPathV1` is 1192 bytes with 8-byte alignment. Capture takes a
caller-owned array and reports the required row count, including when capacity
is zero. A capacity refusal may fill the available prefix; callers consume a
snapshot only on success. Null output with nonzero capacity is invalid.

Paths retain the resolved definition and occurrence identities used to install
each voice, ordered from outer ancestor to Sound. An occurrence row identifies
its owner and stable ordinal; definition rows use UINT32_MAX. Output and Send
identify the installed Sound routes. Voice instance values are generational
observation identities and are not public playback-control handles.

Capture is passive and externally synchronized with other logic calls. Each
voice's activity comes from audio-published state with the same render generation.
Preparing, cancelled, retired, and raw PCM voices are excluded; draining voices
remain present while rendering. The capture covers the configured render capacity
independently of the legacy snapshot prefix. It does not start a Monitor capture,
advance playback, or substitute selection decisions for rendered activity.
