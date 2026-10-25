# Studio 公共表格规范与治理清单

最近核对：2026-10-20。本文记录已确认的方案、业务边界及实施状态，供后续表格开发长期借鉴。Inspector 字段细节见 [inspector-tables.md](inspector-tables.md)。

## 职责边界

公共组件提供能力，业务决定如何组合，主题与皮肤负责外观。configTable、displayTable 是两种用途分类及默认模板，不是两套皮肤，也不通过类型强制创建 footer、刷新按钮或决定窗口空间分配。

| 归属 | 职责 |
|---|---|
| 公共 Table | 列布局、选择、行移动、视觉列换位、分组、滚动、可选 footer 等能力的实现 |
| 业务使用者 | 数据、稳定行列身份、编辑器、命令、权限、业务排序、所需能力和高度策略 |
| 表格模板 | 提供配置表、展示表的常用默认组合，减少重复配置 |
| 皮肤与主题 | 字体、颜色、边框、图标、间距、状态绘制及相应尺寸指标 |
| 宿主布局 | 页面滚动、可用空间、表格与其他字段或曲线区的对齐 |

业务声明能力，不修改表格内部 layout、滚动条、原生表头的外观，也不查找 ObjectRef 内部标签调整其颜色、尺寸或文本省略。控件 objectName 可用于定位和测试，不应产生业务专用表格皮肤。

表格只管理行背景、选择、边框与布局，不强制覆写内部控件的皮肤或交互状态。勾选框与 ObjectRef 等字段沿用各自的公共控件样式；普通行与分组勾选框仅在指针进入勾选框自身区域时悬停高亮，同一行内移入、移出均即时刷新，不继承整行悬停。悬停、禁用和三态状态由公共皮肤统一绘制。

## 分类及能力

configTable 编辑当前字段或表单中的配置条目集合。displayTable 枚举已有实体、资源、依赖、查询结果或运行数据，也可以编辑、删除或保存这些实体。是否为空、是否可编辑、是否落盘或是否被动刷新，均不能单独判定类别。

| 能力 | 配置表常用约定 | 展示表常用约定 |
|---|---|---|
| 单元格编辑与保存 | 由业务决定 | 由业务决定 |
| # 列 | 可选；序号与允许移动是不同能力 | 不使用配置序号和移动把手 |
| 末尾详情展开占位列 | 按需声明 | 不使用配置详情占位；分组展开另行处理 |
| 行移动 | 显式启用，业务提供命令及约束 | 结果行通常由数据源或查询排序 |
| 外部拖入 | 按需提供接收校验及提交处理 | 同左 |
| 列宽调整 | 弹性压缩，长内容省略 | 共用列布局，滚动策略按场景声明 |
| 视觉列换位 | 不启用 | 按需启用，不能改变数据字段与业务顺序 |
| 表体高度 | 默认按内容展开；独立编辑面可使用宿主可用高度 | 默认撑满可用空间，也可按内容展开 |
| footer | 不需要就不创建、不占位；按需声明操作 | 同左 |
| 左下刷新 | 不提供手动刷新入口 | 有真实刷新动作时按需提供；自动更新不强制增加刷新 |
| 分组 | 保留 Sources 语言等业务分组 | 支持 Pack General/语言、快捷键分类等 |

“可用”不等于“自动启用”。模板不取代业务配置。高度策略统一为 `Content` 与 `Expanding`：`Content` 无高度上限，容纳全部可见行和展开详情，列宽适配视口，内部永远不显示水平或垂直滚动条，由外层页面负责滚动；表体收到的滚轮事件继续向外层传递，不能因内部滚动条禁用而吞掉事件。`Expanding` 使用宿主可用高度，纵向滚动槽常驻，仅内容溢出时显示滑块。`Expanding` 声明 `Scrollable` 列策略时横向槽也常驻，按溢出显示滑块；`Content` 始终按 `Fit` 布局，切回 `Expanding` 后恢复声明的列策略及原生表格各列独立宽度，不因临时压缩覆盖用户列宽。

外部拖入通过 `TableWidget::setDropHandler` 显式接入，业务提供 MIME 接收校验与提交处理。公共层处理拖放生命周期、Copy 动作及有效目标边框反馈，落下时重新校验，提交成功后才接受操作；支持控件行和原生表体，空表可使用表头及 footer 落点。单元格内已有拖放控件保留自身行为，其拒绝的拖入也不转为表格新增。Container Inspector / Selections 使用此入口批量添加当前 Container 的合法直接子对象，与加号共用添加及平台提交路径，一次拖入形成一步撤销；Child 引用字段仍用于替换，条目重复规则及对象归属不变。

两种高度策略共用空表时的 4 像素表体留白。`Expanding` 的最小高度完整包含当前可见表头、该留白及横向滚动槽，再由外层布局计入 footer 和边框；有数据时也不能将表体压缩到这一留白以下。控件行、原生模型表、树形表及 Matrix 共用高度计算，表头增高或显隐时同步更新，不能只依赖控件行的最小尺寸提示补偿表头。

停靠宿主按内容当前的最小尺寸计算约束，并计入停靠标题栏和外框；不能缓存表格首次加入时的尺寸，也不能由 Control 单独补偿边框。拖动停靠分隔条、缩小主窗口或浮动窗口、恢复布局后，都必须保留表头、完整留白和表格底边，验证时须检查父容器裁切后的实际可见范围。

## 共同外观与样式可替换性

主题数据以 `ThemeSnapshot` 为统一来源。`QtControlSkin` 将快照中的颜色、字体尺寸、间距和资源填入应用级 QSS，由 `ThemeManager` 在启动和主题切换时发布。`dark_theme.qss` 虽保留历史文件名，实际是标准 Qt 控件的集中皮肤模板，仍服务不同主题；它不是待删除的业务局部样式，也不承担完整主题运行时。领域 Renderer、Parameter Presenter 和组件 Recipe 分别负责其他绘制与呈现能力。

`TableWidget` 负责公共表格结构、布局和交互，读取主题度量并声明供皮肤匹配的语义。`QTableView`、`QTreeWidget` 等是其内部原生表体，负责模型数据、单元格、选择及编辑。普通业务通过 `createModelView` 提供模型，或通过 `createItemView` 提供表头和初始列宽比例，由公共层一次完成创建与接入；特殊视图通过 `createCustomView` 工厂接入。原公开 `setView` 已移除，不能再把“业务先创建，再记得适配”作为普通开发流程。Qt 本身仍允许直接创建原生视图，本轮没有引入禁止绕过的代码门禁。导航树及二维 Grid 等专用编辑面按下文边界处理。

通用表头基线直接定义在应用级 `QHeaderView::section`，不依赖 `tableRole` 才取消全大写、加粗和额外字距；其背景、留白、分隔线和字号沿用公共主题值。修改默认外观时更新此基础规则，不保留一套旧默认再靠表格属性覆盖。原生视图漏接时也能得到正确的基础表头，但这不代表它已经具备公共表格的完整能力。业务确需视觉差异时声明语义，由集中皮肤实现。

