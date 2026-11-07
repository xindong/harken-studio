# Studio 主题色卡与颜色职责

最近核对：2026-11-07（界面缩放）；字体与尺寸规范核对日期为 2026-11-06，颜色职责核对日期为 2026-11-04。

主题拥有自己的颜色职责、职责到绘制位置的绑定和一个或多个色卡。色卡提供该主题各职责的颜色值及明暗模式，用户修改按主题和色卡隔离。不同主题可以使用不同的职责清单；一个主题可以让网格复用边界色，另一个主题可以提供独立网格色。Renderer 继续消费完整、不可变的样式快照。

内置主题正式命名为 **Studio Default**（`harken.studio.default`），提供 **Dark**（`dark`，默认）和 **Light**（`light`）两张色卡。Dark 保留原有深色视觉；Light 提供适合浅色表面的文字、交互、边界和领域绘图配色。之前用于测试的 Amber 已移除。项目对象标记色仍属于项目数据。

## Studio Default 字体职责

默认排版只区分四种职责，不为面板、控件类型或绘图领域另设字号层级。Dark 与 Light 共用这套规范。

| 职责 | 默认字号 | 使用边界 |
|---|---|---|
| `Body` 正文 | 10pt | 字段名与字段值、菜单、按钮、表格、Dock 标签、蓝图正文、普通数值输入 |
| `Caption` 辅助文字 | 9pt | 坐标刻度、元数据、副标题、提示文字和紧凑状态信息 |
| `Title` 页面或内容标题 | 12pt | 页面标题与 Assistant 内容标题；不用于普通分组条目 |
| `Mono` 等宽技术文本 | 跟随正文，默认 10pt | 代码、命令和需要字符对齐的原始日志；音量、时间、频率等界面数值使用正文体 |

比例字体按 Inter、Segoe UI 的顺序选择已安装字体，两者均不可用时使用 Qt 默认无衬线字体。Windows 的等宽字体使用 Consolas，其他平台使用 Qt 的系统等宽字体。中文由 Qt 的字体回退补齐。默认字宽为 100%，不追加字距或词距。字重只使用常规 400 与强调 600：分节、分组和蓝图对象标题采用正文尺寸加强调；字段名、字段值和普通列头采用常规字重。已有的修改态斜体等状态语义继续保留。

`ThemeTypography` 是默认值的唯一来源，`FontManager` 统一解析字体并生成供 QSS 使用的字体声明。控件和 Painter 选择相同的职责，禁止另写像素字号、任意字重或独立字体候选列表。空的比例字体族继承正文；辅助与等宽文字的默认字重继承正文，等宽字号也继承正文。强调字重共用标题字重，不再增加一个强调字号层级。工具提示和 Assistant 富文本也使用此入口。蓝图副标题的占用高度由实际字体指标计算。

面板字号设置的 `0` 表示跟随主题，不再保存七组不同的面板默认字号。加载旧设置时，旧版默认值归零，非默认值保留；Assistant 的旧像素字号转换为 pt。新值保存为 `PanelFontSizePt_*`，优先于旧的 `PanelFontSize_*`。这一迁移无法区分“用户显式选择了恰好等于旧默认的值”和“从未修改默认值”，两者都会跟随主题。

主题包仍使用版本 7。加载入口把旧 `heading`、`label`、`axis` 分别归并为 `title`、`body`、`caption`，同一文件同时提供新旧名称时采用新名称。旧 Curve 的 readout 字号与 Waveform 的标签字号归并到辅助文字；显式字体角色配置优先。运行时快照不再保留这些细分角色与领域字号。

应用主题时，先发布正文默认字体供原生样式表解析继承值，再更新样式表和调色板，随后重新发布正文默认字体，清除 Qt 安装样式时重新载入的控件类别字体，最后设置辅助文字提示字体并通知显式字体消费者。主题应用入口负责菜单、菜单栏和普通控件的默认字体约束，控件显式选择的字体职责和用户字号继续生效。首次创建、已有控件和连续主题切换均须遵循同一规范。

2026-11-06 在 Windows、Qt 6.11.1、RelWithDebInfo 下编译通过，27 项主题与控件测试通过，字体继承、显式字号和绘制一致性的 3 项检查同时通过 Windows 原生运行。隔离验证中的实际菜单恢复为 Segoe UI 10pt、常规字重、25px 行高，与按主干字体规则初始化的同一菜单截图完全一致。使用临时配置和工程检查 Dark / Light 在 100%、150%、200% Qt 缩放下的主窗口、Inspector、蓝图、时间轴、设置、Assistant、日志、菜单、提示、Meter 与曲线，以及自定义字体主题的切换和恢复。截图和字体采样位于 `.temp/typography-review/font-restored-*`；完整界面的缩放检查使用真实 Qt 控件的 offscreen 渲染，原生菜单检查使用 Windows 平台，未覆盖实际多屏 DPI 或 macOS。

## 布局与绘图尺寸

主题提供基础尺寸，统一放在 `ThemeMetrics` 和已有领域样式中，由 `BuiltinTheme.cpp` 声明 Studio Default 的默认值。界面使用语义尺寸或内容测量结果，不按当前默认数字相同就借用其他组件的参数。Dark / Light 共用尺寸。

| 类别 | 默认档位或职责 |
|---|---|
| 通用间距 | `compact / xSmall / small / medium / large / xLarge`：2 / 4 / 8 / 12 / 16 / 24 个 Qt 逻辑像素 |
| 控件高度 | 紧凑 20、常规 24、较大 32；实际文字较高时由内容测量决定 |
| 图标 | 小 16、常规 20、大 24 |
| 边框与圆角 | 普通边框 1、强调边框 2；圆角按紧凑、小、中、大分别为 2 / 4 / 6 / 8 |
| 布局职责 | `form`、`panel`、`dialog`、`chrome`、`popup`、`card`、`startup`、`toast` |
| 编辑与绘图职责 | `inspector`、`table`、`parameter`、`toggle`、`node`、`connection`、`timeAxis`、`timeline`、`colorPreview`；曲线、Meter、Waveform 和 Dock 使用各自已有样式 |

长度使用 Qt 逻辑像素，字体仍使用字体职责中的 pt。列比例、采样数量、动画时间、音量、时间轴业务坐标和已保存的画布位置不是界面长度；矢量图形可以保留参考坐标，但须映射到主题定义的实际范围。窗口首选大小用于首次布局，用户拖动后的窗口大小不在每次主题切换时强制恢复。

`ThemeManager::bindMetrics` 先应用当前快照，再以目标 QObject 的生命周期订阅完整主题变化。`ThemeLayout` 为布局边距、间距和 spacer 提供相同入口，spacer 删除时同时断开绑定。绑定在创建布局或控件时建立一次；刷新函数直接设置尺寸，不重复注册绑定。具有显式主题管理器的消费者使用同一实例读取和订阅尺寸。纯颜色预览沿用 `appearanceChanged`，不执行尺寸绑定。

检查器表单、列比例、装饰槽、分节、设置、参数控件和窗口壳层在现有控件上更新几何。蓝图节点和连线先通知 Graphics Scene 几何变化，再更新测量与绘图缓存；端口绘制与命中区域共享尺寸。时间轴与 Source Editor 的手柄矩形共用计算入口，波形绘制与幅度刻度共用带显式留白的范围计算。Assistant 的换行宽度、高度测量和实际布局使用相同边距。菜单尺寸由字体、图标和 padding 共同决定。

主题包继续使用 `schemaVersion: 7`，在 `tokens.metrics` 中按语义键覆盖新增尺寸，未提供的值继承基础主题。加载时检查开关内部空间、端口与连线命中范围、非零手柄、提示宽度上下限及表单列比例；不合法候选不会替换活动主题。

2026-11-06 在 Windows、Qt 6.11.1、RelWithDebInfo 下，编辑器和测试目标构建通过，142 项相关合同测试通过，覆盖主题候选校验、输入与手势保留、布局与 spacer 生命周期、时间轴绘制和命中几何。QSS 归属检查通过，样式表的 113 个令牌均有解析入口。隔离工程分别在 offscreen 和原生 Windows 平台生成 42 张真实窗口截图，检查 Dark / Light、170 个基础尺寸增至约 1.5 倍，以及恢复默认值后的界面；覆盖主窗口、检查器、蓝图、时间轴、设置、选色器、About、Assistant、Meter、曲线、日志和菜单。动态切换保留输入选择与检查器控件，About 的标题栏与 Logo 均计入布局范围。证据位于 `.temp/metrics-review/`，测试结果位于 `.temp/metrics-tests.xml`。这是主题尺寸与生命周期验证，字体未随该探针放大，不等同于完整 UI 缩放验收；已有蓝图位置保持原坐标，较大的节点主题可能需要用户重新排列，自动布局的尺寸来源已经统一。

## 界面缩放

入口为 **Preferences → Appearance → Editor Scale**，使用不可编辑的预设下拉框。按用户指定提供 11 个百分比：**50%、60%、70%、80%、100%、125%、150%、175%、200%、250%、300%**，默认 100%。显示、实际尺寸和持久化使用相同的预设百分比。旧设置中的有效百分比加载时归入最接近的预设百分比，超出范围时归入 50% 或 300%。

**View → Appearance → Zoom In / Zoom Out** 与设置使用同一份状态。`Ctrl + =`、`Ctrl + +`（含数字键盘）放大一档，`Ctrl + -`（含数字键盘）缩小一档；macOS 使用 Command。快捷键可在 Shortcuts 中修改，到达上下限后停止；不绑定 `Ctrl + 0`。选择 100% 即可恢复默认大小。设置、菜单和快捷键立即应用并保存，不需要重启。该值继续保存在用户设置的 `Appearance/GlobalUIScale`，不属于主题或色卡；切换主题、切换色卡、接受或取消颜色预览都保留当前比例。

