# HarkenStudio 对象右键菜单现状快照

## 范围与统计

| 统计项 | 当前数量与范围 |
|---|---|
| 覆盖界面 | Explorer 的 Assets 对象树与 Hierarchy：对象行、工程 Folder、分类根、树内空白、键盘菜单及对应快捷键 |
| 范围外 | Sources 文件树、主窗口菜单栏、Blueprint、Inspector、Timeline 等其他编辑面 |
| 顶级入口 | 15 个定义，其中 Set Color... 隐藏；分隔线不计数 |
| 顶级子菜单 | 4 个：Create / New Child、New Parent、New Action、Operation |
| 顶级直接命令 | 11 个定义，其中 Set Color... 隐藏 |
| 创建类型 | 10 类：Folder、Sound、Container、Track、Piece、Layer、Bus、Action、Variable、Attenuation |
| Container 创建模式 | 4 个：Random、Sequence、Match、Blend |
| New Action 动作 | 4 个：Play、Stop、Set Variable、Trigger Action |
| Operation 命令 | 2 个：Expand All、Collapse All |
| 树内快捷键 | 5 个：Cut、Copy、Paste、Delete、Duplicate；作用于焦点所在树及其子控件 |
| 分类标题 | Layer、Bus、Action、Variable、Attenuation |

表注：Folder 是工程分类内可嵌套的物理目录，不充当混音对象的业务父级。菜单无 tooltip 或禁用原因说明行；不提供 Replace Source File...。快捷键见[快捷键快照](shortcuts.md)。

## 显示与启用

| 情况 | 当前呈现 |
|---|---|
| 命令与对象类型或位置无关 | 隐藏 |
| 命令相关，数量、只读、保护或结构条件不满足 | 显示并禁用 |
| 混选仅部分对象支持 | 显示相关命令或候选；完整目标集合不合格则禁用 |
| 子菜单无相关候选 | 隐藏入口 |
| 子菜单有候选但均不可执行 | 保留入口并禁用 |
| 子菜单有可执行候选 | 入口启用，各候选分别判断 |

表注：混选候选取并集，执行检查完整选区；同一选区内右键不同对象不改变整组目标。隐藏项移除后清理空分组及多余分隔线。

## 选区与位置

| 触发位置或方式 | 当前行为 |
|---|---|
| 右键已选对象 | 保留完整选区，命中行设为当前行，命令使用整组选区 |
| 右键未选对象 | 切为本地单选，不接管全局编辑或试听 |
| 右键分类根 | 清除对象选区，以分类位置为目标 |
| 右键空白 | 清除对象选区和位置目标；Operation 禁用 |
| 键盘菜单 | 以当前行和当前选区建立目标 |
| 对象快捷键 | 使用触发面板的选区与当前行 |
| 关闭菜单／取消输入 | 不额外改变本次右键已确定的本地选区 |
| 执行前目标失效 | 任一目标缺失即拒绝整组 |

| 上下文 | 顶级显示范围 |
|---|---|
| Layers / Actions / Variables / Attenuations 分类根 | Create、Paste、Operation |
| Busses 分类根 | Create、Operation |
| Folder 单选 | New Child、Rename、Delete、Operation |
| 空白处 | Operation，禁用 |

| 面板交互 | 当前影响 |
|---|---|
| Explorer／Hierarchy 主动对象选择 | 驱动编辑与试听；各树分别保留选区、展开与滚动位置 |
| Explorer 选择／双击 | 更新 Blueprint 高亮；双击聚焦画布并打开编辑器 |
| Hierarchy 选择／双击 | 不改变 Blueprint 选区或视口；双击仍打开编辑器 |
| Blueprint／其他编辑视图选择 | Explorer 可跟随，Hierarchy 不变；Blueprint 自身选择不被反向覆盖 |
| 显式 Locate／Open in Blueprint | 前者仅定位 Explorer，后者主动定位画布 |
| 清选／选择 Folder 或分类根 | 清空编辑与试听对象，不从其他树回退 |
| 焦点、搜索、滚动、展开折叠、右键 | 不接管编辑或试听对象 |
| 新建／Duplicate／Paste | 仅发起树选择结果，其他树更新内容并保留有效本地选区 |
| 删除／撤销重做／树重建 | 保留有效浏览状态，清除失效选择及失效编辑、试听对象 |

## 顶级命令

| 顺序 | 入口 | 显示范围 | 启用条件与结果 |
|---|---|---|---|
| 1 | Create / New Child | 有可创建类型的对象、Folder 或分类根 | 单目标且通过创建、只读、引用和深度校验；多选禁用 |
| 2 | New Parent | 可由公开非 Folder 父类型容纳的对象 | 全部为同父级兄弟且可整体移入；根对象、跨父级或结构冲突禁用 |
| 3 | New Action | 有相关动作的目标类型 | 所有目标支持该动作；每个对象创建一个独立 Action |
| 4 | Duplicate | 可复制对象；纯 Bus / Folder 隐藏 | 整组可复制，副本保留各自父级或分类位置，使用新身份与避重名称 |
| 5 | Rename | 对象及 Folder | 仅单目标，行内编辑 |
| 6 | Delete | 对象及 Folder | 整批通过保护和结构校验后直接删除并清理引用；Master Bus 阻止整批删除 |
| 7 | Cut | 可复制对象；纯 Bus / Folder 隐藏 | 整组合格后写内部剪贴板，此时不移动 |
| 8 | Copy | 同 Cut | 整组合格后写内部剪贴板 |
| 9 | Paste | Layers 区对象及非 Bus 分类根 | 单目标，全部来源可放入；空剪贴板、不兼容或继承冲突禁用 |
| 10 | Paste Properties | Sound、Container、Track、Piece、Layer | 剪贴板仅一个有效同类型属性源，全部明确目标兼容 |
| 11 | Open Containing Folder | 含 Sound 的选区 | 单个 Sound，当前平台与语言的所选音源存在 |
| 12 | Open in New Tab | 含非 Folder 对象的选区 | 全部可打开，逐个打开独立 Inspector 标签页；混入 Folder 禁用 |
| 13 | Open in New Window | 含非 Folder 对象的选区 | 单个可打开对象，打开浮动 Inspector |
| 14 | Set Color... | 隐藏 | 不可用 |
| 15 | Operation | 所有上下文 | 按目标子树展开状态启用；无可执行子项时禁用 |

