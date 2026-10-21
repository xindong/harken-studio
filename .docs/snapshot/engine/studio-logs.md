# Studio 日志原句

最近核对：2026-08-23。

定位证据：

- `HarkenStudio/Source/MainWindow.cpp`
- `HarkenStudio/Source/Core/AuditionManager.cpp`
- `HarkenStudio/Source/Core/ProjectManager_RuntimeSync.cpp`

`[Audio]` 与 `[RuntimeSync]` 分类有意义。按对象名和时间把相邻行配对。

## Log 与 Toast 的职责边界

2026-10-21：Log 承接需要记录、查询的运行事件和任务结果；Toast 承接无需留存的临时操作反馈。同一次播报不得同时写入 Log 和显示 Toast，Toast 组件不依赖日志接口。

- 当前只有 Blueprint 的用户交互层使用 Toast。无效连线等预校验拒绝显示临时反馈；导入、导出及修改提交失败进入 Log。数据源和程序调用只返回操作结果，不直接弹提示。
- `Widgets/Common/ToastPresenter` 统一负责展示、主题、定位和生命周期。调用方只传入消息，不区分等级，不自行实现浮层，不配置时长。实例由所属视图持有，无全局主窗口绑定。
- 提示挂载在画布外框，与滚动 viewport 并列，作为固定覆盖层显示于 Blueprint toolbar 下方的可操作画布顶部中央。平移、缩放不改变提示位置；随视图尺寸更新并避开左侧嵌入电平表。无左侧等级亮条，总计显示 3 秒，包含 200ms 淡入和 300ms 淡出；新提示替换旧提示并重新计时。隐藏或关闭视图后清除，不抢焦点、不拦截鼠标。
- 非 Blueprint 的临时校验反馈使用已有字段提示或状态栏；需要保留的文件、资源、打包信息写入对应 Log 分类，已有日志的调用点不再追加相同播报。
- Studio 底部信息栏不再重复显示 Remote 状态。左侧日志摘要使用 Log 的等级配色，悬浮显示手形光标，点击打开或激活已有 Log 面板；普通状态更新恢复 Info 配色。
- 实测入口：在 Blueprint 将一个普通节点的输出 pin 拖到另一个节点的输出 pin，松手后显示连接方式无效的 Toast，Log 不增加该条提示。

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
