# HarkenStudio Inspector 表格现状快照

## 表格清单

| 统计项 | 当前范围 |
|---|---|
| Inspector 主面板 | Sound、Container、Layer、Track、Segment、Bus、Action、Attenuation、Variable、Effect，共 10 类对象 |
| 表格种类 | 9 类：7 类配置表、2 类只读展示表 |
| 对象展示位置 | 19 处：Sound 3、Container 2、Layer 2、Track 3、Segment 2、Bus 4、Action 1、Attenuation 1、Variable 1 |
| 范围外 | Selection Editor、External Editor、Effect Editor、Timeline、Grid 等独立编辑面 |

| 对象 | 表格 | Tab > Section | 列名 | 数据与编辑性质 |
|---|---|---|---|---|
| Sound | Sources | Basic > Source | # / Audio / Waveform / Active | 音频引用配置 |
| Track | Clips | Basic > Clip | # / Child | 音频片段配置 |
| Action | Steps | Basic > Step | # / Type / Target / Delay / 无标题展开列 | 动作步骤配置，末列展开详情 |
| Attenuation | Dimensions | Basic > Dimension | # / Property / Curve | 衰减维度配置 |
| Variable | Labels | Basic > Label | # / Name | 标签配置，# 显示实际整数索引 |
| Sound、Container、Layer、Track、Segment、Bus | Effects | Basic > Effect | # / Effect / Active | 整链配置与平台覆盖；除 Bus 外沿对象归属层级继承 |
| Sound、Container、Layer、Track、Segment、Bus | Sends | Route > Send | # / Target / Volume / Mode | Mix、Ducking、Sidechain 路由配置；除 Bus 外可继承 |
| Bus | Inputs | Route > Input | Source / Volume | 输出路由的反向查询，只读 |
| Bus | Receives | Route > Receive | # / Mode / Source / Volume | Send 路由的反向查询，只读 |

表注：Volume 以 dB 显示。Container 的 Selections 位于独立 Selection Editor，Inspector 保留 Mode、Play Type 及对应行为字段；独立表格见[表格快照](tables.md)。

## 公共展示与操作

| 项目 | 当前行为 |
|---|---|
| 字段标题 | 表格上方不重复显示独立字段标签；非表格字段显示表单标签 |
| 列宽与滚动 | 列宽随面板弹性分配，保留用户调整比例；表格按内容展开，由 Inspector 页面滚动 |
| 行选择 | 全部表格可选行，无整行悬停高亮 |
| 配置表排序 | # 列拖动、右键上下移动、Alt+上／下方向键；支持撤销与重做 |
| 排序约束 | Sources 仅同语言组排序且保留 Active；Clips 保留时间位置与片段身份；Labels 在原索引位置间移动身份与名称，隐藏项不变 |
| 只读展示表 | Inputs 无 #；Receives 的 # 仅显示行号；两表均无行移动或增删表尾，视觉列换位不改写路由 |
| 编辑限制 | 只读继承值、混合平台值及其他禁止编辑状态不能修改 |

## 专项字段行为

