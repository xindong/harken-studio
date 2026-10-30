# Studio 主题色卡与颜色职责

最近核对：2026-10-30。

主题拥有自己的颜色职责、职责到绘制位置的绑定和一个或多个色卡。色卡提供该主题各职责的颜色值及明暗模式，用户修改按主题和色卡隔离。不同主题可以使用不同的职责清单；一个主题可以让网格复用边界色，另一个主题可以提供独立网格色。Renderer 继续消费完整、不可变的样式快照。

内置主题正式命名为 **Studio Default**（`harken.studio.default`），提供 **Dark**（`dark`，默认）和 **Light**（`light`）两张色卡。Dark 保留原有深色视觉；Light 提供适合浅色表面的文字、交互、边界和领域绘图配色。之前用于测试的 Amber 已移除。项目对象标记色仍属于项目数据。

## Studio Default 颜色职责

以下职责与共享关系属于 Studio Default 的设计，不要求其他主题采用相同目录。

| 职责 | 主题键 | 使用边界 |
|---|---|---|
| 主要文字 | `text.primary` | 普通字段值、只读有效值、普通正文；单行、多行输入与节点字段值一致 |
| 次要文字 | `text.secondary` | 字段名、坐标刻度、辅助标签、节点字段名与连线说明 |
| 弱化文字 | `text.muted` | 提示、空状态、占位和弱化元信息；不因字段只读就使用此色 |
| 禁用文字 | `text.disabled` | 控件不可用时的文字，独立于只读状态 |
| 强调 | `accent.primary` | 控件激活、焦点与强调边框、滑条填充、旋钮有效弧线、曲线编辑点与手柄激活 |
| 强调悬停 | `accent.hover` | 强调元素自身的悬停变化 |
| 强调底上的文字 | `accent.text` | 高亮菜单和选择背景上的文字，不能直接用普通字段文字替代 |
| 基础表面 | `background.primary/secondary/tertiary` | 保留既有表面层级，节点普通主体使用 tertiary |
| 交互表面 | `background.hover/selected` | 悬停与选中；节点选中主体沿用 hover 的既有视觉 |
| 普通边界 | `border.primary/secondary/view` | 控件与面板边框、较强边框、画布外框和网格的基础色 |
| 面板与轨道表面 | `panel.background`、`header.background`、`track.background`、`track.backgroundAlternate`、`bus.background` | 保留既有较暗的工作区层次，不因 RGB 接近而合并为一个背景 |
| 反馈 | `status.success/warning/error/info` | 操作反馈；领域 feedback 同义槽位由这里解析 |
| 分类色板 | `palette.categorical0` 至 `palette.categorical6` | 区分多个条目，不承担固定对象类型或强度数值语义 |

字段名统一声明 `textRole=formLabel`。普通文本标签可声明 `textRole=primary/secondary/muted`；节点内容保留 `nodeContentRole=label/value` 以适配现有绘制入口，但不再拥有独立颜色来源。`roValue` 只读值使用主要文字。禁用状态覆盖这些文字角色。

对象引用的有效只读值同样使用主要文字；关闭选择入口不等同于禁用整个控件。继承引用保留次要文字，失效引用保留错误色，真正禁用时统一使用禁用文字。分段控件进入数值编辑时使用主要文字，普通分段选项保持原有层次。

同一颜色可能由 QSS、QPainter 或 QPalette 消费；绘制技术和所在面板都不构成新增颜色角色的理由。透明度、渐变与交互几何可以不同，颜色来源仍可共享。

## 解析与发布

`ThemeColorResolver` 接受当前主题的职责绑定与当前色卡值，将它们写入固定的渲染颜色位置。绑定可指定独立 Alpha。原有共享文字、网格、节点背景等派生关系已成为 Studio Default 的绑定声明，解析器不再为所有主题强制建立相同关系。

Studio Default 将 Qt 原生窗口、输入框、提示框和按钮背景绑定到对应表面职责，并同步原生文字与辅助颜色；色卡默认值、用户预览和取消恢复共用这些绑定。其他主题可以定义自己的对应关系。

