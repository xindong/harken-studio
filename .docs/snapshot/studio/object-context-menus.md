# HarkenStudio 对象右键菜单现状快照

最近核对：2026-09-27。快捷键注册与设置见 [shortcuts.md](shortcuts.md)。本篇记录当前工作区实现；代码变化后同步更新，不作为待实施方案。

## 范围与统计

覆盖 Explorer 的 Assets 工程对象树与 Hierarchy，包括对象行、工程 Folder、分类根、树内空白、键盘菜单和对应快捷键。两处继承同一 `ProjectTreePanel`，共用菜单定义与命令目标规则。

Sources 文件树、主窗口菜单栏、Blueprint、Inspector、Timeline 等其他编辑面的菜单不计入。这里的 Folder 表示工程桶内的物理目录，不是 ProjectItem，没有 GUID、color 或持久化元数据。目录可以嵌套，不能作为混音对象的子级。

Folder 相关行为核对：2026-10-08。创建、重命名、移动和删除先修改内存目录树，支持撤销与重做；保存时统一写入磁盘。尚未保存的目录创建、名称或位置变化以斜体和星号标记，按最近一次成功保存的状态计算，撤销与重做同步更新。跨目录的对象父级变更与目录变更属于同一次撤销。删除目录包含其中内容及必要的引用清理；保存后撤销仍保留对象原 GUID，重新保存恢复文件。目录展开态由界面保存，默认展开。

删除 Folder 与 Item 直接进入撤销栈，不弹出确认或删除完成提示。各 bucket 内已保存内容被删除或对象目录地址改变、尚未再次保存时，bucket 显示斜体和星号；标记按最近一次成功保存的状态计算，撤销与重做同步更新。新建后又删除且从未保存的内容不产生 bucket 删除标记；普通属性变更和仅改变混音父级不产生 bucket 目录变更标记。

删除对象统一清理引用方并标记引用方未保存，清理和删除属于同一次撤销。Variable 的关联 Binding 及曲线、Match 轴及条件列被删除；混合行和曲线保留，只清空 Variable 引用；Action 步骤保留，只清空目标或时序引用。被引用的 Attenuation 可以删除，对应 Sound 的衰减引用同步清空，包括暂未生效的继承历史引用。对象星号以最近一次成功保存的内容为基线，撤销或重做回到该内容时清除；其他未保存编辑仍保留星号。

| 统计项 | 当前数量与范围 |
|---|---|
| 顶级菜单入口定义 | 15 个，其中 Set Color... 暂时隐藏；实际显示数量随上下文变化，分隔线不计数 |
| 顶级子菜单 | 4 个：Create / New Child、New Parent、New Action、Operation；Create 与 New Child 是同一入口的不同标题 |
| 顶级直接命令 | 11 个定义：Duplicate、Rename、Delete、Cut、Copy、Paste、Paste Properties、Open Containing Folder、Open in New Tab、Open in New Window、Set Color...（暂时隐藏） |
| 创建类型目录 | 10 类：Folder、Sound、Container、Track、Piece、Layer、Bus、Action、Variable、Attenuation；按位置筛选 |
| Container 创建模式 | 4 个：Random、Sequence、Match、Blend |
| New Action 动作目录 | 4 个：Play、Stop、Set Variable、Trigger Action |
| Operation 命令目录 | 2 个：Expand All、Collapse All |
| 树级快捷键绑定 | 5 个：Cut、Copy、Paste、Delete、Duplicate，均为 `WidgetWithChildrenShortcut` |
| 工程分类 | 5 个：Layers、Busses、Actions、Variables、Attenuations；界面分类标题分别为 Layer、Bus、Action、Variable、Attenuation |

`Replace Source File...` 已从这两处对象菜单移除。菜单不提供 tooltip 或禁用原因说明行。

Set Color 尚未确定落地和发布，工程树与 Timeline 轨道菜单均隐藏该入口；脚本/IPC 的 `set_item_property` 也拒绝写入 `color`，返回 `COLOR_EDITING_UNAVAILABLE`，不修改对象或产生撤销记录。

