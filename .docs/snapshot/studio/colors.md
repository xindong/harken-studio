# Studio 颜色职责与收束清单

最近核对：2026-10-29。

Studio 的颜色按语义职责分配。同一职责跨面板、跨控件使用共同来源；默认 RGB 相同但语义不同的数据和状态颜色仍可独立。对象类型由图标形状和文字表达，不分配类别识别色；对象标记色由用户定义。主题提供颜色，业务组件声明用途，Renderer 使用解析后的样式。

颜色收束阶段统一字段文字和通用交互，移除 10 个重复 UI 颜色字段，将 62 个领域颜色配置项收束为共享角色的解析结果，另删除 7 个 Source 预览独立颜色项和 18 个对象类别、节点角色颜色项。保持现有深色背景层次、专业数据配色和用户项目标记色。在此基础上，Preferences 的 Appearance 页面现已提供按职责分类的颜色定制。

## 通用颜色职责

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

`ThemeColorResolver` 在内置主题构造完成、外部主题父子继承及覆盖全部完成后，统一生成共享样式。Qt 的主要文字、禁用文字、链接、高亮及高亮文字也在此解析。

`ThemeSnapshot` 中保留领域 Renderer 所需的已解析颜色槽位。这些槽位是输出，不再是独立的主题配置项；保留它们使 Renderer 只依赖自身样式，不需要读取全局主题管理器。快照发布后保持不可变，重新加载产生新快照。

主题包使用 `schemaVersion: 6`，不接受旧版本；已收束的细分键也不作为别名保留。旧版本返回版本诊断，版本 6 中继续填写已移除键会返回 `UnknownSemanticKey`，不会静默忽略。调整共享用途须修改下表列出的源角色。

## 用户颜色定制

`ThemeColorCatalog` 定义对外开放的 76 个源颜色角色，共 11 类。每项包含职责名称、用途说明和主题语义键；不开放 `ThemeColorResolver` 的派生槽位，不重新引入被删除的细分项。窗口壳层按钮细节、Logo 扫光和声谱图固定热力色阶未加入这份用户定制目录；项目对象标记色仍属于项目数据。启动波浪的四档色阶单独表达动画配色，不绑定界面强调色；Logo 底板复用已有 `panel.background`。

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

入口为可停靠的 **Preferences → Appearance** 面板。上方 Appearance 分组保留 `Theme` 和 Information Language。`Theme` 下拉字段切换完整主题，包括字体、图标及控件尺寸。下方 **Global Colors** 分组统一包含 Color Presets、分类与搜索、详细颜色表格及重置操作。每个主题提供一个默认色板；切换主题时自动选中其默认色板，之后可以通过 Color Presets 标准下拉框选择任意全局色板。Color Presets 沿用上方字段的标准表单列比例，左侧标签、右侧下拉框。色板应用不改变主题身份或非颜色属性。下拉框的收起状态和展开选项均在左侧显示色板名称，右侧绘制较薄的色条，不附加 `Default colors` 文案。色条按顺序展示主背景、控件背景、选中背景、强调色、主要文字、次要文字、成功、警告、错误、信息十种代表色。

内置完整主题只有 `Harken Dark`（`harken.builtin.dark`）。其默认色板和暖炭背景、琥珀强调色的 `Harken Amber` 色板属于同一个全局色板集合，Amber 不再作为主题发现、选择或继承。已安装外部主题各自提供的默认色板也进入这一共享集合，所有主题都能使用。主题默认色板 ID 为 `theme:<themeId>`，独立 Amber 色板 ID 为 `harken.palette.amber`；色板身份独立于当前主题，下拉框按已选色板 ID 标识选择。

Global Colors 编辑的是当前全局色板，修改不会另存为某个主题的私有配色；在其他主题中选中同一色板时使用同一份修改。分类和搜索共同过滤颜色列表，搜索覆盖职责名称、分类、用途说明及颜色键。列表并列展示当前颜色与 `Palette Default`，两列均使用 44 像素宽的色块，下方等宽的 3 像素细条与色块紧贴、共用外边框，中间仅以一条边框线分隔。细条按 Alpha 值显示不透明度：0 为空条，255 为满条；填充固定白色、背景固定黑色，不参与色板管理。选中项在下方显示用途说明。双击表格行打开 `ColorEditorDialog`：上方是 SV 二维选色区域和窄 H 色相条，下方分别排列 H/S/V、R/G/B 数值，底部通过公有 `LinearSliderPresenter` 提供共用的 A 透明度滑条与数值。H 为 0–359，S/V 为 0–100，R/G/B/A 为 0–255，数值不显示单位后缀；A 独立于两种色彩坐标，修改透明度不重算 RGB。面板不包含 Basic colors、Custom colors、HTML 输入或独立大预览色块。底部操作行左侧的 20×20 吸管图标用于屏幕拾色，按钮沿用公有图标按钮的圆角和交互状态，通过 `iconButtonBackground` 使用 `background.tertiary` 作为常态背景；拾色保留当前 A 值；拾色时按 Esc 取消此次拾取并恢复拾色前的颜色。

