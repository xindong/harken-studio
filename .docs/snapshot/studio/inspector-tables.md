# HarkenStudio Inspector 表格现状快照

## 表格清单

| 统计项 | 当前范围 |
|---|---|
| Inspector 主面板 | Sound、Container、Layer、Track、Segment、Bus、Action、Attenuation、Variable，共 9 类对象 |
| 表格种类 | 8 类：6 类配置表、2 类只读展示表 |
| 对象展示位置 | 13 处：Sound 2、Container 1、Layer 1、Track 2、Segment 1、Bus 3、Action 1、Attenuation 1、Variable 1 |
| 范围外 | Selection Editor、External Editor、Timeline、Grid 等独立编辑面 |

| 对象 | 表格 | Tab > Section | 列名 | 数据与编辑性质 |
|---|---|---|---|---|
| Sound | Sources | Basic > Source | # / Audio / Waveform / Active | 音频引用配置 |
| Track | Clips | Basic > Clip | # / Child | 音频片段配置 |
| Action | Steps | Basic > Step | # / Type / Target / Delay / 无标题展开列 | 动作步骤配置，末列展开详情 |
| Attenuation | Dimensions | Basic > Dimension | # / Property / Curve | 衰减维度配置 |
| Variable | Labels | Basic > Label | # / Name | 标签配置，# 显示实际整数索引 |
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
| Sends / 新增 | 加号先选择 Mix、Ducking 或 Sidechain，再选择目标 Bus；取消任一步不新增行 |
| Sends / Mode | 可编辑并参与保存、继承和撤销；旧数据缺失 Mode 时按 Mix 解释；同一目标按 Mode 区分，单个 Mode 内目标唯一；与 Output 冲突或重复的 Mode 选项禁用 |
| Sends / Volume | 三种 Mode 均表示发送的源信号增益，范围 −96～+24 dB；Ducking 中用于调节送入避让检测端的信号强度，并非指定目标衰减量；同一行各 Mode 分别记忆 Volume，Target 共用；首次进入未配置 Mode 沿用当前值，切回恢复上次值，记忆随工程保存并参与撤销 |
| Sends / Output | Mix、Ducking 与 Output 目标互斥；Sidechain 可指向 Output 的目标；沿用自连与成环限制 |
| Sends / 显示与发布 | Receives 和信号图显示 Mode；图中按 Mode 区分增益编辑、删除与撤销；本阶段试听与打包仅发布 Mix，Ducking、Sidechain 保留编辑配置 |
| Sends / 平台范围 | Bus 的 Receives 和信号图读取当前平台生效路由；图上删除仅修改对应范围；移动、删除目标及撤销覆盖共享值和平台覆盖，循环校验覆盖所有受影响的平台 |
| Sources / Language | 关闭时显示普通组，开启时仅显示 Toolbar 当前语言组；两类数据分别保留 |
| Sources / Active | 同一平台与语言只选一个来源；多平台不一致时显示半勾选 |
| Sources / Waveform | 展示音频裁剪与循环范围，可打开范围编辑；文件缺失时留空且不可编辑 |
| Sources / 缺失引用 | 保留引用及 Active；Audio 标红，仅当前平台与语言的 Active 来源缺失使 Sound 标红 |
| Steps / Set Variable | 详情含 Value 与 Scope；Scope 为 Global／Emitter，默认 Emitter，随步骤保存，更换 Target 不重置 |
| Steps / PostAction | 拒绝直接或间接循环；候选显示循环路径，拒绝不产生修改或撤销记录；重复引用和无环菱形依赖允许 |
| Labels / 索引编辑 | 双击 # 编辑；仅接受当前范围内未占用的整数，空名称行也占用索引 |
| Labels / 提交与取消 | Enter 或失焦提交，Esc 取消；非法输入标红并拒绝提交 |
| Labels / 编辑结果 | 保留标签身份与名称，按索引升序排列，选择跟随标签；支持一次撤销与重做，不自动填空或重编号 |
| Labels / 新增 | 加号补当前范围内最低空闲索引 |
| Labels / 编辑中切换 | 对象、范围、权限或外部数据变化取消草稿；相同数据刷新保留草稿 |
| Variable 曲线 / Step | 标签、整数刻度与阶梯预览 |
| Variable 曲线 / Smooth | 浮点刻度与连续曲线；切换类型刷新显示但不改写曲线 |

表注：语言分组、导入、缺失恢复与文件操作见[音频源业务结果表](audio-source-lifecycle.md)。

## 新建变量绑定

| 项目 | 当前行为 |
|---|---|
| 入口 | Inspector 绑定按钮与 Binding 表格的 New Variable |
| 位置与名称 | 选择 Variables 根或已有目录；名称预填避重并全选，要求全工程唯一 |
| 目录操作 | 单击只改变创建位置，双击不提交；目录改名或移动保留目标，目录删除后须重新选择 |
| 创建 | Create 确认后同时创建变量与绑定；名称非法或冲突时原地报错 |
| 取消与失败 | 取消、关闭或改绑不创建对象；绑定失败撤回新变量 |
| 撤销 | 创建及绑定合为一次撤销；失败不增加撤销记录 |
| Pitch 贡献 | 绑定纵轴统一为 −4800–4800 cent，新建绑定初始为 0 中性平线；保留已有曲线点值，超范围部分位于可视轴外，新编辑限制在 ±4800 |

## 可继承字段

| 范围 | 包含内容 | Inherit 位置 |
|---|---|---|
| 输出 | Target（目标 Bus）、Volume（输出音量） | Route > Output |
| 发送 | Sends | Route > Send |
| 空间属性 | 声像、3D 开关、衰减引用、衰减距离缩放 | Spatial > Rating |
