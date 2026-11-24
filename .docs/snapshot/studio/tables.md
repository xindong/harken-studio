# Studio 表格快照

## 数量与范围

| 类别 | 去重用途数 | 页面 / 对象类型位置数 |
|---|---:|---:|
| 配置表 | 12 | 27 |
| 展示表 | 15 | 15 |
| 普通表格合计 | 27 | 42 |
| 导航配置（不计入普通表格） | 2 | 2 |

注：Send、Sidechain、Effects 各自在六类对象中使用，各按用途计一项、按对象类型计六处。对象实例、Binding 字段及同一对话框的不同打开位置不重复计数；Match、Blend 等专用编辑面另列。

## 普通表格清单

| ID | 分类 | 位置 / 用途 | 内容注解 |
|---|---|---|---|
| C01 | 配置 | Sound Inspector / Sources | 按语言分组，仅同组内移动 |
| C02 | 配置 | Container Selection Editor / Random、Sequence | Child 条目；Weight 仅 Random 显示 |
| C03 | 配置 | Track Inspector / Clips | 移动保留片段身份与时间语义 |
| C04 | 配置 | Action Inspector / Steps | Action 执行步骤 |
| C05 | 配置 | Attenuation Inspector / Dimensions | 衰减维度与曲线 |
| C06 | 配置 | Variable Inspector / Labels | 序号为真实索引；移动后名称与身份进入原索引位置，范围外条目不参与 |
| C07 | 配置 | Sound、Container、Layer、Track、Segment、Bus / Send | 混入目标 Bus 的发送增益 |
| C08 | 配置 | External Editor / Binding | 仅编辑本地贡献 |
| C09 | 配置 | Container Selection Editor / Transition | # / Source / Exit / Fade Out / Target / Entry / Fade In；按匹配具体程度排序，默认规则最后 |
| C12 | 配置 | Conversion Editor / Configuration | # / Original size range / Target sample rate / Target format / Target quality。表上方的文件大小分段条与行号从 1 开始对应，规则按原始文件大小从 `0 B` 到 `Inf` 排列 |
| C13 | 配置 | Sound、Container、Layer、Track、Segment、Bus / Effects | Basic > Effect；# / Effect / Active；本地效果器链配置 |
| C14 | 配置 | Sound、Container、Layer、Track、Segment、Bus / Sidechain | 辅助信号路由与目标 Ducking 开关 |
| D01 | 展示 | Bus Inspector / Inputs | 路由反向查询，无序号列 |
| D02 | 展示 | Bus Inspector / Receives | 按来源显示 Type 与 Ducking，# 仅展示行号，不提供行移动 |
| D03 | 展示 | Control / Variables | 变量试听值及作用域 |
| D04 | 展示 | Control / Variable Bindings | 变量绑定与试听曲线 |
| D05 | 展示 | Publisher / Pack | Init、General 与语言分区；以勾选确定导出范围 |
| D07 | 展示 | Remote Connect / Targets | 远程目标，左下刷新 |
| D08 | 展示 | Settings / Shortcuts | 按类别展示命令与快捷键 |
| D09 | 展示 | Settings / Layout | 已有布局资源，支持重命名与删除 |
| D10 | 展示 | Monitor / Voices | Voice 运行记录 |
| D11 | 展示 | Monitor / Buses | Bus 运行记录 |
| D12 | 展示 | Monitor / Effects | Effect 运行记录 |
| D13 | 展示 | Monitor / Events | 事件运行记录 |
| D14 | 展示 | Import Audio Resources / 同名音频冲突选择 | 导入冲突处理 |
| D15 | 展示 | Conversion Editor / Result | Sound / Active Source / Language / Original format / Original rate / Original size / Converted format / Converted rate / Converted size / Ratio。两个 format 列显示音频编码。只读，表体直达底边。无表脚、状态列或汇总行 |
| D16 | 展示 | External Editor / Sound Source / File Properties | Property / Value 两列，随 Original 或 Converted 显示当前文件的属性 |

## 分类与公共能力

