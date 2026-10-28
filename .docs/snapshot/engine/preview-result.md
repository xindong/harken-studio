# VnPreviewResult

| 值 | 名字 | 含义 |
|---:|---|---|
| 0 | `VN_PREVIEW_OK` | 起播接受或控制成功 |
| 1 | `VN_PREVIEW_INVALID_ARG` | 参数或结构非法 |
| 2 | `VN_PREVIEW_STRUCT_SIZE_MISMATCH` | 描述符 `structSize` 与当前接受的结构版本不匹配 |
| 3 | `VN_PREVIEW_NOT_SUPPORTED` | 所需能力不可用 |
| 4 | `VN_PREVIEW_UNKNOWN_HANDLE` | 控制请求使用未知 preview handle |
| 5 | `VN_PREVIEW_STALE_HANDLE` | 控制请求使用过期 preview handle |
| 6 | `VN_PREVIEW_NO_PROGRAM` | Preview program 缺失 |
| 7 | `VN_PREVIEW_INSTANCE_CAPACITY` | Preview 实例容量耗尽 |
| 8 | `VN_PREVIEW_DEVELOPMENT_API_DISABLED` | 开发预览能力关闭 |
| 9 | `VN_PREVIEW_PROGRAM_MISMATCH` | Preview program 不匹配 |
| 10 | `VN_PREVIEW_RESULT_CAPACITY` | 结果槽容量耗尽 |
| 11 | `VN_PREVIEW_HOST_ERROR` | 宿主或 Runtime 不可用、需要重建，或发生未映射失败 |
| 12 | `VN_PREVIEW_EFFECT_CAPACITY` | Preview 效果容量耗尽 |
| 13 | `VN_PREVIEW_RESULT_EXPIRED` | Preview 结果过期 |
| 14 | `VN_PREVIEW_UNKNOWN_ID` | Playable 未知或媒体不匹配 |
| 15 | `VN_PREVIEW_NO_VOICE` | 起播 voice 容量耗尽 |
| 16 | `VN_PREVIEW_BACKPRESSURE` | 起播或控制背压 |

表注：本表为非负 Preview 状态值，独立于 `VnResult`；Studio 日志转换见 [Studio 日志](studio-logs.md)。

## 相关枚举

| 类别 | 值或范围 | 含义 |
|---|---|---|
| Playable kind | Sound `0`、Container `1`、Track `2`、Piece `3` | 实例对象类型 |
| Lifecycle | `VN_PREVIEW_LIFECYCLE_*` | 实例生命周期状态，不是起播失败码 |
| Reason | `VN_PREVIEW_REASON_*` | 实例状态原因，不是起播失败码 |
