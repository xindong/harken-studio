# HarkenStudio Inspector 表格现状快照

最近核对：2026-10-08。全量表格规范见 [tables.md](tables.md)，对象树批量打开 Inspector 的行为见[对象右键菜单快照](object-context-menus.md)。

## 范围与清单

范围为 Sound、Container、Layer、Track、Piece、Bus、Action、Attenuation、Variable 九类对象的 Inspector 主面板。External Editor、Timeline、Grid 等独立编辑面不计入。

共 **9 类表格、14 处对象展示位置**。位置按 `Category > Section` 表示，category 在界面中呈现为 tab；列名采用界面文本。

| 对象 | 字段名称 | Inspector 字段标识 | Category > Section | 列名 | 数据来源 |
|---|---|---|---|---|---|
| Sound | Sources | `sources` | `Basic > Source` | # / Audio / Wave / Active | `VSound.sources` |
| Container | Selections | `selections` | `Basic > Selection` | # / Child / Weight | `VSelection.selections` |
| Track | Clips | `clips` | `Basic > Clip` | # / Child | `VTrack.clips` |
| Action | Steps | `steps` | `Basic > Step` | # / Type / Target / Delay / 无标题展开列 | `VAction.steps` |
| Attenuation | Dimensions | `dimensions` | `Basic > Dimension` | # / Property / Curve | `VAttenuation.dimensions` |
| Variable | Labels | `labels` | `Basic > Label` | # / Name | `VVariable.labels` |
| Sound、Container、Layer、Track、Piece、Bus | Sends | `sends` | `Route > Send` | # / Target / Volume | 五类可继承对象：`ProjectItem.fieldGroups.send`；Bus：`VMixNode.sends` |
| Bus | Inputs | `inputs` | `Route > Input` | Source / Volume | `ProjectManager::collectMainInputs()`，主输出路由的反向查询 |
| Bus | Receives | `receives` | `Route > Receive` | Source / Volume | `ProjectManager::collectIncomingSends()`，Send 路由的反向查询 |

按对象计数：Sound 2、Container 2、Layer 1、Track 2、Piece 1、Bus 3、Action 1、Attenuation 1、Variable 1。

Container 的 Weight 列仅在 Random 模式显示：Play Type = Single 且 Discipline = Draw 时为频率权重，Shuffle 或 Queue 时为顺序权重。Inputs、Receives 是只读派生表。Sends、Inputs、Receives 的 Volume 均以 dB 显示。

Action 的 Set Variable 步骤详情包含 Value 与 Scope（Global / Emitter，默认 Emitter），序列化类型名为 `SetVariable`，写入作用域由每个步骤独立保存。Target 继续使用 ObjectRefWidget；更换 Target 不重置 Scope。Variable 对象本身不声明、不显示、不保存 Scope。试听面板的 Scope 属于试听会话，默认 Emitter；恢复默认值按当前试听 Scope 赋值，不清除单个对象的覆盖。本项核对：2026-09-21。

表格列宽随当前面板宽度弹性分配，不显示横向滚动条。弹性列使用共享的列宽拖动下限，不以内部控件的最小建议宽度限制列宽；面板空间不足时继续收缩，恢复宽度时保留用户调整的比例。

所有表格均可选择行，无整行悬停高亮。七类配置表的 `#` 列提供拖动排序、右键上下移动及 Alt+上/下方向键移动。普通表格显示行序号；Variable Labels 显示真实整数索引，包括 0、非零起始与稀疏位置。排序经过现有编辑命令，支持撤销和重做；Sources 仅在同一语言组内排序，保持 Active 引用；Clips 保持时间位置与 occurrence 身份；Labels 的身份与名称随普通插入式移动进入原有索引位置，隐藏项不变。只读继承值、混合平台值及其他禁止编辑状态沿用原有约束。

Variable Labels 的 `#` 支持双击编辑 index：只接受当前 range 内、未被其他行占用的整数，空 Name 的行也占用 index；不合法输入显示红框且不能提交。Enter 或失焦提交，Esc 取消；修改保留 Label GUID 与 Name，按 index 升序排列，选择跟随同一 GUID，支持一次撤销及重做。修改后允许留下空缺，不自动补行或重编号；加号仍补当前 range 内最低的空闲 index。切换对象、range、编辑权限或外部数据变化会取消旧 index 草稿；相同数据刷新保留草稿。编辑期间 `#` 列临时扩宽，退出后按实际索引文字恢复自然列宽。

Inputs、Receives 明确声明为展示表，不含 `#`、配置行移动或增删 footer；行顺序由查询结果提供，支持视觉列换位，不改写路由。它们与 Inspector 配置表统一按内容展开，不设高度上限，内部无水平或垂直滚动条，由 Inspector 页面负责滚动。字段描述、UI 与导出的 schema 共同表达表格分类。

## 字段与布局