- 表头与普通单元格使用 Body 字体，默认 10pt。对象图标使用 objectIconExtent，默认 20 逻辑像素。
- 字段高度由 QtControlSkin::fieldHeight 计算，默认基准 24 逻辑像素，随字体需要增高。表格行包含字段高度与留白，不等于字段本身的高度。
- 控件行的单元格通过 `TableWidget::createCell(parent)` 创建，由 `TableCell::addWidget(widget, stretch, alignment)` 放入业务控件。单元格统一读取主题 `table.cellPaddingX`、`table.cellPaddingY`，默认左右各 4、上下至少各 2 逻辑像素，字段在行内垂直居中；内部相邻控件间距为 `2 * cellPaddingX`，默认 8，与相邻两列控件之间的总留白一致。
- `TableCell` 使用普通横向布局。同一单元格内可伸展的业务控件默认 `stretch=1`，扣除留白与间距后等分可用宽度；共享单元格释放这些控件的横向宽度约束。固定装饰图标、紧凑勾选框和行号等不参与等分，放入单元格时显式使用 `stretch=0`；明确限宽的单编辑器也可使用 `stretch=0` 保留业务对齐。行号独立列继续通过 `createRowNumber` 声明；不需要序号时，调用 `createRowHandle(cell, index)` 为共享单元格声明独立六点柄，公共层负责放置，不再通过 `addWidget` 加入字段布局。该单元格与 Matrix 变量表头共用 handle、内容区和选中态绘制规则，handle 与引用控件之间不增加普通字段的 8px 间距；默认 handle 从左侧 4px 开始、宽 22px，引用框从 26px 开始，右侧留白 4px，字段垂直居中。纯文字标签可直接登记为列，由公共表格应用外层留白。
- 原生 model/item 视图中的编辑器，包括通过 `setIndexWidget`、`setCellWidget`、`setItemWidget` 插入的控件，由 `TableItemDelegate::updateEditorGeometry` 应用同一组 X/Y 留白并垂直居中。业务直接插入编辑器，不再包 `TableCell`，避免两次留白和字段高度被压缩；波形预览与引用、输入字段遵循同一规则。原生文字项的留白由公共 QSS 提供。编辑行高度至少容纳 `fieldHeight + 2 * cellPaddingY`，且不小于主题 `table.rowHeight`。
- 限宽编辑器用最大宽度表达上限，并允许随列宽收缩；不能以固定宽度或固有最小尺寸挤出单元格。右对齐和宽度上限不改变这一约束。验收检查父容器裁切后的实际可见范围，不能只检查控件自身的高度。
- `ResizableTableView` 声明 `compositeTable`，用于单元格内承载子表、画布等可伸展内容。公共 delegate 将其嵌入控件铺满单元格，内部布局负责留白，不应用普通字段的固定高度。换主题保留独立行高和最小行高；纵向按像素滚动。选中底色仅绘制在声明的 `#` 列，内部内容保持普通背景。
- 主行排列各列的 layout 保持零边距、零间距，列宽包含单元格留白。业务不得通过修改字段控件自身或内部 layout 的 margin 补偿表格间距，也不得递归改写 ObjectRef 等复合控件内部布局；展开详情保留其自身布局。
- slider 与数值编辑器是共享 `ParameterBinding` 的独立编辑入口，不由复合控件锁定二者的布局。试听 Variables 的 Value 单元格放入仅含 slider 的 host、独立 `ParameterValueEditor` 和 Scope 下拉框，三个兄弟控件分别以 `stretch=1` 等分宽度并使用统一间距。`ParameterValueEditor` 随 Smooth/Step 显示数值输入框或选择框；常规 parameter presenter 可以组合 slider 与 value 的布局。
- 深色行背景、方角、连续分隔线、表头、footer、空表和分组行由公共样式提供，不在业务页面重画。
- 常驻滚动槽以 Settings / Layout 为布局基准：纵向槽从表格内框顶部延伸到表体底部，表头止于槽左缘，整表 footer 位于滚动区之外。控件行可按需创建分组 footer，放在组内行之后并随表体滚动；与整表 footer 复用 ListFooter 的主题、尺寸及动作入口，空组也可提供添加动作，分组 footer 不参与行选择或排序。控件行将表头托管在滚动区的视口上边距内，与原生视图保持相同结构；横纵槽同时存在时使用透明交角。槽位沿用 Explorer 的透明背景和滑块样式，保留槽与表体之间的分隔线，表格外框及交角边线保持连续。普通控件行与原生表体使用相同的表面底色；复合画布保留其自身表面，槽位仍透明。
- 普通行可选择，无悬停底色。焦点、编辑器边界悬停、有效拖放目标反馈各自独立。ObjectRef 悬停仅边界高亮，禁用态无悬停。
- 带 `#` 的配置表将选中底色与强调色竖线限制在 `#` 列，并贯穿主行及其展开内容的完整高度；其他列保持普通行背景。行号及 `#` 表头居中。
- 配置行以单元格内的六点柄代替独立 `#` 时，选中底色和强调线只绘制在承载柄的单元格，其他列保持普通背景；行的选中身份、拖动排序和业务联动仍按整行处理。
- 业务表达 selected、disabled、error、modified、group、dropTarget 等语义，皮肤决定其视觉。领域波形、曲线等内容不属于应删除的局部表格皮肤。
- 皮肤替换应同时覆盖原生 model/view 和控件行。换皮肤不改数据、命令与交互；影响布局的皮肤指标通过共享接口提供，不依赖某个业务 QSS selector 才能工作。
- Inspector 右缘由宿主公共布局处理 columnPadding 与槽位留白，表格核心不感知 Inspector。
- 控件行保留复杂字段编辑能力；大数据展示保持 model/view，不要求每个单元格创建 QWidget。
- 刷新后按稳定行身份恢复选择；视觉列换位后按稳定列身份处理编辑、复制和动作。模型字段顺序不能随用户换位改变。
- 列宽比例转像素时按累计边界取整，保证总宽度守恒且整数宽度可往返恢复。连续拖动只调整目标列和用于补偿的相邻可见列；相邻列达到最小宽度前，其他列的宽度与边界必须不变。不得逐列向下截断并在下一次拖动中重新计入舍入误差；多列表格会因此出现持续左移和抖动。验收需要连续多次鼠标移动、反向拖回、隐藏列及列换位，不能只测试一次 resizeSection。
- 首次打开和自动获得焦点不默认选择第一行。刷新保留未选中状态；主动删除通过公共 `removeSelectedRow` 选择原位置的下一行，删除末行则选择上一行，空表取消选择。控件行通过 `registerRow` 声明稳定 `groupId` 时，删除后的选择限定在同组；该组为空时取消选择，分组和行重建或换序不改变此约束。多选删除保留仍存活的选择，取消选择或切换模型后不恢复旧选择。新增、用户点选或显式定位可以建立选择。平台草稿尚未分配 ID 时使用唯一名称恢复，落盘后使用 ID。
- 表体和 footer 空白、表格内 Esc 支持取消选择；原生单选表也支持 Ctrl 点选已选行取消。取消同时清除当前行与选择，删除等动作必须据此禁用。单元格编辑和 footer 按钮不被当作空白；编辑器先处理自己的 Esc，移动中的 Esc 先取消移动。外部详情或曲线区交互是否取消选择由业务决定。
- 配置表与展示表使用统一分组标题：Sources 语言组、快捷键分类、Pack 的 General/语言分区均由公共 `TableGroupHeader` 绘制，居中加粗、使用主题表头背景和弱化文字、保留下分隔线，无选中态或强调色竖条。控件行通过 `TableWidget::createGroupHeader` 创建标题；原生树形表通过 `ProportionalTreeWidget::createGroup` 声明分组，使用同一绘制及主题高度，跨整行显示，不受列换位影响。业务只提供组名和数据，不拼装标题或维护局部样式。分组表尾按需启用，与下一组标题之间保留主题分隔线，末组使用表格外框收尾；空组标题与表尾的相邻边框合为一条，避免叠线。