## 展示与启用策略

**功能范围决定显示，当前条件决定启用。** 显示判断不等于执行资格，不能用“至少一个对象支持”启用整组操作。

| 情况 | 当前呈现 | 例子 |
|---|---|---|
| 命令与当前对象类型或位置无关 | 隐藏 | Sound 的 New Child；纯 Bus / Folder 选区的 Copy；空白处的 Rename |
| 命令相关，但数量或当前状态不满足 | 显示并禁用 | 多选时的 Rename；剪贴板为空或不兼容时的 Paste；缺失源文件的 Open Containing Folder |
| 命令相关，但受保护、只读或结构约束限制 | 显示并禁用 | Master Bus 的 Delete；到达深度上限的 New Child；跨父级选区的 New Parent |
| 混选中只有部分对象属于命令范围 | 保留相关命令或候选，整组不支持时禁用 | Sound + Bus 的 Copy；Sound + Variable 的 New Action |
| 子菜单没有相关候选 | 隐藏整个入口 | Layer 的 New Action；Sound 的 New Child |
| 子菜单有相关候选，但全部不可执行 | 保留入口并禁用 | 不同父级下两个 Sound 的 New Parent |
| 子菜单有可执行候选 | 入口启用；各候选独立判断可用性 | New Parent 中允许当前包装的父类型启用，相关但不合法的父类型置灰 |

混选的展示范围取相关能力的并集，执行资格检查完整目标集合；在同一选区内右键命中不同对象，不改变集合命令的显示、启用或目标。纯粹无关的选项不会因为目录中存在就显示。

剩余菜单项保持定义顺序；隐藏后清理首尾分隔线、连续分隔线和空分组。合法操作的命名仍可打开对话框，删除直接执行并可撤销，能力不支持通过禁用表达。

## 选区与位置上下文

| 触发位置或方式 | 当前行为 |
|---|---|
| 右键已选对象 | 保留完整选区，将命中行设为当前行；命令使用整个选区 |
| 右键未选对象 | 切换为该对象单选，再建立菜单上下文 |
| 右键分类根 | 清除对象选区，记录分类位置；不使用此前对象作为目标 |
| 右键空白处 | 清除对象选区和位置上下文，Operation 保留禁用 |
| 键盘调用上下文菜单 | 定位当前行，使用当前选区；不使用鼠标事件位置替代目标 |
| 对象快捷键 | 从触发时的选区和当前行建立上下文，执行对应能力校验 |
| 关闭菜单或取消输入 | 释放菜单上下文；不额外更改已经由本次右键确定的选区 |

菜单上下文保存按树顺序排列的对象 GUID、命中对象 GUID、分类和 Folder 地址。执行时重新解析；任一目标缺失则拒绝整组，不只处理仍存在的对象。对话框返回后再次解析和校验，命名流程同时核对工程及目录上下文。

分类根、工程 Folder 和空白处使用不同菜单范围：

| 上下文 | 顶级显示范围 |
|---|---|
| Layers / Actions / Variables / Attenuations 分类根 | Create、Paste、Operation |
| Busses 分类根 | Create、Operation |
| 工程 Folder 单选 | New Child、Rename、Delete、Operation |
| 空白处 | Operation，因没有目标节点而禁用 |

## 顶级命令清单

下表按定义顺序列出全部 15 个入口。显示范围涉及混选时采用前述并集规则；启用条件仍针对完整目标。