- 字段标识用于定位属性，字段名称用于表达其业务含义，section 用于组织展示位置。每个 Inspector 根字段都有唯一标识、非空名称和已注册的 section。
- section 的 category、显示标题和顺序由 `InspectorLayout` 定义。category 与 section 是展示元数据，不写入 `.vp`；tab 是 category 的界面呈现形式。
- 表格是独立的 `PropType::List` 字段，与普通数值字段共同进入 Inspector schema。UI 表头、字段上下文和 IPC 导出的列描述使用同一份字段描述。
- 所有表格均省略表格上方的独立 label，与字段名称是否等于 section 标题无关。字段名称仍用于 schema、可访问名称和字段上下文；非表格字段显示表单标签。
- 控件行的表格字段放入 `TableWidget::createCell(parent)` 创建的共享单元格，通过 `TableCell::addWidget` 声明控件；默认左右各 `table.cellPaddingX=4`、上下至少各 `table.cellPaddingY=2` 逻辑像素，字段垂直居中，随主题更新。单元格内控件间距为 `2 * cellPaddingX=8`，主行排列各列的 layout 保持零边距、零间距，避免重复留白。
- `TableCell` 使用普通横向布局，同一单元格内可伸展的业务控件默认 `stretch=1` 等分可用宽度；装饰图标、紧凑勾选框等使用 `stretch=0`，明确限宽的单编辑器可保留原有业务对齐。行号由 `createRowNumber` 创建，纯文字列可直接登记，由表格统一应用外层留白。
- 原生 model/item 表体的编辑器，以及 `setIndexWidget`、`setItemWidget` 插入的控件，由 `TableItemDelegate` 统一设置 X/Y 留白和垂直居中，直接插入编辑器即可，不再套 `TableCell`。编辑行高度至少容纳共享字段高度与上下留白。
- 单元格规则只管理外层容器，不改 ObjectRef、数值控件或曲线缩略图的内部 margin，不递归处理引用控件内部结构和展开详情。Child / Weight、Variable / Curve 等列使用同一外层留白规则，详情内部仍由其字段布局负责。
- slider 与 value 编辑器共享 `ParameterBinding`，各自保持独立编辑能力；常规 presenter 可将两者组合，业务布局可以分别放置。试听 Variables 使用仅含 slider 的 host、独立 `ParameterValueEditor` 和 Scope 下拉框三个兄弟控件等分 Value 单元格，不能通过把前两者绑定成不可拆的复合控件来分配宽度。
- slider 不再显示跟随手柄的 Label 浮标，Step 的名称由选择框显示。Variable Binding 与混合曲线共用 Variable X 轴配置：Step 使用 Label、整数刻度和阶梯预览，Smooth 使用正常浮点刻度与连续曲线；切换 kind 会刷新已打开的曲线编辑器而不改写曲线，Label 配置与 SDK 名称解析继续适用于两种 kind。
- Source 中的 Language（`distinguishLanguage`）位于 section 标题右侧，未勾选时显示普通音源，勾选时显示分语言音源；项目没有配置语言时禁止开启，已开启时仍可关闭并恢复普通音源，两组音源数据均保留。语言列表变化后同步更新可操作状态。它与 Selection 中的 Mode（`mode`）、Behavior（`behavior`）均为独立字段。标题右侧控件统一保留主题的小间距，与 Inherit 对齐。
- 表格列可以表达数据、状态或操作入口。Sources 的 Active 使用标准勾选框表达选择状态，同一平台与语言内只选一个音源，多平台选择不一致时显示半勾选；Wave 是候选音频引用的波形与范围编辑入口；Steps 的无标题列是行详情展开入口。

## 数据归属与持久化

`VSound`、`VSelection`、`VTrack`、`VPiece`、`VAttenuation` 等类型是挂载在 `ProjectItem` 上的业务组件。表格字段的存储、派生方式和编辑行为由业务模型决定，section 只组织其 Inspector 展示。

Container 的四种模式共享唯一的 `selections` 数据。Random/Sequence 的简单条目表位于 Inspector，Match 的复杂条件矩阵仍位于 Selection Editor。矩阵读写当前平台 selections 中的条件与轴顺序，切换模式保留已有数据。

Match 矩阵的列结构为 `# | Target | Variable…`，空表保留一个空 Variable 表头。`#` 是真实序号列，单独承载行选择底色，并通过公共表格交互拖动整行、提示插入位置和撤销移动；Target 是普通对象引用字段，变量表头支持独立列选择，与行选择互斥。多变量表头通过 ObjectRef 左侧独立拖动柄复用 Display Table 的列移动动画；变量和整列条件一同移动，改变匹配优先级，支持撤销，`#` 与 Target 保持固定。ObjectRef 的定位、导航、选择、对象拖入和右键菜单保持可用，列边界继续调整比例宽度。当前胜出行的条件单元格显示预览底色，重复与不可达条件的行号显示警告色，单元格及 Target 提示说明原因和阻挡行号；预览读取变量覆盖值或默认值，不追踪实际播放实例。条件值通过可输入的下拉框编辑，通配值为 `*`（下拉项为 `* (Any)`）；范围超过 64 个整数时只列出标签，其余整数值直接输入。