主题继续提供基础尺寸和 pt 字号。`ThemeManager` 保留未缩放的候选快照，每次发布时从基础值生成当前比例下的显示尺寸，避免往返调整造成累积误差。`FontManager` 在解析字体时应用同一比例；主题字号和用户显式设置的面板字号仍保存基础值，不把放大后的字号写回设置，基础字号下限在缩放前应用。整数像素尺寸在显示边界取整，正尺寸至少保留 1px，浮点绘图长度保留精度；列比例、时间、计数、音量和未指定尺寸的哨兵值不随界面缩放变化。

已有控件通过主题、尺寸和字体通知更新布局与绘制。缩放设置会同步反映外部修改和恢复默认值；打开的颜色编辑器保留预览及编辑状态。颜色预览取消只恢复配色，不回滚其间独立修改的缩放比例。

蓝图的项目坐标与画布缩放比例保持独立。界面比例作用于视图变换，节点内的控件尺寸配合局部变换避免重复缩放；节点之间的世界坐标距离、已保存位置和网格吸附坐标不被改写。连线与命中测试使用同一世界几何，画布自身的滚轮缩放仍以独立比例计算。时间轴时间、曲线数据和其他项目业务坐标也不转换为放大后的显示像素。

历史验收：调整为最初 10 档预设之前，2026-11-07 在 Windows、Qt 6.11.1、RelWithDebInfo 下，编辑器与测试目标构建通过，155 项相关回归通过，覆盖设置持久化、预览事务、字体与尺寸同步、输入选择、编辑手势、蓝图世界坐标，以及主窗口、浮动面板和模态弹窗的快捷键分发。默认快捷键别名随修改、清除和恢复同步，冲突检测包含别名；主键盘 Shift 加号另外通过向隔离窗口发送原生 Windows 按键消息验证。结果明细见 `.temp/scale-vscode-validation-summary.json`，原生键盘探针为 `.temp/zoom-native-keys.cpp`。

历史几何验收：此前使用隔离设置与工程，在原生 Windows 平台检查 Studio Default 的 Dark / Light 在约 23%–430% 的旧范围内的实际窗口，并检查已有控件连续放大至最高档后恢复默认，生成 191 张截图及几何采样，位于 `.temp/scale-review/vscode-native/`。About、新建项目、颜色编辑器和设置另有原生及 offscreen 的 56 张截图，每个平台检查 20 个窗口状态和 44 项控件可达性，确认窗口受屏幕范围约束、内容可滚动，输入选择和控件实例保留；记录位于 `.temp/scale-geometry-review/vscode-native/` 与 `vscode-offscreen/`。当时的 QSS 归属检查与 `git diff --check` 通过；未覆盖多屏 DPI 切换或 macOS。

2026-11-07 调整为最初 10 档预设后，编辑器与测试目标重新构建通过，57 项相关回归通过，覆盖主题尺寸、设置持久化与预览事务、快捷键分发和蓝图坐标。测试结果位于 `.temp/scale-presets-tests.xml`。

2026-11-07 根据实际截图反馈重新检查后，修复了此前验收遗漏的显示与生命周期问题：原生样式初始化期间，列表可能先按纯文字高度缓存行位置，随后按带留白的完整高度绘制，造成文字重叠；现在统一在样式初始化完成后更新项目布局。菜单勾选标记使用主题图形及尺寸。参数 presenter、组件配方布局、Mixing 与 Transition 在结构不变时直接更新几何，不再因缩放销毁布局、隐藏编辑器或结束输入。Inspector 绑定图标、范围分隔符、排练行、对象引用边距和字段列宽均实时读取尺寸；Notes、自定义 Timeline 行高与标题列宽按基础尺寸保存，活动尺寸拖动随缩放重新建立基准。蓝图拖线预览同步刷新端点与主题线宽。添加平台、采样率范围弹窗的表单可滚动，操作按钮保持可达。

本轮编辑器和测试目标构建通过，155 项相关回归全部通过，结果为 `.temp/scale-audit-tests-final.xml`。新增的列表回归读取实际绘制矩形，检查新建控件和已有控件的行不相交，避免仅查询 sizeHint 而漏掉缓存行位置错误。实际检查覆盖全部 10 档预设、Dark / Light、初次创建、连续往返及保留中的编辑；Windows 原生及 offscreen 分别检查设置页、搜索、平台与语言配置、采样率弹窗、Inspector、Notes、Mixing、Transition、Timeline、Meter 与监视曲线。蓝图还在原生 DPR 1.25 下检查缓存与活动控件、四种流向、六种字段、标题编辑、端口命中，以及真实按住滑块时使用缩放快捷键。完整 Studio、菜单、Assistant、日志、颜色编辑器及 About 的原生截图位于 `.temp/scale-review/audit-native/`，最终动态更新复核位于 `audit-final-dynamic/`；专项记录位于 `.temp/scale-style-audit/final/`、`.temp/blueprint-scale-audit/`、`.temp/settings-scale-audit/fixed-*/`、`.temp/settings-range-audit/fixed-*-isolated/` 和 `.temp/surface-scale-review/drag-final/`。QSS 归属检查及 `git diff --check` 通过；没有覆盖实际多屏 DPI 切换或 macOS。

随后增加 70% 档位，设置下拉框和缩放快捷键共享的预设增至 11 档，默认仍为 100%。编辑器与测试目标重新构建通过，6 项相关回归通过，覆盖主题缩放、尺寸边界、设置持久化与预览独立性、窗口快捷键分发；结果位于 `.temp/scale-70-tests.xml`。

提交 PR 前同步 main，波形预览边距迁入共享 PreviewField，曲线字体与标签避让规则接入 CurvePlotGeometry；主线新增的拖动边框读取主题边框尺寸。合并后编辑器与测试目标构建通过，157 项相关回归全部通过，包含滤波器预览随平台切换及图形编辑手势，结果为 `.temp/scale-pr-tests.xml`。QSS 归属检查及其 6 项脚本测试、PR 模板和 DCO 检查通过。

另外记录一项与缩放无关的既有布局问题：Transition 状态栏徽标和计数在 100% 下也会因 `QSizePolicy::Ignored` 与零 stretch 收缩为零宽。本次保持该部分原状；它不属于上述缩放修复的通过项。

## Studio Default 颜色职责

以下职责与共享关系属于 Studio Default 的设计，不要求其他主题采用相同目录。

| 职责 | 主题键 | 使用边界 |
|---|---|---|
| 首要文字 | `text.primary` | 字段名与字段值、只读有效值、普通正文、菜单文字、顶部项目名称、蓝图对象名称、AI 空状态气泡与提示文字，以及常态操作图标；节点字段名与值一致 |
| 次要文字 | `text.secondary` | 注释说明、表格列头、输入占位提示、其他空状态、坐标刻度、单位、状态栏摘要、继承引用和面包屑 |
| 标题文字 | `text.title` | 已加粗的分节与分组标题，包括 Inspector section、Explorer 的 Assets / Sources、设置与偏好标题、GroupBox、模板标题和表格内部的分组条目 |
| 禁用文字 | `text.disabled` | 控件不可用时的文字，独立于只读状态 |
| 强调 | `accent.primary` | 控件激活、焦点与强调边框、滑条填充、曲线编辑点与手柄激活 |
| 强调悬停 | `accent.hover` | 强调元素自身的悬停变化 |
| 强调底上的文字 | `accent.text` | 高亮菜单和选择背景上的文字，不能直接用普通字段文字替代 |
| 基础表面 | `background.primary/secondary/tertiary` | 保留既有表面层级，节点普通主体使用 tertiary |
| 交互表面 | `background.hover/selected` | 悬停与选中；节点选中主体沿用 hover 的既有视觉 |
| 首要边界 | `border.primary` | 界面框架排布分割、面板边界和内容分隔线；包括 Dock 面板、画布外框、声道分隔及面板滚动条 |
| 次要边界 | `border.secondary` | 控件与按钮描边、表格完整外框、网格及行列分隔、控件内滚动条；禁用态控件描边仍属此类 |
| 刻度线 | `border.tick` | 时间轴、曲线、波形与 Meter 的刻度短线和坐标网格；刻度文字仍使用次要文字 |
| 面板与轨道表面 | `panel.background`、`header.background`、`track.background`、`track.backgroundAlternate`、`bus.background` | 保留既有较暗的工作区层次，不因 RGB 接近而合并为一个背景 |
| 反馈 | `status.success/warning/error/info` | 操作反馈；领域 feedback 同义槽位由这里解析 |
| 分类色板 | `palette.categorical0` 至 `palette.categorical6` | 区分多个条目，不承担固定对象类型或强度数值语义 |

字段名统一声明 `textRole=formLabel`，使用首要文字。普通文本标签可声明 `textRole=primary/secondary/title`；节点内容保留 `nodeContentRole=label/value`，两个绘制位置均绑定首要文字，节点辅助副标题使用次要文字。`roValue` 静态展示值使用首要文字；继承等运行时只读状态下，列表标签与数值随编辑状态禁用。公共禁用文字规则覆盖普通 QLabel、表格序号与列头、单位及上述文字角色，原生 palette 同步设置 Disabled WindowText。仍需支持查看导航的文字可通过 `dimmed=true` 使用同一禁用色；FX Custom 名称采用此方式，保留双击打开 Effect Editor 的能力。