| 顺序 | 入口 | 显示范围 | 启用条件与执行语义 |
|---|---|---|---|
| 1 | Create / New Child | 有相关可创建类型的对象、Folder 或分类根 | 唯一对象或位置；通过创建、只读、引用和深度校验。多选保留相关候选但禁用 |
| 2 | New Parent | 选中类型可被某种公开的非 Folder 父类型容纳 | 所有目标为同一实际父对象下的兄弟对象；新父可在当前位置创建，且整组可移入。根对象、跨父级及结构限制使相关候选禁用 |
| 3 | New Action | 选区存在四种公开动作的目标类型 | 所有明确目标均支持所选动作并通过创作校验；每对象创建一个独立 Action |
| 4 | Duplicate | 存在既有可复制对象；纯 Bus / Folder 隐藏 | 整组子树可复制；各副本保留各自父级或分类位置，分配新的 GUID 并处理名称冲突 |
| 5 | Rename | 有对象目标，含 Folder 和 Bus | 仅一个对象；命名对话框确认后重命名，多选禁用 |
| 6 | Delete | 有对象目标，含 Folder 和 Bus | 整批实际删除范围通过保护、引用及结构校验后直接删除，可撤销。Master Bus 阻止整批操作；被引用对象随删除同步清理引用方 |
| 7 | Cut | 存在既有剪贴板来源类型；纯 Bus / Folder 隐藏 | 全部归一化来源合格后记录工程内部剪贴板；此时不移动对象 |
| 8 | Copy | 同 Cut | 全部归一化来源合格后记录工程内部剪贴板 |
| 9 | Paste | Layers 区对象及非 Bus 分类根 | 唯一目标，剪贴板全部来源可放入；空剪贴板、不兼容、多目标或继承冲突时禁用 |
| 10 | Paste Properties | 存在可作为剪贴板来源且具有 `VMixNode` 的目标；当前为 Sound、Container、Track、Piece、Layer | 剪贴板恰有一个有效同类型属性源，所有目标均兼容；分别应用到明确选中的对象 |
| 11 | Open Containing Folder | 选区包含 Sound | 唯一 Sound，当前平台与预览语言选出的来源已注册且实际文件存在；多选或来源缺失时禁用 |
| 12 | Open in New Tab | 存在非 Folder 对象 | 全部对象可打开；逐个打开绑定各自对象的独立 Inspector 标签页，保持两棵树多选。混入 Folder 则禁用 |
| 13 | Open in New Window | 存在非 Folder 对象 | 唯一可打开对象；打开浮动 Inspector，多选禁用 |
| 14 | Set Color... | 暂时隐藏 | 功能尚未确定发布，任何上下文均不显示 |
| 15 | Operation | 所有树上下文 | 子项按目标子树的实际展开状态启用；无可执行子项时入口保留禁用 |

Copy / Cut / Duplicate 的来源资格复用 `isClipboardEligible()`，不支持 Bus、Folder、空间对象或未公开类型。复制、删除等子树命令的整组判定包含祖先覆盖归一化；详见目标规则。

## 子菜单候选

### Create / New Child

分类根使用标题 Create，对象及工程 Folder 使用 New Child。下表是展示候选，临时限制只改变启用状态。

| 位置或对象类型 | 候选类型，按显示顺序 |
|---|---|
| Layers 分类根及其 Folder | Folder、Sound、Container、Track、Piece、Layer |
| Layer | Sound、Container、Track、Piece、Layer |
| Container | Sound、Container、Track、Piece |
| Track | Sound、Container、Piece |
| Piece | Track |
| Busses 分类根、Bus | Bus；分类根创建时以 Master Bus 为父对象 |
| Actions 分类根及其 Folder | Folder、Action |
| Variables 分类根及其 Folder | Folder、Variable |
| Attenuations 分类根及其 Folder | Folder、Attenuation |
| Sound、Action、Variable、Attenuation、空白处 | 无候选，隐藏入口 |

Container 候选展开四种模式：Random、Sequence、Match、Blend。Random 与 Sequence 默认 Play Type = Single，可在 Inspector 改为 Queue，一次触发遍历完整一轮。Queue 不再是创建菜单中的独立 Mode。创建、所选模式及撤销重做保持一致。Folder 通过目录树组织各桶内的对象文件，不充当对象的业务父级。

多选时候选取各目标可容纳类型的并集，但当前不提供向多个父对象批量创建子对象，整个入口禁用。例如 Sound + Piece 仍显示禁用的 New Child，其唯一相关候选是 Track。

### New Parent

