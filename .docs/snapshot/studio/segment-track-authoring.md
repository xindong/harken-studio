# Segment、Track 与 Transition 创作现状

## 分支状态

2026-11-09，Container Transition 开发成果固定到独立分支 `dev/feat/container-transition`，承接 `feat/transition-fade-timeline` 的提交 `4543ee97a8693cfef629b40b170d74958b113450` 及当时全部未提交改动。当前阶段单独保留，暂不合并 `main`；本分支的 Transition 创作能力与执行边界以本文为准。

## 恢复来源与命名

当前仓库首个提交 `f543365` 已包含音乐创作限制，因此恢复依据来自旧仓 `venus-studio`：`321c4045b` 限制音乐时钟，`f0db0af41` 隐藏标记、调度时机及 Transition 入口。

实现沿用当前 `Segment`、`Marker`、`spliceByMarker` 命名，以及当前对象身份、Selection 模式、公共表格、主题、撤销与属性写入流程。旧仓的 Piece、Cue 只用于历史追溯。

## 编辑能力

| 编辑面 | 当前能力 |
|---|---|
| Segment Timeline | 新建默认 Musical；Timeline 边框外的独立参数行提供 Tempo、Time Signature；无时钟域切换，Tempo 输入框不带单位；左侧 chevron 提供 Seconds、Measure-Beats、Seconds with Measure-Beats、Measure-Beats with Seconds，默认单行 Measure-Beats |
| Track Timeline | 继承父 Segment 时钟域；Segment 内轨道默认高度为 112px，保留手动高度；右侧竖向控件统一缩放所有轨道并滚动轨道列表；独立 Track 不提供竖向缩放控件，波形高度随窗口适配；缩小至原最小高度附近时 Clip 只保留标题及原有上下留白，最小轨高为标题高度加留白，跳过波形准备、加载和绘制；重新拉高恢复波形；保留片段长度编辑、多选拖动、跨轨移动与缩放 |
| Marker | 新建 Segment 预置 Entry／Exit；Entry 在起点，Exit 在初始时间轴末尾；12px 手柄带位于刻度轴底部，与左侧头部共用完整轴高和背景；数值区短刻度与手柄带网格统一使用更明显的 borderTick 配色，上下同色连续，不加横分割线；12×10px 双握槽手柄顶部对齐，网格连续穿过手柄带，竖杆与手柄同色连续；Entry 黄色、Exit 红色；手柄与画布整根竖杆均可拖动，支持悬停读数、拖动浮窗、精确输入、边缘滚动、取消、删除、补建、吸附与撤销；Custom 创建及编辑能力隐藏 |
| Cursor | 默认显示绿色；15×12px 对称倒三角带亮色轮廓，与贯穿刻度轴及轨道的竖线共用物理像素中心；零点处完整显示手柄；支持定位、拖动、吸附、精确输入、边缘滚动、取消拖动和视图状态恢复 |
| 时钟域转换 | 同时转换 Track 片段边界与 Marker；尽量保持实际秒数，音乐坐标按整数 tick 舍入；一次撤销恢复原数据 |
| Queue Container | Splice by Marker；仅 Random／Sequence 的 Queue 显示此选项 |
| Transition | Selection Editor 的 Transition 分区（与 Content 分区采用 Explorer 的面板分区方案：标题居中、分区间可拖动调高、可收起，两张表均铺满分区并预留滚动条区域），仅在过渡表生效时显示（Queue，或 Live 响应的 Match）；一行一条规则，列为 # / Source / Exit / Target / Entry / 展开；Source／Target 左侧 chevron 菜单可选 `* (Any)` 或 Select Child...（任意端显示弱化的 `*`），默认规则两端固定为 `*`；Exit 为 Immediate／Natural End／Next Beat／Next Measure／Exit Marker，Entry 为 Initial Position／Entry Marker，新建与默认规则均为 Exit Marker + Entry Marker；所有选项与开关始终可选，端点无法满足时显示警告样式，tooltip 与 Information 说明兜底（无音乐时钟按 Immediate、无 Exit Marker 按 Natural End、无 Entry Marker 按 Initial Position、无限循环时 Natural End 不会发生，Any 端取决于实际子对象）；展开后编辑 Source Tail、Target Lead-In（默认开启，仅在 Exit Marker／Entry Marker 实际生效时播放，否则视为 0 秒并警告）及 Fade 预览（锚点居中，宽度取淡变最远端的两倍、无淡变时左右各 1 秒，并以颜色标出 Tail／Lead-In 覆盖段）；点击 Fade 在 External Editor 打开过渡时间轴：外框与滚动缩放条沿用 Timeline，中央锚点为交接点，上轨 Source、下轨 Target，Sound 显示波形，可拖动淡出／淡入的起点、时长与曲线，未播放部分以灰幕表示；Fade 默认关闭，非默认即视为开启；Tail／Lead-In 与淡变偏移目前仅 Studio 存储，打包与引擎尚未消费；Bridge 已停用，Studio 不再存储或打包 |
| Transition 时钟引用 | 按 source、组织层级与 Clip placement 查找时钟和标记；Blueprint 显示引用路径，适配概览缩放 |
| 自动化入口 | `set_item_property` 支持 `segmentTime`、`markers`、`clips`、`spliceByMarker`、`transitions`，与 GUI 共用校验及撤销 |