Sources 语言分组、Inspector 各级 section 与 Explorer 的 Assets / Sources 标题统一使用主题 Body 字号（默认 10 pt）、600 字重、无额外字距。字号统一以 pt 表达，Inspector 标题不再单独使用 11 px 或按嵌套层级改变字重和字距。

常态操作图标使用首要色，包括树节点、折叠分节、表格分组、下拉框和排序的 chevron，binding / attenuation 入口、添加与缩放、重置、新建会话和表格拖拽手柄。分节自定义指示色继续控制装饰边线，chevron 本身使用首要色。禁用图标保持禁用色，已有激活、选中和强调背景状态保留对应职责。普通 SVG 图标默认从应用级 `ButtonText` 获取首要色，局部文字的次要色不改变其图标归属。

AI 空状态的气泡图标与提示文字使用首要色，输入占位提示和步骤状态保持次要色。Orphaned 媒体状态的图标与文字经用户确认继续使用次要色；默认过渡规则的只读标记仍保留次要色。Orphaned 在 Explorer 的 Assets 树中对 Sound 图标与名称共同着色，Blueprint 对应路径只改变标题文字，不是独立状态图标。

图标职责补正后，编辑器与测试目标构建通过，52 项现有主题、设置、样式和表格测试通过。Dark / Light 代表性实际组件完成 195 项高对比离屏核对，覆盖普通、禁用、激活、连续预览及取消恢复，使用独立临时设置；未覆盖完整工程和原生输入。报告、样张与上述状态图例位于 `.temp/icon-role-review/`。

AI 空状态归入首要色后，追加 3 项现有预览与样式测试通过；实际 AssistantPanel 在 Dark / Light 连续预览及取消恢复中的图标和文字共 14 项检查通过，记录与更新图例位于 `.temp/ai-empty-role-review/`。

Studio Default 不再开放 `text.muted` 作为独立编辑职责。既有 `text.muted` 渲染位置和 `textRole=muted` 消费者保留，统一由 `text.secondary` 提供颜色；其他主题仍可为这些位置定义自己的职责。Dark / Light 的标题默认色分别沿用各自首要文字的默认色，标题职责可独立编辑，没有另行指定新的 RGB 值。

2026-11-01 将边界职责收束为首要、次要和刻度线。首要与次要默认值保持，刻度线沿用原 `border.view` 的 Dark `#272E3C` / Light `#B9C5D1`，保留已有网格与主次刻度的透明度层次。`border.view` 和 `section.separator` 渲染位置保留并绑定首要边界；Studio Default 的旧 `border.view` 覆盖值迁移到 `border.tick`，已有 `border.tick` 覆盖优先，其他主题的同名职责不迁移。表格属于控件，外框与内部表头、分组、行列分隔统一使用次要边界，覆盖原生表格、组合表格和矩阵表格。Dock、主窗口与 Inspector 内嵌分割器的缝隙使用原有背景色。

Filter 图表与字段共用首要边界外框；两者之间及 Explorer 的 Assets / Sources 之间保留 1px 分隔边界，常态使用首要边界色，启用时悬停使用强调色。拖动命中范围向两侧扩展，不增加可见线宽或布局间距。Explorer 任一分节收起后边界不可拖动，也不显示悬停强调色。

Blueprint 内嵌 Meter 的宽度边界及通用表格的列宽边界在悬停时使用强调色，沿用原有 1px 线宽和拖动命中范围；表格覆盖组合表头、原生表头、树表和矩阵表头，固定列和禁用状态不高亮。Notes 右下角调节柄在悬停及拖动时使用强调色，保持原有形状、尺寸和线宽，移开后恢复原色。

本次补正按用户决定只重新归类表格描边。GroupBox、菜单、弹出层和独立列表/树视图的边界保持现有绑定，语义归属留待后续确认。

顶层 `CollapsibleSection` 分节标题保留首要色下边线，上边线按布局中的实际位置决定。位于布局内容起点的标题不重复画上边线，Inspector 首个可见标题与容器共用外框；后续标题绘制首要色上边线，与前一节内容分隔。筛选隐藏或恢复前面的分节后，标题随布局移动重新判断边界。普通标题与带自定义指示色的标题共用此规则。Dark / Light 的实际组件高对比离屏检查覆盖展开、收起、筛选后首节变化、重新显示、连续预览、取消恢复及预览/确认一致性，并联合检查容器外框与首个标题交接处只有一像素边线，166 项通过；记录位于 `.temp/section-joined-border-review/`。此前 `.temp/section-border-review/` 仅检查标题自身边线，未覆盖其与容器外框的叠加。

参数 Slider 的横向、纵向和紧凑样式统一使用已有 `slider.track` 提供未填充轨道颜色；禁用态轨道也使用该职责，滑块与紧凑禁用态填充保留禁用色。启用态填充和滑块保留强调色。Toggle 的颜色绑定本轮不改。

滚动条滑块按所属区域复用边界职责：面板滚动区域、Explorer 项目树、Log 日志区、会话内容区以及 Timeline / 波形 / Blueprint 画布使用首要边界；表格、Notes、普通文本编辑器、下拉选项和弹窗列表内使用次要边界。共享 QSS 按滚动条的直接所属区域及既有 `scrollRole` 匹配，避免外层面板或控件影响嵌套区域。横纵方向、常态、悬停和拖动保持同一职责，移除原纵向悬停从首要切到次要的规则；原有禁用时隐藏滑块的规则保留，不新增颜色职责或默认值。

滚动条调整后编辑器与测试目标构建通过，15 项现有样式与表格测试通过。Dark / Light 下对 16 类代表性滚动区域完成 360 项离屏核对，覆盖实际绘制缓冲区中的预览颜色、原生绘制入口的常态/悬停/拖动、嵌套 Notes 与面板的归属，以及确认、重复预览和取消恢复。记录及高对比样张位于 `.temp/scrollbar-role-review/`；该核对不代表完整工程和原生输入验收。

旋钮没有实际业务页面入口，已删除 `RotaryKnobPresenter`、`RotaryKnobRenderer`、注册与构建项、`knob.indicator` 颜色职责和 `parameter.knobExtent` 尺寸配置。原 EffectParameter 默认展示器改用已有 `SliderWithValue`；恢复 Studio Default 已保存设置时移除旧旋钮职责覆盖，其他主题的同名职责保留。

本轮范围为分割线、border 与刻度线，不将控制柄、十字参考线、播放头、连接线、定位网点和其他辅助图形重新收纳到三类边界职责。十字参考线退出原先共用的 `border.view` 绑定，保留其 Dark / Light 预设色及独立渲染位置，不新增编辑职责。2026-11-01 曾按用户决定保留 clip 完整描边；2026-11-03 按新的 Timeline 绘制要求取消。`timeline.clipBorder` 与 `timeline.clipSelectedBorder` 的主题合同字段和既有色卡值继续保留，当前不参与 clip 绘制。

补正后 `harken_editor` 与 `harken_editor_tests` 构建通过，64 项主题、设置、控件、表格和时间刻度相关测试通过。新增通用回归检查：已有不透明子控件在连续纯颜色预览和取消时收到重绘，保持布局、编辑内容、选区与原生样式不变。Dark / Light 的实际 Preferences 调色器、三种表格和 Dock 离屏核对共 166 项通过；直接读取窗口绘制缓冲区检查点击 OK 前的边界像素，没有调用强制重绘来掩盖刷新遗漏。较早的 112 项静态核对未覆盖这类连续预览问题，也沿用了错误的表格外框分类。此次核对未改写用户配色，不覆盖完整工程和原生输入。QSS 归属检查与 `git diff --check` 通过，记录和样张位于 `.temp/border-preview-fix/`。

Timeline clip 当前无描边，与轨道上下边缘各留 1px；标题使用独立背景的 title tab。主体和普通标题优先使用对象颜色，否则使用 `timeline.clip`；选中或拖动标题无对象颜色时使用 `timeline.clipSelected`，该职责同时保留名义范围背景的用途。标题先填充画布基础表面，再按状态叠加颜色，保留源 RGBA 的 Alpha；波形位于标题下方。普通 clip 按开始时间由早到晚绘制，同起点保留存储顺序；鼠标按相反顺序命中，拖动预览保持置顶。重叠标题仅在左端连续可见区域内对名称做右侧省略，左端被遮挡时不绘制名称，不向右侧空白段避让；Blend 保留原有名称避让。画布轨道分界与左侧标题区对齐，使用 `border.primary`，不借用时间刻度的 `border.tick`。

Timeline 的 Entry 使用 `timeline.entry` 黄色、Exit 使用 `timeline.exit` 红色，播放与编辑 Cursor 使用 `music.cursor` 绿色；三者均由主题职责控制，画布不再局部覆盖 Cursor 颜色。Marker 位于刻度轴底部的 12px 区域，复用 Source Editor trim 的双握槽手柄，Entry 向右、Exit 向左；悬停与选中按同一职责色加亮。

Timeline 不再显示遗留的 `EXT` 引用标记。clip 跨轨移动同步更新目标对象的树归属，多选保持相对轨道偏移；部分共用对象的 clip 换轨时，为迁移部分建立独立对象并共享原音频资源，其余 clip 留在原轨，整组换轨则迁移原对象。树归属、clip 数据和物理文件夹位置作为同一次撤销事务处理。

Track 与 Segment 的拖动、拉伸吸附对齐线统一使用现有 Track 样式：`accent` 按 160/255 系数保留源 Alpha，1px 实线、关闭抗锯齿。Segment 不再将吸附线画成 `status.success` 的 2px 绿色虚线；编辑游标和播放头继续使用各自原有绘制规则。