| 项目 | 配置表 | 展示表 |
|---|---|---|
| 数据用途 | 编辑字段或表单中的配置条目 | 枚举实体、资源、依赖、查询或运行结果 |
| 编辑与保存 | 由具体业务决定 | 由具体业务决定 |
| 序号与行移动 | 按需提供序号或拖动柄；移动受业务约束 | 无配置序号与移动柄，通常按数据或查询排序 |
| 详情展开 | 按需提供 | 不使用配置详情占位；分组可独立展开 |
| 外部拖入 | 按场景启用，落下时校验并提交 | 同左 |
| 视觉列换位 | 不启用 | 按需启用，字段含义与业务顺序保持不变 |
| 高度 | 默认按内容展开；独立编辑面可撑满宿主 | 默认撑满宿主，也可按内容展开 |
| 表尾 | 按需提供增删、动作与状态，无内容时不占位 | 同左 |
| 刷新入口 | 无手动刷新入口 | 有实际刷新动作时按需提供 |
| 分组 | Sources 语言组等 | Pack 分区、快捷键分类等 |

| 公共行为 | 当前表现 |
|---|---|
| 内容高度 | 展开全部行与详情，列宽适配视口，无内部滚动；滚轮交由外层页面 |
| 撑满高度 | 使用宿主可用空间，纵向滚动槽常驻；允许横向滚动的表格同时保留横向槽 |
| 最小高度 | 保留完整表头、4 px 表体留白、所需滚动槽及外框；整表表尾位于滚动区外 |
| 列宽 | 连续拖动优先由相邻可见列补偿；到达最小宽度前不改变其他列；临时压缩不覆盖独立列宽偏好 |
| 列宽调节反馈 | 悬停可调节边界时仅将原有 1px 边线切为主题 accent；组合表头、原生表头、树表和矩阵表头共用该行为，固定列与禁用状态不高亮，边界随列隐藏、重排和滚动更新 |
| 初始与刷新选择 | 首次打开不自动选首行；刷新按稳定身份保留已有选择或未选中状态 |
| 删除后选择 | 选原位置下一行，末行则选上一行；有分组时限定同组，组空则清选；多选保留存活项 |
| 取消选择 | 表体及表尾空白、Esc；原生单选表支持 Ctrl 点选已选行。编辑器和移动操作优先处理自身 Esc |
| 行移动 | 拖动柄、右键上下移与 Alt＋上下方向键共用业务排序约束；Esc 取消 |
| 分组 | 标题居中、不可选；勾选与折叠独立，半选点击转全选；分组本身不增加内容缩进，组内表尾随表体滚动 |
| 拖入反馈 | 有效目标显示边框；单元格自身拒绝的拖入不转为表格新增 |
| 选择高亮 | 普通行无悬停底色；配置行高亮限于序号或拖动柄所在单元格，并覆盖所属展开详情 |

## 外观与尺寸

字段文字、共享颜色来源与主题配置的收束边界见 [Studio 颜色职责与收束清单](colors.md)。

| 项目 | 当前值 / 表现 |
|---|---|
| 表头与文字 | Body 字体，默认 10 pt；保留原大小写，普通表头不额外加粗 |
| 对象图标 | 默认 20 逻辑像素 |
| 文本框 | 内容左对齐，未聚焦时从首字符开始显示 |
| 字段高度 | 默认基准 24 逻辑像素，按字体增高；行高包含字段及上下留白 |
| 单元格留白 | 默认左右各 4、上下至少各 2 逻辑像素；字段垂直居中 |
| 同格控件 | 相邻间距默认 8 逻辑像素；可伸展字段等分宽度，装饰及限宽控件按自身尺寸布局 |
| 行背景与边框 | 深色背景、方角、连续分隔线；滚动槽透明，保留分隔线与外框 |
| 勾选框与对象引用 | 使用各自字段样式；仅指针进入控件自身区域时悬停，禁用态无悬停 |
| 复合画布 | 内容铺满单元格，独立行高；保留领域曲线与波形的绘制 |

## 导航配置与布局

| ID / 位置 | 内容与当前行为 |
|---|---|
| C10 / Platform Configuration | 左侧平台导航；右侧为所选平台的名称、基础平台类型与 Default conversion ObjectRef，指定该平台新建根对象的默认预设，可清空 |
| C11 / Language Configuration | 左侧语言导航与增删，右侧名称；无语言时右侧留空，加号通过弹窗新增 |
| 平台与语言编辑 | 结束编辑即提交，无效输入恢复旧值；支持项目撤销并随项目保存 |
| 平台默认值 | 新项目依次创建 Windows、Android、macOS、iOS。Windows 和 macOS 默认使用 Default Quality，Android 和 iOS 默认使用 Medium Quality。内置的 Multiple Quality 可手动选用 |
| 导航布局 | 无表头，保留当前激活项；纵向槽常驻、无横向滚动，不适用普通表格的空白与 Esc 清选 |
| 主 toolbar | 提供 Original / Converted 试听开关，与平台（含 ALL）、语言一起按本机工程记忆；播放控件居中，Layout 在最右侧 |
| Layout 切换与保存 | 切换保留各布局在当前工程的工作状态；同名保存更新预设，另存后切换当前布局身份 |
| Layout 重置与删除 | 重置当前或经确认重置全部均保留预设；Default 恢复内置状态，删除当前预设后恢复 Default 工作状态 |