Seconds 与 Measure-Beats 为单行模式，不预留次轴高度；默认 Measure-Beats。Seconds with Measure-Beats 与 Measure-Beats with Seconds 分别沿用原 Time、Clock 双行模式，前者以时间为主轴，后者以音乐小节为主轴。双行按同一主刻度成组显示，两值均存在时绘制短横线。时间使用 `MM:SS`，按刻度精度附加 0–3 位小数；Measure-Beats 只显示整数小节号，不显示拍号或小数。主轴使用 Body 字体及 textPrimary 颜色，画布网格及吸附随主轴排布；双行模式的主轴位于下行，上行显示该位置对应的另一种数值，使用较小的 Axis 字体及 textMuted 颜色，不绘制次轴刻度线。Seconds 主刻度只有精确落在小节起点时才显示 Measure-Beats 次值，其余只显示时间，避免重复小节号或将小节号放在错误位置；Measure-Beats 主轴上方显示小节起点的准确时间。上下行按各自字体高度紧凑排布，次轴不预留刻度线空间。数字均从主刻度右侧开始，双行按较宽数字成组避让；右边缘空间不足时省略整组，不将文字左移。固定精度下不重复绘制同一时间标签，仍保留细分刻度与吸附。切换不改写 Clip／Marker 坐标、Tempo 或 Time Signature，保留缩放、滚动与 Cursor，并随面板视图快照保存。加号左对齐，与轴左侧 chevron 均使用 Studio 的 20px 标准。轴左侧不显示刻度模式标题，Marker 行不显示标题或手柄旁的 Entry／Exit 文字；颜色和手柄表达标记类型，悬停时显示名称及 Time／Clock 读数。

Marker 与 Cursor 的交互对齐 Source Editor：整根可见竖杆使用水平拖动指针并联动高亮，覆盖 Clip 和下部空白画布的部分同样可抓取；按下保持抓取偏移并显示实时读数浮窗，双击或右键 `Enter Value...` 可输入位置。Cursor 的刻度区、Marker 带与画布共用状态；重叠时 Cursor 优先，Marker 高于 Clip，零点 Entry 仍可从手柄右侧抓取。Marker 拖动期间只改预览，松手一次提交，Esc 取消；Entry／Exit 的交互边界遵循非负起点及 Entry 不晚于 Exit。Exit 精确输入可超出已有内容末尾，提交后扩展时间轴范围并将 Marker 移入视图，不要求预先添加更长的 Clip。