## 全量清单

普通功能表格按用途去重共 25 项：配置表 11 项、展示表 14 项。Sends 在六类对象中的使用展开后，共 30 个页面/对象类型展示位置；另列平台标签配置变体。不同对象实例、Binding 字段地址或同一对话框的打开位置不重复计数；Monitor 采用运行记录模型。

| ID | 分类 | 位置 / 用途 | 当前入口 |
|---|---|---|---|
| C01 | config | Sound Inspector / Sources | SourcesSectionProvider |
| C02 | config | Container Selection Editor / Random、Sequence | SelectionListWidget |
| C03 | config | Track Inspector / Clips | ClipsSectionProvider |
| C04 | config | Action Inspector / Steps | StepsSectionProvider |
| C05 | config | Attenuation Inspector / Dimensions | DimensionsSectionProvider |
| C06 | config | Variable Inspector / Labels | ListFieldDrawer |
| C07 | config | Sound、Container、Layer、Track、Piece、Bus / Sends | ListFieldDrawer |
| C08 | config | External Editor / Binding | BindingFieldEditor |
| C09 | config | Transition Editor / Rules | TransitionEditorWidget |
| C10 | 标签配置（非表格） | Project Settings / General / Platform Configuration：左侧平台标签，右侧配置面板 | ProjectConfigPage / ProjectPlatformEditor |
| C11 | 标签配置（非表格） | Project Settings / General / Language Configuration：左侧语言导航与增删，加号通过弹窗新增，右侧 Language 名称详情；无语言时保留右侧面板、内容留空，切换保留草稿，Apply 保存 | ProjectConfigPage |
| C12 | config | Project Platform / Resample Policies | ProjectPlatformEditor |
| D01 | display | Bus Inspector / Inputs | ListFieldDrawer |
| D02 | display | Bus Inspector / Receives | ListFieldDrawer |
| D03 | display | Control / Variables | RehearsalTable |
| D04 | display | Control / Variable Bindings | VariableBindingTable |
| D05 | display | Publisher / Pack | PublisherPanel |
| D06 | display | Pack Structure | PackStructureDialog |
| D07 | display | Remote Connect / Targets | RemoteConnectDialog |
| D08 | display | Settings / Shortcuts | ShortcutsPage |
| D09 | display | Settings / Layout | LayoutPage |
| D10 | display | Monitor / Voices | MonitorVoicesPanel |
| D11 | display | Monitor / Buses | MonitorBusesPanel |
| D12 | display | Monitor / Effects | MonitorEffectsPanel |
| D13 | display | Monitor / Events | MonitorPanel |
| D14 | display | Sources Import / 同名音频冲突选择 | SourcesTreeWidget / sourceImportConflicts |

Inputs/Receives 是路由反向查询，不再适用“所有 Inspector 表都带 #”的旧约定。Shortcuts 枚举命令目录；Layout 枚举已有布局资源，重命名与删除不改变其展示表职责。

主 toolbar 左侧在 Capture 后依次排列平台（含 ALL）与语言，播放控件居中，Layout 位于最右侧。

Layout 的主入口位于 toolbar 最右侧，使用公共 `MenuComboBox` 显示当前布局名称，提供切换、保存当前、重置当前、重置所有和管理入口，菜单栏不再单列 Layout。切换保留各布局的工作状态；重置当前恢复当前预设，重置所有经确认后恢复当前工程内所有布局的已保存状态，均不删除预设。Default 的重置目标为内置默认状态。保存同名布局更新预设，另存名称后切换当前布局身份；管理页重命名会同步工作状态及当前名称，删除当前预设后恢复 Default 的工作状态。共享预设在其他工程中被删除或重命名后，重开原工程会恢复该工程的 Default 工作状态；旧缓存没有 Default 工作状态时恢复内置 Default。用户目录中的命名预设维持原格式；工程 `.cache/editor_layout.json` 保留原当前布局字段，并增加 `workingLayouts` 保存各布局的工作状态，空名称键表示 Default。旧缓存仍可读取，工作状态按工程隔离；这些本地界面缓存不属于 Editor/Pack schema，不需要合同升级或工程迁移。

平台配置没有表头，进入时选中首个平台，选择左侧标签后直接编辑右侧名称、基础平台和转换配置，切换标签保留各平台草稿。平台标签及添加窗口的支持平台列表均通过 `TableWidget::createNavigationView(name)` 创建，使用公共 `footer()`、边框、尺寸与滚动管理，不由业务拼装列表、表脚和滚动条。导航保留当前激活项及 accent 标记，不适用普通表格初始无选择及空白、Esc 清选规则。两处列表按可用高度布局，纵向滚动槽常驻，仅内容溢出时显示滑块，横向不滚动。

左右区域通过零间距横向布局组成整体；右侧公共 `panel` 保留内边距并声明 `joinedEdge=left` 去掉左框，两侧边框均使用 `borderPrimary`，仅以左侧 TableWidget 的右框作为中间分隔线。表脚完整位于 TableWidget 外框内，继承公共控件的布局与样式。策略表仍按内容展开，由外层页面承担滚动。内嵌面板只保留 Reset，修改由项目设置的 Apply 统一提交；Reset 恢复所选基础平台的默认转换配置。左侧加号打开支持平台列表，可预览各平台默认配置后添加。新项目默认按 Windows、Android、macOS、iOS 顺序创建四个平台。

Pack 的 Init 独立显示且始终必选；General 包含不区分语言的 Action，工程中的每种语言各自形成分区，同时显示全部语言，不受 toolbar 当前语言筛选。每个 Action 按平台和语言独立出包，取消自定义合包、成员行、组名编辑及合包拖放。旧工程的合包平台选择迁移到成员 Action。语言变体由完整打包计划决定，即使取消所有平台也保留对应行。每行仅显示当前观察平台下一个包的文件、大小及构建状态，缺失值留空，不再聚合语言或显示横线；发布状态按清单中的 Action 归属核验。

Action targeting 可携带 language：省略时保留整 Action 的平台选择语义，空字符串指定 General，非空字符串指定对应语言。各语言平台设置独立保存，批量勾选通过同一命令提交并支持撤销。正常导出读取各目标平台保存的全部语言选择，并按相同语言展开 PostAction 依赖，被取消的依赖必须报告错误。各语言只编译所选 Action 和公共包的定义，Init 汇总所有已选语言需要的 Layer；若工程默认语言没有被导出，产物以工程语言顺序中的首个已导出语言作为默认入口，工程默认语言不变。Export All 清除快照中的筛选，不修改工程勾选。