两种 Timeline 均取消 clip 的虚线预览轮廓，拖动和拉伸统一沿用 Segment 的选中态填充；移除 Track 专用的额外加亮状态。左侧区域的 Shift＋滚轮按 Segment 的轨道滚动路径处理，不再由 Track 的 dB 轴单独触发时间横移；画布和时间标尺上的 Shift＋滚轮仍横移。Track 的 dB 轴、振幅缩放与 Segment 的多轨管理保持各自用途。

表格列头使用次要文字；表格内部加粗的分组条目使用标题文字。蓝图对象名称使用常规字重和首要色，绘制、宽度测量与重命名输入框保持一致。蓝图底部栏目的文字，包括 Reset Layout 和缩放比例，使用首要色。Inspector 分类 Tab 的常态文字使用首要色，选中时保留强调色。面包屑当前项与可点击祖先项均使用次要色。

对象引用进入只读或关闭选择入口时，名称与对象图标使用禁用色，保留可用的导航入口。可编辑状态下，继承引用保留次要文字，失效引用保留错误色；整个控件禁用时统一使用禁用文字。分段控件进入数值编辑时使用主要文字，普通分段选项保持原有层次。

菜单栏常态与悬停文字的绘制位置 `shell.menuText`、`shell.menuHoverText` 均绑定首要文字；下拉菜单普通文字同样使用首要文字。菜单栏与下拉菜单不可用时使用禁用文字，强调背景上的文字使用 `accent.text`。

同一颜色可能由 QSS、QPainter 或 QPalette 消费；绘制技术和所在面板都不构成新增颜色角色的理由。透明度、渐变与交互几何可以不同，颜色来源仍可共享。

## 解析与发布

`ThemeColorResolver` 接受当前主题的职责绑定与当前色卡值，将它们写入固定的渲染颜色位置。绑定的 `alpha` 表示 0 至 255 的透明度乘数，实际 Alpha 为职责颜色 Alpha × `alpha / 255`；省略时完整保留 RGBA。原有共享文字、网格、节点背景等派生关系已成为 Studio Default 的绑定声明，解析器不再为所有主题强制建立相同关系。

业务绘制与样式层同样保留源 RGBA，再乘透明度系数；最终 Alpha 为职责 Alpha × 绑定系数 × 业务系数。2026-10-31 的叠乘修正保留了既有系数，后续视觉取舍以各领域记录为准。颜色插值保留 Alpha，多频段波形按频段权重混合源 Alpha 后再乘波形系数。Dock 和样式颜色字符串传递完整 RGBA；SVG 的着色 Alpha 与图形自身透明度相乘，并保持栅格图像与导出的 SVG 一致。

2026-10-31 Alpha 修正后，`harken_editor` 与 `harken_editor_tests` 构建通过，56 项主题、资产、样式及相关测试通过，QSS 归属检查与 `git diff --check` 通过。本地对照图 `.temp/palette-alpha-review/palette-alpha-comparison.png` 使用当前保存的 Studio Default / Dark 配色，17 组实际组件及生产 Renderer 配合静态示例数据绘制，列出源职责 RGBA、现有业务系数与实际效果；未强制修改职责 Alpha，也未改写用户设置。图中包含收起与展开的 Meter、曲线、Timeline、Inspector、Dock、SVG、蓝图及音源范围等样本，不代表完整工程或所有动态状态的截图。渲染记录与测试结果同存于 `.temp/palette-alpha-review/`。

Studio Default 将 Qt 原生窗口、输入框、提示框和按钮背景绑定到对应表面职责，并同步原生文字与辅助颜色；色卡默认值、用户预览和取消恢复共用这些绑定。其他主题可以定义自己的对应关系。

主题发布与取消预览均向所有控件安排重绘。只更新顶层窗口会被 Qt 的不透明子控件优化截断；即使纯描边颜色变化没有改变 QPalette 或原生 QSS，已有子控件也必须重绘。调色仍复用原生样式与匹配缓存，无需重新安装样式或重建控件。

`HarkenStyle` 刷新控件调色板时，统一以当前主题快照生成的应用调色板为基础，再应用控件自己的样式声明。旧实现从控件已有调色板开始，部分颜色需要等待 Qt 后续传播，导致预览过程中生成的色块读取旧底色，而确认时读取新底色。预览、确认与取消共用这一发布路径。

输入占位提示通过 `qtPalette.placeholderText` 绑定到次要文字，样式层直接使用该绘制位置，不再从首要文字生成半透明颜色。

启动画面底部的项目名、进度百分比和状态文字统一使用首要文字，包括失败与取消状态；版本号使用次要文字。失败进度条仍使用错误色。

2026-10-31 文字职责调整后，`harken_editor` 与 `harken_editor_tests` 编译通过，58 项主题、设置、表格及蓝图控件合同测试通过。隔离设置下的 Dark / Light 高对比离屏渲染共 86 项检查通过，覆盖真实折叠标题、Explorer、设置分节、顶部项目名、表格内部的分组条目与普通列头，以及字段、禁用、输入框与下拉框占位提示；蓝图对象名称的首要色与常规字重、重命名输入框，以及同一下拉框从占位到选值再回空的颜色恢复也已核对，预览未改变已提交设置。该检查使用代表性组件，不代表完整工程与原生输入操作验收。记录位于 `.temp/text-role-review/followup/`。

`ThemeSnapshot` 中保留领域 Renderer 所需的已解析颜色槽位。主题职责可以绑定这些槽位，Renderer 只依赖自身样式，不需要读取任意主题职责名或全局主题管理器。快照发布后保持不可变，重新加载产生新快照。

主题包使用 `schemaVersion: 7`。`entries.colors` 可指定颜色定义文件，文件声明 `roles`、`palettes` 和 `defaultPalette`。每个职责提供 `key`、`group`、`label`、`description` 及 `bindings`；每个绑定提供有效绘制位置 `target`，可选 `alpha`。每张色卡声明 `id`、`displayName`、`colorScheme` 与完整的职责颜色 `colors`。显式颜色定义替换继承的职责和色卡定义；未声明时继承基础主题。色卡 ID 仅在所属主题中有效。

主题加载校验角色、色卡和绘制目标的唯一性，检查职责引用、颜色值、Alpha 及色卡完整性。未知目标不会静默忽略。职责可使用主题自己的名称，但必须绑定到实际渲染位置。

声明 `entries.colors` 的继承层以该文档中的色卡值为准，同层 `tokens` 或 `visuals` 再赋值任何已绑定目标会报告冲突。未声明颜色定义的继承层可通过任意不带 Alpha 乘数的绑定目标更新所属职责，绑定顺序不改变语义；同层多个目标对同一职责给出不同颜色会报告冲突，给出相同颜色则允许。带 Alpha 乘数的目标不能直接反向覆盖职责值。未绑定的绘制位置可继续使用 `tokens` 或 `visuals` 中的字面颜色。

### 控件样式缓存与启动边界

`WidgetAppearance` 将目标控件匹配与祖先路径匹配分层缓存。规则先按控件类型筛选，再记录实际读取的名称、属性、类型和必要父链，包括导致不匹配的条件。祖先变化复用目标控件匹配结果，同一轮匹配每个属性只读取一次；全局规则与局部样式共享属性版本，局部样式作用域按当前父链查找。颜色声明变化复用既有匹配结果，选择器变化只清理对应样式的规则缓存。控件销毁立即清理缓存与观察连接，并使仍被引用的依赖失效。

`HarkenStyle` 负责调色板和几何更新，不将每次 `StyleChange` 当成整棵子树失效。输入观察器仅同步仍被缓存使用的属性，样式层消费变化结果，不重复扫描。真实结构变化引起的后代刷新在事件循环内合并，跳过被祖先覆盖的重复请求及尚未 polish 的控件；首次 polish、显示和样式读取补齐当前外观。Preferences 与业务面板的现有按需创建边界不变。

拥有独立样式表面的自定义 `QWidget` 面板须自行声明背景绘制职责。`ControlPanel` 显式启用样式背景，使现有背景色与顶部边框在展开、重显及颜色预览后仍被绘制；共享样式层不因此扩大自动背景绘制范围。真实面板的像素与几何检查先复现缺失再验证修复，41 项相关测试在原生 Windows 与 offscreen 平台通过，默认主窗口与 #193 前的截图仍逐像素一致。

2026-10-30 启动对照使用 #193 合入前主线 `aaf1d2fe` 与基于 `25befb47` 的样式修复工作区：同机、同编译配置、隔离设置、原生 Windows，每次新进程创建空工程，预热后交替运行五次。main 入口到 Ready 的中位数分别为 966.389 / 1014.414 ms；主窗口结构为 119.142 / 140.861 ms，初始布局为 102.448 / 110.717 ms，工程界面恢复为 111.629 / 121.047 ms。历史时间用于差异审查，不作为必须完全一致的指标。完整启动的离屏路径在 Qt 的 `QWidget::render(QPicture)` 中异常，因此端到端验收采用原生平台，未修改该绘制路径。

40 项相关测试在原生 Windows 平台通过，128 组颜色预览渲染对照通过。通用回归测试覆盖缓存与重新解析结果的等价性，包括初始失配后来命中、复合条件短路恢复、原生祖先属性、替换选择器、换父级、局部样式、销毁及地址复用。绘制层恢复 Qt 自动背景范围，保留工具栏在原生样式包装下的补绘入口与原生文字对齐声明，并由尺寸握柄的样式规则接管其绘制。统一状态栏示例文字后，修复版与 #193 前的主窗口截图逐像素一致，677 个控件、115 个可见控件及全部控件几何一致。计时与视觉记录位于本地 `.temp/startup-before193-comparison/targeted-layered-*`、`normalized-verification-3/`；诊断位于 `.temp/startup-cache-detail-targeted/targeted.log`，颜色预览渲染位于 `.temp/theme-preview-profile/targeted-layered/`。

