# Engine 结果码

最近核对：2026-09-01。

本册是 Runtime / Preview / Studio 日志共用的结果码知识集。它会过期：以代码为准，发现失真就改表，而不是让文档默默作废。

路径缩写：`Runtime/` = `HarkenEngine/Source/Runtime/`，`Preview/` = `HarkenEngine/Source/Tooling/Preview/`，`Studio/` = `HarkenStudio/Source/`。

Agent 提示词不在本目录。需要自动匹配日志时，用 gitignore 下的本地 Skill 引用本册，不要把码表再抄进 `.claude/skills`、`.agents/skills` 或 `.github/skills`。

## 文档

| 篇 | 内容 |
|---|---|
| [engine.md](engine.md) | 引擎现状、Source / Tool / Test 目录边界与两类远程通道 |
| [vn-result.md](vn-result.md) | `VnResult` 数值、分段、无符号打印 |
| [preview-result.md](preview-result.md) | Preview ABI 正整数、`MapStart` / `MapControl` |
| [studio-logs.md](studio-logs.md) | Studio 日志原句、Preview 被压成引擎码、`-301` 读法 |

内部宿主和管道客户端位于 `HarkenEngine/Tool/`，验收入口位于 `HarkenEngine/Test/`；共享数值表仍以 `Source/Tooling/Remote/wire/harken_wire_table.json` 为准。Studio 与游戏的正式远程连接由 `Runtime/remote/` 实现，不使用该内部管道客户端。

## 怎么读一个数字

1. 留下整行日志和对象名，不要只抄数字。
2. 先分层，再查表。
3. 用当前源码核对本册，不要信任记忆值。

| 层 | 类型 | 成功 | 典型失败 | 日志样子 |
|---|---|---|---|---|
| Runtime / SDK / 桥接 | `VnResult` / `EngineResult` | `0` | `-1xx` 参数，`-2xx` 身份，`-3xx` 容量/状态，`-4xx` 设备与内容 | `engine result -301` |
| Preview ABI | `VnPreviewResult` | `0` | `1`…`16` 的正整数 | Preview status `16` |
| DirectWorld 预览装载 | `VnCurrentWorldLoadStatus` | `0` | 另一套从 `1` 起的正整数 | 预览直装报告，不是 `engine result`，也不是第三套磁盘合同；游戏加载 `.pack` |

`VN_BACKPRESSURE` 是引擎 `-301`。`VN_PREVIEW_BACKPRESSURE` 是 Preview `16`。Studio 会把 Preview `16` 改写成引擎 `-301` 再打日志。第三方头文件里的同号常量与 Harken 无关。

## Studio 把 Preview 压成引擎码

定位：`Studio/Core/AuditionManager.cpp` 的 `previewEngineResult()`。

| Preview 状态 | 日志里的 engine result | 名字 |
|---|---|---|
| `VN_PREVIEW_BACKPRESSURE` (`16`) | `-301` | `VN_BACKPRESSURE` |
| `VN_PREVIEW_HOST_ERROR` (`11`) | `-303` | `VN_NOT_INITIALIZED` |
| 其它非 OK（含 unknown id / not supported / invalid arg） | `-200` | `VN_UNKNOWN_ID` |

因此日志 `-301` 不一定是命令队列满，日志 `-200` 也不一定是引擎 `VN_UNKNOWN_ID`。原始 Preview 码要查 Preview/Runtime 侧记录。

超时常量（核对源码，不要硬记）同文件：

- 修订屏障等待：`kPreviewRevisionTimeoutMs`（当前 5000 ms），到期仍报 `-301`
- Preview 背压重试：`kPreviewStartRetryLimit` × `kPreviewStartRetryIntervalMs`（当前 500 × 10 ms），耗尽仍报 `-301`

## 看到 `-301` 时怎么分

1. 同一对象刚打过 `Preview queued while Runtime synchronizes`，约 5 秒后失败：修订屏障没在时限内变成 Ready。
2. 同时出现 `Sound '…' has no runtime preview: authored Sources semantics …`：该 Sound 没有进 Current World。后面的失败是症状。
3. 屏障已 Ready，立刻或短重试后仍 `-301`：才更接近真正的 `VN_BACKPRESSURE` / Preview `16`。拒绝边界必须零部分提交。
4. 第二次点击就成功：第一下是时序/同步缺口，不是可接受 UX。

普通编辑如果经常靠重试才能出声，问题在 Studio 的 revision 合同，而不是再加一层重试。

## 权威源

| 内容 | 文件 |
|---|---|
| `VnResult` 名字、分段注释 | `Runtime/api/include/harken/Harken.h` |
| 生成用数值表 | `HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json` |
| C# 镜像 | `HarkenSDK/Unity/Harken/Runtime/Generated/HarkenTypes.g.cs` |
| Preview 码 | `Preview/include/harken_preview/PreviewAbi.h` |
| Preview 起播映射 | `Preview/src/PreviewAbi.cpp` `MapStart` / `MapControl` |
| Studio 透传常量 | `Studio/RuntimeBridge/PlaybackBridge.h` |
| Studio 日志与超时 | `Studio/MainWindow.cpp`、`Studio/Core/AuditionManager.cpp` |
| Sources 无法预览 | `Studio/Core/ProjectManager_RuntimeSync.cpp` |