| 表格／字段 | 当前行为 |
|---|---|
| Effects / 模式与增删 | 加号选择 Limiter 插入 Custom，或 Select Preset... 引用已有 Effect；允许跨对象及同链重复引用；减号删除选中行，支持排序与撤销重做 |
| Effects / Custom | 普通标签显示插件名称，双击标签或单击右侧 detach 按钮打开 Effect Editor，单击标签不打开；内容左侧的独立 chevron 使用默认箭头指针，Replace 子菜单提供 Limiter 和 Select Preset...，另提供 Save As Preset... |
| Effects / Preset | ObjectRef 保留定位、双击激活和预设选择器；内容左侧的独立 chevron 使用默认箭头指针，提供 Replace > Limiter 和 Convert to Custom |
| Effects / 切换 | 保留行位置、行 GUID、Active；替换插件使用默认参数，Convert to Custom 复制预设当前插件与参数后解除共享，不改预设资产 |
| Effects / 保存预设 | Save As Preset... 使用标准目录／名称选择器，将当前 Custom 配置存为共享 Effect 并切换为引用；创建与切换合为一次撤销 |
| Effects / Active | 新增默认开启，映射现有旁通配置；Bus 沿用既有支持，非 Bus 含关闭项时仍受现有试听／发布能力检查限制 |
| Effects / 继承 | Sound、Container、Layer、Track、Segment 沿归属父级继承整条链，包括 Custom／Preset、顺序、Active，不与父链叠加；根对象直接编辑自身配置，Bus 不继承 |
| Effects / 本地历史 | 首次关闭 Inherit 复制当前有效链并分配本对象槽位身份；已有本地历史时恢复历史；开启继承保留历史；Rebuild Local from Current Effective Values 使用当前有效链重建本地配置 |
| Effects / 平台 | 沿用 Shared 与平台覆盖规则；All Platforms 编辑共享链并清除该链的平台覆盖，单平台只改当前平台，配置与 Shared 相同时自动消除该覆盖；继承开关按平台独立保存，沿用标题菜单 Reset Inherit 恢复共享继承模式；Bus 也支持平台链，但没有 Inherit |
| Effects / 有效值 | 继承状态展示当前平台的有效链并禁止增删、排序和修改；继承 Custom 仍可打开编辑器查看，Preset 保留 ObjectRef 导航；平台有效链不同且不能编辑共享本地链时显示混合状态 |
| Effects / 资产 | Assets 的 Effect 桶通过 New Effect > Limiter 新建；创建后插件类型固定，Inspector 仅显示 Notes，无 Status section |
| Effects / Effect Editor | 双击 Effect 对象同步打开独立编辑器；Custom 双击槽位插件名称标签或单击 detach 按钮打开；Limiter 使用两行标准表单：Ceiling 范围 −24～0 dB、默认 0 dB，Release 范围 1～1000 ms、默认 100 ms，提供标准滑条、数值输入与复位；Preset 与 Custom 参数支持编辑、撤销重做及保存，Preset 参数跨平台共享，Custom 参数编辑遵循当前平台作用域，继承 Custom 只读；支持多实例、固定和布局恢复，槽位编辑器随该槽位的替换、模式切换及当前平台有效配置更新 |
| Effects / 删除资产 | 删除 Effect 移除共享配置、各平台覆盖及未生效本地历史中的对应 Preset 引用行，保留 Custom；撤销一并恢复引用行及其原顺序、Active |
| Sends / 新增 | 加号先选择 Mix、Ducking 或 Sidechain，再选择目标 Bus；取消任一步不新增行 |
| Sends / Mode | 可编辑并参与保存、继承和撤销；旧数据缺失 Mode 时按 Mix 解释；同一目标按 Mode 区分，单个 Mode 内目标唯一；与 Output 冲突或重复的 Mode 选项禁用 |
| Sends / Volume | 三种 Mode 均表示发送的源信号增益，范围 −96～+24 dB；Ducking 中用于调节送入避让检测端的信号强度，并非指定目标衰减量；同一行各 Mode 分别记忆 Volume，Target 共用；首次进入未配置 Mode 沿用当前值，切回恢复上次值，记忆随工程保存并参与撤销 |
| Sends / Output | Mix、Ducking 与 Output 目标互斥；Sidechain 可指向 Output 的目标；沿用自连与成环限制 |
| Sends / 显示与发布 | Receives 和信号图显示 Mode；图中按 Mode 区分增益编辑、删除与撤销；试听发布 Mix 和 Ducking，Ducking 继承配置按最终 Sound 生效，Bus 可直接发送；打包仍仅发布 Mix；Sidechain 保留编辑配置，尚未接入插件辅助输入 |
| Sends / 平台范围 | Bus 的 Receives 和信号图读取当前平台生效路由；图上删除仅修改对应范围；移动、删除目标及撤销覆盖共享值和平台覆盖，循环校验覆盖所有受影响的平台 |
| Sources / Language | 关闭时显示普通组，开启时显示全部项目语言组（含空组）；Toolbar 当前语言置顶，其余按项目语言顺序；两类数据分别保留 |
| Sources / 拖入 | 组内导入边框限定目标语言组；Audio 引用框独立处理引用替换与高光，拒绝的拖入不转为组内导入 |
| Sources / Active | 同一平台与语言只选一个来源；多平台不一致时显示半勾选 |
| Sources / Waveform | 展示音频裁剪与循环范围，可打开范围编辑；文件缺失时留空且不可编辑 |
| Sources / 缺失引用 | 保留引用及 Active；Audio 标红，仅当前平台与语言的 Active 来源缺失使 Sound 标红 |
| Playback / Filter | Sound、Container、Layer、Track、Segment、Bus 的 MixNode 属性；标签加共享预览窗，点击进入 External Editor；全默认时淡化，有非默认值时激活，均可编辑 |
| Steps / Set Variable | 详情含 Value 与 Scope；Scope 为 Global／Emitter，默认 Emitter，随步骤保存，更换 Target 不重置 |
| Steps / PostAction | 拒绝直接或间接循环；候选显示循环路径，拒绝不产生修改或撤销记录；重复引用和无环菱形依赖允许 |
| Labels / 索引编辑 | 双击 # 编辑；仅接受当前范围内未占用的整数，空名称行也占用索引 |
| Labels / 提交与取消 | Enter 或失焦提交，Esc 取消；非法输入标红并拒绝提交 |
| Labels / 编辑结果 | 保留标签身份与名称，按索引升序排列，选择跟随标签；支持一次撤销与重做，不自动填空或重编号 |
| Labels / 新增 | 加号补当前范围内最低空闲索引 |
| Labels / 编辑中切换 | 对象、范围、权限或外部数据变化取消草稿；相同数据刷新保留草稿 |
| Variable / Status | Basic > Status 包含 Kind、Range、Default Value 等变量状态字段 |
| Variable 曲线 / Step | 标签、整数刻度与阶梯预览 |
| Variable 曲线 / Smooth | 浮点刻度与连续曲线；切换类型刷新显示但不改写曲线 |