`ThemeSnapshot` 中保留领域 Renderer 所需的已解析颜色槽位。主题职责可以绑定这些槽位，Renderer 只依赖自身样式，不需要读取任意主题职责名或全局主题管理器。快照发布后保持不可变，重新加载产生新快照。

主题包使用 `schemaVersion: 7`。`entries.colors` 可指定颜色定义文件，文件声明 `roles`、`palettes` 和 `defaultPalette`。每个职责提供 `key`、`group`、`label`、`description` 及 `bindings`；每个绑定提供有效绘制位置 `target`，可选 `alpha`。每张色卡声明 `id`、`displayName`、`colorScheme` 与完整的职责颜色 `colors`。显式颜色定义替换继承的职责和色卡定义；未声明时继承基础主题。色卡 ID 仅在所属主题中有效。

主题加载校验角色、色卡和绘制目标的唯一性，检查职责引用、颜色值、Alpha 及色卡完整性。未知目标不会静默忽略。职责可使用主题自己的名称，但必须绑定到实际渲染位置。

声明 `entries.colors` 的继承层以该文档中的色卡值为准，同层 `tokens` 或 `visuals` 再赋值任何已绑定目标会报告冲突。未声明颜色定义的继承层可通过任意不带固定 Alpha 的绑定目标更新所属职责，绑定顺序不改变语义；同层多个目标对同一职责给出不同颜色会报告冲突，给出相同颜色则允许。带固定 Alpha 的目标不能直接反向覆盖职责值。未绑定的绘制位置可继续使用 `tokens` 或 `visuals` 中的字面颜色。

### 控件样式缓存与启动边界

`WidgetAppearance` 将目标控件匹配与祖先路径匹配分层缓存。规则先按控件类型筛选，再记录实际读取的名称、属性、类型和必要父链，包括导致不匹配的条件。祖先变化复用目标控件匹配结果，同一轮匹配每个属性只读取一次；全局规则与局部样式共享属性版本，局部样式作用域按当前父链查找。颜色声明变化复用既有匹配结果，选择器变化只清理对应样式的规则缓存。控件销毁立即清理缓存与观察连接，并使仍被引用的依赖失效。

`HarkenStyle` 负责调色板和几何更新，不将每次 `StyleChange` 当成整棵子树失效。输入观察器仅同步仍被缓存使用的属性，样式层消费变化结果，不重复扫描。真实结构变化引起的后代刷新在事件循环内合并，跳过被祖先覆盖的重复请求及尚未 polish 的控件；首次 polish、显示和样式读取补齐当前外观。Preferences 与业务面板的现有按需创建边界不变。

拥有独立样式表面的自定义 `QWidget` 面板须自行声明背景绘制职责。`ControlPanel` 显式启用样式背景，使现有背景色与顶部边框在展开、重显及颜色预览后仍被绘制；共享样式层不因此扩大自动背景绘制范围。真实面板的像素与几何检查先复现缺失再验证修复，41 项相关测试在原生 Windows 与 offscreen 平台通过，默认主窗口与 #193 前的截图仍逐像素一致。

2026-10-30 启动对照使用 #193 合入前主线 `aaf1d2fe` 与基于 `25befb47` 的样式修复工作区：同机、同编译配置、隔离设置、原生 Windows，每次新进程创建空工程，预热后交替运行五次。main 入口到 Ready 的中位数分别为 966.389 / 1014.414 ms；主窗口结构为 119.142 / 140.861 ms，初始布局为 102.448 / 110.717 ms，工程界面恢复为 111.629 / 121.047 ms。历史时间用于差异审查，不作为必须完全一致的指标。完整启动的离屏路径在 Qt 的 `QWidget::render(QPicture)` 中异常，因此端到端验收采用原生平台，未修改该绘制路径。

40 项相关测试在原生 Windows 平台通过，128 组颜色预览渲染对照通过。通用回归测试覆盖缓存与重新解析结果的等价性，包括初始失配后来命中、复合条件短路恢复、原生祖先属性、替换选择器、换父级、局部样式、销毁及地址复用。绘制层恢复 Qt 自动背景范围，保留工具栏在原生样式包装下的补绘入口与原生文字对齐声明，并由尺寸握柄的样式规则接管其绘制。统一状态栏示例文字后，修复版与 #193 前的主窗口截图逐像素一致，677 个控件、115 个可见控件及全部控件几何一致。计时与视觉记录位于本地 `.temp/startup-before193-comparison/targeted-layered-*`、`normalized-verification-3/`；诊断位于 `.temp/startup-cache-detail-targeted/targeted.log`，颜色预览渲染位于 `.temp/theme-preview-profile/targeted-layered/`。