## Conversion Editor 与源文件属性

| 项目 | 当前表现 |
|---|---|
| 入口与布局 | 双击 Assets 中的 Conversion 预设或 Inspector 的 Settings 引用，在中央区域下方打开独立 Conversion Editor。Configuration 在上，Result 在下，与 Selection Editor 共用居中标题、可折叠分区和可拖动的分隔线。规则表脚仅保留增删按钮，结果表铺满 Result 分区。Conversion Editor 独立记忆分区高度和展开状态 |
| 配置规则 | `#` 使用标准编号列宽，其余四列默认等宽。Original size range 左对齐，用 `[0 B, 32 KB]`、`(32 KB, 256 KB]`、`(256 KB, Inf)` 表示区间。等于切点的文件使用左段的采样率、格式和质量。不同平台可引用不同预设 |
| 编辑范围 | 双击 Original size range 可同时调整该行的两个切点。下限也是前一行的上限，上限也是后一行的下限。确认或撤销时，两侧边界一起更新。首行下限固定为 `0 B`，末行上限固定为 `Inf`。只有一行时，用加号添加首个切点 |
| 切分大小 | 编号区块使用主题的多类顺序色，文件大小每满 1024 进一档单位。刻度、范围和切点输入框最多显示两位小数，省略末尾的零。切换单位保留原始字节数。区块至少留出表格编号列的点选宽度，上方的密集刻度优先显示当前切点。色块间的空隙透出 Configuration 背景，默认缩放下宽 2 px。悬停或拖动时，整条空隙使用表头分隔线的高亮色。拖动时相邻两行实时预览，松开后可一步撤销。双击分隔线可编辑大小，选 `B` 可精确输入整数字节数 |
| 悬浮与添加切点 | 在可添加切点的位置，文件大小读数叠画在色块上方同一行的固定刻度上，轮廓外的文字始终可见。色块内显示高亮竖虚线，色块下方的圆形 `+` 与读数对齐。`+` 和读数沿用坐标轴指示块的底色与前景色。`+` 使用控件描边 `border.secondary`，悬浮时仅填充色切换为 `background.hover`。悬停或拖动分隔条时只显示分隔条高亮和固定刻度。单击 `+` 或从右键菜单选择 `Add Split...`，均以预览处的大小打开切分弹窗。固定刻度与游标读数只读。表脚加号切分选中段，未选中时切分末段。新段继承原段配置，可在弹窗中调整并确认 |
| 选行与删除 | 单击色块内部选中对应行，选择表格行也会高亮同编号区块。选框画在色块内部，四边等宽。分隔条占用独立空间，按下实际分隔条清除选行，拖动和松开时均保持空选。减号仅对选中的有限规则可用。删除后其区间并入下一条规则，下一条自动选中，区块与行号同步更新 |
| 格式与采样率 | Target format 为 PCM／Vorbis。Vorbis 的 Target quality 通过滑块或数值框选择 0–10 的整数档，默认 4，PCM 的 Target quality 单元格留空。Target sample rate 可选 Original（不重采样）或受支持的离散值，有限阈值可增删，`Inf` 末行不可删除 |
| 引用结果 | 下半区列出主平台上有效引用此预设的 Sound。Active Source 显示该行语言激活条目的名称，悬停显示源文件路径。Sound 引用单击定位、双击打开。没有有效缓存时 Converted 各列和 Ratio 留空 |
| Convert | Convert 是 Configuration 右下角的独立按钮，位于规则表下方，与表格右缘对齐。点击后为当前平台引用该预设的源文件生成产物，已有缓存直接跳过。收起 Result 后仍可执行转换。转换不显示进度或状态，Esc 可取消未完成工作 |
| 文件属性 | Sound → Sources 引用行打开的 External Editor（波形编辑）下半区显示 File Properties 表，可拖动分隔条调整高度。Relative Path 相对工程根目录，Absolute Path 指向同一文件。两种路径均使用操作系统的目录分隔符。表中还显示声道、采样率、时长、大小与音频编码 |
| 属性切换 | 跟随 Toolbar 的 Original／Converted 与当前平台；Converted 无缓存时留空，有效 Conversion 为空时显示原件；File Viewer 不显示 Studio 处理后的信息，始终显示并播放源文件 |
| Vorbis 试听与导出 | Converted 试听和 Pack 播放共用 Engine 的常驻 Vorbis 解码器，支持 1、2、4、6、8 声道 |

