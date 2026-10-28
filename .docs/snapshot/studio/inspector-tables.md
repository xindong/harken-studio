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
| Sound、Container、Layer、Track、Segment、Bus | Sends | Route > Send | # / Target / Volume | 发送路由配置；除 Bus 外可继承 |
| Bus | Inputs | Route > Input | Source / Volume | 主输出路由的反向查询，只读 |
| Bus | Receives | Route > Receive | Source / Volume | Send 路由的反向查询，只读 |

表注：Volume 以 dB 显示。Container 的 Selections 位于独立 Selection Editor，Inspector 保留 Mode、Play Type 及对应行为字段；独立表格见[表格快照](tables.md)。

## 公共展示与操作

| 项目 | 当前行为 |
|---|---|
| 字段标题 | 表格上方不重复显示独立字段标签；非表格字段显示表单标签 |
| 列宽与滚动 | 列宽随面板弹性分配，保留用户调整比例；表格按内容展开，由 Inspector 页面滚动 |
| 行选择 | 全部表格可选行，无整行悬停高亮 |
| 配置表排序 | # 列拖动、右键上下移动、Alt+上／下方向键；支持撤销与重做 |
| 排序约束 | Sources 仅同语言组排序且保留 Active；Clips 保留时间位置与片段身份；Labels 在原索引位置间移动身份与名称，隐藏项不变 |
| 只读展示表 | Inputs、Receives 无 #、行移动或增删表尾；视觉列换位不改写路由 |
| 编辑限制 | 只读继承值、混合平台值及其他禁止编辑状态不能修改 |

## 专项字段行为

| 表格／字段 | 当前行为 |
|---|---|
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

## 可继承字段

| 范围 | 包含内容 | Inherit 位置 |
|---|---|---|
| 主输出 | 输出 Bus、输出音量 | Route > Main Output |
| 发送 | Sends | Route > Send |
| 空间属性 | 声像、3D 开关、衰减引用、衰减距离缩放 | Spatial > Rating |
