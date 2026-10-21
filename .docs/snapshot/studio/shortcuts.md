# HarkenStudio 快捷键现状快照

最近核对：2026-09-10。源码基线：`main` / `66fe38d4f763b2816e7fb32b1e95e7d171d82498`。

Edit 菜单补充核对：2026-10-21，当前工作区。

本篇记录当前快捷键的注册、绑定、设置与验证边界，供后续修改时持续核对。以当前源码为准，行为变化时直接更新对应表项。对象树菜单与快捷键的选区规则见[对象右键菜单快照](object-context-menus.md)。

路径缩写：`Source/` = `HarkenStudio/Source/`，`EditorTest/` = `HarkenStudio/Test/UnitTest/`。

## 范围与计数

统计 Studio 自有源码中的管理器命令和显式键盘处理。Qt 控件内建的文本编辑、选择与焦点导航、菜单助记键、操作系统快捷键、第三方控件行为及鼠标修饰键不计入命令表；编辑过程中的确认、取消按键单列。IPC 按键注入仅作为验证边界记录。

| 统计项 | 当前值 | 口径 |
|---|---:|---|
| 管理器注册命令 | 13 | File 2、Edit 8、Audition 2、Tools 1 |
| 非空默认键 | 11 | Reset Container、Audio Settings 默认无键 |
| 有管理器绑定的命令 | 12 / 13 | Audio Settings 无消费者；其余命令重绑可同步到已创建的 QAction |
| `bindAction()` 业务调用点 | 4 | MainWindow 的 Save、Preferences、Undo、Redo |
| `bindWidgetAction()` 业务调用点 | 14 | MainWindow 2、Project Tree 5、Curve 5、Timeline 2 |
| MainWindow 绑定命令 | 6 | 其中 5 个有非空默认键，Reset Container 默认无键 |
| 自有源码中的直接 `QShortcut` 绑定 | 0 | 当前已无 Create Template 的局部 Undo/Redo 绑定 |

调用点按源码位置计数，不包括管理器内部实现和测试。Project Explorer、Hierarchy 及多个编辑器实例会各自创建 QAction，运行时对象数随面板生命周期变化；表中数量不代表同时启用的快捷键数量。

## 命令注册与默认键

注册表依据 [ShortcutManager.h](../../../../HarkenStudio/Source/Core/ShortcutManager.h) 的 `Shortcuts` ID 和 [ShortcutManager.cpp](../../../../HarkenStudio/Source/Core/ShortcutManager.cpp) 的 `setupDefaultShortcuts()`。下表 `QKeySequence::Save` 等表示 StandardKey；带引号的序列表示源码指定的键位；“空”表示已注册但默认不分配按键。