## Selection Editor

| 模式 | 表格 / 画布结构 | 当前能力 |
|---|---|---|
| Random | #、Child、Weight、Waveform | 支持重复 Child、引用替换、增删、拖入与排序；Single + Draw 使用频率权重，Shuffle / Queue 使用顺序权重 |
| Sequence | #、Child、Waveform | 与 Random 共用条目编辑，不显示 Weight |
| Match | #、Child、Waveform、Variable 条件列 | 双维度矩阵；行与变量列分别选择及排序，变量列顺序影响匹配优先级 |
| Blend | #、Variable 与 Child 副表、混合曲线画布 | 每行一个可选 Variable，Child 副表含 Child 与 Waveform；支持独立行高与控制区比例调宽 |

注：四种模式共用当前平台的条目，切换保留非活动策略数据；Inspector 只保留模式与行为字段。预览及 Pack 仅消费当前模式与平台。

| 共同行为 | 当前表现 |
|---|---|
| Child 选择范围 | 仅当前 Container 的类型匹配直属孩子；父级仅显示归属，不可选 |
| Waveform | 显示 Child 为 Sound 时当前平台、语言的激活源；其他类型留空，ALL 下源不一致显示 Mixed；点击有效波形打开对应源编辑器 |
| 默认列比例 | 除 # 外：Random 为 20% / 20% / 60%，Sequence 为 40% / 60%，Match 为 25% / 25% / 首个条件列 50%；Blend 副表为 40% / 60% |
| 行试听 | Random、Sequence、Match 选中 Child 行设为试听目标；Inspector 与编辑器保持当前 Container；清选恢复当前层级 Container |
| 引用替换与刷新 | 成功替换已选 Child 后同步试听目标；其他行编辑、被动刷新和焦点变化不接管目标 |
| Control | 未固定时跟随试听目标，固定时保留绑定；Reset 跟随试听目标 |
| 嵌套与失效 | 双击嵌套 Container 进入并保留返回路径；试听目标被删除（含随祖先子树删除）或工程关闭时清除试听目标 |

| Match 项目 | 当前表现 |
|---|---|
| 空表 | 零行，保留 #、Child、Waveform 与一个空 Variable 引用列；空引用不形成数据 |
| 条件输入 | 通配值显示 `*`，下拉项为 `* (Any)`；范围不超过 64 个整数时列出全部值，超出时列标签并允许输入整数；拒绝非整数与越界值 |
| 缺失引用 | Label 保留 (missing) 及原引用，未经编辑不转为通配；更换 Variable 保留原条件，无法解析或越界时拒绝 |
| 增删 | 加号提供 Add Row、Add Variable；Add Row 直接追加无 Child 的空行，Child 列随后选择，右键可清空；减号删除所选行或变量列，列头菜单也可删列 |
| 排序 | #、Child、Waveform 固定在左侧；移动变量同时移动条件列，移动行保留整行内容，均可撤销；同等匹配优先级下行序决定胜者 |
| 预览与诊断 | 使用 Control 覆盖值或变量默认值；胜出条件高亮，重复或无法胜出的行显示警告及阻挡行号，无 Child 的行显示警告且不参与匹配与构建，不改项目数据或用户选择 |

| Blend 项目 | 当前表现 |
|---|---|
| 增删与排序 | 外层管理混合行，Child 副表管理成员；行与成员均可排序，成员曲线随之移动；高亮限于 # 或 Child 单元格 |
| 坐标与类型 | 横轴为 Variable，纵轴为 0–100% 混合比例；Smooth 连续曲线，Step 整数与 Label 阶梯预览，更换变量保留原坐标 |
| 范围变更 | Variable 最小值必须小于最大值，Step 取整后也必须满足；通过基本校验即弹出 Stretch / Fix / Cancel，确认后才扫描消费方并仅计算所选方案。Stretch 同比缩放曲线和写值；Fix 保持 X 并裁剪曲线，完全越界的 Blend 成员保留为零增益，Binding 扩展区间延续端点值，写值钳制到新范围。扫描发现不可达 Match 条件后明确确认是否保留；所有变化共用一步撤销，取消不扫描、不写入，无法保持合法曲线时整体拒绝；既存越界曲线纳入画布范围，保持可编辑 |
| 曲线编辑 | 点坐标、增删点、贝塞尔手柄、复制粘贴及撤销；标题平移整个横向区间，首尾外为 0%，拖动松开提交一步撤销 |
| 选择与试听 | Child 与曲线选择联动；试听游标使用 Control 覆盖值，Child 提示显示各行增益及跨行结果 |
| 合成关系 | 同行同一 Child 仅一条曲线；跨行引用共用播放且增益相乘，成员不自动归一化 |