Pack 表格的产物信息跟随主 toolbar 的当前平台。平台归属使用公共勾选框皮肤，遵守全局单平台／ALL 编辑语境，ALL 下归属不一致时显示半勾选；保留点击、空格和拖动连续勾选，沿用原有平台命令与撤销语义。表格不提供独立的平台观察切换。底部 Export Platforms 使用公共 `MultiSelectComboBox`，框内显示已选平台名称，与主 toolbar 下拉框一样按 12 字符设置基准宽度，不随选项文字增长，剩余空间留在控件与导出按钮之间；窗口空间不足时可被布局压缩，文字省略并通过悬停展示完整列表。展开列表复用公共菜单绘制，已选项仅显示对钩，支持连续多选。多选指定 Export Checked 和右键导出的目标平台，不限制表格的观察或编辑范围，未选择目标时仍可查看和编辑当前平台。

Export 左侧提供使用公共皮肤的 Force Re-export 和 Self-cleaning 勾选框，Force Re-export 默认关闭，Self-cleaning 默认开启，仅作用于 Export 按钮（含 Export All）。底栏始终保持单行，不约束面板的最小宽度，空间不足时优先压缩平台选择框；未选择任何平台时 Export 主按钮禁用。默认验证已有包并比较内容，未变化的包跳过生成且保留文件时间戳；缺失、损坏或内容变化时重新生成。Force Re-export 强制重新生成本次范围内的包。正常导出按当前选择重建平台与语言清单、Action 发布目录、波形附属数据及平台索引，总索引仅包含本次选择的平台。导出目标目录由导出器整体管理：未勾选 Self-cleaning 时保留退出清单的文件；勾选时，在所选平台全部导出成功后扫描整个目标目录，仅保留本次有效产物，删除其他文件和空目录，包括取消、删除或改名的平台目录、任意后缀文件及 Unity .meta。清理不依赖旧清单或历史导出记录；工程 .cache/pack-inputs 仅按包内容哈希缓存构建输入指纹，可删除重建，不参与清理判断。平台发布与根目录收尾清理均通过发布事务提交并支持失败恢复。

右键提供 Export / Force Re-export，作用于右键点击的 Action 包。Publisher 不提供行选择，常规批量导出范围由勾选框决定。右键忽略底部两个勾选框且始终不清理旧包，只更新当前包及清单中的对应记录，保留其他已发布包、Init 及其记录；首次输出尚无 Init 时创建 Init。依赖内容只作为编译上下文，不自动导出未选包。单包导出用于快速查看变化，不检查或提示新旧包之间的共享内容兼容性；无法为混合状态生成匹配的波形伴随数据时，不输出该伴随文件。投入游戏前使用常规 Export 刷新整套状态。

Init 不包含 Action 发布目录，各语言清单共用平台根目录中的 Init。SDK 从所选清单绑定独立 Action 目录，原生引擎继续校验项目、Init 版本、Action 归属和依赖；常规导出旧发布时移除 Init 中的旧目录，统一语言 Init 引用。

Sources 仅在同语言组内移动；Binding 仅编辑本地贡献；Clips 移动保持 occurrence 身份和时间语义；Labels 的 # 显示真实索引，身份与名称随普通行移动进入原有索引位置，范围外记录不参与。Transition 按匹配具体程度排序，默认规则最后；Resample Policies 按阈值排序且保留 Infinite 行。共享表格仅扩展自定义编号显示，保留原有拖动逻辑。

## 专用编辑面边界

Random、Sequence、Match 的 Selection Editor 行选择可将该行引用的 Child 设为试听目标；编辑器与 Inspector 继续显示当前 Container，不经过全局对象导航。选中只设置目标，播放按钮和播放快捷键使用该目标；Child 本身为 Container 时正常播放该对象自身。用户成功替换已选行的 Child 引用后，试听目标同步为新对象；替换失败、编辑其他行或后台刷新不改变当前试听目标。工具栏只保留播放控制，不显示目标对象；Control 未固定时同步显示该目标的试听参数，固定时保留原绑定，Reset 跟随试听目标。

表内空白、Esc、Match 中 Ctrl 点击已选行或改选 Variable 列等解除行选择的操作，将试听目标恢复为当前编辑器显示的 Container；钻入嵌套对象后恢复的是当前层级，不是最外层绑定对象。点击工具栏、Control 或切换焦点遵循公有表格规则，保留行选择和 Child 试听目标。Hierarchy 中重复点击已选对象也可重新指定目标。绑定、重建、属性刷新和布局恢复不自行取得试听目标；显式进入或返回嵌套 Container 时载入当前显示对象。目标对象或其所属子树删除、工程关闭时清除目标。试听不改变公有选择与拖动规则，选中高亮仍仅出现在 `#` 或承载 handle 的单元格。

子对象试听复核覆盖三种模式的行选择与清选、嵌套编辑层级、编辑面板保持、Control 固定与解除固定、Hierarchy 键盘选择及重复点击、Blueprint 重选、后台刷新、无关目录删除和目标失效。树的实际选中变化独立发布试听请求，被动同步不改变目标。本轮 89 项相关回归全部通过，Release 编辑器与测试目标、RelWithDebInfo 编辑器构建成功，局部 QSS 所有权检查通过。

Match Selection Editor 使用公共 `MatrixTableWidget`，定位与 Config Table 一致。它在 `TableWidget` 基础上提供双维度配置表：托管 QTableWidget、列表头控件和业务列移动请求。真实序号列 `#` 保持主题紧凑宽度，Child、Waveform 与所有条件列通过公共 `TableColumnLayout` 按比例分配剩余宽度，填满可用空间，禁止横向滚动；列边界拖动按相邻列补偿，窗口恢复宽度后保留比例。表体撑满编辑器，行高由主题固定，仅行内容超出时纵向滚动。变量列复用 Display Table 的 `ReorderHeaderView` 动画：拖动表头随指针浮动，相邻表头让位，松手后归位；引用控件随表头移动。完成时先恢复原生视图顺序，再由业务提交变量及整列条件的移动，调整匹配优先级，形成一步可撤销操作；Esc 或刷新表格取消未完成的拖动。序号列、Child 与 Waveform 列固定在左侧，不参与换位。该组件不包含 Variable、Selection 或匹配规则。

Target 单元格和 Variable 表头均使用可编辑的标准 `ObjectRefWidget`，不包含变量试听控件。空表默认四列零行：`#`、`Child`、`Waveform`、一个空 Variable 引用列；空引用只是编辑入口，不写入空 GUID。变量表头支持独立列选择，与行选择互斥；Ctrl 点击已选表头取消选择。控件由公共矩阵层按主题留白定位，左右让出原生列宽拖动区域，随比例列宽及主题切换同步。存在多列变量时，引用控件左侧提供独立的六点拖动柄，悬停显示水平移动箭头，起拖不占用 ObjectRef 区域；列边界显示原生调宽双箭头，调宽期间不切换成移动光标。单击定位、双击导航、选择器、更换引用的对象拖入及右键菜单沿用原有规则。条件单元格使用可输入的下拉框，通配值显示为 `*`，下拉项显示 `* (Any)`，真实名称为 `Any` 的 Label 保留其实际值；范围不超过 64 个整数时列出全部取值，超出时只列出标签，其余整数值直接输入，非整数和越界输入不提交。编辑器沿用 `TableItemDelegate` 的单元格留白，矩阵网格线和表头状态由集中皮肤提供。footer 仅保留右下角标准加减按钮：加号点击弹出菜单，不叠加菜单箭头；菜单提供 New Variable（Smooth / Step）、Select Variable 和 Select Child，减号删除当前所选行或变量列。变量列也可通过列头菜单删除。更换变量保留原匹配值，无法解析或超出新变量范围时拒绝替换。页面默认只有矩阵；双击嵌套 Target 进入其编辑器，并显示返回路径。