本地 `build-nc` 配置引用 Qt 6.11.1。平台列按该版本的 [QKeySequence 标准键与 Apple 映射](https://raw.githubusercontent.com/qt/qtbase/v6.11.1/src/gui/kernel/qkeysequence.cpp) 解释主序列：`⌘` 为 Command，`⇧` 为 Shift，`⌦` 为 Forward Delete。Qt 序列中的 `Ctrl` 在 macOS 对应 Command，因此固定字符串中的 `Ctrl` 也按此映射。macOS 列未作设备实测。

| 命令 ID | 分类 | 设置显示名 | 默认键定义 | Windows | macOS |
|---|---|---|---|---|---|
| `file.save` | File | Save Project | `QKeySequence::Save` | `Ctrl+S` | `⌘S` |
| `file.preferences` | File | Preferences | `QKeySequence("Ctrl+,")` | `Ctrl+,` | `⌘,` |
| `edit.undo` | Edit | Undo | `QKeySequence::Undo` | `Ctrl+Z` | `⌘Z` |
| `edit.redo` | Edit | Redo | `QKeySequence::Redo` | `Ctrl+Y` | `⇧⌘Z` |
| `edit.cut` | Edit | Cut | `QKeySequence::Cut` | `Ctrl+X` | `⌘X` |
| `edit.copy` | Edit | Copy | `QKeySequence::Copy` | `Ctrl+C` | `⌘C` |
| `edit.paste` | Edit | Paste | `QKeySequence::Paste` | `Ctrl+V` | `⌘V` |
| `edit.delete` | Edit | Delete | `QKeySequence::Delete` | `Delete` | `⌦` |
| `edit.selectAll` | Edit | Select All | `QKeySequence::SelectAll` | `Ctrl+A` | `⌘A` |
| `edit.duplicate` | Edit | Duplicate | `QKeySequence("Ctrl+D")` | `Ctrl+D` | `⌘D` |
| `audition.playStop` | Audition | Play/Stop | `QKeySequence("Space")` | `Space` | `Space` |
| `audition.reset` | Audition | Reset Container | 空 | 空 | 空 |
| `tools.audioSettings` | Tools | Audio Settings | 空 | 空 | 空 |

每个命令保存一个 `QKeySequence`，通过 `QAction::setShortcut()` 绑定；当前未使用 `setShortcuts(StandardKey)` 安装平台的全部候选键。例如 Windows 的 Redo 默认只绑定 `Ctrl+Y`，不能把 Qt 标准键表中的其他候选自动列为 Studio 的业务绑定。

## 管理与触发链路

键盘链路为 `ShortcutManager -> QAction -> 业务处理函数`。`QKeySequence` 负责表示按键，管理器负责键位状态和同步，QAction 与 Qt 负责触发。Edit 菜单通过 `createMenuAction()` 根据打开菜单前的焦点查找已有控件动作，再调用同一业务入口。

| 环节 | 当前实现 |
|---|---|
| 初始化 | 单例构造时先注册默认键，再从 QSettings 加载覆盖值 |
| 绑定已有动作 | `bindAction()` 保存 QAction，立即设置当前键；动作所属菜单和作用域由调用方决定 |
| 绑定控件动作 | `bindWidgetAction()` 创建由宿主控件持有的 QAction，设置作用域、连接 handler、调用 `widget->addAction()` 后交给管理器绑定；默认作用域为 `Qt::WidgetShortcut` |
| 创建菜单入口 | `createMenuAction()` 展示注册名称与目标控件的有效键位，仅显示快捷键提示，不再注册按键；监听修改、重置和导入通知刷新提示。打开菜单时按焦点控件及祖先的动作作用域、启用和可见状态决定是否可用；点击时重新校验目标，目标销毁后不再执行 |
| 修改与单项恢复 | `setShortcut()` / `resetToDefault()` 更新同 ID 的全部存活动作，并发出 `shortcutChanged` |
| 全量恢复 | `resetAllToDefaults()` 更新全部动作，发出 `shortcutsReset` |
| 生命周期 | QAction 销毁时自动从绑定集合移除；后创建的面板绑定时取得当前键位 |
| 业务条件 | 是否可执行由动作启用状态及 handler 的业务校验决定，绑定成功不等于操作一定成功 |

### Qt 作用域

下表用于解释绑定声明；实际按键仍受控件事件处理、动作状态和窗口状态影响。作用域定义见 [Qt::ShortcutContext](https://doc.qt.io/qt-6/qt.html#ShortcutContext-enum)，QAction 未显式设置时默认使用 [WindowShortcut](https://doc.qt.io/qt-6/qaction.html#shortcutContext-prop)。

| 作用域 | 触发范围 |
|---|---|
| `Qt::WindowShortcut` | 动作宿主属于当前活动顶层窗口 |
| `Qt::ApplicationShortcut` | 应用的某个窗口处于活动状态 |
| `Qt::WidgetShortcut` | 宿主控件自身有焦点 |
| `Qt::WidgetWithChildrenShortcut` | 宿主或其子控件有焦点；除弹出窗口外的独立顶层子窗口不包含在内 |

### 消费者清单

表中命令名对应上一节的注册 ID。全部通过管理器绑定，因此同一命令的重绑会影响表中全部消费者；没有按面板分别配置键位的能力。

| 宿主 | 命令 | Qt 作用域 | 业务入口 | 键位提示 |
|---|---|---|---|---|
| MainWindow / File 菜单 | Save Project | `WindowShortcut` | `onSaveProject()` | 菜单 QAction |
| MainWindow / File 菜单 | Preferences | `WindowShortcut` | `onOpenPreferences()` | 菜单 QAction；声明 `QAction::PreferencesRole` |
| MainWindow / Edit 菜单 | Undo、Redo | `ApplicationShortcut` | `UndoRedoManager` 创建的动作，调用 `undo()` / `redo()` | 菜单 QAction，并跟随可撤销／重做项更新文字与启用状态 |
| MainWindow / Edit 菜单 | Cut、Copy、Paste、Duplicate、Delete、Select All | 仅菜单入口，不增加快捷键绑定 | 按焦点复用已有控件动作；文本编辑和原生列表全选调用 Qt 控件接口 | 菜单文本中的 `NativeText` 有效键位；管理器绑定被清空后仍可点击菜单 |
| MainWindow | Play/Stop | `ApplicationShortcut` | `onPlayPressed()` | AuditionBar Play 按钮 tooltip |
| MainWindow | Reset Container | `ApplicationShortcut` | `onResetPressed()` → `audition_reset` action | AuditionBar Reset 按钮 tooltip |
| ProjectTreePanel 的 `treeWidget_` | Cut、Copy、Paste、Delete、Duplicate | `WidgetWithChildrenShortcut` | `onCut()`、`onCopy()`、粘贴路由、`onDelete()`、`onDuplicate()` | 对象上下文菜单文本 |
| CurveCanvas | Select All、Cut、Copy、Paste、Delete | `WidgetShortcut` | 全选曲线点及曲线点剪贴板／删除函数 | 未提供键位提示 |
| TimelineMarkerRuler | Delete | `WidgetShortcut` | `deleteMarker()`，成功后清除所选 marker | 未提供键位提示 |
| TimelineCanvas | Delete | `WidgetShortcut` | `deleteSelectedClips()` | 未提供键位提示 |

绑定源码：[MainWindow.cpp](../../../../HarkenStudio/Source/MainWindow.cpp)、[ProjectTreePanel.cpp](../../../../HarkenStudio/Source/Widgets/ProjectTree/ProjectTreePanel.cpp)、[CurveCanvas.cpp](../../../../HarkenStudio/Source/Widgets/CurveEditor/CurveCanvas.cpp)、[TimelineClipCanvas.cpp](../../../../HarkenStudio/Source/Widgets/Timeline/TimelineClipCanvas.cpp)（包含 `TimelineMarkerRuler`）、[TimelineCanvas.cpp](../../../../HarkenStudio/Source/Widgets/Timeline/TimelineCanvas.cpp)。Undo/Redo 动作状态见 [UndoCommands.cpp](../../../../HarkenStudio/Source/Core/UndoCommands.cpp) 的 `createReplayAction()`。

### 选择、焦点与提示行为

| 场景 | 当前行为与边界 |
|---|---|
| Play/Stop，焦点位于 Source Editor 或其 dock 内 | 优先调用该编辑器的 `handlePlayStopShortcut()`，切换该文件的原始试听；处理失败也在此分支结束。Source Editor 未另外注册快捷键 |
| Play/Stop，焦点位于其他位置 | 存在非 RawFile 试听时调用 `onStopPressed()` 停止对象试听；否则尝试播放 Project Explorer 的当前所选对象。模拟模式、无选择或对象不可播放时由 handler 拒绝 |
| AuditionBar 的鼠标按钮 | Play 点击调用 `onPlayRequested()`，Stop 点击调用 `onStopPressed()`；快捷键的焦点路由和播放／停止切换在 `onPlayPressed()` 中。Play 的 tooltip 展示的是 Play/Stop 快捷键 |
| Reset Container | 使用 `auditionResetItem_`，由选择路由及 Blueprint 选择更新。当前要求目标为 Container、有 `VSelection`、模式属于 Random / Sequence、`runtimeStableId != 0`；Single 按 Memory Scope 清选择记忆，Queue 清当前发声体的队列记忆；handler 再检查 Reset 按钮是否可用。默认无键，分配键位后才有键盘入口 |
| Project Tree | Project Explorer 和 Hierarchy 继承同一 `ProjectTreePanel`，共用五个绑定。粘贴到分类根时走 `pasteClipboardAtBucketRoot()`，其他目标走 `pasteInto()`。树未绑定管理器的 Select All |
| Curve | 操作曲线点而非工程对象；Delete/Cut 的可编辑点限制、粘贴载荷与点数限制由曲线函数处理。Select All 的管理器消费者仅在 CurveCanvas |
| Edit 菜单 | 顺序为 Undo / Redo、分隔线、Cut / Copy / Paste / Duplicate / Delete、分隔线、Select All。操作作用于打开菜单前的焦点控件，菜单取得焦点不会改变目标；无对应动作时置灰，已有业务动作的选择与剪贴板限制仍由其 handler 校验 |
| Edit 菜单中的文本与原生列表 | `QLineEdit`、`QTextEdit`、`QPlainTextEdit` 优先编辑文字，不向外层对象传播；按选区、只读状态和剪贴板内容启用，密码输入框不允许复制或剪切。原生多选列表支持 Select All。Qt 控件的内建键盘映射仍由 Qt 处理，不随管理器重绑；对应可用菜单项显示原生标准键，已绑定的业务控件显示配置键 |
| Timeline | ruler 与 canvas 共用 `edit.delete`，按焦点删除 marker 或所选 clips；canvas 支持删除多选 clips，不只单个 clip |
| 菜单与 tooltip | MainWindow 的 Save、Preferences、Undo、Redo 直接展示 QAction 当前键；Edit 新增的六个入口和 Project Tree 菜单用 `label + '\t' + NativeText` 展示键位，菜单动作自身不注册快捷键；Curve 上下文菜单调用点编辑函数，但不持有快捷键或键位提示 |

提示刷新见 [ShortcutManager.cpp](../../../../HarkenStudio/Source/Core/ShortcutManager.cpp) 的 `createMenuAction()`、[ProjectItemMenuController.cpp](../../../../HarkenStudio/Source/Widgets/Shared/ProjectItemMenuController.cpp) 的 `refreshShortcutHints()` 与 [AuditionBar.cpp](../../../../HarkenStudio/Source/Widgets/AuditionBar/AuditionBar.cpp) 的 `refreshPlayShortcut()` / `refreshResetShortcut()`；均监听 `shortcutChanged` 和 `shortcutsReset`。Source Editor 切换见 [SourceEditorWidget.cpp](../../../../HarkenStudio/Source/Widgets/SourceEditor/SourceEditorWidget.cpp)；[AuditionManager.cpp](../../../../HarkenStudio/Source/Core/AuditionManager.cpp) 的 `stopAll()` 跳过 RawFile 试听。

## 未经管理器的固定按键

以下处理直接检查 Qt 键事件，不出现在 Preferences 的 Shortcuts 页，也不随管理器重绑。它们没有 `Qt::ShortcutContext` 声明，只有事件到达对应控件时才执行；不能把它们记为 QAction 绑定。

| 控件 / 场景 | 固定键 | 当前行为 | 定位 |
|---|---|---|---|
| TransitionEditorWidget | `Qt::Key_Delete` | 有所选显示行时调用 `removeSelectedRule()`；默认规则等删除限制仍由业务校验决定。修改 `edit.delete` 不会修改此键 | [TransitionEditorWidget.cpp](../../../../HarkenStudio/Source/Widgets/TransitionEditor/TransitionEditorWidget.cpp) / `keyPressEvent()` |
| TimelineCanvas | `Qt::Key_Home` | 将编辑游标设为 `viewOriginCoordinate()` | [TimelineCanvas.cpp](../../../../HarkenStudio/Source/Widgets/Timeline/TimelineCanvas.cpp) / `keyPressEvent()` |
| TimelineCanvas | `Qt::Key_End` | 将编辑游标设为所有 lane 中最大的 `clipStart + clipDuration`，下限为 `viewOriginCoordinate()`；不是播放头跳转或 nominal span 末尾 | 同上 |

这三项直接比较 `event->key()`，未附加修饰键相等检查；表中键名表示 Qt 键码，不承诺带修饰键时一定能到达该处理函数。

编辑过程中的按键另列如下，不增加管理器命令数量：

| 控件 / 场景 | 按键 | 当前行为 | 定位 |
|---|---|---|---|
| TimelineCanvas 有活动手势 | `Esc` | `cancelGesture()`，取消拖动、框选或放置预览 | `TimelineCanvas.cpp` / `keyPressEvent()` |
| Timeline marker 名称编辑框 | Return、`Esc` | `returnPressed` 提交重命名；事件过滤器处理 Esc 取消；失焦也提交 | `TimelineClipCanvas.cpp` / `commitRename()`、`cancelRename()`、`eventFilter()` |
| ValueSegmentedControl 数值编辑框 | Return / Enter、`Esc` | 确认或取消当前数值编辑；失焦按确认处理 | [ValueSegmentedControl.cpp](../../../../HarkenStudio/Source/Widgets/Common/ValueSegmentedControl.cpp) / `eventFilter()` |