实际 MainWindow 的背景色、强调色各自确认/取消四组检查通过：1426 个控件、542 个跟踪控件、8 个模型保持不变，编辑绑定、预览中的已提交设置及最终持久值均符合预期，未触发模型重置或行增删。记录位于 `.temp/theme-preview-profile/targeted-layered-results.json`。

PR 集成复验基于已含 #194 的主线 `9235952a`：编辑器与测试目标重新构建，40 项原生测试、程序自检、四组实际窗口预览检查及完整截图对照再次通过。同条件五次交替启动的 #193 前 / 集成修复版中位数为 988.154 / 1063.991 ms；主窗口结构为 118.384 / 157.918 ms，初始布局为 103.162 / 112.914 ms，工程界面恢复为 112.420 / 119.011 ms。这组数据包含后续主线改动，与前一组样式修复独立对照分开记录。证据位于 `.temp/startup-before193-comparison/pr-*` 与 `.temp/theme-preview-profile/pr-main-results.json`。

## 用户颜色定制

Studio Default 提供 72 个可编辑源颜色角色，共 11 类。每项包含职责名称、用途说明、主题职责键和绘制绑定。外部主题可声明自己的目录，Appearance 按当前主题生成分类与颜色行。窗口壳层按钮细节、Logo 扫光和声谱图固定热力色阶未加入 Studio Default 的用户定制目录；启动波浪的四档色阶单独表达动画配色，Logo 底板复用 `panel.background`。

| 分类 | 数量 | 开放范围 |
|---|---|---|
| Text | 4 | 首要文字、次要文字、标题文字、禁用文字 |
| Surfaces | 8 | 主背景、控件背景、抬升背景、面板、标题、轨道、交替轨道、Bus 背景 |
| Interaction | 9 | 强调及悬停、强调底文字、悬停/选中背景、三类边界、滑条底轨 |
| Feedback | 4 | 成功、警告、错误、信息 |
| Categorical palette | 7 | 区分多个条目的七个分类色 |
| Waveform | 7 | 静态波形低/高幅度、三段频率贡献、裁剪及循环标记 |
| Curves | 4 | 音量衰减、音高衰减、参数绑定、Grid 淡变 |
| Meters | 8 | Peak 四段渐变、RMS、峰值保持、频谱主线/峰值线 |
| Timeline | 8 | Clip 背景、上下文范围高亮、Clip 文字及普通/选中边界、进入/退出/过渡标记 |
| Blueprint | 9 | 画布、网格点、三类端口、三类关系连线、节点波形 |
| Startup | 4 | 启动波浪的暗部、低色阶、高色阶和高光 |

入口为可停靠的 **Preferences → Appearance** 面板。上方 Appearance 分组保留 `Theme` 和 Information Language。`Theme` 下拉字段切换完整主题，包括字体、图标及控件尺寸。下方 **Theme Colors** 分组包含 Color Palette、分类与搜索、详细颜色表格及重置操作。切换主题时选中目标主题的默认色卡，Color Palette 仅列出该主题提供的色卡。字段沿用标准表单列比例；色卡选择不改变主题身份或非颜色属性。下拉框左侧显示色卡名称，右侧色条按顺序展示主背景、控件背景、选中背景、强调色、主要文字、次要文字、成功、警告、错误、信息十种渲染颜色。色条通过当前主题绑定生成，不依赖主题使用固定的职责名称。

Theme Colors 编辑当前主题的当前色卡。即使另一个主题使用相同色卡 ID，颜色修改也彼此隔离。分类和搜索共同过滤颜色列表，搜索覆盖职责名称、分类、用途说明及颜色键。列表并列展示当前颜色与 `Palette Default`，两列默认使用 48 个逻辑像素宽的色块，下方等宽的 4 个逻辑像素细条与色块紧贴、共用外边框，中间仅以一条边框线分隔。色块外框与内部横线使用次要边界 `border.secondary`，两列均随实时预览刷新。细条按 Alpha 值显示不透明度：0 为空条，255 为满条；填充固定白色、背景固定黑色，不参与色卡管理。选中项在下方显示用途说明。双击表格行打开 `ColorEditorDialog`：上方是 SV 二维选色区域和窄 H 色相条，下方分别排列 H/S/V、R/G/B 数值，底部通过公有 `LinearSliderPresenter` 提供共用的 A 透明度滑条与数值。H 为 0–359，S/V 为 0–100，R/G/B/A 为 0–255，数值不显示单位后缀；A 独立于两种色彩坐标，修改透明度不重算 RGB。底部操作行左侧的 20×20 吸管图标用于屏幕拾色；拾色保留当前 A 值；拾色时按 Esc 取消此次拾取并恢复拾色前的颜色。

底部右侧并排保留 `Reset Selected` 和 `Reset All`，分别恢复当前主题所选色卡中的选中角色和全部颜色覆盖。默认值来自该色卡，恢复操作立即保存，其他主题和色卡的修改保持不变。

选色器的 SV 二维颜色区域与 H 色相条都是输入控件，两者外框统一使用次要边界 `border.secondary`，每次绘制读取当前主题颜色，并随主题统一发布实时重绘。

主题选择、色卡选择、完成颜色编辑、恢复默认及 Information Language 均即时保存。切换主题选择目标主题的默认色卡，重新加载主题和程序重启则保留已保存的主题与所选色卡。颜色对话框支持 Alpha 与实时预览：预览只发布临时快照，OK 后保存，Cancel、Esc 或关闭选色器恢复已提交外观。关闭或停靠 Preferences 不撤销已保存设置；AI 连接配置仍使用独立的 Save Connection 事务。

主题选择保存在 `Appearance/ThemeId`，色卡选择保存在 `Appearance/PaletteId`。颜色覆盖以主题 ID、色卡 ID、职责键分层保存在 `Appearance/ThemePaletteColors`，不修改主题包或项目。旧设置在读取时迁移到所属主题的默认色卡；测试 Amber 不再作为可选配色保留。暂不可用主题的旧默认覆盖以空色卡键暂存，主题恢复后再迁入其声明的默认色卡；已有明确色卡覆盖优先。活动职责失效导致恢复失败时，仍保留其他主题与色卡的覆盖，避免随后保存时丢失。

Studio Default 已保存的 `text.muted` 覆盖迁入同一色卡的 `text.secondary`；已有 `text.secondary` 覆盖优先，并移除旧职责键。该迁移仅作用于内置主题，不改写其他主题的同名职责。

读取当前及旧版存储格式时跳过无法解析的色值，保留所有有效覆盖，包括未安装主题的私有职责。损坏的存储值不阻断后续切换、预览或保存；新提交的非法颜色仍被拒绝。

组合顺序为主题结构与色卡基础快照 → 当前主题当前色卡的用户覆盖 → 按主题职责绑定解析 → 最终不可变快照。色卡保留主题的尺寸、字体、组件布局和资源。用户覆盖与主题包共用 `applyThemeColor` 的绘制位置赋值路径。预览取消使用已提交候选，未知角色、非法颜色、存储不可写或跨线程发布均不提交新设置。

连续预览同一主题和色板时复用首次加载的基础候选与色板，每次都从未应用用户覆盖的颜色生成结果，避免反复扫描主题清单及解析继承链，也避免移除覆盖后残留上一次预览的颜色。取消、成功提交、成功切换或重新加载主题后结束该缓存的生命周期；提交和显式重新加载继续读取基础主题和色板。

选色器立即更新自身颜色，Appearance 页面在连续输入期间以 16 ms 周期合并输入，只发布最新颜色；没有待发值时停止定时器。已有定时器不因新输入反复延后，也不在每次绘制完成后重新等待完整周期。确认直接保存选色器的最后值，不依赖待发定时器；取消、隐藏、销毁或外部提交会清除待发输入，避免结束编辑后重新出现预览。颜色表的预览、确认与取消共用完整刷新：更新所有行的当前值与默认值色块、色卡缩略图及关联展示。移除预览只更新当前行的分支，避免边框和透明棋盘底色变化时漏掉其他色块。

2026-11-01 的一致性修复后，`harken_editor` 与 `harken_editor_tests` 构建通过，65 项主题、设置、控件、表格和时间刻度相关测试通过。通用回归逐一编辑 Dark / Light 的全部 72 项职责，核对预览与确认的完整主题快照、所有当前/默认色块和色卡缩略图一致，以及重复预览、取消恢复和预览不持久化。参数 Slider 的六种方向/样式/禁用组合另有 36 项窗口绘制缓冲区检查通过，覆盖两张色卡下的 border 独立性、轨道颜色、预览/确认一致和取消恢复。检查使用隔离设置与 offscreen 平台，不代表完整工程和原生输入验收；记录分别位于 `.temp/theme-preview-contract/` 与 `.temp/parameter-slider-review/`。

每次外观发布都发送 `appearanceChanged`；`themeChanged` 只用于主题、资源及非颜色结构变化。字体按实际排版值比较，纯色发布不发送 `fontChanged`。颜色消费者更新绘图、图标或失效缓存，保留树模型、参数 presenter、组件布局和正在进行的编辑。Blueprint 将 Qt `PaletteChange` 与外观通知合并为绘图缓存刷新，保留隐藏的节点编辑器；渲染器内数值未变时复用尺寸，增益节点列宽仅在渲染编辑器创建时测量，颜色变化重新记录绘图。数据变化仍更新值与尺寸，字体、样式及完整主题变化仍走结构刷新并重新测量。资源版本与快照版本分离，同一套资源跨调色快照复用；显式重新加载主题仍更新资源版本，旧快照的资源句柄保持有效。相同应用调色板、字体和 QSS 不重复安装，不再全局清空 QPixmapCache。

