# UE5 StateTree 完整指南

> 本指南基于 **Unreal Engine 5.7** 源码（位于 `E:\UnrealEngine\Engine\Plugins\Runtime\StateTree`）以及官方文档、社区实践整理而成。
> 编写日期：2026-04
> 作者助手：Claude（基于源码确认所有执行顺序与默认值）

---

## 📚 文档结构

| # | 文档 | 内容 |
|---|------|------|
| 0 | **README.md**（本文件） | 总览、阅读指南、术语速查 |
| 1 | [01_概念与心智模型.md](./01_概念与心智模型.md) | StateTree 是什么、设计哲学、与行为树/状态机的对比 |
| 2 | [02_执行流程详解.md](./02_执行流程详解.md) | 状态选择、Tick、Transition 评估的完整流程（**含源码引用**） |
| 3 | [03_反直觉点与陷阱.md](./03_反直觉点与陷阱.md) | 7 个最容易踩坑的设计点详解 |
| 4 | [04_Cpp_Task_完整指南.md](./04_Cpp_Task_完整指南.md) | C++ Task 编写、所有标志位、生命周期 |
| 5 | [05_Transition_深度解析.md](./05_Transition_深度解析.md) | Transition 的所有类型、评估顺序、优先级 |
| 6 | [06_数据流与绑定.md](./06_数据流与绑定.md) | Parameters、Context、Evaluators、Bindings、PropertyRef |
| 7 | [07_高级特性.md](./07_高级特性.md) | LinkedAsset、Subtree、Utility Selection、ScheduledTick |
| 8 | [08_最佳实践与避坑清单.md](./08_最佳实践与避坑清单.md) | 实战经验总结 |
| 9 | [09_源码索引.md](./09_源码索引.md) | 关键源码文件、类、函数索引（5.7） |

---

## 🎯 阅读路径建议

### 初学者
1. 先读 **01 概念与心智模型**，建立基本认知
2. 然后 **02 执行流程详解**，理解 Tick / Transition 怎么跑
3. 最后 **03 反直觉点与陷阱**，知道哪里会摔跤

### 已经会用，想深入
1. 先读 **03 反直觉点与陷阱**，对照自己的项目反思
2. 然后 **04 C++ Task 完整指南**、**05 Transition 深度解析**
3. 配合 **09 源码索引** 边读源码边核对

### 准备做架构设计
1. 重点读 **07 高级特性**、**08 最佳实践**
2. 配合 **06 数据流与绑定** 设计 Schema 和数据传递

---

## 🗝️ 术语速查表

| 术语 | 简称 | 含义 |
|------|------|------|
| **State** | 状态 | 树的节点，逻辑容器 |
| **Task** | 任务 | 状态激活时执行的动作（移动、播放动画等） |
| **Transition** | 过渡 | 从一个状态跳到另一个状态的规则 |
| **Enter Condition** | 进入条件 | 决定某个状态是否能被选中 |
| **Evaluator** | 求值器 | 提供外部数据，5.5+ 已被 Global Task 取代 |
| **Global Task** | 全局任务 | 在树的整个生命周期都运行 |
| **Parameters** | 参数 | 树级变量（类似行为树 Blackboard） |
| **Schema** | 架构 | 决定 StateTree 可用哪些节点、上下文数据 |
| **Selector State** | 选择器状态 | 含子状态的中间状态，本身不被直接选中 |
| **Active States** | 活动状态链 | 当前所有正在 active 的状态（从 Root 到叶子） |
| **Linked Asset** | 链接资产 | 引用另一个 StateTree 资产作为子树 |
| **Subtree** | 子树 | 同一个资产内可被复用的状态分支 |
| **Utility** | 效用 | 5.5+ 引入的状态评分系统，类似 GOAP |
| **Property Ref** | 属性引用 | 类似 Blackboard Key，可读可写的引用 |

---

## ⚡ 关键快速参考

### EStateTreeRunStatus（任务返回值）

```cpp
enum class EStateTreeRunStatus : uint8
{
    Running,    // 任务还在跑（最常用！）
    Stopped,    // 被外部停止
    Succeeded,  // 成功完成 → 触发 Transition
    Failed,     // 失败完成 → 触发 Transition
    Unset,      // 未设置
};
```

### Task 的关键标志位（5.7 源码默认值）

```cpp
bShouldStateChangeOnReselect = true   // 重选时是否重发 Enter/Exit
bShouldCallTick              = true   // 是否调用 Tick（C++ Task 5.7 默认 true！）
bShouldCallTickOnlyOnEvents  = false  // 仅事件来时才 Tick
bShouldCopyBoundPropertiesOnTick      = true
bShouldCopyBoundPropertiesOnExitState = true
bShouldAffectTransitions     = false  // 是否调用 TriggerTransitions
bConsideredForScheduling     = true   // 是否参与调度 Tick 计算
bConsideredForCompletion     = true   // 5.6+：是否参与 State 完成判定
```

### Transition Trigger 类型

```cpp
None              = 0
OnStateCompleted  = 0x1 | 0x2  // Succeeded 或 Failed
OnStateSucceeded  = 0x1
OnStateFailed     = 0x2
OnTick            = 0x4         // 每帧评估
OnEvent           = 0x8
OnDelegate        = 0x10
```

### 关键硬限制

| 项目 | 限制 | 源码位置 |
|------|------|---------|
| 单条激活链最大状态数 | **8** | `FStateTreeActiveStates::MaxStates` |
| 单次 Tick 内最大重选迭代次数 | **5** | `FStateTreeExecutionContext::ApplyTransitions` 中 `MaxIterations` |
| 一个 State 内最大 Tasks 数（位图） | **64**（取决于位图大小） | `TTasksCompletionStatus<T>::MaxNumTasks` |

---

## 🔑 一句话总结 StateTree 的核心思想

> **StateTree = 行为树的"分层选择 + 数据绑定" + 状态机的"显式状态 + 条件过渡"**
>
> 核心模型是：**激活链上所有状态同时活跃，所有 Task 并发执行，任何 Task 完成都会触发从叶子向根的 Transition 评估**。
>
> 这套设计带来强大的"分层数据共享"和"行为分组"，但代价是执行模型比传统状态机复杂得多。

---

## 📦 本地资源位置

- 源码：`E:\UnrealEngine\Engine\Plugins\Runtime\StateTree\`
  - `Source\StateTreeModule\Public\` — 公共头文件
  - `Source\StateTreeModule\Private\StateTreeExecutionContext.cpp` — 核心执行流程（约 8000+ 行）
  - `Source\StateTreeModule\Public\StateTreeTaskBase.h` — Task 基类
  - `Source\StateTreeModule\Public\StateTreeExecutionTypes.h` — 所有枚举与类型
- 蓝图扩展插件：`E:\UnrealEngine\Engine\Plugins\Runtime\GameplayStateTree\`
- 编辑器源码：`Source\StateTreeEditorModule\`