Container 的四种选择策略统一保存于 `VSelection::selections`，均在 Selection Editor 编辑。Random/Sequence 使用公共 `TableWidget` 的撑满表体，列为 `# | Child | Weight | Waveform`，Sequence 隐藏 Weight；支持重复 Child、引用替换、增删、拖入及排序。Inspector 保留模式及行为字段，切换 Play Type 或 Discipline 同步刷新权重范围。双击 Container、模式切换、固定窗口及嵌套导航使用统一入口。所有策略视图通过 `set_item_property` 的 `selections` 字段整体提交当前平台数据；Match 条目保存目标与条件，`matchAxes` 保存轴顺序。模式切换保留非活动策略数据，撤销、复制、引用清理及变量消费关系覆盖完整 selections；预览和 Pack 仅消费当前模式与平台。

Selections 与 Sound Sources 共用 `Common/SourceRangeThumb` 绘制音频、裁剪和循环范围。Selections 仅展示目标 Sound 当前平台和语言下的激活源；非 Sound 目标留空，All 下激活条目不一致时显示 Mixed。点击有效波形打开对应 Sound 源条目的编辑器，源数据和上下文变化同步刷新。除固定序号列外，Random 默认比例为 Child 20% / Weight 20% / Waveform 60%，Sequence 为 Child 40% / Waveform 60%，两种模式分别保留列宽偏好。Match 的 Waveform 位于 Child 右侧，默认 Child 25% / Waveform 25% / 首个条件列 50%；新增条件列按同类列权重参与标准分配，不固定条件列总占比。

Child 选择器仅展示当前 Container 及类型匹配的直属孩子，父级用于表达归属且不可选；不展示更高祖先、分类桶或孙级对象。

无法解析的 Label 条件在编辑器中保留 `(missing)` 状态及原 GUID，未经修改的提交不会转为通配值。显式选择通配值、其他 Label 或输入有效整数后才替换条件，撤销可恢复原缺失引用。

矩阵使用原生行选择，选中底色与左侧强调线只显示在真实 `#` 序号列。序号列接入公共 `setRowMoveHandler`：悬停显示上下移动光标，拖动期间显示主题强调色的整行插入线，松开提交一次整行移动，Esc 取消；右键 Move Up / Move Down 和 Alt＋上下方向键使用同一命令。Target 和全部条件作为一行移动，支持撤销；同等匹配优先级下，行顺序决定胜者。移动反馈与匹配预览各自独立。首次打开不自动选择，空白、Esc 或 Ctrl 点选已选行可取消选择，编辑器优先处理自身 Esc；删除行沿用 Config Table 的相邻行选择规则。刷新按变量 GUID 保留列宽比例和当前单元格，同一行结构中的编辑保留行选择。

匹配结果预览读取 Control 的变量覆盖值，未覆盖时读取变量默认值，并应用变量范围与离散读取规则；它不依赖播放开始，也不追踪引擎内已经播放的实例。当前胜出行的条件单元格显示预览底色。条件与更早行完全相同的行，其行号显示警告色：Target 相同为重复行，Target 不同为无法胜出的行。单元格和 Target 提示显示具体原因与阻挡行号，并保留原有值与引用信息；编辑消除冲突后清除警告，撤销后恢复。预览与诊断不改变用户选择、未提交的编辑值或项目数据。

Blend 模式的 Selection Editor 使用无表头的混合表，结构为 `# | Variable 与 Child 副表 | 混合曲线画布`。每行（row）对应一个可选的 Variable；控制区上方是可编辑的 Variable ObjectRef，下方是公共 `Child | Waveform` 副表，不设序号列。画布横轴与 Variable Binding 共用 Variable 轴配置：Smooth 使用数值刻度与连续曲线，Step 使用整数刻度、Label 和阶梯预览；Kind、Label、Range 变化自动刷新，保留曲线原始坐标。纵轴显示 0–100% 混合比例。公共 `ResizableTableView` 承载跨行统一的比例分区宽度及独立行高，控制区与画布的边界、每行底边分别拖动调宽和调高；`#` 为固定宽度。画布不叠加外框，边界由宿主统一绘制；坐标轴保持 1px。分区边线止于最后一行，表格行外空白区与 Child 副表统一使用公共表体的深色主背景 `bgPrimary`，与其他模式一致。嵌套对象继续使用同一编辑器与返回路径。

右下角标准加减号管理混合行：加号菜单为 Add Mixing，添加一行后通过该行的 Variable 引用控件选择变量；减号为 Remove Mixing，移除当前行；外层 `#` 接入公共整行选中、拖动排序、插入位置提示、右键上下移动与键盘操作。整行选中高亮仅限外层 `#`，不覆盖 Variable、Child 副表和画布。每行的 Child 副表在 Child 右侧显示 Waveform，默认比例为 40% / 60%，使用独立的右下角加减号；Child 单元格内采用公共六点行拖动柄与标准 ObjectRef，显示对象名称，保留行选择、拖动排序及整条曲线随成员移动。高亮仅限 Child 单元格，Waveform 保持普通背景；handle、引用框布局及选中绘制与 Match 变量表头共用公共皮肤规则。标准 ObjectRef 支持更换引用、定位和进入对象。更换 Variable 保留曲线数值坐标。所有业务写入通过 `selections` 命令形成撤销步骤；混合行是统一条目中混合曲线的编辑视图，存档字段 `blendAxes` 保存混合行身份、Variable 引用和行顺序，不另存成员名单。删除曲线保留条目的其他策略数据；调宽、调高属于编辑器布局状态。

child 的多条曲线叠加显示，未选中统一为暗青色，所选 child 的曲线为亮青色，色彩来自主题的曲线强调色。每条曲线有带 child 名称的容器，标题区横向拖动整个区间，首尾点可单独移动；曲线下方至 0% 的区域使用渐变填充。容器不绘制贯穿全高的左右边框，首尾曲线点到 0% 轴的辅助线使用 1px 虚线。Step 的实际阶梯结果为实线，原始连续曲线作为淡色虚线辅助编辑；选中与未选中曲线、渐变填充和命中检测均采用相同的离散读取规则。点选标题或曲线与 Child 副表选择联动，编辑继续复用公共点与贝塞尔手柄交互。成员列表和曲线区按比例适应宽度，无横向滚动；多行与较长成员列表使用纵向滚动。试听游标读取 Control 的变量覆盖值，各行增益及跨行相乘后的结果通过 Child 提示查看，试听状态不占用行选择高亮。

