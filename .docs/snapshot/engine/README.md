# Engine 快照

## 分册

| 篇 | 内容 |
|---|---|
| [引擎能力](engine.md) | 渲染、媒体、观测、对象参数与公开能力 |
| [VnResult](vn-result.md) | 引擎结果码数值与含义 |
| [VnPreviewResult](preview-result.md) | Preview 结果码与实例类型 |
| [Studio 日志](studio-logs.md) | 日志含义、Preview 结果映射与反馈边界 |

## 结果码族

| 层 | 类型 | 成功 | 失败范围 | 输出形式 |
|---|---|---|---|---|
| Runtime / SDK / 桥接 | `VnResult` / `EngineResult` | `0` | `-1xx` 参数、`-2xx` 身份、`-3xx` 容量与状态、`-4xx` 系统与内容 | `engine result -301` |
| Preview ABI | `VnPreviewResult` | `0` | `1`…`16` | Preview status `16`；Studio 日志会转换为引擎码 |
| DirectWorld 预览装载 | `VnCurrentWorldLoadStatus` | `0` | 从 `1` 起的小正整数 | 预览直装报告；独立于前两类结果码 |

表注：DirectWorld 是预览装载信封，游戏加载 `.pack`，不构成另一套磁盘合同。