Measure-Beats 刻度及网格区分小节、拍、拍内细分三档。细分只能从单拍长度二分，最小为十六分音符（960 PPQ 下为 240 tick），继续放大也不再细分。Measure-Beats 的刻度间距阈值为原阈值的两倍，默认从 8px 提到 16px；Seconds 主轴保持原来的生成规则。缩小至无法辨认单拍时使用完整小节倍数，不二分整个小节，避免奇数拍号生成偏离拍点的网格；拍长短于十六分音符时同样使用满足下限的小节倍数。标尺、画布网格和吸附使用同一组音乐坐标。

新建 Segment 使用 Tempo 120、Time Signature 4/4，初始时间轴恢复原来的 0–10 秒，因此 Entry 为 0、Exit 为 19200 tick。初始范围按秒换算到音乐坐标，不再误用 10 拍。音乐时间使用 960 PPQ 整数 tick，目前仅支持 tick=0 的一个速度点和一个拍号点；Tempo／Time Signature 变化不重写音乐坐标。新建 Transition 规则默认 Exit Marker + Entry Marker，Next Beat／Next Measure 需可达的音乐时钟才生效，否则按 Immediate 执行。旧项目迁移不属于本轮范围。

Action 调度继续隐藏，Timing、Reference、Absent Behavior 不对外提供编辑入口，不属于本次恢复及后续开发范围。

Timeline 视口起点固定为 0，Cursor 与 Clip 的编辑起点不能小于 0。多选 Clip 向左移动时以最早片段为边界，整组停止并保留相对间距；精确输入与外部拖入遵循同一边界，公共 Clip 编辑接口拒绝负起点。Entry 之前的内容可放在 0 与 Entry 之间，不依赖负轴；底层有符号坐标与序列化合同不变。

Seconds 主轴按实际主刻度精度统一显示 0–3 位小数：整秒显示 `00:00`，十分之一秒显示 `00:00.0`，百分之一秒显示 `00:00.00`，毫秒显示 `00:00.000`，刻度间距使用对应文字宽度。Measure-Beats with Seconds 的时间对照标签省略尾随零，同时保留非整 Tempo 位置所需的毫秒精度。

## 执行边界

创作数据可以保存，但实际试听、同步和打包仍由当前能力诊断决定。

| 内容 | 当前编译链 |
|---|---|
| 固定 BPM／拍号的 Musical Segment | 已有编译支持 |
| Queue 的 Marker 拼接 | 已有 MarkerAligned 下沉 |
| Transition Immediate／Natural End | 已有基本转场支持，仍需满足目标与模式约束 |
| Transition Next Beat／Next Measure／Exit Marker | 可编辑，当前编译链拒绝执行（未改动的默认表不进入打包） |
| Transition Initial Position | 可编辑，当前编译链拒绝执行 |

此次恢复不扩展引擎合同，也不恢复旧版轨头布局或废弃对象名称。

## 验证与查看

恢复功能此前已构建 Debug 与 RelWithDebInfo；相关测试去重后 161 项通过，局部 QSS 检查通过。隔离示例工程完成保存、关闭、重新打开，确认音乐时钟、Marker、Clip、Transition 及 Splice 数据往返一致；明暗主题和窄窗口布局已检查。

2026-11-04 的调整已隐藏 Action 调度及 Custom Marker 编辑，重排时钟参数、恢复 Cursor、预置 Entry／Exit，并修正默认十秒范围和 Marker 手柄。`build-nc/bin/RelWithDebInfo/HarkenStudio.exe` 与测试程序构建成功，运行依赖及 Schema 快照已部署。相关测试 59 项通过，1 项符号链接测试因环境不支持跳过；隔离工程的 8 张明暗主题宽窄窗口截图确认新建范围为 0–10 秒、手柄完整显示、颜色及 Cursor 连续线正确。未操作用户运行的 Studio。后续每次修改均重新构建 RelWithDebInfo。