曲线支持公共点坐标编辑、插入删除、手柄塑形、复制粘贴与撤销。精确数值输入使用公共 `DecimalSpinBox`，百分比在保存和 Pack 编译时转换为线性增益；未改动的坐标保持原有精度。拖动松开提交一步撤销，取消或切换对象丢弃未提交预览。同一行中的同一 child 只允许一条曲线，跨行引用共用播放、增益相乘，成员之间不自动归一化。

混合表、Curve Editor、Timeline 是领域画布，不套用普通功能表格模板。

Hierarchy、Explorer、Sources、Selector 及 Template Chooser 预览属于导航或选择树。普通 Log 使用文本控件，不算 Monitor 的另一张表。不得通过全局 QTreeView 样式修改这些导航语义。

## 实施与验收

治理顺序：公共能力与两种表体适配；Inspector 分类修正；独立编辑器及项目配置；Audition、Settings、Remote；Pack；Monitor；清理已替代样式并核对全量清单。

- [x] 确认分类、能力配置和皮肤职责边界，建立长期清单。
- [x] 公共表体布局策略与可选 footer，对控件行和原生视图生效。
- [x] 26 项普通功能表格全部接入公共 Table 模板或视图适配。
- [x] 配置表取消内部滚动，保留业务排序；Binding、平台策略由外层页面滚动。
- [x] 展示表按需配置 footer、高度和列换位，移除 Inputs/Receives 的 #。
- [x] Monitor 通过公共选项保留原生列宽调整、横向滚动和现有表头状态恢复。
- [x] Sources、Pack、Shortcuts 使用公共分组；原生表头、行背景、分隔线和语义文字状态收敛。
- [x] 删除 Settings 表格专用皮肤、Binding 专用表框、Pack 专用输入框皮肤及 Audition 对 ObjectRef 内部标签的修改。
- [x] Remote 使用公共左侧刷新入口；Monitor 使用公共 footer 动作与状态位置。
- [x] ListField 混合值提示使用公共表体消息位置，Pack 移除旧文字颜色覆盖。
- [x] 默认编辑器及测试目标构建通过，累计 534 项不同的相关回归通过（整体验证 468 项，收尾针对性复验 110 项，含重复项）。

验收包括空表、多行、展开详情、极窄窗口、恢复窗口宽度、字段禁用/只读、主题切换、行移动约束、列换位后的数据定位、外层滚轮、Inspector 右缘，以及 Sources/Pack/Shortcuts 分组。测试优先扩展现有共享表格和业务回归测试，不以检查源码字符串代替行为验证。

本轮使用 `windows-ninja-clang-dev` 对应的 `build-nc`、`RelWithDebInfo` 构建 `harken_editor` 和 `harken_editor_tests`。回归以 `QT_QPA_PLATFORM=offscreen` 运行，覆盖公共表格、主题、ObjectRef、Inspector、Sources、Binding、Audition、Transition、项目设置、Pack、Remote 和 Monitor；没有启动 Studio 做人工观感验收。构建产物为 `build-nc/bin/RelWithDebInfo/HarkenStudio.exe`。

选择与分区回归补充：默认编辑器及测试目标构建通过，350 项相关回归分批全部通过，包含 8 项新增用例；共享表格和项目设置最后复验 46 项通过。覆盖首次打开与自动设焦、未选中刷新、取消后动作禁用、主动新增与定位、删除后不转选其他行、单元格 Esc 优先取消编辑、footer 操作保留选择，以及 Pack 分区跨整行显示在窗口缩放和列换位后保持。既有语言删除测试改为显式鼠标点选，保留保存与撤销断言。分组样式统一后，Pack 的渲染断言改为与公共控件标题逐像素比较，主题切换用例同步核对控件与原生分组的高度，避免固化业务面板的特殊样式。

列宽回归补充：纯布局、独立原生表格及真实 Pack 的连续拖动均复现了公共像素取整误差，修复位于 TableColumnLayout。新增 3 项回归，覆盖反复拖动时无关列的位置、最小宽度补偿、隐藏列和列换位。默认编辑器及测试目标构建通过，353 项相关离屏回归全部通过。

默认表头回归补充：统一规则下沉到应用级 `QHeaderView::section`，删除 `tableRole` 表头的重复覆盖。新增原生视图与公共适配的离屏绘制用例，修复前复现了裸视图的大写、字重、字距及背景差异；修复后验证正常 Body 字体的文字像素、大小写保留、背景、分隔线和主题切换。默认编辑器及测试目标构建通过，213 项相关回归全部通过，局部 QSS 所有权门禁通过；没有启动 Studio 或截屏。

公共创建 API 补充：4 个 Monitor 模型表体、6 个项目设置/Remote/Pack Structure 条目表体均改由 Table 创建；Pack 的特殊子类通过显式工厂接入，控件行表格沿用既有托管表体。移除公开 `setView`，Catalog 模型切换改走公共 `setModel`。默认编辑器及测试目标构建通过，411 项相关离屏回归全部通过，包含模型替换后的编辑与行高更新、列宽恢复、表体所有权及特殊工厂主题传递；本轮未新增原生控件使用门禁或测试框架。

高度与滚动槽复核：随宿主高度变化的表格共 14 类，包括 Control 两表、Monitor 四表、Publisher、Pack Structure、Remote Targets、Settings Shortcuts、Settings Layout、Match 矩阵、混合主表及 Child 子表。Control 两表和混合 Child 原先将表头放在滚动区外，槽位起点与 Layout 的原生视图不同；公共 Table 已改为在滚动区内部预留表头视口边距，并统一两种表体的实际最小高度约束。槽底色已移除，原有分隔线保留；控件行的宽泛后代样式已收窄，滚动条使用公共皮肤。14 类表格已逐类离屏渲染核对槽位起点、透明绘制、分隔线，以及压缩至最小尺寸后的表头和表体留白；编辑器及测试目标构建通过，267 项相关回归通过，局部 QSS 所有权检查通过。

控件嵌入复核：检查 26 项普通功能表格及 Match、Blend、Timeline 的创建、控件嵌入与尺寸声明。Match 的 Waveform 原先在原生字段内部再次包装 TableCell，24px 控件实际只显示 22px；现改为直接插入字段，与 Child 引用保持同一留白，非 Sound 行保持空单元格。Sources 同名音频导入冲突表补入清单并改用公共展示表；ListField 限宽数值框改用最大宽度并允许收缩。没有新增业务专用皮肤或留白补偿。

本次离屏渲染核对 Match、导入冲突对话框、ListField，以及控件行、原生模型表和树形表内的引用、波形、文本、数值、下拉框及快捷键字段。Match 波形完整显示 24px；实际 ListField 数值框在窄列下由 64px 缩至 37px，仍完整可见。扩展现有回归，覆盖实际可见范围、窄列恢复、主题刷新和非 Sound 留空。Release 编辑器与测试目标、RelWithDebInfo 编辑器均构建成功；213 项相关回归批跑通过 212 项，一项音频替换撤销用例间歇失败。两项批跑中曾失败的音频文件用例分别单独复测三次均通过，旧测试产物也复现了替换用例失败，本次未改动文件事务代码。更窄尺寸的数值框回归单独通过，局部 QSS 所有权检查通过。

## 当前公共入口