表注：Cut、Copy、Duplicate 不支持 Bus、Folder、空间对象及未公开类型。

## 子菜单候选

### Create / New Child

| 位置或对象类型 | 候选类型，按显示顺序 |
|---|---|
| Layers 分类根及其 Folder | Folder、Sound、Container、Track、Piece、Layer |
| Layer | Sound、Container、Track、Piece、Layer |
| Container | Sound、Container、Track、Piece |
| Track | Sound、Container、Piece |
| Piece | Track |
| Busses 分类根、Bus | Bus；分类根创建时以 Master Bus 为父级 |
| Actions 分类根及其 Folder | Folder、Action |
| Variables 分类根及其 Folder | Folder、Variable |
| Attenuations 分类根及其 Folder | Folder、Attenuation |
| Sound、Action、Variable、Attenuation、空白 | 无候选 |

表注：分类根标题为 Create，其他位置为 New Child。Container 提供 Random、Sequence、Match、Blend；Random／Sequence 默认 Play Type = Single，可改为 Queue，Queue 不是独立创建模式。多选显示候选并集，入口禁用。

### New Parent

| 选中对象类型 | 相关父类型，按显示顺序 |
|---|---|
| Sound、Container、Piece | Container、Track、Layer |
| Track | Container、Piece、Layer |
| Layer | Layer |
| Bus | Bus |
| Folder、Action、Variable、Attenuation | 无候选 |

表注：多选候选取并集，仍须全部为同父级兄弟并满足结构约束，不通过剔除后代改变分组。

### New Action

| 子项 | 目标类型 | 自动名称前缀 |
|---|---|---|
| Play | Sound、Container、Track、Piece | Play_ |
| Stop | Sound、Container、Track、Piece | Stop_ |
| Set Variable | Variable | SetVariable_ |
| Trigger Action | Action | TriggerAction_ |

表注：仅显示相关动作，无 Reset。每个明确目标生成一个仅含对应步骤的 Action；Set Variable 默认 Scope = Emitter，可改为 Global，作用域随步骤保存。

### Operation

| 子项 | 启用条件 | 作用范围 |
|---|---|---|
| Expand All | 存在未展开的非叶分支 | 各目标及全部后代；分类根覆盖整个分类 |
| Collapse All | 存在已展开的非叶分支 | 同上 |

表注：纯叶节点、空容器和空白处保留禁用项；混入叶节点不阻止其他分支操作。

## 目标、结果与撤销

| 命令 | 目标处理 | 完成后的选区与撤销 |
|---|---|---|
| Cut / Copy | 去重，剔除被已选祖先覆盖的后代 | 保持选区；不产生工程撤销 |
| Duplicate | 同上 | 选中全部副本根；整批一次撤销／重做 |
| Delete | 同上；Folder 同时去除重复覆盖的文件根 | 清除失效选择；删除及引用清理一次撤销／重做 |
| Paste | 使用归一化来源，目标唯一 | 选中全部结果；整批一次撤销／重做 |
| Paste Properties | 仅去重，保留明确选中的父子 | 保持选区；有变化时整批一次撤销／重做 |
| New Action / Open in New Tab | 仅去重，保留明确选中的父子 | 前者选中全部新 Action 并整批一次撤销；后者保持选区且不写工程撤销 |
| New Parent | 全部为同父级兄弟 | 选中新父；创建及重挂一次撤销／重做 |
| Expand All / Collapse All | 去除被已选祖先覆盖的子树 | 保持选区，不写工程撤销 |

表注：目标按树顺序处理。Rename、New Child、Paste、Open in New Window、Open Containing Folder 保持单目标限制。撤销不恢复历史选区。

## 命名、目录与删除状态

| 项目 | 当前结果 |
|---|---|
| 创建后命名 | Create / New Child、Folder、New Parent、单目标 New Action 预填避重名称并进入行内编辑；Enter／失焦提交，Esc 保留默认名，非法输入原地修正 |
| 不自动进入命名 | 多目标 New Action、复制、粘贴、导入、撤销与重做 |
| Action／Variable 名称 | 各自全工程唯一；英文字母开头，仅含英文字母、数字和下划线；已腾空名称可复用，新对象使用新身份 |
| Folder 修改 | 创建、改名、移动、删除支持撤销；保存时写入磁盘，跨目录父级和目录变更合为一次撤销 |
| 未保存标记 | 以最近成功保存为基线；目录创建、改名、移动及已保存内容删除显示斜体和星号，撤销回到基线时清除 |
| 删除交互 | 不弹确认或完成提示；被引用对象可删除，引用清理合入同一次撤销 |
| 删除 Variable | 移除关联 Binding、绑定曲线及 Match 条件列；混合行保留并清空变量引用 |
| 删除 Action 目标／Attenuation | Action 步骤保留并清空目标或时序引用；Sound 的衰减引用清空，含未生效的继承历史引用 |