表注：语言分组、导入、缺失恢复与文件操作见[音频源业务结果表](audio-source-lifecycle.md)。

Filter 编辑器上方使用共享曲线画布和刻度，下方沿用 Inspector 的 Low Pass、High Pass、Low Shelf、High Shelf 折叠分节、参数 Slider 和右侧变量绑定插槽。图表与字段共用面板外框，中间 1px 分隔边界可拖动调整高度，悬停只将边界颜色切为主题 accent，拖动命中范围另行扩展。四节依次使用 categorical 0–3 的标题背景、曲线和点。没有 Enable 字段；只有非默认分节显示控制点。大小预览均按工程输出采样率计算频响，横轴固定为 Frequency 的 20–20000 Hz。

Frequency 范围为 20–20000 Hz，四节默认值依次为 20000、20、200、6000 Hz；Q Value 范围为 0.1–10，默认 0.70710678；两个 Shelf 的 Gain 范围为 −48–48 dB，默认 0。预览显示编写值，不跟随 Variable Rehearsal 或叠加绑定贡献。保存及平台覆盖跟随对象定义的 Volume/Pitch 编辑范围，图上多参数拖动合为一次撤销。当前仅实现 Studio 编写、保存和可视预览；Engine 的 CurrentWorld、实时播放和 Pack Filter 合同尚未接入。

## 新建变量绑定

| 项目 | 当前行为 |
|---|---|
| 入口 | Inspector 绑定按钮与 Binding 表格的 New Variable |
| 位置与名称 | 选择 Variables 根或已有目录；名称预填避重并全选，要求全工程唯一 |
| 目录操作 | 单击只改变创建位置，双击不提交；目录改名或移动保留目标，目录删除后须重新选择 |
| 创建 | Create 确认后同时创建变量与绑定；名称非法或冲突时原地报错 |
| 取消与失败 | 取消、关闭或改绑不创建对象；绑定失败撤回新变量 |
| 撤销 | 创建及绑定合为一次撤销；失败不增加撤销记录 |
| Filter 贡献 | 使用原字段单位和上下限作为纵轴；新建绑定为 0 中性平线，Frequency/Q 的 0 保留在可视轴外；前端不合成或限制原值与贡献之和 |
| Pitch 贡献 | 绑定纵轴统一为 −4800–4800 cent，新建绑定初始为 0 中性平线；保留已有曲线点值，超范围部分位于可视轴外，新编辑限制在 ±4800 |

## 可继承字段

| 范围 | 包含内容 | Inherit 位置 |
|---|---|---|
| 输出 | Target（目标 Bus）、Volume（输出音量） | Route > Output |
| 发送 | Sends | Route > Send |
| 空间属性 | 声像、3D 开关、衰减引用、衰减距离缩放 | Spatial > Rating |
| 效果器 | 整条 Effects 链，含 Custom／Preset、顺序、Active | Basic > Effect |