实际 MainWindow 的背景色、强调色各自确认/取消四组检查通过：1426 个控件、542 个跟踪控件、8 个模型保持不变，编辑绑定、预览中的已提交设置及最终持久值均符合预期，未触发模型重置或行增删。记录位于 `.temp/theme-preview-profile/targeted-layered-results.json`。

PR 集成复验基于已含 #194 的主线 `9235952a`：编辑器与测试目标重新构建，40 项原生测试、程序自检、四组实际窗口预览检查及完整截图对照再次通过。同条件五次交替启动的 #193 前 / 集成修复版中位数为 988.154 / 1063.991 ms；主窗口结构为 118.384 / 157.918 ms，初始布局为 103.162 / 112.914 ms，工程界面恢复为 112.420 / 119.011 ms。这组数据包含后续主线改动，与前一组样式修复独立对照分开记录。证据位于 `.temp/startup-before193-comparison/pr-*` 与 `.temp/theme-preview-profile/pr-main-results.json`。

## 用户颜色定制

Studio Default 提供 76 个可编辑源颜色角色，共 11 类。每项包含职责名称、用途说明、主题职责键和绘制绑定。外部主题可声明自己的目录，Appearance 按当前主题生成分类与颜色行。窗口壳层按钮细节、Logo 扫光和声谱图固定热力色阶未加入 Studio Default 的用户定制目录；启动波浪的四档色阶单独表达动画配色，Logo 底板复用 `panel.background`。

| 分类 | 数量 | 开放范围 |
|---|---|---|
| Text | 4 | 主文字、次文字、弱化文字、禁用文字 |
| Surfaces | 8 | 主背景、控件背景、抬升背景、面板、标题、轨道、交替轨道、Bus 背景 |
| Interaction | 10 | 强调及悬停、强调底文字、悬停/选中背景、三类边界、滑条底轨、旋钮指示 |
| Feedback | 4 | 成功、警告、错误、信息 |
| Categorical palette | 7 | 区分多个条目的七个分类色 |
| Waveform | 7 | 静态波形低/高幅度、三段频率贡献、裁剪及循环标记 |
| Curves | 4 | 音量衰减、音高衰减、参数绑定、Grid 淡变 |
| Meters | 11 | Peak 四段渐变、RMS、峰值保持、频谱主线/峰值线、三种 LUFS 读数 |
| Timeline | 8 | Clip 背景、上下文范围高亮、Clip 文字及普通/选中边界、进入/退出/过渡标记 |
| Blueprint | 9 | 画布、网格点、三类端口、三类关系连线、节点波形 |
| Startup | 4 | 启动波浪的暗部、低色阶、高色阶和高光 |

入口为可停靠的 **Preferences → Appearance** 面板。上方 Appearance 分组保留 `Theme` 和 Information Language。`Theme` 下拉字段切换完整主题，包括字体、图标及控件尺寸。下方 **Theme Colors** 分组包含 Color Palette、分类与搜索、详细颜色表格及重置操作。切换主题时选中目标主题的默认色卡，Color Palette 仅列出该主题提供的色卡。字段沿用标准表单列比例；色卡选择不改变主题身份或非颜色属性。下拉框左侧显示色卡名称，右侧色条按顺序展示主背景、控件背景、选中背景、强调色、主要文字、次要文字、成功、警告、错误、信息十种渲染颜色。色条通过当前主题绑定生成，不依赖主题使用固定的职责名称。