`TableOptions` 是值配置，`TableKind` 只表达分类。公共方法不根据类型拒绝 footer、分组或布局能力；上面的分类约定由业务接入和回归保证。

| 入口 | 当前行为 |
|---|---|
| `TableWidget(parent)` / `TableOptions{}` | config 默认组合；内容高度、弹性列宽、无内部滚动，不启用列换位 |
| `TableOptions::display(sizing)` | display 默认组合；默认撑满表体、弹性列宽、启用视觉列换位；调用方可以覆盖各项 |
| `setBodySizing(Content / Expanding)` | 无上限内容展开且无内部滚动，或使用宿主可用高度并常驻滚动槽；不再提供有上限的内容模式 |
| `setColumnSizing(Fit / Scrollable)` | Fit 压缩到视口；Scrollable 仅在 Expanding 下允许横向滚动并常驻横向槽。Monitor 原生视图在 Scrollable 下保留各列独立宽度 |
| `setColumnsMovable(bool)` / `moveColumn(logicalColumn, visualIndex)` | 控件行与原生视图都支持视觉列换位，业务绑定仍使用逻辑列 |
| `setColumns(key, columns)` / `registerRow(row, cells)` | 控件行入口；列 ID、标题、初始比例及必要最小宽度由调用方声明；编辑器仍由业务创建 |
| `createCell(parent)` / `TableCell::addWidget(widget, stretch = 1, alignment = {})` | 控件行使用普通横向布局统一单元格外层主题留白及控件间距；默认等分可伸展业务控件宽度，固定装饰控件或明确限宽的单编辑器使用 `stretch=0`，不改业务控件内部布局 |
| `createModelView(name, model)` | 创建并托管 QTableView；列标题与数据由模型提供，公共层完成表格角色、表头、尺寸和 delegate 接入；模型所有权仍归调用方 |
| `createNavigationView(name)` | 创建并托管无表头的单选 QListWidget；共用 Table 的外框、尺寸、滚动与 `footer()`，保留导航激活项和 accent 标记，不启用普通表格的清选规则 |
| `MatrixTableWidget` | 双维度配置表，继承公共 Table 的 footer、主题、原生行选择和单元格规则；`setHeaderWidget` 托管列表头字段，列宽边界和独立移动柄避让字段控件；复用 Display Table 列移动动画，`setColumnsReorderable` 指定可移动列起点，`columnMoveRequested` 将业务排序交给调用方 |
| `setViewColumns(key, columns)` / `setRowNumberColumn(column)` | 原生 model/item 表体声明稳定列 ID、标题、比例与最小宽度；显式序号列居中，并将行选择底色限制在该列 |
| `TableActiveRole` / `TableWarningRole` | 原生表体通过行首单元格声明命中或警告，公共 delegate 在数据列底部画独立细线，不占用序号列选择底色 |
| `createItemView(name, headers, columnWeights)` | 创建并托管 ProportionalTreeWidget；表头数与初始比例数一致，业务填充条目、分组和编辑逻辑 |
| `createCustomView(name, factory)` | 特殊视图的显式工厂入口；工厂接收宿主 QWidget 和当前 ThemeManager，返回视图后公共层完成接入，保留具体返回类型 |
| `TableItemDelegate::updateEditorGeometry` | 原生编辑器及 index/item widget 的统一 X/Y 留白与垂直居中入口；业务直接插入编辑器，不再套控件行的 `TableCell` |
| `setModel(model)` | 替换外部模型表体的数据模型，重绑布局监听并更新尺寸；不替换条目控件自身的内部模型 |
| `createGroupHeader(text, options)` / `ProportionalTreeWidget::createGroup(text, options)` | 分别创建控件分组标题和原生树形分组；共用 `TableGroupHeader` 的绘制与主题高度，标题不可选中，业务不维护样式；`TableGroupOptions` 中 `checkable`、`collapsible` 独立启用，默认均关闭 |
| `TableGroupHeader::setCheckState` / `ProportionalTreeWidget::setGroupCheckState` | 业务回填全选、未选或半选状态；可单独禁用勾选而保留折叠。勾选通过 `checkStateRequested` / `groupCheckStateRequested` 请求业务批量提交，公共层不推断业务平台、命令或可操作范围 |
| `TableGroupRole` | 原生分组语义，由公共创建入口设置；公共视图按整行绘制，不依赖列的视觉位置 |
| `ProportionalTreeWidget::setHierarchyIndentation` / `cellContentsRect` | 设置业务子条目的缩进宽度并提供内容区域；创建公共分组后，分组祖先不计入缩进，业务层级仅缩进内容，单元格边框和选择背景仍铺满整行；领域 delegate 和命中区域共用该几何规则 |
| `setSelectionClearHandler(handler)` / `clearSelection()` | 控件行接入业务取消选择函数；原生表体由公共层清除当前索引和选择；空白、Esc 等交互共用此入口 |
| `bodyMessage()` | 按需创建公共表体消息标签；字体、边距与最小高度由共享层处理，业务设置文本和可见性 |
| `createRowNumber` / `createRowHandle` / `setRowNumberColumn` / `setRowMoveHandler(handler, guard)` | 控件行使用序号或单元格内的独立拖动柄，原生表体声明序号列，显式启用移动后共用光标、插入线、拖动取消、右键上下移与 Alt＋方向键；调用业务命令，guard 保留继承、分组等移动约束 |
| `footer(standardButtons)` | 按需创建，未调用就没有 footer；是否显示标准增删按钮在首次创建时决定 |
| `createGroupFooter(standardButtons)` | 为控件行创建一个新的 ListFooter 并追加到表体布局；调用方在组内行之后调用，绑定所属组的业务动作；空白取消选择、按钮保留选择的规则与整表 footer 一致 |
| `registerRow(row, cells, details, groupId)` | 可选的稳定分组 ID，用于删除后在同组恢复选择；默认无分组，分组名与业务含义由调用方管理 |
| `ListFooter::setRefreshAction` | 绑定左侧刷新动作 |
| `ListFooter::addActionButton` / `addStatusWidget` | 提供左、右动作和状态位置，业务不重排 footer 内部 layout |
| `TableTextRole` / `TableDropTargetRole` | 原生单元格提交 muted、disabled、error、modified、custom、success 等状态及拖放目标语义 |

控件行与原生视图择一使用，每个 Table 创建一个表体。原生创建接口的 `name` 在接入前设置为视图对象名并用于现有列宽恢复；视图由 Table 销毁。返回的视图供业务填充条目、连接信号和配置编辑行为，不要求为 Qt 数据接口逐一创建转发层；布局策略仍通过 Table 配置。特殊工厂负责构造必要子类，领域 delegate 在返回后设置，通过继承 `TableItemDelegate`、重写 `paintContents` 提供领域内容；公共 `paint` 保留分隔线、选择和拖放目标绘制。

一个不需要 footer、按内容自适应高度的展示表：

```cpp
auto options = TableOptions::display(TableBodySizing::Content);
options.columnsMovable = false;
auto* table = new TableWidget(options, parent);
auto* view = table->createModelView(QStringLiteral("Results"), model);
```

一个需要刷新与导出动作的展示表，在原生视图接入前声明横向策略：