同日加入 Time／Clock 双轴后，已再次构建 RelWithDebInfo，Timeline、Source Editor 和 Transition 相关测试 29 项全部通过，局部 QSS 检查通过。隔离工程完成 18 张截图检查，覆盖默认 120／4/4、123／7/8、明暗主题、宽窄窗口、两种主轴和独立 Track；确认主轴网格及吸附、20px 控件、视图快照恢复和最大缩放下刻度映射。验证记录位于 `.temp/segment-dual-axis-tests.xml` 与 `.temp/segment-dual-axis-review/review.json`。

随后完成文字清理、下行主轴、`HH:MM:SS` 展示、仅绘制主轴刻度线及数字位于刻度右侧的调整。最新一轮收紧行距后，默认标尺高度从 44px 降至 37px，并修复 Clock 缩小时二分奇数小节造成的偏拍网格、补齐小节／拍／细分三层视觉区分。RelWithDebInfo 已重建，30 项相关回归测试全部通过。隔离检查包含 44 张截图及 28 组跨拍号缩放检查，覆盖长时间范围、小数秒、跨小时边界和每拍约 6px 的 7/8 场景。最新测试记录为 `.temp/segment-clock-grid-tests.xml`，截图与报告更新于 `.temp/segment-dual-axis-review/`。

用户进一步确定 Clock 只显示整数小节号后，时间改为 `MM:SS.mmm`，两种数值以主刻度坐标成组排列并加入短分割线，标尺仍为 37px。随后移除 Time 模式重复的所属小节号，只有真实小节起点与时间刻度重合时保留次标签及分割线。RelWithDebInfo 再次构建成功，30 项相关回归测试通过；44 张截图和 28 组网格检查确认配对坐标、整数小节、毫秒进位及宽窄布局，并验证时间转 Tick 舍入不会误标小节起点。最新测试记录为 `.temp/segment-ruler-labels-tests.xml`，截图与报告继续位于 `.temp/segment-dual-axis-review/`。

Clock 随后限定最小十六分音符并将间距阈值加倍。RelWithDebInfo 已重建，30 项相关测试通过；隔离检查确认最大缩放仍满足 240 tick 下限，默认宽窗口 Clock 刻度由 81 个降至 41 个，123／7/8 宽窗口由 83 个降至 42 个。最新测试记录为 `.temp/segment-clock-density-tests.xml`。

轨道默认高度调整为 112px，并限制 Cursor／Clip 起点和视口原点为 0 后，RelWithDebInfo 已重建，26 项 Timeline 相关测试与 31 项主题测试全部通过。回归覆盖多选越界拖动的预览、吸附、提交与撤销重做，以及 Cursor 的跨时钟域定位和视图恢复。隔离检查的 4 张明暗宽窄截图确认默认轨高和零点布局；手动高度恢复保持不变。记录位于 `.temp/segment-track-boundary-tests.xml`、`.temp/segment-track-boundary-theme-tests.xml` 及 `.temp/segment-track-boundary-review/`。

Marker 手柄带重新对齐 Source Editor，并加入时间标签自适应精度后，RelWithDebInfo 已重建，30 项 Timeline／Source Editor 相关测试全部通过。隔离检查生成 9 张截图，覆盖明暗宽窄窗口、手柄悬停及 0–3 位小数尺度；像素检查确认 12px 手柄带、12×10px 顶对齐手柄、竖杆与网格跨行连续。记录位于 `.temp/segment-marker-alignment-tests.xml` 及 `.temp/segment-marker-alignment-review/`。

随后将底部 12px Marker 区并入刻度轴，默认完整轴高为 49px，左侧头部及 20px 按钮同步使用完整高度，取消中间分隔线。Cursor 改为 15×12px 对称倒三角与统一物理像素中心的竖线，零点手柄可完整显示并从左半部拖动。RelWithDebInfo 已重建，30 项相关测试通过；隔离检查覆盖 Time／Clock、明暗宽窄布局及 100%／125%／150%／200% 显示缩放，共 24 张截图。记录位于 `.temp/segment-cursor-header-tests.xml` 及 `.temp/segment-cursor-header-review/`。