底部右侧并排保留 `Reset Selected` 和 `Reset All`，分别恢复所选色板中的选中颜色角色和全部颜色覆盖。默认值来自该色板本身，恢复操作立即保存，并影响所有主题随后使用该色板时的颜色；其他色板的修改保持不变。

主题选择、色板选择、完成颜色编辑、恢复默认及 Information Language 均即时保存，不保留全局 Apply/Cancel 或预览开关。切换主题选择目标主题的默认色板，重新加载主题和程序重启则保留已保存的主题与所选全局色板。颜色对话框支持 Alpha 与实时预览：预览只发布临时快照，OK 后保存，Cancel、Esc 或关闭选色器恢复已提交外观。关闭或停靠 Preferences 不撤销已保存设置；AI 连接配置仍使用独立的 Save Connection 事务。

主题选择保存在 `Appearance/ThemeId`，色板选择保存在 `Appearance/PaletteId`，颜色覆盖以色板 ID 为键保存在 `Appearance/PaletteColors`，不修改主题包或项目。旧 `Appearance/Colors` 的主题键迁移为对应默认色板键；旧 Amber 主题选择迁移为 Dark 主题加 Amber 色板，已有颜色编辑保留。成功保存新结构后移除旧 Colors 键。暂不可用色板的有效覆盖仍保留，来源主题重新安装后可继续使用。

组合顺序为完整主题 → 全局色板的完整颜色集合 → 所选色板的用户颜色覆盖 → `ThemeColorResolver` → 最终不可变快照。色板携带主题包支持的完整颜色值及明暗模式，包含未开放为编辑项的合法颜色；应用时只复制颜色，保留目标主题的尺寸、字体、组件布局和资源。用户覆盖与主题包共用 `applyThemeColor` 的语义赋值路径。所有主题使用同一组合路径，避免应用同一色板时残留目标主题的旧颜色。预览取消使用已提交候选，未知角色、非法颜色、存储不可写或跨线程发布均不提交新设置。

连续预览同一主题和色板时复用首次加载的基础候选与色板，每次都从未应用用户覆盖的颜色生成结果，避免反复扫描主题清单及解析继承链，也避免移除覆盖后残留上一次预览的颜色。取消、成功提交、成功切换或重新加载主题后结束该缓存的生命周期；提交和显式重新加载继续读取基础主题和色板。

实现入口为 `ColorPalette.*`、`BuiltinTheme.*`、`ThemeColorCatalog.*`、`ThemeManager.*`、`ThemePackageLoader.*`、`Dialogs/ColorEditorDialog.*` 和 `Dialogs/Settings/Pages/AppearancePage.*`。契约测试覆盖单一内置主题、全局色板跨主题共享、主题切换默认色板、保留非颜色属性、预览取消、重置、重载、重启及旧设置迁移。

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

## 已收束的领域配置

下表左列为原主题键；同一行列出的项目现在共享右列的源。未写透明度时保留源颜色，写明透明度时在解析结果上应用固定 Alpha，范围为 0 至 255。

| 原曲线与波形键 | 共同来源 |
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

| 原仪表键 | 共同来源 |
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

| 原节点与反馈键 | 共同来源 |
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

启动波浪的四档抖动色阶使用 `startup.wave.shadow/low/high/highlight`，默认依次为 `#06121F`、`#173D5C`、`#5F9EC7`、`#E4F3FF`，精确保留原蓝色色系。两套内置预设的波浪默认值相同，各自可保存覆盖，Reset 恢复这组默认值。波浪的 Alpha 在着色器中与启动窗口现有的 `background.primary` 合成：Alpha 为 0 时显示背景，为 255 时显示原色，中间值按比例混合；原生窗口仍为不透明表面。Logo 本体保留原灰白扫光（`#B5B5B5`、`#D3D3D5`、`#FFFFFF`），不与波浪或文字主题色联动；底板使用已有 `panel.background`，没有独立配置项。`StartupLoadingWindow` 在创建 Renderer 时传入已恢复用户设置的只读主题快照；渲染线程读取该快照，不访问 ThemeManager。波浪自定义在下次启动时生效。

外部主题通过 `manifest.entries.tokens` 对应 JSON 的 `colors` 配置上述四键；已用实际外部包读取验证，不增加主题格式版本或单独的启动视觉文档。

启动动画的验收使用临时 Renderer 副本在交换缓冲前读回实际 OpenGL 首帧，不添加产品调试接口。当前记录位于 `.temp/color-customization-qa/`：`startup-restored-blue.png` 验证原四档蓝色及原 Logo 灰白；`startup-independent-ui.png` 验证修改界面强调色、主次文字色不影响波浪与 Logo；`startup-wave-custom.png` 验证波浪独立覆盖及底板对已有面板背景的复用，Logo 扫光保持原灰白。像素计数见 `startup-restored-verification.json`，设置入口见 `startup-palette-final-window.png`。分类 Reset 已确认只移除波浪覆盖，保留其他分类的修改。早期 `startup-dark.png`、`startup-amber.png`、`startup-custom.png` 记录的是已撤销的通用界面颜色联动，不作为当前波浪及 Logo 配色的验收依据。

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