`QtControlSkin` 和 `QtAdsThemeAdapter` 的既有声明继续提供唯一的样式来源。`WidgetAppearance` 将颜色、边框、圆角和资源等绘制属性交给 `HarkenStyle` 在绘制时读取，仅把字体与布局所需声明安装为稳定的 Qt 样式表；Qt 会绕过代理绘制的少数子控件几何和字体也由相同声明提供。选择器按类型、对象名、属性、祖先关系、状态、特异性和源顺序匹配，局部 QSS 保留层级优先级。折叠面板的标题、指示线和层级竖线复用同一解析器托管局部声明，调色时不重装局部 QSS，字体、间距、展开状态和自定义指示色保持原有来源。颜色更新只解析变化的声明块并复用控件匹配缓存，类型、属性、层级或样式变更时才使相关缓存失效。全部使用 Qt 公开 API，不依赖 Qt 私有样式缓存。该调整不改变主题包合同：完整主题仍可替换字体、尺寸、组件布局和资源，色板继续只覆盖主题的颜色。

菜单和工具栏迁移复审发现，Qt 样式表包装层会接管部分绘制入口，并把菜单栏项目矩形缩至内容区域；原迁移遗漏这些分工，造成表面和边框缺失、高亮未覆盖留白、透明底色重复叠画以及下拉按钮状态错误。现由 `PE_Widget` 绘制菜单栏与工具栏表面，菜单栏状态背景恢复声明的 padding 和 border，文字和图标沿用 Qt 公开绘制入口；浮动工具栏在原生背景填充后补画声明边框，分隔符和三种工具按钮 popup 模式分别遵守其绘制入口。圆角栅格和半透明直角边框叠画顺序也按原 QSS 行为恢复。修复继续读取同一主题声明，没有恢复调色期间的全局 QSS 重装。

通用表单复审还确认四类实际形状回退：进度内容盖住外边框、下拉框箭头被横向压窄、窄滚动条的超限圆角变成胶囊，以及标签页面板的方角被错误圆角化。进度内容区域现保留声明的 margin、border 和 padding，填充长度沿用 Qt 截断规则；下拉箭头沿用声明的子控件尺寸并保持图片比例。半径使用横、纵两个分量，`border-radius` 按 Qt QSS 的一至两个值解析后应用四角，不按 CSS 的四角缩写展开；相邻半径超过边长时也沿用 Qt 的归零规则。单角声明和不等宽边框保留对应的横、纵半径。

下拉框的盒模型、尺寸提示、编辑区、箭头区和鼠标命中统一由 `HarkenStyle` 按声明计算，避免 Qt 包装层继续采用默认 16px 区域而与原 20px 箭头错位。`WidgetAppearance` 只在声明几何变化时应用尺寸约束并刷新布局，纯调色保留业务代码随后设置的固定尺寸和当前选项。原生 QSS 继续提供字体及其他非盒模型声明。Slider 填充区分界沿用 Qt 对偶数尺寸 handle 的中心计算，半透明颜色不再暴露一像素错位。所有实线边框共用逐边直条、梯形和半角弧段，保留不等宽、不同色边框交接处的原栅格覆盖。

2026-10-30 的隔离性能探针使用 Windows、Qt 6.8.3、RelWithDebInfo、offscreen、HarkenStyle/Fusion，每组 3 次预热与 27 次采样。300 个普通按钮/输入/下拉/标签共创建 826 个 QWidget，背景色和强调色的发布加事件处理 p50 分别从 92.84 / 92.01 ms 降为 9.59 / 12.17 ms；同时打开 Appearance 和选色器共 932 个 QWidget 时，从 118.72 / 119.26 ms 降为 13.43 / 14.87 ms。连续每 8 ms 输入颜色、持续 2 秒的 932 控件场景，两种颜色均发布 124 次，约 62 Hz，输入同步处理约 0.04 ms。

最终实际 MainWindow 探针使用隔离临时工程，包含 Segment、Track、Sound、Timeline、Inspector、Explorer、Blueprint、Preferences 和选色器；窗口为 1600×1000，共 1426 个 QWidget。外观修复后的背景色确认/取消场景分别发布 42.95 / 43.65 次每秒，强调色分别为 37.04 / 37.58 次每秒，输入同步处理 p50 为 0.061–0.064 ms。修复前对应记录为背景色 53.53 / 51.14、强调色 40.53 / 42.06 次每秒；本轮复测较低，后续性能评估应以外观正确的最终版本为准，当前数据未单独量化各项修复的成本。强调色每帧局部 QSS 重装已消除，连续 2 秒的 StyleChange 从 438–450 次降至 3 次，剩余为三个隐藏折叠标题各一次；FontChange 仅剩这些标题各两次，Blueprint 临时测量标签的字体事件已消除。跟踪的 542 个控件、4 个隐藏节点编辑器及 8 个模型均保持，模型 reset 和行增删为零。预览不写入设置；停手后最后颜色正确、快照稳定、无重复发布且定时器停止；确认和取消后的快照与持久值均符合预期。这些数字为 Qt offscreen 事件循环的预览发布频率，不等同于显示器帧率，也不能据此承诺所有工程达到 60 fps。剩余成本涉及调色板传播、绘图缓存更新与窗口绘制。

上一轮渲染探针没有真正覆盖 `QMenuBar` 和 `QToolBar`，当时的菜单对照结论不能外推到菜单栏、工具栏及其完整交互状态。本轮以 `d9b21986` 的原 `HarkenStyle` 和未改动的 QSS 声明冻结基准，固定 Fusion、96 DPI、DPR 1，Windows/offscreen 与默认/半透明四种组合共 248 张菜单和停靠工具栏图像全部逐像素一致，控件及菜单项几何一致。范围包含图标、禁用、快捷键、勾选、子菜单、分隔符、普通及语义工具按钮和三种 popup 模式；浮动工具栏另由表面与边框契约测试覆盖。已通过 Windows 原生鼠标键盘操作核对隔离 MainWindow 的 File、Edit、View、Editors 菜单、子菜单、Windows 工具栏下拉和 ALL 按钮；多屏 DPI 未覆盖。

通用表单的 136 个预览组合全部满足预览与完整应用一致、取消恢复原图，并保留焦点、选区、可见性和控件几何。与冻结旧 QSS 的 32 组对照中，菜单与下拉弹层逐像素一致；表单每张仅剩 Tab 底部 5 个接点，ARGB 最大通道差 2，来自零圆角背景路径的栅格覆盖，不涉及轮廓、内容区域或控件尺寸缺失。表头字体的显式属性标记序列不同，字体指标与实际文字渲染一致。不将这些结果表述为整个软件逐像素完全一致。

下拉框另有独立尺寸与点击范围对照：真实生产样式和表单 24/24 组、经受管入口安装显式 min/max 声明的 4/4 组均与旧 QSS 一致，覆盖 LTR/RTL、可编辑/不可编辑、尺寸提示、硬约束、frame/edit/arrow、实际 lineEdit 和逐位置 hitTest。直接调用 `setStyleSheet` 安装未受管下拉框盒模型的 4 个人工场景仍会由 Qt 接管几何并采用默认箭头宽度，保留为接口边界，未更改预期掩盖差异；仓库没有这一生产调用路径，盒模型扩展须经主题或受管样式入口。

性能及渲染原始数据位于本地 `.temp/theme-preview-profile/`，最终实际窗口使用 `main-window-visual-fix7-` 前缀，菜单工具栏汇总为 `menu-toolbar-fix7-summary.json`，通用表单为 `render-menu-toolbar-fix7*`。下拉框尺寸与命中范围记录位于 `.temp/combo-size-contract/fix6-size-contract-summary.json`；fix7 仅调整共享边框绘制，没有改变该几何实现。

上一轮预览性能与绘制修复的 `harken_editor` 和 `harken_editor_tests` 编译通过，相关 78 项测试、QSS 归属检查及其 6 项脚本测试通过，Studio `--self-test` 返回 `ok: true`。这些历史记录覆盖全部可编辑颜色不触发结构/字体重建、完整主题仍应用字体与结构、资源缓存跨配色快照复用、局部样式层级、编辑状态保持，以及预览停手、确认和取消的生命周期；不作为本次主题色卡合同变更的验收结果。

实现入口为 `ColorPalette.*`、`BuiltinTheme.*`、`ThemeColorCatalog.*`、`ThemeColorResolver.*`、`ThemeManager.*`、`ThemePackageParser.*`、`ThemePackageLoader.*`、`Dialogs/ColorEditorDialog.*` 和 `Dialogs/Settings/Pages/AppearancePage.*`。主题色卡契约覆盖不同职责目录、同名色卡跨主题隔离、主题切换默认色卡、非颜色属性保持、预览取消、重置、重载、重启及旧设置迁移。

本次主题色卡模型在 Windows x64、Qt 6.8.3、RelWithDebInfo 下完成 `harken_editor` 与 `harken_editor_tests` 构建，相关 65 项测试通过，QSS 归属检查及其 6 项脚本测试通过，Studio `--self-test` 返回 `ok: true`。隔离设置的真实 Qt 验收窗口检查了 Dark / Light 切换、颜色编辑器、复选框和 Light 启动画面；未覆盖所有业务面板或实际多屏 DPI。新增浅色验收发现的勾号固定白色与栅格着色缩半问题已修复，通用测试验证启用／禁用勾号可见性及 1x／2x 着色后的像素覆盖和 DPR 保持。记录位于 `.temp/theme-palettes-final-tests.xml` 与 `.temp/theme-palette-validation/`。

