# VnPreviewResult

最近核对：2026-08-23。

权威源：`HarkenEngine/Source/Tooling/Preview/include/harken_preview/PreviewAbi.h`。
起播/控制映射：`HarkenEngine/Source/Tooling/Preview/src/PreviewAbi.cpp`（`MapStart`、`MapControl`）。
Studio 压成引擎数字：`HarkenStudio/Source/Core/AuditionManager.cpp` `previewEngineResult()`。

这些是**非负小整数**，不是 `VnResult`。

## 表

| 值 | 名字 | 典型来源 |
|---:|---|---|
| 0 | `VN_PREVIEW_OK` | 接受 / 控制成功 |
| 1 | `VN_PREVIEW_INVALID_ARG` | 参数或结构非法；`PlayableStartStatus::InvalidInput` |
| 2 | `VN_PREVIEW_STRUCT_SIZE_MISMATCH` | 描述符 `structSize` 不是当前二进制接受的 vintage |
| 3 | `VN_PREVIEW_NOT_SUPPORTED` | `PlayableStartStatus::CapabilityUnavailable` |
| 4 | `VN_PREVIEW_UNKNOWN_HANDLE` | 对未知 preview handle 做控制 |
| 5 | `VN_PREVIEW_STALE_HANDLE` | 对过期 preview handle 做控制 |
| 6 | `VN_PREVIEW_NO_PROGRAM` | Preview program 缺失 |
| 7 | `VN_PREVIEW_INSTANCE_CAPACITY` | Preview 实例容量 |
| 8 | `VN_PREVIEW_DEVELOPMENT_API_DISABLED` | `HARKEN_DEVELOPMENT_PREVIEW` 关闭 |
| 9 | `VN_PREVIEW_PROGRAM_MISMATCH` | Preview program 不匹配 |
| 10 | `VN_PREVIEW_RESULT_CAPACITY` | 结果槽容量 |
| 11 | `VN_PREVIEW_HOST_ERROR` | 宿主/Runtime 不可用（`NotInitialized`、`RuntimeRecreateRequired`、未映射失败）。Studio 日志为引擎 `-303` |
| 12 | `VN_PREVIEW_EFFECT_CAPACITY` | Preview 效果容量 |
| 13 | `VN_PREVIEW_RESULT_EXPIRED` | Preview 结果过期 |
| 14 | `VN_PREVIEW_UNKNOWN_ID` | `UnknownPlayable` 或 `MediaMismatch`。Studio 日志为引擎 `-200` |
| 15 | `VN_PREVIEW_NO_VOICE` | `PlayableStartStatus::Capacity` |
| 16 | `VN_PREVIEW_BACKPRESSURE` | 起播或控制背压。Studio 日志为引擎 `-301` |

## MapStart（起播）

| `PlayableStartStatus` | Preview 码 |
|---|---|
| `Accepted` | `VN_PREVIEW_OK` |
| `InvalidInput` | `VN_PREVIEW_INVALID_ARG` |
| `UnknownPlayable`、`MediaMismatch` | `VN_PREVIEW_UNKNOWN_ID` |
| `Capacity` | `VN_PREVIEW_NO_VOICE` |
| `Backpressure` | `VN_PREVIEW_BACKPRESSURE` |
| `CapabilityUnavailable` | `VN_PREVIEW_NOT_SUPPORTED` |
| `RuntimeRecreateRequired`、`NotInitialized` | `VN_PREVIEW_HOST_ERROR` |

## MapControl（停止 / 查询）

| `PlayableControlStatus` | Preview 码 |
|---|---|
| `Ok` | `VN_PREVIEW_OK` |
| `InvalidHandle` | `VN_PREVIEW_UNKNOWN_HANDLE` |
| `StaleHandle` | `VN_PREVIEW_STALE_HANDLE` |
| `Backpressure` | `VN_PREVIEW_BACKPRESSURE` |
| `NotInitialized` | `VN_PREVIEW_HOST_ERROR` |

## Playable kind 与 lifecycle（不是错误码）

Kind：Sound `0`，Container `1`，Track `2`，Piece `3`。

`PreviewAbi.h` 里的 `VN_PREVIEW_LIFECYCLE_*`、`VN_PREVIEW_REASON_*` 描述实例状态，不是起播失败码。