Theme Colors 编辑当前主题的当前色卡。即使另一个主题使用相同色卡 ID，颜色修改也彼此隔离。分类和搜索共同过滤颜色列表，搜索覆盖职责名称、分类、用途说明及颜色键。列表并列展示当前颜色与 `Palette Default`，两列均使用 44 像素宽的色块，下方等宽的 3 像素细条与色块紧贴、共用外边框，中间仅以一条边框线分隔。细条按 Alpha 值显示不透明度：0 为空条，255 为满条；填充固定白色、背景固定黑色，不参与色卡管理。选中项在下方显示用途说明。双击表格行打开 `ColorEditorDialog`：上方是 SV 二维选色区域和窄 H 色相条，下方分别排列 H/S/V、R/G/B 数值，底部通过公有 `LinearSliderPresenter` 提供共用的 A 透明度滑条与数值。H 为 0–359，S/V 为 0–100，R/G/B/A 为 0–255，数值不显示单位后缀；A 独立于两种色彩坐标，修改透明度不重算 RGB。底部操作行左侧的 20×20 吸管图标用于屏幕拾色；拾色保留当前 A 值；拾色时按 Esc 取消此次拾取并恢复拾色前的颜色。

底部右侧并排保留 `Reset Selected` 和 `Reset All`，分别恢复当前主题所选色卡中的选中角色和全部颜色覆盖。默认值来自该色卡，恢复操作立即保存，其他主题和色卡的修改保持不变。

主题选择、色卡选择、完成颜色编辑、恢复默认及 Information Language 均即时保存。切换主题选择目标主题的默认色卡，重新加载主题和程序重启则保留已保存的主题与所选色卡。颜色对话框支持 Alpha 与实时预览：预览只发布临时快照，OK 后保存，Cancel、Esc 或关闭选色器恢复已提交外观。关闭或停靠 Preferences 不撤销已保存设置；AI 连接配置仍使用独立的 Save Connection 事务。

主题选择保存在 `Appearance/ThemeId`，色卡选择保存在 `Appearance/PaletteId`。颜色覆盖以主题 ID、色卡 ID、职责键分层保存在 `Appearance/ThemePaletteColors`，不修改主题包或项目。旧设置在读取时迁移到所属主题的默认色卡；测试 Amber 不再作为可选配色保留。暂不可用主题的旧默认覆盖以空色卡键暂存，主题恢复后再迁入其声明的默认色卡；已有明确色卡覆盖优先。活动职责失效导致恢复失败时，仍保留其他主题与色卡的覆盖，避免随后保存时丢失。

读取当前及旧版存储格式时跳过无法解析的色值，保留所有有效覆盖，包括未安装主题的私有职责。损坏的存储值不阻断后续切换、预览或保存；新提交的非法颜色仍被拒绝。

组合顺序为主题结构与色卡基础快照 → 当前主题当前色卡的用户覆盖 → 按主题职责绑定解析 → 最终不可变快照。色卡保留主题的尺寸、字体、组件布局和资源。用户覆盖与主题包共用 `applyThemeColor` 的绘制位置赋值路径。预览取消使用已提交候选，未知角色、非法颜色、存储不可写或跨线程发布均不提交新设置。

连续预览同一主题和色板时复用首次加载的基础候选与色板，每次都从未应用用户覆盖的颜色生成结果，避免反复扫描主题清单及解析继承链，也避免移除覆盖后残留上一次预览的颜色。取消、成功提交、成功切换或重新加载主题后结束该缓存的生命周期；提交和显式重新加载继续读取基础主题和色板。

选色器立即更新自身颜色，Appearance 页面在连续输入期间以 16 ms 周期合并输入，只发布最新颜色；没有待发值时停止定时器。已有定时器不因新输入反复延后，也不在每次绘制完成后重新等待完整周期。确认直接保存选色器的最后值，不依赖待发定时器；取消、隐藏、销毁或外部提交会清除待发输入，避免结束编辑后重新出现预览。颜色表只更新正在编辑的行与当前色板缩略图。

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

以上 10 个 UI 字段连同独立配置键均已删除，消费者直接使用共同来源。旋钮指示线、窗口关闭按钮、标题栏文字等仍保留其视觉职责。

## Studio Default 的领域绑定

下表记录 Studio Default 的领域绘制位置与角色绑定。其他主题可以为这些位置声明不同的来源。未写透明度时保留源颜色，写明透明度时在解析结果上应用固定 Alpha，范围为 0 至 255。