以下为全局色板模型调整前的历史验收记录，不代表当前模型的验收结果。

颜色定制的实际窗口验收复用完整 Studio 与隔离用户设置。历史对话框阶段的截图与响应记录位于本地 `.temp/color-customization-qa/`，不作为同步可停靠面板后即时保存行为的验收依据；不覆盖操作系统原生输入链路与多屏 DPI。

当前主干基线为 `049e8af`。Windows x64、Qt 6.11.0、`windows-ninja-clang-dev` / `RelWithDebInfo` 下的 `harken_editor` 与 `harken_editor_ci_tests` 构建通过。Studio PR 分组检查共 136 组，352 项通过，1 项符号链接环境测试跳过，无失败或未运行项；记录位于本地 `.temp/studio-color-review-tests/`。新增契约覆盖连续预览的基础候选隔离、移除覆盖以及取消、提交、主题切换和重新加载时的缓存生命周期。QSS 归属检查及其 6 项脚本测试、帮助 JSON 和差异检查通过。

实际可停靠 Preferences 已核对两套十色色板、即时 HEX 保存、无效输入拦截、预设覆盖隔离、三种 Reset、选色器预览/取消/确认，以及切页和真实 dock 关闭重开。关闭时只取消仍打开的选色器预览，已提交配色保留。800×550 与 640×480 浮动窗口的色板及滚动区域可用；800×550 滚动到底部后可完整操作颜色编辑与 Reset。当前截图使用 `immediate-` 前缀，行为记录为 `.temp/color-customization-qa/immediate-qa-report.json`；均为隔离设置下的实际 Qt 控件渲染，不覆盖原生鼠标键盘链路和多屏 DPI。

## 已收束的 UI 配置

| 已移除的键 | 共同来源 |
|---|---|
| `border.accent`、`slider.fill`、`knob.arc`、`shell.menuPressed` | `accent.primary` |
| `panel.border` | `border.primary` |
| `knob.face`、`shell.buttonPressed` | `background.tertiary` |
| `shell.menuHover`、`shell.buttonHover` | `background.hover` |
| `shell.menuPressedText` | `accent.text` |

以上 10 个 UI 字段连同独立配置键均已删除，保留的消费者直接使用共同来源。表中的旋钮绑定是历史收束记录，旋钮实现与剩余的指示线职责已在 2026-11-01 清理。窗口关闭按钮、标题栏文字等仍保留其视觉职责。

## Studio Default 的领域绑定

下表记录 Studio Default 的领域绘制位置与角色绑定。其他主题可以为这些位置声明不同的来源。未写透明度时保留源颜色；表中的 Alpha 数值均表示乘数的分子，实际系数为该值 / 255，不覆盖源颜色的 Alpha。

| 曲线与波形绘制位置 | Studio Default 来源 |
|---|---|
| `curve.canvasBackground` | `panel.background` |
| `curve.canvasBorder` | `border.primary` |
| `curve.plotBackground`、`curve.handleFill` | `background.primary` |
| `curve.grid`、`waveform.dbScale.grid` | `border.tick`，Alpha 100 |
| `curve.plotBorder` | `border.primary` |
| `waveform.splitStereo.divider` | `border.primary` |
| `music.beatGridMajor`、`music.beatGridMinor` | `border.tick`，Alpha 分别为 48、18 |
| `curve.axisText`、`curve.handleBorder`、`waveform.dbScale.label` | `text.secondary` |
| `curve.handleLine` | `text.secondary`，Alpha 110 |
| `curve.handleActiveBorder`、`curve.handleActiveFill`、`curve.pointBorder`、`curve.pointSelectedFill` | `accent.primary` |
| `curve.pointSelectedBorder`、`curve.readoutText` | `text.primary` |
| `curve.crosshair` | 色卡预设值，不绑定边界职责 |
| `curve.crosshairContrast` | `background.primary` |
| `curve.readoutBackground` | `background.secondary` |
| `waveform.previewPlayhead` | `status.success`，与 Timeline 的播放头沿用同一来源；字段与独立配置键均删除 |

`waveform.previewGradientTop`、`waveform.previewGradientMid`、`waveform.previewLoopFill`、`waveform.previewLoopBorder`、`waveform.previewPeakClip`、`waveform.previewPeakSafe` 没有实际绘制消费者，成员、默认值和独立配置键一并删除，不再保留无效色板项。Source Editor 的字段名、值、空状态、警告、表面、边界和坐标均遵循共享职责；裁剪与循环标记继续使用缩略图和 External Editor 共用的波形样式。

| 仪表绘制位置 | Studio Default 来源 |
|---|---|
| `meter.interaction.crosshair` | 色卡预设值，不绑定边界职责 |
| `meter.interaction.readoutBackground` | `background.primary` |
| `meter.interaction.readoutText` | `text.primary` |
| `meter.peakMeter.canvasBackground`、`meter.peakMeter.trackBackground`、`meter.spectrum.canvasBackground`、`meter.spectrogram.canvasBackground` | `background.primary` |
| `meter.peakMeter.grid`、`meter.spectrum.horizontalGrid`、`meter.spectrum.verticalGrid`、`meter.spectrogram.grid` | `border.tick` |
| `meter.peakMeter.tickText`、`meter.spectrum.axisText`、`meter.spectrogram.axisText`、`meter.spectrogram.timeText` | `text.secondary` |
| `meter.peakMeter.label` | `text.primary` |

2026-11-01 的 Meter 调整取消零刻度与普通刻度的网格强度差异。Peak 和 Loudness 共用的电平柱在展开时，刻度、读数与网格按源 RGBA 的 100% 绘制；收起时统一乘 50%。柱体与峰值保持标记保留原有绘制。Spectrum 和 Spectrogram 的网格按 100% 绘制。相关职责的 Dark / Light 默认 Alpha 均为 255，用户自定义 Alpha 仍与业务比例相乘。

声谱图历史图像使用保留 Alpha 的 ARGB32 格式，与色表的 RGBA 数据一致。存在历史图像时直接合成到画布，只有图像不存在时才单独填充空历史色，避免半透明 `meter.spectrogram.emptyHistory` 叠加两次。回归覆盖色表 Alpha 0/64/128/255 的连续变化、未采样及已采样像素，以及图像存在与不存在时的单次合成结果；修复前两项新增检查失败，修复后相关 17 项主题与仪表测试通过。编辑器与全部 CI 测试目标构建通过，记录位于 `.temp/pr208-spectrogram-alpha/`。

当前 Meter、曲线和音源编辑器使用共用的 `AxisCrosshair` 绘制交错虚线。前景保留原 Dark `#272E3C` / Light `#B9C5D1` 预设色，对比底线继承 `background.primary`，不新增可编辑职责，默认 Alpha 为 255，业务按 100% 绘制；浮动读数的文字继承 `text.primary`，底色继承 `background.secondary`，同样不额外衰减。旧 `meter.interaction` 渲染位置也保留完整源 RGBA。

当前前端的 Loudness（RMS / LUFS）由 `MeterPanel` 调用 `PeakMeterRenderer`，与 Peak 共用色柱。旧 `LoudnessMeterRenderer` 的蓝、绿、橙三柱没有产品调用入口；已删除该绘制器、专属帧与样式、`meter.lufsMomentary/lufsShortTerm/lufsIntegrated` 三项职责及 `meter.loudness.*` 配置键。恢复 Studio Default 已保存设置时只移除这三项旧职责覆盖，其他配色与其他主题同名职责保留。音频测量能力不受影响。

VU 已退出产品入口。按用户要求删除遗留 `VUMeterRenderer`、专属渲染帧、`MeterStyle::VU`、默认值、色卡复制、职责绑定和全部 `meter.vu.*` 颜色及指标配置入口，并移除构建清单中的对应文件。

此前 `.temp/palette-alpha-review/palette-alpha-comparison.png` 的 03 使用了该旧绘制器，02 的浮动读数使用了旧渲染帧入口，不能代表当前前端的对应画面。本轮改用当前 `MeterPanel` 和 `CurveCanvas` 的实际渲染入口与示例数据核对，更新图为 `.temp/meter-color-review/meter-color-comparison.png`，沿用当前保存的 Dark 配色。`harken_editor` 与 `harken_editor_tests` 构建通过，58 项主题、资产、样式、仪表数据及历史图像测试通过；Dark / Light Alpha、交错虚线像素及当前组件样本共 44 项临时检查通过。QSS 归属检查与 `git diff --check` 通过，未改写用户设置。

| 节点与反馈绘制位置 | Studio Default 来源 |
|---|---|
| `nodeGraph.node.bodyNormal` | `background.tertiary` |
| `nodeGraph.node.bodySelected` | `background.hover` |
| `nodeGraph.node.contentLabel`、`nodeGraph.node.contentValue` | `text.primary` |
| `nodeGraph.node.titleText` | `text.primary` |
| `blueprint.edgeLabelText` | `text.secondary` |
| `feedback.success/warning/error/info` | 对应的 `status.success/warning/error/info` |
| `feedback.neutral` | `text.secondary` |
| `feedback.overlayHighlight` | `accent.primary` |

## 对象颜色职责

对象默认使用共同界面角色，不按 Sound、Audio Layer、Bus、Receive Bus、Attenuation、Action 等类型分配颜色。Blueprint 节点标题底统一使用 `header.background`，对象名称使用常规字重，名称文字和图标默认使用 `text.primary`；节点波形统一使用 `nodeGraph.node.blueprintWaveform`，不再按 Source、Self、Parent、Bus、ReceiveBus 角色着色。普通波形仍以透明度表达选择状态。

下列 18 个配置键及对应快照成员、内置值一并删除，不保留旧键兼容：

