# Segment 与 Track 创作现状

| 编辑面 | 当前能力 |
|---|---|
| Segment Timeline | 以秒为单位的绝对时间轴，新建时初始范围为 0–10 秒；对齐多条 Track |
| Track Timeline | 片段移动、跨轨移动、长度编辑、多选拖动、吸附；Segment 内轨道默认高度 112px，右侧控件统一缩放轨高并滚动；独立 Track 的波形高度随窗口适配 |
| 刻度 | 仅秒轴，时间使用 `MM:SS`，按刻度精度附加 0–3 位小数；网格和吸附使用同一组刻度 |
| Cursor | 定位、拖动、吸附、精确输入、边缘滚动、取消拖动和视图状态恢复 |
| 视口 | 起点固定为 0，Cursor 与 Clip 起点不能小于 0；多选左移以最早片段为边界 |
| Segment 结束 | 以内容结束为准，没有 Entry／Exit 标记 |

## 不提供的能力

Tempo、Time Signature、小节刻度、Marker、Splice by Marker、Transition、Action quantize 均无编辑入口、命令和工程字段，Studio 打包不再产生对应数据。Engine 与 Pack 布局保留。

## 旧工程读取

| 数据 | 读取结果 |
|---|---|
| `transitions`、`spliceByMarker`、`markers`、Action `quantize` | 丢弃；非默认内容记录日志 |
| Musical 的 `segmentTime` | 拒绝打开且不改文件。片段以 tick 保存，与 Segment 分属不同文档，直接丢弃会改变片段时间 |
| 工程模板 | 与工程使用相同的读取规则 |