| 曲线与波形绘制位置 | Studio Default 来源 |
|---|---|
| `curve.canvasBackground` | `panel.background` |
| `curve.canvasBorder` | `border.view` |
| `curve.plotBackground`、`curve.handleFill` | `background.primary` |
| `curve.grid`、`waveform.dbScale.grid` | `border.view`，Alpha 100 |
| `curve.plotBorder` | `border.primary` |
| `curve.axisText`、`curve.handleBorder`、`waveform.dbScale.label` | `text.secondary` |
| `curve.handleLine` | `text.secondary`，Alpha 110 |
| `curve.handleActiveBorder`、`curve.handleActiveFill`、`curve.pointBorder`、`curve.pointSelectedFill` | `accent.primary` |
| `curve.pointSelectedBorder`、`curve.readoutText` | `text.primary` |
| `curve.crosshairContrast` | `background.primary`，Alpha 153 |
| `curve.readoutBackground` | `background.secondary` |
| `waveform.previewPlayhead` | `status.success`，与 Timeline 的播放头沿用同一来源；字段与独立配置键均删除 |

`waveform.previewGradientTop`、`waveform.previewGradientMid`、`waveform.previewLoopFill`、`waveform.previewLoopBorder`、`waveform.previewPeakClip`、`waveform.previewPeakSafe` 没有实际绘制消费者，成员、默认值和独立配置键一并删除，不再保留无效色板项。Source Editor 的字段名、值、空状态、警告、表面、边界和坐标均遵循共享职责；裁剪与循环标记继续使用缩略图和 External Editor 共用的波形样式。

| 仪表绘制位置 | Studio Default 来源 |
|---|---|
| `meter.interaction.crosshair` | `text.primary`，Alpha 89 |
| `meter.interaction.readoutBackground` | `background.primary`，Alpha 221 |
| `meter.interaction.readoutText` | `text.primary`，Alpha 229 |
| `meter.peakMeter.canvasBackground`、`meter.peakMeter.trackBackground`、`meter.spectrum.canvasBackground`、`meter.spectrogram.canvasBackground` | `background.primary` |
| `meter.peakMeter.grid`、`meter.spectrum.horizontalGrid`、`meter.spectrum.verticalGrid`、`meter.spectrogram.grid` | `border.view` |
| `meter.peakMeter.tickText`、`meter.loudness.targetText`、`meter.vu.analogNegativeTick`、`meter.spectrum.axisText`、`meter.spectrogram.axisText`、`meter.spectrogram.timeText` | `text.secondary` |
| `meter.peakMeter.label`、`meter.loudness.labelText`、`meter.loudness.valueText` | `text.primary`，Alpha 204 |
| `meter.loudness.canvasBackground`、`meter.vu.canvasBackground` | `panel.background` |
| `meter.loudness.trackBackground`、`meter.vu.trackBackground` | `track.background` |
| `meter.vu.titleText`、`meter.vu.analogLabelText`、`meter.vu.barLabelText` | `text.primary` |
| `meter.vu.analogArc`、`meter.vu.barGrid` | `border.primary` |
| `meter.vu.analogValueText`、`meter.vu.barValueText` | `text.muted`，保留 VU 读数原有的弱化层级 |

| 节点与反馈绘制位置 | Studio Default 来源 |
|---|---|
| `nodeGraph.node.bodyNormal` | `background.tertiary` |
| `nodeGraph.node.bodySelected` | `background.hover` |
| `nodeGraph.node.contentLabel`、`blueprint.edgeLabelText` | `text.secondary` |
| `nodeGraph.node.contentValue`、`nodeGraph.node.titleText` | `text.primary` |
| `feedback.success/warning/error/info` | 对应的 `status.success/warning/error/info` |
| `feedback.neutral` | `text.muted` |
| `feedback.overlayHighlight` | `accent.primary` |

## 对象颜色职责

对象默认使用共同界面角色，不按 Sound、Audio Layer、Bus、Receive Bus、Attenuation、Action 等类型分配颜色。Blueprint 节点标题底统一使用 `header.background`，标题文字和图标默认使用 `text.primary`；节点波形统一使用 `nodeGraph.node.blueprintWaveform`，不再按 Source、Self、Parent、Bus、ReceiveBus 角色着色。普通波形仍以透明度表达选择状态。

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