| 已移除的键组 | 数量与原用途 |
|---|---|
| `nodeGraph.sound/audioLayer/bus/receiveBus/attenuation/action/fallback` | 7 项，已无实际消费者的对象类型色 |
| `nodeGraph.header.sound/audioLayer/bus/receiveBus/attenuation/action/fallback` | 7 项，按对象类型区分的节点标题底 |
| `nodeGraph.role.source/parent/bus/receiveBus` | 4 项，按节点角色区分的波形色 |

用户对象色属于 `ProjectItem.color` 项目数据，设置、撤销和序列化不变；项目树图标色块、Timeline 对象剪辑和轨道继续读取它。Blueprint 原本未读取用户对象色，本轮不新增这一显示入口。音源失效、过期、播放等仍使用状态色；连接线和端口仍使用连接语义。

## 保留独立的颜色职责

| 类别 | 保留原因与范围 |
|---|---|
| 连接与运行状态 | 音频、控制、数值端口、路由、播放、暂停、虚拟化等表达不同含义，本轮不建立这些角色之间的新联动 |
| 专业数据 | Peak、RMS、LUFS、削波、波形模式、频谱色阶、相关度、衰减曲线与空间参数有各自的数据含义 |
| 彩色底上的文字 | 剪辑文字、削波标签保留对比需求，不能一律换成普通字段名或值 |
| 项目对象标记 | 用户手动指定的轨道、对象颜色属于项目数据，主题切换不改写它们 |
| 固定数据视觉 | 声谱图固定插值色阶仍作为连续数据映射处理，不替换成 UI 强调色或分类色板 |

启动波浪的四档抖动色阶使用 `startup.wave.shadow/low/high/highlight`。Dark 保留原蓝色色系 `#06121F`、`#173D5C`、`#5F9EC7`、`#E4F3FF`，Light 使用适合浅色启动表面的色阶；两张色卡各自保存覆盖，Reset 恢复当前色卡的默认值。波浪的 Alpha 在着色器中与启动窗口的 `background.primary` 合成：Alpha 为 0 时显示背景，为 255 时显示原色，中间值按比例混合；原生窗口仍为不透明表面。Logo 使用 `startup.logo.base/mid/highlight` 三个渲染颜色位置，Dark 保留原灰白扫光（`#B5B5B5`、`#D3D3D5`、`#FFFFFF`），Light 提供深蓝与青色扫光；Studio Default 不将这三项开放为用户编辑职责。Logo 底板使用 `panel.background`。`StartupLoadingWindow` 在创建 Renderer 时传入已恢复用户设置的只读主题快照；渲染线程读取该快照，不访问 ThemeManager。启动颜色修改在下次启动时生效。

外部主题可在颜色定义中为波浪或 Logo 渲染位置声明自己的职责绑定，并在各色卡中提供独立颜色。未绑定的启动颜色也可通过 `entries.tokens` 对应 JSON 的 `colors` 指定字面值。

此前深色启动动画的验收使用临时 Renderer 副本在交换缓冲前读回实际 OpenGL 首帧，不添加产品调试接口。历史记录位于 `.temp/color-customization-qa/`：`startup-restored-blue.png` 验证原四档蓝色及原 Logo 灰白；`startup-independent-ui.png` 验证修改界面强调色、主次文字色不影响波浪与 Logo；`startup-wave-custom.png` 验证波浪独立覆盖及底板对已有面板背景的复用。像素计数见 `startup-restored-verification.json`，设置入口见 `startup-palette-final-window.png`。这些记录不代表新增 Light 或外部主题 Logo 绑定的验收结果；早期 `startup-dark.png`、`startup-amber.png`、`startup-custom.png` 记录的是已撤销的通用界面颜色联动。

Alpha 验证使用隔离设置加载实际启动 Renderer，读回默认四色、四槽统一 Alpha 0/128/255，以及四槽分别为 0/64/128/255 的五组画面。Logo 外像素的默认 RGB 与原四档一致，透明度合成符合前景与 `background.primary` 的混合值（允许 1 级量化误差），最终窗口表面保持不透明。14 项像素检查通过，临时设置已恢复；截图为 `startup-alpha-*.png`，记录为 `startup-alpha-report.json`。

## 默认视觉变化边界

以下记录颜色收束阶段的历史变化。2026-10-31 的文字职责调整以本文前述 Studio Default 职责与领域绑定表为准。

共同来源保持现有四级文字、青色强调和深色工作区的值。Settings、Source Editor、Publisher 等字段名统一至次要文字；多行输入与只读有效值统一至主要文字。普通滑条、旋钮及被收束的曲线、仪表背景和边框保持原默认色。

节点字段名和连线标签由 `#AEB3BD` 改为 `#AAAAAA`，节点字段值由 `#E8E9EC` 改为 `#E0E0E0`。节点标题底取消类型色，统一为共享标题背景 `#1B2130`，默认标题文字由白色归入 `#E0E0E0`；普通节点波形取消角色色，统一使用现有 Blueprint 波形色 `#0FB5A8`，保留选中与未选中的透明度差异。未开放的独立 Source Editor 预览播放头由私有红色 `#FF6464` 改为与 Timeline 一致的 `#4CAF50`。其余被收束领域槽位保持原默认 RGBA，包括原有透明度。

实现入口：`HarkenStudio/Source/Core/ThemeSnapshot.h`、`ThemeColorResolver.*`、`BuiltinTheme.cpp`、`ThemePackageLoader.cpp`、`QtControlSkin.cpp`、`HarkenStudio/Resources/styles/dark_theme.qss`。公共表格和 Inspector 的其他外观边界见 [tables.md](tables.md) 与 [inspector-tables.md](inspector-tables.md)。

## 实际组件验收（2026-10-28）

使用当前构建的完整 Studio、内置深色主题和复制的 Playback 示例工程，隔离 QSettings 与 Hub 用户目录，通过 Studio 自带 IPC 和临时 Qt 进程内探针抓取实际窗口、状态和 palette。临时入口复用产品 Main、组件、资源和链接库；未新增产品接口。截图及响应记录位于本地 `.temp/color-ui-audit/`，不入仓。

| 范围 | 已核对内容 | 结果与边界 |
|---|---|---|
| Blueprint、Inspector | 实际节点、连线、字段名和值、选中对象、表格引用 | 未见明显配色或布局破坏 |
| Preferences | Appearance、Shortcuts、Layout、AI Agent、Extension；悬停及禁用 | 字段名、值、占位和禁用层次正常 |
| ObjectRef | 普通、只读、继承、悬停、禁用 | 补齐只读与禁用文字规则；有效只读值 `#E0E0E0`，禁用 `#555555` |
| 分段数值控件 | Inspector 的 Loop 数值编辑、实际焦点、选中文字、禁用 | 编辑值改用 `#E0E0E0`，禁用 `#555555`；选择高亮保持独立职责 |
| Timeline | 临时 Track 与实际音源 clip、名称、波形、坐标、网格 | 未见明显配色破坏 |
| Meter | Peak、Loudness、Spectrum、Spectrogram 的实际窗口 | 空闲状态的坐标、网格、引用和模式选择正常；未覆盖活跃电平、削波和完整热力色阶 |
| External Editor 的音源裁剪 | 从 Inspector 波形缩略图打开；Trim In、Trim Out 数值弹窗 | 补齐 `formLabel`；实测标签 `#AAAAAA`，输入与选择高亮正常 |
| 独立 SourceEditorPanel 组件 | 实际音源、元数据、范围字段、禁用态、波形与播放头绘制；360、480、800、1200 像素宽度及往返切换 | 9 个元数据标题完整显示，范围字段两列/四列均占满可用宽度；正式入口仍为 `FeatureDisabled`，以真实组件独立窗口验收 |

本次修复三类遗漏：ObjectRef 只读/禁用文字、分段数值编辑文字、裁剪弹窗字段名。不新增独立颜色项。

SourceEditorPanel 的元数据标题零宽度问题已修复：恢复 QLabel 默认宽度策略，使布局保留标题的文本宽度。范围字段重排时同步清理非活动列的伸展权重，修复窄窗口仍给空白列分配宽度的问题。四种宽度均测得 9 个标题宽度足以容纳文本，字段名 `#AAAAAA`、字段值 `#E0E0E0`、禁用文字 `#555555`；记录在 `.temp/color-ui-audit/source-followup.json`。播放头用实际组件的时钟驱动绘制核对，未将其计作音频播放链路验证。

Windows 桌面控制服务在本次环境不可用；以上为真实 Qt 窗口与组件渲染验收，不覆盖操作系统合成结果、多屏 DPI 和原生输入链路。早先代表性控件样张的像素变化比例不能外推为整套 Studio 的变化比例。

当前构建通过；首轮验收相关 CTest 共 15 项通过，Source Editor 补充整理后的主题与音源相关 CTest 共 13 项通过，两轮各有 1 项符号链接环境测试跳过。QSS 归属检查与 `git diff --check` 通过。

取消对象类别色后的构建通过，主题、项目树和信号图相关 CTest 共 16 项通过、1 项符号链接环境测试跳过。使用实际 `NodeItem` 绘制入口检查 8 种类型/角色组合与 6 种显示状态，共 48 组：标题底均为 `#1B2130`，同状态下的波形像素一致；选中、悬停、播放描边和注入的标题异常状态可辨。组合样张位于 `.temp/color-ui-audit/object-colors-nodes.png`；其中异常色是直接设置组件状态的绘制核对，不代表每种对象都存在音源异常业务。完整 Studio 的实际 Layer、Master Bus 节点也已核对，截图位于 `.temp/color-ui-audit/object-colors-studio.png`。