| 选中对象类型 | 相关父类型，按显示顺序 |
|---|---|
| Sound、Container、Piece | Container、Track、Layer |
| Track | Container、Piece、Layer |
| Layer | Layer |
| Bus | Bus |
| Folder、Action、Variable、Attenuation | 无候选，隐藏入口 |

表中只表达类型相关性，不保证当前位置能创建该父类型。多选候选取并集，每个候选再检查全部对象、共同父级和结构约束；不会通过剔除后代来改变 New Parent 的分组含义。

### New Action

| 子项 | 目标类型 | 自动名称前缀 |
|---|---|---|
| Play | Sound、Container、Track、Piece | `Play_` |
| Stop | Sound、Container、Track、Piece | `Stop_` |
| Set Variable | Variable | `SetVariable_` |
| Trigger Action | Action | `TriggerAction_` |

只展示选区相关的动作，顺序如表。Layer、Bus、Folder、Attenuation 均不是这些菜单动作的目标；当前对象菜单不提供 Reset。

Set Variable 新步骤默认 Scope 为 Emitter，可以在步骤详情中改为 Global。作用域保存在步骤上，不来自目标 Variable 的声明；批量创建和撤销重做保持此字段。本项核对：2026-09-21。

对 N 个明确目标创建 N 个独立 Action，每个 Action 仅含一个本次指定类型的 step，分别引用对应目标。Container 与其 Sound 同选时仍创建两个 Action。重复 GUID 只处理一次；长名称截断后预留去重后缀空间，批内及既有名称冲突均自动处理。整个批次一次 Undo/Redo，完成后选中全部新 Action，不将多个对象合并到一个 Action，也不回退成其他动作类型。

### Operation

| 子项 | 显示范围 | 启用条件 |
|---|---|---|
| Expand All | 所有上下文 | 目标子树存在尚未展开的非叶分支 |
| Collapse All | 所有上下文 | 目标子树存在已展开的非叶分支 |

两项均作用于目标节点自身及其所有层级子节点；分类根作为桶的根节点，操作覆盖整个桶，不影响其他桶。纯叶节点、空容器和空白处保留禁用的选项；混入叶节点不会阻止其他相关分支的展开、折叠。多选时处理各选中子树，父子同选时按子树覆盖关系去重，不改变对象选区，不写工程 Undo。

## 目标、选区与撤销

| 命令类别 | 目标归一化 | 完成后的选区与撤销 |
|---|---|---|
| Cut / Copy | 按 GUID 去重，消除被已选祖先整体覆盖的后代 | 保持选区；写剪贴板不产生工程 Undo |
| Duplicate | 同上 | 一次选中全部副本根；整批一次 Undo/Redo |
| Delete | 同上；Folder 另计所覆盖的文件根，避免重复删除 | 移除失效选择；整批一次 Undo/Redo |
| Paste | 剪贴板来源为归一化后的子树，目的地必须唯一 | 一次选中全部复制或移动结果；整批一次 Undo/Redo |
| Paste Properties | 仅按 GUID 去重，保留明确选中的父和子 | 保持完整选区；有实际变化时整批一次 Undo/Redo |
| New Action / Open in New Tab | 仅按 GUID 去重，保留明确选中的父和子 | Action 选中全部创建结果并一次撤销；打开标签页保持选区且不写工程 Undo |
| New Parent | 原始选区必须全部为同父级兄弟对象 | 选中新父对象；创建父对象及重挂整批一次 Undo/Redo |
| Expand All / Collapse All | 按树行和已选祖先覆盖关系去重；分类根覆盖整个桶 | 保持选区，不写工程 Undo |

Rename、New Child、Paste、Open in New Window、Open Containing Folder 均保持单目标限制，不因子树归一化后只剩一个根而把多选视为单选。对象顺序使用树顺序，不依赖指针地址或哈希迭代顺序。

创建子对象后树定位新对象；普通重命名、定位源文件不另建结果集合。撤销恢复对象及引用，不承诺恢复任意历史选区。