## Effects 表格与共享资产

| 项目 | 当前表现 |
|---|---|
| 位置与列 | Sound、Container、Layer、Track、Segment、Bus 的 Basic > Effect；# / Effect / Active |
| 容量 | 最多 8 槽，满时禁用加号；删除后恢复，撤销重做同步。GUI 新增回调、共享创作校验、CLI 与编译入口均拒绝超限 |
| 模式与增删 | 加号选择 Limiter 插入 Custom，或 Select Preset... 引用共享预设，允许重复引用；减号删除所选行，支持排序及撤销重做 |
| Custom | 普通标签显示插件名称，双击标签按普通停靠及既有布局打开或复用独立 Effect Editor，单击标签不打开；每次单击右侧 detach 按钮均新建独立浮动 Effect Editor，已有同类型实例及原面板保留；新窗沿当前屏幕已有浮窗向右下错开，遇屏幕边缘回绕，已有窗口及已记忆位置不重排；独立 chevron 位于内容左侧，使用默认箭头指针，Replace 子菜单含 Limiter 和 Select Preset...，另提供 Save As Preset... |
| Preset | 标准 ObjectRef 保留定位、激活和选择器，独立菜单 chevron 位于内容左侧；chevron 使用默认箭头指针，含 Replace > Limiter、Convert to Custom |
| 转换与替换 | 保留行身份、顺序、Active；替换插件使用默认参数，Convert to Custom 复制预设当前配置并解除共享；Save As Preset 创建资产并切换引用，合为一次撤销 |
| Active | 新增默认开启，映射旁通配置；Bus 与非 Bus 均接通试听和 Pack，关闭时保留效果实例并旁通 |
| 在播编辑 | Voice／Bus 编辑前后的链均只含 Limiter（含空链）时，参数、Active、增删、替换和重排即时作用于当前播放，无需停止重播；涉及其他效果类型时，链结构与来源替换延后生效，仅已安装槽位的 Active 和来源兼容的参数可即时更新 |
| 继承范围 | Sound、Container、Layer、Track、Segment 沿归属父级继承整条 Effects 链，不与父链叠加；根直接编辑自身配置，Bus 不提供 Inherit |
| 本地历史 | 首次关闭 Inherit 复制当前有效链并建立独立槽位身份，已有本地历史时恢复历史；重新继承保留历史；Rebuild Local from Current Effective Values 重建本地链 |
| 平台配置 | Shared 与单平台分别保存整链配置，Bus 同样支持；All Platforms 修改共享链并清除该链的平台覆盖，单平台只修改当前平台，与 Shared 相同的链自动消除覆盖；非 Bus 的继承开关也按平台保存，沿用标题菜单 Reset Inherit 恢复共享继承模式 |
| 有效值与只读 | 继承展示当前平台解析出的有效链，禁止增删、排序和修改，保留编辑器查看及 ObjectRef 导航；混合平台有效链不能作为一条链编辑；可编辑的 All Platforms 显示共享本地链 |
| 配置与身份 | 继承关系、本地历史和平台覆盖为保存依据，有效链为解析结果；Custom 参数和 Preset 引用随整链保存，复制到另一对象建立独立槽位身份，同一对象各平台的同一槽位保留身份 |
| 共享资产 | Assets 的 Effect 桶保存类型与全部参数，New Effect > Limiter 为当前唯一类型入口；创建后插件类型固定，Inspector 常规属性区只显示 Notes，无 Status section |
| 独立编辑器跟随 | 所有未 Pin 实例只跟随有效的 Effect 预设或 FX 行单选；单选不新建窗口。选择 Bus 等所属对象（无论是否有 FX）、其他对象或取消选择均保留当前效果器，不自动选择其他槽位；Pin 保持自身绑定，解除 Pin 时若最近一次效果器选择仍有效则立即同步，否则保留自身绑定。当前效果器失效时隐藏参数，槽位删除后撤销可恢复，关闭工程清空绑定和选择记录 |
| 资产编辑区 | 双击 Effect 对象在 Inspector 下半区显示 Effect Editor，标题暂用居中的 Details，上半区保留常规属性；初始上下比例 1:1，分隔边框可拖动；每个 Inspector 首次展开后，选择其他 Effect 直接更新内容并保留比例，切换到非 Effect、清空选择、删除当前资产或关闭工程时清空并隐藏，再选择 Effect 自动恢复；每次单击右上 detach 均为当前资产新建独立浮动 Effect Editor，已有同类型实例及内嵌编辑区保留；新窗沿当前屏幕已有浮窗向右下错开，遇屏幕边缘回绕，已有窗口及已记忆位置不重排；固定 Inspector 保持自身对象 |
| 删除资产 | 清理共享链、各平台覆盖及未生效本地历史中的 Preset 引用行，保留 Custom；撤销一并恢复原顺序和 Active |