补齐 Marker／Cursor 整杆交互与 Source Editor 风格的提示后，RelWithDebInfo 已重建，32 项 Timeline／Source Editor 回归测试通过。测试覆盖 Marker 的手柄带、Clip 上方和下部空白画布，以及 Cursor 的刻度区、Marker 带和画布；验证共享预览、单次提交撤销、Esc 取消、精确输入及零点重叠命中。隔离检查的 12 张明暗主题截图确认悬停提示、拖动浮窗和精确输入完整可读。记录位于 `.temp/segment-handle-interaction-tests.xml` 及 `.temp/segment-interaction-review/`。

2026-11-05 将剩余创作能力整理为正式 PR，基于包含 Tooltip 修复的 `main`（`6ca194a7`）合并验证，保留主分支的 Effect 与 Blueprint 更新。`windows-ninja-clang-dev` / `RelWithDebInfo` 的 Studio 与测试程序构建成功；Timeline、Transition、Inspector、自动化命令、主题、保存往返及 Queue Sound／Track／Segment 导出播放相关 99 项回归全部通过，无跳过。本地 QSS 归属检查及其 6 项脚本测试通过。缺少音乐时钟时的提示改为引导选择可访问音乐时钟的 Source，不再要求使用已移除的时钟域切换。记录位于 `.temp/segment-track-pr-build.log`、`.temp/segment-track-pr-tests.log` 和 `.temp/segment-track-pr-tests.xml`。

同日修复 PR 审查发现的 Exit 精确输入上限，并修正旧 Piece 测试夹具的字段重命名及完整 Marker 集合校验。RelWithDebInfo 的 Studio 与全部 Studio CI 测试目标构建成功；完整 PR 测试分组共 145 组、501 项，500 项通过，1 项符号链接检查因环境限制跳过。新增回归覆盖两种时钟域下空 Segment 的末尾延长、放大后的可见性、撤销重做及输入边界，当前 schema 9 的文档往返用例也通过。额外执行的历史 schema 7／8 资格用例仍在 Main Layer 的 `fieldGroups.shared.mainOutput` 迁移验证处失败；相关迁移代码与主分支相同，旧项目迁移继续不纳入本轮修复。记录位于 `.temp/pr251-review-build.log`、`.temp/pr251-review-focused.xml` 和 `.temp/pr251-review-gate/summary.json`。

2026-11-05 将轨道最小高度收拢为 Clip 标题加原有上下留白，并跳过收拢状态的波形准备与加载。RelWithDebInfo 的 Studio 和测试程序构建成功，22 项 Timeline、表格生命周期及主题布局回归全部通过。记录位于 `.temp/compact-track-build.log`、`.temp/compact-track-tests.log` 和 `.temp/compact-track-tests.xml`。后续修改完成后均构建 RelWithDebInfo。

同日将右侧竖向控件移至 Segment，统一缩放轨高并代理轨道列表滚动，独立 Track 改为随窗口高度适配。RelWithDebInfo 已构建，22 项相关回归通过；明暗主题隔离验证确认两轨 112／168px 同步放大至 140／210px、最小高度保留 20px 标题及上下各 1px 留白，独立 Track 窗口增加 320px 时波形可用高度同步增加 320px。滚动、鼠标锚点、取消手动调整及视图恢复验证通过。记录位于 `.temp/segment-vertical-zoom-final-build.log`、`.temp/segment-vertical-zoom-tests.xml` 及 `.temp/segment-vertical-zoom-review/`。

同日将刻度模式扩展为 Seconds、Measure-Beats 及两种双轴组合，默认改为单行 Measure-Beats。RelWithDebInfo 的 Studio 与测试程序构建成功，运行依赖及 Schema 已部署，27 项相关回归通过。隔离验证确认单轴 34px、双轴 49px，同主轴的网格及 401 个位置的吸附结果一致，四模式快照恢复、Cursor／Marker 几何及明暗宽窄布局通过。记录位于 `.temp/axis-modes-final-build.log`、`.temp/axis-modes-tests.xml` 及 `.temp/segment-axis-modes-review/`。
