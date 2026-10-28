# Studio 日志与反馈

## 反馈边界

| 项目 | 当前状态 |
|---|---|
| Log | 保存可查询的运行事件与任务结果；导入、导出、资源、打包及修改提交失败归入对应日志分类 |
| Toast | 仅用于 Blueprint 用户交互中的临时反馈，如无效连线；同一播报不同时进入 Log；数据源和程序调用只返回结果 |
| Toast 展示 | 位于工具栏下方画布顶部中央，避开左侧电平表，不随平移缩放移动；显示 3 秒，含 200 ms 淡入与 300 ms 淡出；新提示替换旧提示，隐藏或关闭视图时清除，不抢焦点或拦截鼠标 |
| 其他临时反馈 | 非 Blueprint 校验使用字段提示或状态栏 |
| 底部信息栏 | 不重复展示 Remote 状态；日志摘要按等级配色，点击打开或激活 Log 面板；普通状态恢复 Info 配色 |

## Preview 日志结果映射

| Preview 状态 | 日志中的 engine result | 日志码名称 |
|---|---|---|
| `VN_PREVIEW_BACKPRESSURE` (`16`) | `-301` | `VN_BACKPRESSURE` |
| `VN_PREVIEW_HOST_ERROR` (`11`) | `-303` | `VN_NOT_INITIALIZED` |
| 其他非 OK 状态，包括未知身份、能力不支持与非法参数 | `-200` | `VN_UNKNOWN_ID` |

表注：转换会丢失原始 Preview 错误类别；`-301` 也可由同步等待或重试超时产生，不能仅凭日志数字判断引擎队列已满。

## 试听与同步日志

| 日志或状态提示 | 含义 | 结果性质 |
|---|---|---|
| `Preview queued while Runtime synchronizes: NAME` | 点击已接受，修订屏障尚未就绪，起播推迟 | 尚无引擎结果码 |
| `Waiting for Runtime synchronization: NAME` | 状态栏显示等待修订屏障 | 尚无引擎结果码 |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -301).` | 屏障等待超时、Preview 重试耗尽，或 Preview 背压 | 日志结果可能合成或转换 |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -303).` | Runtime 不可用、会话或运行代际不一致、屏障失败，或 Preview 宿主错误 | 日志结果可能合成或转换 |
| `Playback failed for 'NAME' after waiting for Runtime synchronization (engine result -200).` | Preview 失败且不属于背压或宿主错误 | 转换后的结果，原始类别未保留 |
| `Play triggered: NAME` / `Playing: NAME` | 起播已提交，无需等待 | 成功 |
| `Cannot play 'NAME': the Container is empty.` | Studio 预检拒绝，未提交 Runtime | 无引擎结果码 |
| `Bus topology is still synchronizing; preview is not ready.` | 总线拓扑尚未同步完成 | 非引擎结果码 |
| `Bus topology failed for 'NAME' (engine result N).` | 拓扑应用被拒绝 | `N` 为原始引擎结果 |
| `Playback was refused for 'NAME' (engine result N).` | 立即拒绝起播 | `N` 为原始引擎结果或转换后的 Preview 结果 |
| `Sound 'NAME' has no runtime preview: authored Sources semantics (multi-source, trim, loop region, or schedule offset) are not representable by the current runtime` | Sound 未进入 Current World，其自身或父 Container 后续试听可能超时或失败 | 内容原因；后续 `-301` / `-200` 为结果表现 |

## 试听等待限制

| 项目 | 当前值 | 耗尽结果 |
|---|---|---|
| 修订屏障等待 | 5000 ms | `-301` |
| Preview 背压重试 | 最多 500 次，间隔 10 ms | `-301` |
