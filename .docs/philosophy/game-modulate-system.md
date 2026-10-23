# Game Modulate 系统

Game Modulate 把游戏与声音系统之间的交流收敛为两个入口：Action 输送一次性意图，Variable 输送持续状态。瞬时事件不伪装成电平，持续状态也不承担命令职责。

## Variable：唯一的状态对象

Variable 是具名浮点输入槽，不拆成状态、开关与参数多个对象族。它声明：

- **kind**：Step（离散）/ Smooth（连续）；
- **range**：合法数值范围，`0 <= min <= max`；Step 下端点为整数；
- **default**：槽位诞生时的初值；
- **labels**：整数位置上的可选枚举名称与稳定身份。Label 的身份、当前索引、名称分别保存；重命名或移动不改变身份。名称为空时显示当前索引，非空名称在同一 Variable 内区分大小写且唯一，隐藏项也参与重名检测。数值读数按 `floor(value + 0.5)` 查对应整数位置的名称，稀疏间隙显示数字。Label 同时适用于 Step 和 Smooth，切换 kind 保留其配置和引用方式。

新建 Variable 时选择 Smooth 或 Step。Smooth 初始范围为 `[0, 100]`，无预置 Label；Step 为 `[0, 3]`，预置 `0 = None`；两者 Default 均为 0。None 是可改名、清空和移动的普通名称，不表示空值或通配。创建预设不在 kind 切换时重用。

Label 表格显示 `# / Name`，`#` 是不可编辑的真实索引，支持 0 和任意起始位置。普通行移动将名称和身份分配到参与移动的原有索引位置，稀疏位置集合与窗口外记录保持不变。缩小 Range 隐藏并保留范围外记录；Pack、名称映射和 SDK Catalog 只包含索引严格位于当前 Range 内的项。

Match 的条件表保存 Variable 与 Label 身份，重命名和重排后仍引用同一状态，构建时解析为当前整数比较值。选择未配置 Label 的整数会与格子修改一起创建未命名身份。空格子继续表示通配，0 与 None 均不等于空格子。删除仍被引用的 Label 被拒绝；范围变化保留引用，导出前必须修复不可达条件。

Blend 模式的混合行和 Binding 保存数值坐标。同一 Label 移到新索引后，其输入使用新坐标，曲线不自动移动。Default、Set Variable Action 和已经写入的运行时值也保留数值语义。

Native 与 Unity 可按当前名称或稳定 Label 身份赋值，控制侧解析当前 Pack 映射后使用现有数值写入路径。旧名称、删除或范围外的身份明确失败，不回退为数字、None 或默认值。生成的 Label 常量保存身份，不缓存枚举数值。映射与编译后的离散条件通过 Pack 依赖合同一起校验和发布；改名更新名称入口与 Catalog，不重置数值槽位。


Variable 的定义描述数值本身，不声明作用域。每个 Variable 都有全局值，也允许发声体拥有自己的局部值。全局值在运行状态建立时以 default 初始化。统一的 `GlobalWithEmitterOverride` 契约表示这两层存储，不提供 `GlobalOnly` 类别。

值恒存在，没有空值态。消费时，发声体有局部值便使用局部值，否则读取全局值；没有发声体上下文的消费者读取全局值。default 是初始化值，不是第三级读取来源。

作用域属于一次赋值。Emitter 赋值只改变指定发声体的局部值；Global 赋值更新全局值，并在同一次操作中清除所有发声体对这个 Variable 的局部值。此后各发声体读取新的全局值，直到新的 Emitter 赋值再次建立局部值。即使赋入的全局数值没有变化，已有局部值也必须清除。赋值只影响所指定的 Variable。

系统不提供单独清除某个发声体的 Variable 局部值的操作。发声体销毁时，其局部状态随生命周期结束。

kind 不门控消费者。离散消费者按取整读，连续消费者按原值读；改变 kind 不应让既有 Binding 或网格轴失效。

## Emitter 上下文与消费边界

Emitter 上下文是消费者在运行时关联的发声体身份，也是 Variable 局部值的共享边界。局部值归属于一个 Emitter 与一个 Variable 的组合；判断局部值是否共享，以 Emitter 身份是否相同为准。

