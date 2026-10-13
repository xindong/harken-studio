# Studio 日志原句

最近核对：2026-08-23。

定位证据：

- `HarkenStudio/Source/MainWindow.cpp`
- `HarkenStudio/Source/Core/AuditionManager.cpp`
- `HarkenStudio/Source/Core/ProjectManager_RuntimeSync.cpp`

`[Audio]` 与 `[RuntimeSync]` 分类有意义。按对象名和时间把相邻行配对。

## 试听 / Preview

| 日志 | 实际发生了什么 | 去哪查码 |
|---|---|---|
| `Preview queued while Runtime synchronizes: NAME` | 点击已接受，修订屏障还不是 Ready，起播推迟 | 还不是 `VnResult` |
| `Waiting for Runtime synchronization: NAME` | 同上，状态栏 | 还不是 `VnResult` |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -301).` | 屏障等待碰到 `kPreviewRevisionTimeoutMs`，Preview 背压重试耗尽，或 Preview 返回 `16` 被改写 | 日志名为 `VN_BACKPRESSURE`；经常**不是**引擎队列满 |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -303).` | Runtime 不可用、session/epoch 不一致、屏障 Failed，或 Preview `HOST_ERROR` 被改写 | 日志名为 `VN_NOT_INITIALIZED` |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -200).` | Preview 非 OK 且不是背压/宿主错误，经 `previewEngineResult()` 压扁 | 日志名为 `VN_UNKNOWN_ID`；先找回真实 `VnPreviewResult` |
| `Play triggered: NAME` / `Playing: NAME` | 起播已提交，没有等待 | 成功路径 |
| `Cannot play 'NAME': the Container is empty.` | Studio 预检，对象没送进 Runtime | 没有引擎码 |
| `Bus topology is still synchronizing; preview is not ready.` | `PlaybackFailure::TopologySyncPending` | 不是 `VnResult` |
| `Bus topology failed for 'NAME' (engine result N).` | 拓扑应用被拒绝 | `N` 是真实 `EngineResult` |
| `Playback was refused for 'NAME' (engine result N).` | 立即拒绝，没有同步等待那句 | `N` 是真实 `EngineResult` 或被压扁的 Preview 结果 |

## Runtime 同步 / 内容

| 日志 | 实际发生了什么 | 接着看 |
|---|---|---|
| `Sound 'NAME' has no runtime preview: authored Sources semantics (multi-source, trim, loop region, or schedule offset) are not representable by the current runtime` | `syncSound` 返回 `0`，该 Sound 不进入 Current World | 随后对这个 Sound 或父 Container 的 Preview 可能超时成 `-301` 或压成 `-200`。这一行是内容原因，后面的引擎数字是症状 |

## 如何读一串 `-301` Preview 日志

常见形状：

1. 若干 `[Audio] Preview queued while Runtime synchronizes: …`
2. `[Audio] Playback failed … (engine result -301).`
3. 有时同一对象或相关对象还有 `[RuntimeSync] Sound '…' has no runtime preview: …`

读作：Studio 把 Preview 排在 Current World 屏障后面，屏障没有在时限内变成成功起播，并且/或者创作对象从未进入世界。不要先去加 voice 上限或再套一层重试。

顺序：

1. 正在跑的 `HarkenStudio.exe` 是否就是正在读的这棵树。
2. 具名对象（或子 Sound）是否被 Sources semantics 跳过。
3. 是否有更新的创作修订顶掉正在等待的那一版。
4. Runtime 是否被重建（epoch）或工程 session 是否变了——那些失败是 `-303`，不是 `-301`。
5. 这之后才把 `-301` 当成现场 `VN_BACKPRESSURE` / Preview `16`。