```cpp
auto options = TableOptions::display();
options.columnSizing = TableColumnSizing::Scrollable;
auto* table = new TableWidget(options, parent);
auto* view = table->createModelView(QStringLiteral("Capture"), model);
auto* footer = table->footer(false);
footer->setRefreshAction(refreshAction);
footer->addActionButton(exportAction);
```

一个由业务逐项填充的配置表：

```cpp
auto* table = new TableWidget(parent);
auto* items = table->createItemView(QStringLiteral("Platforms"),
    {tr("Name"), tr("Base Platform")}, {600, 400});
```

特殊视图保留领域交互，例如 Pack 的连续勾选：

```cpp
auto* table = new TableWidget(TableOptions::display(), parent);
auto* view = table->createCustomView(QStringLiteral("Packs"),
    [](QWidget* owner, ThemeManager* themeManager) {
        return new PublisherTree({280, 70, 90, 150, 80, 80, 120, 60}, owner, themeManager);
    });
```

配置表的 `#`、行增删和详情入口同样需要业务显式接入，不因选择 config 模板就自动生成。宿主决定 Table 所在区域的伸缩、边距和页面滚动；表体高度策略只管理 Table 内部空间。

分组可分别启用 `TableGroupOptions{true, false}`（勾选）、`{false, true}`（折叠）或 `{true, true}`（两者）。勾选框固定在整行最左侧，chevron 固定在最右侧，标题两边保留对称空间，始终按整行居中；列换位、隐藏或滚动不改变控件位置。半选点击转为全选，两项操作互不触发，也不改变已有行选择。原生分组以子条目表达归属，由树管理展开；控件分组通过 `expandedChanged` 让业务控制对应内容容器的显隐。

公共分组不增加子条目的视觉缩进：Init、General 和语言分区中的直接条目，以及 Shortcuts 分类下的命令，仍从对应列的正常留白起排。真正的业务父子层级保留内容缩进，绘制、编辑器和点击命中使用同一内容区域；分隔线和选择背景不随内容缩进。未创建公共分组的普通树继续使用原生层级布局。

Publisher 的 General 和语言分区使用实际父子关系，并按语言 ID 恢复折叠状态。Init 独立显示且始终包含在发布中，未变化时可复用；各分区同时启用勾选和折叠，聚合当前编辑平台上的可操作包状态。批量操作包含搜索过滤隐藏的包，沿用平台 targeting 命令写入本地缓存，不进入工程撤销栈；不改变导出平台选择。没有可操作包或编辑平台时，勾选框禁用，折叠仍可使用。

Publisher 数据归属：Name 和语言分区由当前工程 Action 及其语言依赖派生。勾选与 Export Platforms 存于 `.cache/pack-export-selection.json`，Built、Size、Modified、File、Version 存于 `.cache/pack-artifacts.json`；产物缓存由成功发布更新，面板刷新不读取产物或 manifest。Built 表示最近成功发布的快照，外部删除或替换产物不会自动同步；Self Cleaning 的已知删除会同步清缓存。CRC32 列已移除。Result 与 Cost 共用会话内结果存储，GUI/IPC 导出按稳定身份更新，工程关闭后清空，不写入缓存或工程文件。两列按 Result、Cost 顺序固定在最右侧，Result 左侧以公共表格竖线分隔表头、数据行和空白区域，遇到整行分组时断开；两列通过公共列强调样式使用略有区别的表头颜色和整列背景（包括表体空白区域）；其他列可在前方移动，所有列仍可调整宽度，开发者列显隐不改变末尾顺序。Cost 为每包资源准备与写包耗时，不包含共享编译和发布阶段。清缓存后默认全选，产物列留空；Build 配置、平台与语言定义仍是工程数据。隐藏面板延后刷新，可见时合并更新，并用共享语言投影代替完整构建计划。

Publisher 的 Init 与 Action 行通过公共 `TableRowProgressRole` / `TableRowProgressStateRole` 和 `ProportionalTreeWidget::setRowProgressEndColumn` 绘制连续背景进度，范围从行左边到 Result 左侧，适应前方列换位、调宽及开发者列显隐；分组不填充，Result/Cost 保持独立区域。进度与结果遵循主 toolbar 的 primary 平台、语言和稳定 unitKey，不聚合导出平台。进度仅在当前工程会话内保存，关闭工程或重启即清空；本轮参与打包的行从零开始并清除旧 Result/Cost，未参与的行保留。准备、编译、写入、发布按阶段推进，仅发布成功到 100%；失败及取消冻结当前位置。GUI 与 IPC 共用会话任务身份隔离迟到事件，进度通知仅更新对应行，不重建整表。面板导出在后台执行，取消入口位于原 Export 主按钮，不再显示独立进度弹窗。

Publisher 不再提供行右键 Export Settings 弹窗；打包输出目录统一在 Project Settings > Build 设置。媒体转换失败固定尝试默认处理参数或原始文件回退，成功后继续并记录首次失败原因；源文件缺失、变更或回退失败仍报错。旧工程的 `conversionFailurePolicy` 字段允许加载，但不再参与导出，保存时不再写入。

控件行通过 `registerRow(row, cells, details)` 将同属 `rowsWidget()` 的主行和展开容器登记为一条逻辑行，`details` 可省略。字段先放入 `createCell` 创建的共享单元格，再登记单元格；不把裸字段控件作为补边距的容器。展开容器提供自己的内容布局；公共表格负责同步选中状态、行号列宽度、内容缩进及主行与详情之间的分隔线，删除主行时一并清理详情。业务用 `rowForWidget(widget)` 将展开区及其子控件的交互归属到主行，不再复制详情的行号或选中状态。

## 后续治理边界

首轮完成全部入口接入和主要皮肤收敛，以下项目仍需后续处理，不能视为已由公共组件统一保证：

- [ ] 统一原生列的稳定 ID 与状态保存接口。控件列已有 `TableColumn::id`；ProportionalTreeWidget 的列宽键仍使用序号，普通原生适配仍使用表头文本，Monitor 沿用现有 headerState。后续加入或翻译列时要迁移这些键。
- [ ] 进一步收敛控件行选择与刷新恢复。公共 Table 已收敛取消选择与移动交互，平台、策略、Binding、Transition 已移除刷新时强选首行；Sources、Binding、ListField 等仍保留各自稳定身份和选择模型。
- [ ] 由用户在运行中的 Studio 核对 25 项入口的实际观感、拖动手感和窗口布局，补充自动回归未覆盖的业务组合。

换主题通过 `ThemeSnapshot` 的颜色、字体、尺寸和图标资源完成；换皮肤的修改边界是公共 QSS、`QtControlSkin`、`TableItemDelegate` 与共享控件渲染。业务命令、模型和数据绑定不应随之改动。本轮沿用这些现有机制，没有新增业务页面专用的皮肤接口，也没有引入独立的可加载皮肤插件系统。

实现定位：`HarkenStudio/Source/Widgets/Common/TableWidget.*`、`TableColumnLayout.*`、`TableItemDelegate.*`、`ProportionalTreeWidget.*`、`ListFooter.*`；主题与字段外观：`Source/Core/ThemeSnapshot.h`、`QtControlSkin.*`、`Resources/styles/dark_theme.qss`。