同一 Emitter 上所有消费该 Variable 的播放实例共享其局部值，包括多次播放、不同 Action 触发以及不同 Layer 下的声音。局部赋值的影响不限定于触发赋值的那一次 Action、某一个声音或某一个 Layer。不同 Emitter 的局部值彼此独立，即使它们使用同一个声音定义、同一个 Layer 或同一个输出 Bus。

运行时触发为播放关联 Emitter 上下文，后代播放沿用该上下文。内容组织、属性继承与音频路由不会自行创建、切换或合并 Emitter 上下文；没有 Emitter 上下文的消费者读取全局值。播放结束不清除仍存活 Emitter 的局部值，后续关联该 Emitter 的播放继续使用当时有效的值。

Layer 的 Binding 随其作用的后代播放实例求值，使用该实例的 Emitter 上下文；存在局部值时读取局部值，否则读取全局值。Layer 不独立持有 Variable 值，同一 Layer 在不同 Emitter 上可以产生不同的调制结果。

Bus 不携带 Emitter 上下文，其 Binding 始终读取 Variable 的全局值。Emitter 局部赋值不改变 Bus 的 Variable 输入，也不会因存在 Bus Binding 而转为全局赋值。

Variable 的写入目标由一次赋值选择，消费者的读取来源由其运行上下文决定。Binding 的配置、属性继承与贡献合成不另行建立 Variable 局部值的作用域。

| 消费位置 | 求值上下文 | Variable 读取规则 |
| --- | --- | --- |
| Sound、Container、Track、Piece 的播放消费者，包含 Match 选择、Blend 增益、Voice Binding | 该播放实例关联的 Emitter | 该 Emitter 的局部值优先，否则全局值；无 Emitter 时读取全局值 |
| Layer Binding | 每个受其影响的后代播放实例的 Emitter | 该 Emitter 的局部值优先，否则全局值；无 Emitter 时读取全局值 |
| Bus Binding | 无 Emitter 上下文 | 始终读取全局值 |

A、B 是两个不同的 Emitter，都播放同一个 Layer 下的 Sound，Layer 与输出 Bus 均绑定 Variable X，依次执行三次赋值：

| 操作 | A 的 Layer Binding 读取 X | B 的 Layer Binding 读取 X | Bus Binding 读取 X |
| --- | ---: | ---: | ---: |
| Global X = 1 | 1 | 1 | 1 |
| A 的 Emitter：X = 2 | 2 | 1 | 1 |
| Global X = 3 | 3 | 3 | 3 |

表中数值是各 Binding 的 Variable 输入；各自的曲线再将输入映射为参数贡献。最后一行同时清除 A、B 对 X 的全部局部覆盖。局部赋值可能通过声源变化改变流入 Bus 的音频，但不会使 Bus 自身的 Binding 读取局部值。若 A 同时播放其他 Layer 下使用 X 的声音，它们也读取 A 的局部值。

## Binding：状态到参数的单跳映射

Binding 从一个 Variable 出发，经内嵌曲线映射到一个参数目标。源与目标的离散/连续性互不约束：Step 可以驱动连续参数，Smooth 也可以经取整语义驱动离散目标。

Binding 保持星形单跳。Variable 永远是源，不是目的地；系统不建立 Variable 到 Variable 的派生图，因此也不引入级联顺序、环检测、写入所有权和跨作用域传播等第二套状态计算规则。

同一参数的最终值由基值和组织/摆放链各对象的 Binding 贡献合成。贡献累加属于信号链决策，不表示子对象继承父对象的字段值，也不表示子对象的贡献覆盖父对象。继承/覆盖仅指同一字段在父值与子对象自身值之间择一生效。Binding 塑形已经存活的声音；Match 决定获胜成员及切换；Blend 启动全部配置成员，混合行只调整其增益，不因权重过零起停成员。