## Publisher 表格

| 项目 | 当前表现 |
|---|---|
| 分区与层级 | Init 独立且必选；General 与全部项目语言分别分区，按当前 Action 目录展示文件夹与包；不受 toolbar 语言筛选 |
| 勾选 | 以当前编辑平台及分区语言为范围，文件夹批量作用于全部后代；ALL 的跨平台差异显示半选，勾选存本地缓存、不进入项目撤销栈 |
| 灰显与搜索 | 排除状态跟随观察平台，灰显不禁用勾选或展开；搜索展开命中祖先，清空后恢复折叠状态 |
| 展开与收起 | 分组和目录箭头只切换当前节点；按住 Ctrl 时同时作用于全部后代，不改变勾选 |
| 产物列 | Built、Size、Modified、File、Version 显示当前观察平台的最近成功产物，缺失留空 |
| 结果列 | Result、Cost 固定在最右侧，允许调宽；结果仅保留在当前工程会话，Cost 为单包准备及写包耗时 |
| 行进度 | Init 与 Action 的进度背景止于 Result 左侧；成功发布到 100%，失败或取消停在当前位置；分组无进度 |
| 导出操作 | 无行选择；批量范围由勾选决定，右键作用于点击的 Action 包；Export Platforms 独立指定目标平台 |

注：发布范围、产物格式与失败处理见 [Pack 输出快照](pack-output.md)。

## 其他专用视图

| 视图 | 范围注解 |
|---|---|
| Curve Editor、Timeline | 领域画布，不计入普通表格 |
| Effect Editor | 内嵌与独立参数表单均不计入普通表格；Limiter 使用两行标准表单：Ceiling 范围 −24～0 dB、默认 0 dB，Release 范围 1～1000 ms、默认 100 ms；提供标准滑条、数值输入与复位，Preset 与 Custom 参数支持编辑、撤销重做及保存，Preset 参数跨平台共享，Custom 参数编辑遵循当前平台作用域，继承 Custom 只读；支持预设资产及稳定槽位上下文，独立编辑器支持多实例、固定和布局恢复，槽位切换模式及当前平台有效配置后继续跟随 |
| Hierarchy、Explorer、Resources、Selector、Template Chooser 预览 | 导航或选择树，不计入普通表格 |
| Log | 文本视图，不计为 Monitor 表格 |

表注：Studio 树的鼠标展开／收起入口支持按住 Ctrl 递归作用于当前节点及全部后代，包括箭头、可切换展开状态的分类行点击和目录双击；不按 Ctrl 时只切换当前节点，保留后代的展开状态。普通对象的双击打开、行选择与复选框维持各自原有行为。

Explorer／Hierarchy 的对象树与 Resources 文件树在拖拽期间支持上下边缘自动滚动，触发区随主题行高缩放，越靠边速度越快；禁止投放的行不阻断滚动。Windows 原生拖拽期间将滚轮消息交回 Qt，保留系统滚动设置与细粒度滚轮增量；使用滚轮后暂停边缘自动滚动，指针再次移动时恢复。滚动或滚轮改变视口后重新判断指针下的投放目标；非滚动状态下悬停折叠分支 600ms 展开一层，保留后代原有展开状态。指针离开边缘即停止自动滚动；拖拽离开、结束、取消或树隐藏时清理导航计时与滚轮转发，已展开分支保持展开。
