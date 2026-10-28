# HarkenStudio 快捷键现状快照

## 范围与数量

| 统计项 | 当前值 | 口径 |
|---|---:|---|
| 可配置命令 | 13 | File 2、Edit 8、Audition 2、Tools 1 |
| 有默认键的命令 | 11 | Reset Container、Audio Settings 默认无键 |
| 已接入操作的命令 | 12 / 13 | Audio Settings 暂无操作入口 |
| 固定操作键 | 3 | Transition Delete、Timeline Home／End；不随偏好设置重绑 |

表注：不统计文本控件内建编辑、焦点导航、菜单助记键、操作系统及第三方控件快捷键，也不统计鼠标修饰键。编辑确认与取消键单列。

## 命令与默认键

| 分类 | 命令 | Windows | macOS | 作用范围 |
|---|---|---|---|---|
| File | Save Project | Ctrl+S | ⌘S | 主窗口及浮动面板 |
| File | Preferences | Ctrl+, | ⌘, | 主窗口 |
| Edit | Undo | Ctrl+Z | ⌘Z | 主窗口及浮动面板 |
| Edit | Redo | Ctrl+Y | ⇧⌘Z | 主窗口及浮动面板 |
| Edit | Cut | Ctrl+X | ⌘X | 对象树、曲线 |
| Edit | Copy | Ctrl+C | ⌘C | 对象树、曲线 |
| Edit | Paste | Ctrl+V | ⌘V | 对象树、曲线 |
| Edit | Delete | Delete | ⌦ | 对象树、曲线、Timeline 标记及片段 |
| Edit | Select All | Ctrl+A | ⌘A | 曲线点 |
| Edit | Duplicate | Ctrl+D | ⌘D | 对象树 |
| Audition | Play/Stop | Space | Space | 主窗口及浮动面板，按焦点路由试听 |
| Audition | Reset Container | 空 | 空 | 当前试听 Container |
| Tools | Audio Settings | 空 | 空 | 尚未接入 |

表注：macOS 列按 Qt 6.11.1 的平台键位映射，未经设备实测；⌘ 为 Command，⇧ 为 Shift，⌦ 为 Forward Delete。每个命令仅配置一个键序列。

## 设置与菜单

| 项目 | 当前行为 |
|---|---|
| 修改快捷键 | 录入完成后立即更新并保存，同命令所有面板同步；后打开的面板使用当前键位 |
| 冲突替换 | 需确认 |
| 清除／单项恢复 | 立即生效 |
| 恢复全部 | 需确认 |
| 关闭或切换偏好面板 | 不回滚已完成的修改 |
| 按面板分别配置 | 不支持 |
| Edit 菜单顺序 | Undo、Redo；Cut、Copy、Paste、Duplicate、Delete；Select All |
| Edit 菜单目标 | 使用打开菜单前的焦点控件，无可执行操作则禁用；清除业务快捷键后仍可点击菜单 |
| 文本输入中的 Edit 菜单 | 优先编辑文字，遵守选区、只读及剪贴板限制；密码输入不可复制或剪切 |
| 原生文本／列表按键 | 使用控件内建键位，不随业务命令重绑；原生多选列表支持 Select All |
| 键位提示 | 主窗口菜单、对象菜单及试听按钮提示当前有效键位；曲线菜单与 Timeline 未提供提示 |

## 作用范围与选择

| 场景 | 当前操作目标与边界 |
|---|---|
| 对象树 | 焦点所在 Explorer／Hierarchy 的本地选区；不合并其他树选区，未绑定可配置 Select All |
| 曲线 | 曲线点；删除与剪切仍受可编辑点限制，粘贴受数据与点数限制 |
| Timeline | 按焦点删除所选标记或本地多选片段；普通单击片段不切换全局对象或试听目标 |
| Play/Stop：焦点在 Source Editor | 切换该文件的原始试听，处理失败也不转向对象试听 |
| Play/Stop：其他焦点 | 存在非原始文件试听时停止；否则播放当前试听目标 |
| 试听目标 | 由主动对象选择或 Selection Editor 的 Child 行指定；仅切换焦点不改目标，不从其他树保留高亮推断 |
| 不可播放情况 | 模拟模式、无目标或对象不可播放时拒绝执行 |
| Reset Container | 当前目标须为可重置的 Random／Sequence Container；Single 清选择记忆，Queue 清当前发声体队列记忆 |
| Reset 与 Control 固定 | Control 固定不改变 Reset 目标；默认无键，须分配后才可从键盘触发 |
| 试听鼠标按钮 | Play 仅发起播放，Stop 仅停止；Play/Stop（默认 Space）按焦点切换试听 |
| Save Project | 保存前提交正在编辑的有效项目设置 |

表注：树间选择、导航和右键菜单目标见[对象右键菜单快照](object-context-menus.md)。快捷键绑定不豁免命令本身的业务限制。

## 固定操作键

| 界面／场景 | 按键 | 当前行为 |
|---|---|---|
| Transition Editor | Delete | 删除所选规则；默认规则等限制仍生效 |
| Timeline，编辑游标可见 | Home | 游标移至视图起点 |
| Timeline，编辑游标可见 | End | 游标移至最末片段结束处，不早于视图起点 |

表注：以上不出现在快捷键设置中。当前 Track／Piece 默认隐藏编辑游标，Home／End 入口不启用；修饰键组合是否生效取决于控件能否收到该按键。

## 编辑确认与取消

| 界面／场景 | 按键 | 当前行为 |
|---|---|---|
| Timeline 活动手势 | Esc | 取消拖动、框选或放置预览 |
| Timeline 标记名称编辑 | Return／失焦、Esc | 提交或取消；打开文本菜单保持编辑，组件尚未接入当前主面板 |
| 分段数值编辑框 | Return／Enter／失焦、Esc | 确认或取消当前数值编辑 |