Match 与 Blend 的 **Variable Response** 是容器局部枚举：**At Start** 只在该实例启动时采样选择或配比，**Live** 在播放尚未结束时响应后续变化。它不锁存 Variable、不冻结 Binding，也不覆盖子容器自己的响应设置。监听本身不保活；零增益成员仍在推进、时间线仍有排程或尾音未结束时，播放仍未完成。Match 默认 At Start，Blend 默认 Live。

Blend 是 Container 的多选与混合模式，不是 Match 条件表的连续版本。`selection.selections` 统一保存四种选择策略的条目；Selection Editor 在 Blend 模式下显示混合表，每行（row）配置一个可选的 Variable，并为参与的每个 child 提供一条完整的自由曲线。横轴为 Variable 数值，纵轴为独立的 0–100% 线性增益；所有 child 的百分比不自动归一化，两个 100% 成员均以原始音量播放。同一行中的同一 child 只有一条曲线，多峰与多个静音区间通过曲线本身表达。行的 Variable 可以为空，此时仅禁用曲线坐标编辑区，Child 副表中的成员按 100% 参与默认混合；已有曲线保留，重新指定 Variable 后恢复使用。

同一 child 可以进入不同 Variable 的混合行，仍共用一份播放，各行贡献相乘；两个 50% 得到 25%。未进入的行不影响该 child。移除某行中的成员只移除这一行的贡献，最后一处成员引用移除后才退出混合成员集合。混合行是统一 selections 的编辑视图，不是另一份成员名单。Track 专指 Piece/Track 对象体系中的轨道。Random/Sequence 的 Inspector 表、Match 条件矩阵与混合表仅因内容复杂度而采用不同编辑入口；模式切换保留条目中的重复播放项、条件和曲线。

曲线首尾点界定有效区间，区间外的贡献为 0%；首尾点不固定在变量范围两端，可以直接编辑坐标。曲线容器标题只沿横轴拖动，所有点与手柄共同平移、百分比不变；调整区间通过首尾点完成，不另设容器边缘缩放。变量范围变更保留既有数值坐标。混合曲线仍使用公共点与贝塞尔手柄表示；Pack 编译以首尾点作为区间边界，将同一 child 的各行贡献放入同一个运行时连续成员，不改变引擎的 VariableControl / ContinuousGrid 术语。

## 曲线：宿主体内的函数

曲线是输入到输出的函数数据，由点与手柄描述统一的贝塞尔形态。直线、阶梯感、缓入缓出和自由形状都是同一种表示的不同姿态。

曲线不是工程对象：它没有独立身份、树位置或引用生命周期。Binding、Transition fade、混合成员与 Attenuation 各自在自己的语义中内嵌曲线。需要复用时复制曲线数据；复制后各宿主独立演化，不建立共享对象或隐藏联动。

曲线只表达纯函数，不表达历史。滞回、防抖、冷却、队列记忆等依赖过去状态的行为必须留在拥有相应状态机的机制或游戏侧，不能塞进曲线。

## Action：意图入口

Action post 表示“发生了一次”。Play、Stop、PostAction、Set Variable 与 Reset 是意图步骤；量化与冷却只属于需要调度的一次性步骤。Set Variable 的每个步骤独立选择 Global / Emitter，默认 Emitter，其身份来自此次 Action 的运行时上下文；缺失 Emitter 时明确失败，不转为全局写入。Global 步骤即使在带 Emitter 的 Action 中执行，也执行全局赋值。

Set Variable 在既有执行边界改变状态，状态引发的交接由 Container 的过渡规则负责，不能再给 Set Variable 叠加第二次量化或独立过渡。相同 Variable 的多次写入按生效时间、逻辑提交顺序及 Action 步骤序号执行；较早的全局赋值不取消未来才执行的局部赋值。

游戏侧赋值时，省略 Emitter 表示全局赋值，传入有效 Emitter 表示局部赋值；明确指定但无效或过期的 Emitter 必须报错，不能回退为全局赋值。

Variable 与 Action 因此形成闭合分工：Action 改变世界或触发内容，Variable 描述世界当前是什么样。无需第三种瞬时通道，也不把瞬时事件编码成先设 1 再复位 0 的脆弱协议。