Blend 模式的 Selection Editor 编辑当前平台 `selections` 条目中的混合曲线，不另存混合成员名单。混合表无表头，外层固定宽度 `#` 列承担整行选中与拖动排序，高亮仅限该列；控制区为标准 Variable ObjectRef 和 Child 副表，右侧为带标题容器的混合曲线画布。横轴是变量数值，纵轴是混合百分比。底部加减号管理混合行，副表加减号管理 child，控制区和画布按比例调整宽度，每行可独立调高。曲线的精确坐标输入沿用公共 DecimalSpinBox、数字字体、对齐与提示规则，未改动的数值不因显示精度而改变。首尾点以外为 0%，标题拖动只平移横坐标。

### 继承字段范围

`FieldGroup` 按业务字段集合定义继承范围，持久化键为 `fieldGroups`，工程的 `propertyModel` 为 `fieldGroupsV1`。Inherit 是独立持久化的继承状态，section 仅承载对应控件。

| 字段范围 | 管辖字段 | Inherit 控件位置 |
|---|---|---|
| `mainOutput` | `mainOutputBusRef`、`outputBusVolume` | `Route > Main Output` |
| `send` | `sends` | `Route > Send` |
| `spatialRating` | `spatialPanning`、`spatialIs3DEnabled`、`spatialAttenuationRef`、`spatialAttenuationDistanceScaling` | `Spatial > Rating` |

## 实现定位

| 事实范围 | 源码与合同 |
|---|---|
| 字段描述、section/category、表格标签与列名 | [PropertyDescriptor.h](../../../../HarkenStudio/Source/Core/PropertyDescriptor.h)、[InspectorLayout.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/InspectorLayout.cpp)、[PropertyEditorEngine.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/PropertyEditorEngine.cpp)、[PropertySchemaJson.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/PropertySchemaJson.cpp) |
| Sound 候选与选择状态 | [VSound.h](../../../../HarkenStudio/Source/Core/Components/VSound.h)、[SoundSourceSemantics.cpp](../../../../HarkenStudio/Source/Core/SoundSourceSemantics.cpp)、[SoundSourceEditing.cpp](../../../../HarkenStudio/Source/Core/SoundSourceEditing.cpp)、[AssetRegistry.cpp](../../../../HarkenStudio/Source/Core/AssetPipeline/AssetRegistry.cpp) |
| Container 选择表与编辑 | [SelectionData.h](../../../../HarkenStudio/Source/Core/SelectionData.h)、[VSelection.cpp](../../../../HarkenStudio/Source/Core/Components/VSelection.cpp)、[SelectionSectionProvider.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/Providers/SelectionSectionProvider.cpp) |
| Attenuation 字段、身份与编辑 | [VAttenuation.cpp](../../../../HarkenStudio/Source/Core/Components/VAttenuation.cpp)、[AttenuationStore.cpp](../../../../HarkenStudio/Source/Core/AttenuationStore.cpp)、[AttenuationInspectorSchema.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/AttenuationInspectorSchema.cpp) |
| Clips、Steps、Labels 与组件标识 | [VTrack.h](../../../../HarkenStudio/Source/Core/Components/VTrack.h)、[VAction.h](../../../../HarkenStudio/Source/Core/Components/VAction.h)、[VVariable.h](../../../../HarkenStudio/Source/Core/Components/VVariable.h)、[ComponentCatalog.cpp](../../../../HarkenStudio/Source/Core/ComponentCatalog.cpp) |
| Sends 与 Bus 派生表 | [SendsFieldSchema.h](../../../../HarkenStudio/Source/Widgets/Inspector/SendsFieldSchema.h)、[VMixNode.h](../../../../HarkenStudio/Source/Core/Components/VMixNode.h)、[BusInspectorSchema.cpp](../../../../HarkenStudio/Source/Widgets/Inspector/BusInspectorSchema.cpp)、[ProjectManager.cpp](../../../../HarkenStudio/Source/Core/ProjectManager.cpp) |
| 继承范围与持久化合同 | [FieldGroups.h](../../../../HarkenStudio/Source/Core/FieldGroups.h)、[FieldGroupJson.cpp](../../../../HarkenStudio/Source/Core/FieldGroupJson.cpp)、[ProjectSerializedSchema.json](../../../../HarkenSchema/Editor/current/ProjectSerializedSchema.json) |
| Inspector 字段与表格展示合同 | [IpcSchemaCoverageTest.cpp](../../../HarkenStudio/Test/UnitTest/IpcSchemaCoverageTest.cpp)、[InspectorSectionWidgetTest.cpp](../../../HarkenStudio/Test/UnitTest/InspectorSectionWidgetTest.cpp) |
