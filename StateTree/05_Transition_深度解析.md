# 05. Transition 深度解析

> 本章基于 UE5.7 源码 `StateTreeTypes.h` 和 `StateTreeExecutionContext.cpp`

---

## 一、Transition 是什么

**Transition** 不是状态切换本身，而是 **触发状态切换的规则**。它由四个部分构成：

```
┌─────────────────────────────────────────┐
│  Transition                              │
│  ├─ Trigger (触发类型)                   │
│  ├─ Condition (过渡条件，可选)           │
│  ├─ Target (目标状态)                    │
│  ├─ Priority (优先级，多个并发时决定胜负)│
│  └─ Fallback (备选行为)                  │
└─────────────────────────────────────────┘
```

---

## 二、Trigger 类型完整列表

源码（`StateTreeTypes.h`）：

```cpp
UENUM()
enum class EStateTreeTransitionTrigger : uint8
{
    None              = 0,
    OnStateCompleted  = 0x1 | 0x2,  // = OnStateSucceeded | OnStateFailed
    OnStateSucceeded  = 0x1,
    OnStateFailed     = 0x2,
    OnTick            = 0x4,
    OnEvent           = 0x8,
    OnDelegate        = 0x10,
    MAX
};
ENUM_CLASS_FLAGS(EStateTreeTransitionTrigger)
```

注意 `OnStateCompleted` 实际是 `Succeeded | Failed` 的位组合，所以一个 Task Failed 同时会匹配 `OnStateCompleted` 和 `OnStateFailed`。

### 各 Trigger 详解

#### `OnStateCompleted`

**含义**：状态中的任一 Task 返回 `Succeeded` 或 `Failed`，且符合状态的完成模式（Any/All）

**评估时机**：仅在 Task 完成事件后

**典型用法**：
```
State 编辑器中:
  Transition: On State Completed → NextState
```

**踩坑**：会同时被 Succeeded 和 Failed 触发，如果想区分，应用 `OnStateSucceeded` / `OnStateFailed`。

#### `OnStateSucceeded`

**含义**：状态成功完成（在 Any 模式下，任一 Task Succeeded；在 All 模式下，所有都 Succeeded）

#### `OnStateFailed`

**含义**：状态失败完成

#### `OnTick`

**含义**：每帧评估 Condition

**评估时机**：每次 Tick 的 ApplyTransitions 阶段

**关键事实**：**这是唯一一种"主动每帧检测"的 Transition**，其他类型都是事件驱动。

**用途**：
- 持续监控全局打断条件（生命值、距离、时间）
- 实现行为树的 "Decorator + Observer Aborts" 效果

**性能考量**：Condition 每帧执行，避免昂贵的检查。

#### `OnEvent`

**含义**：当指定的 GameplayTag 事件被发送时

**配置**：需要在 Transition 上设置 `RequiredEvent`（一个 FGameplayTag）

**发送方式**：
```cpp
// 任何地方都可以发送
Context.SendEvent(EventTag, PayloadStruct, OriginActor);

// 在蓝图里也可以从 StateTreeComponent 发送
StateTreeComponent->SendStateTreeEvent(EventTag);
```

**事件队列**：事件入队后，下次 Tick 才会被处理；处理后从队列移除。

#### `OnDelegate`

**含义**：当指定的 Delegate 被广播时

**配置**：需要绑定一个 `FStateTreeDelegateDispatcher`

**用途**：和事件类似，但更结构化（可以携带类型化数据）

```cpp
// 在 Task 中触发
Context.BroadcastDelegate(MyDispatcher);
```

---

## 三、Trigger 组合（位标志）

由于 Trigger 是 `EnumClassFlags`，可以组合：

```cpp
// 编辑器中的 Trigger 设置可以组合，但通常不建议
// 一个 Transition 同时监听 Tick 和 Event 在编辑器中实现起来不直观
```

实践中很少需要手动组合，编辑器 UI 一般也是单选。

---

## 四、Transition 评估的完整流程

### 算法（来自源码 `TriggerTransitions` 函数）

```
TriggerTransitions:
  // Step 1: 收集本轮要处理的事件
  EventsToProcess = ConsumeEvents()

  // Step 2: 遍历所有活动 frame
  for frame in Exec.ActiveFrames:

    // Step 3: 从叶子向根遍历活动状态
    for stateIdx = frame.ActiveStates.Num() - 1; stateIdx >= 0; --stateIdx:
      state = frame.ActiveStates[stateIdx]

      // Step 4: 遍历该状态的所有 Transition（按编辑器顺序）
      for transition in state.Transitions:

        // Step 5: 检查 Trigger 类型
        if !TriggerMatches(transition.Trigger, currentEvent):
          continue

        // Step 6: 收集匹配的事件实例
        matchingEvents = GetMatchingEvents(transition, EventsToProcess)
        if matchingEvents.IsEmpty():
          continue

        // Step 7: 对每个匹配的事件
        for event in matchingEvents:
          // Step 7.1: 检查 Condition
          if !EvaluateConditions(transition.Conditions):
            continue

          // Step 7.2: 检查目标状态能否被进入
          if !CanSelectTarget(transition.Target):
            continue

          // Step 7.3: 命中！
          RequestedTransition = transition
          // 优先级比较：保留最高优先级的请求
          if newPriority > existingPriority:
            存为新的请求

          break  // 同一 transition 只匹配一次

  return RequestedTransition.IsValid()
```

### 关键事实总结

| 维度 | 顺序 / 规则 |
|------|------------|
| 跨状态评估顺序 | **从叶子向根**（子状态优先）|
| 同状态内评估顺序 | **从上到下**（编辑器顺序）|
| 命中后行为 | **第一个命中即采用**（除非有更高优先级请求覆盖）|
| Priority 作用 | 不改变评估顺序，只影响并发请求时的胜出 |

---

## 五、Priority 优先级机制

### 源码定义

```cpp
UENUM(BlueprintType)
enum class EStateTreeTransitionPriority : uint8
{
    None UMETA(Hidden),
    Low,
    Normal,    // 默认
    Medium,
    High,
    Critical,
};
```

### Priority 怎么工作？

**误解**："高优先级的 Transition 先评估" — ❌ 错的！

**正确**：当**多个 Transition 在同一时机都触发**时（比如多个 Task 同时完成），引擎选优先级最高的那个执行。

伪代码：
```cpp
RequestedTransition = nullptr;
RequestedPriority = None;

for (transition in 所有命中的 Transition) {
    if (transition.Priority >= RequestedPriority) {
        RequestedTransition = transition;
        RequestedPriority = transition.Priority;
    }
}
```

### 实战使用

通常你不需要管 Priority。两种情况会用到：

#### 1. 紧急打断

父状态上挂一个 `Critical` 优先级的 Transition：

```
RootState
└── Transition: On Tick + Health <= 0 → DeathState  [Priority: Critical]
```

如果某帧子状态也触发了 Transition，Critical 优先级的死亡 Transition 仍然会赢。

#### 2. 外部 RequestTransition

```cpp
// 外部代码
StateTreeComponent->RequestTransition(
    TargetState,
    EStateTreeTransitionPriority::High);
```

会插入一个 `High` 优先级的请求，覆盖普通的 Tick / Event 触发的 `Normal` 请求。

---

## 六、Transition 的目标类型

源码（`StateTreeTypes.h`）：

```cpp
UENUM()
enum class EStateTreeTransitionType : uint8
{
    None,                      // 不过渡（用于禁用）
    Succeeded,                 // 标记当前 Tree 成功完成
    Failed,                    // 标记当前 Tree 失败完成
    GotoState,                 // 跳到指定状态（最常用）
    NextState,                 // 跳到下一个兄弟状态
    NextSelectableState,       // 跳到下一个可选的兄弟状态
};
```

### `NextState` vs `NextSelectableState`

#### `NextState`

跳到当前状态在父中的"下一个兄弟"，如果下一个进不去（Enter Condition 失败）就失败。

```
Parent
├── Child1 → NextState
├── Child2 (Enter Condition 失败)
└── Child3
```

Child1 完成 → 试图 NextState → 跳到 Child2 → Enter Condition 失败 → 整个失败！

#### `NextSelectableState`

跳到下一个**能进入**的兄弟，跳过失败的。

```
Parent
├── Child1 → NextSelectableState
├── Child2 (Enter Condition 失败) ← 跳过
└── Child3 ← 实际跳到这
```

**绝大多数时候用 `NextSelectableState` 更安全**。

---

## 七、Fallback 行为

源码（`StateTreeExecutionTypes.h` 中的 `EStateTreeSelectionFallback`）：

```cpp
UENUM()
enum class EStateTreeSelectionFallback : uint8
{
    None,                   // 失败就失败
    NextSelectableSibling,  // 失败时尝试下一个兄弟
};
```

当 Transition 的目标状态选择失败时，Fallback 决定怎么处理。

```
TargetState
├── Child1 (Enter Condition 失败)
└── Child2

Transition target = TargetState (with Fallback = NextSelectableSibling)
→ 尝试 Child1，失败 → 尝试 Child2，成功 → 进入 Child2
```

---

## 八、Transition 的 Condition

每个 Transition 可以挂一组 Condition，所有 Condition 通过才算命中。

### Condition 的类型

| 内置 Condition | 用途 |
|----------------|------|
| `Compare` | 比较两个值（数值、字符串、bool 等）|
| `GameplayTag Match` | 检查 GameplayTag |
| `Object Is Valid` | 对象有效性检查 |
| `Distance` | 距离判断 |
| 自定义 C++/BP Condition | 继承 `FStateTreeConditionBase` 或 `UStateTreeConditionBlueprintBase` |

### 示例：自定义 C++ Condition

```cpp
USTRUCT()
struct FSTC_HealthBelow : public FStateTreeConditionCommonBase
{
    GENERATED_BODY()

    using FInstanceDataType = FSTC_HealthBelowInstanceData;

    virtual const UStruct* GetInstanceDataType() const override
    {
        return FInstanceDataType::StaticStruct();
    }

    virtual bool TestCondition(FStateTreeExecutionContext& Context) const override
    {
        const FInstanceDataType& Data = Context.GetInstanceData(*this);
        return Data.CurrentHealth < Data.Threshold;
    }
};

USTRUCT()
struct FSTC_HealthBelowInstanceData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Input")
    float CurrentHealth = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Parameter")
    float Threshold = 30.0f;
};
```

### Condition 表达式（And / Or）

多个 Condition 可以用 And / Or 组合，源码：

```cpp
UENUM()
enum class EStateTreeExpressionOperand : uint8
{
    Copy,       // 第一个
    And,        // 与
    Or,         // 或
    Multiply,   // 乘（用于 Consideration，不用于 Condition）
};
```

编辑器中可以为每个 Condition 选择前置 Operand 和缩进层级，构建嵌套表达式。

---

## 九、Transition 的延迟（Delay）

每个 Transition 可以设置延迟时间：

- `bGateUntilDelayCompletes`：是否等待延迟完成
- `Delay`：延迟秒数
- `DelayVariance`：随机抖动

**用途**：避免立即跳转造成的"闪烁"。

例如战斗结束后想停留 1 秒再切回 Idle：

```
Combat State
└── Transition: On State Completed + Delay 1s → Idle
```

---

## 十、Transition 在编辑器中的常见模式

### 模式 1：自动 NextState 序列

```
Group
├── Step1 → Next
├── Step2 → Next
├── Step3 → Next
└── Step4 → Done
```

### 模式 2：Try Hard, Fallback to Soft

```
TryHard → On Failed → TrySoft → On Failed → Fallback
```

### 模式 3：父级 Tick 监控 + 子级正常完成

```
Combat (父)
├── Transition: On Tick + EnemyDead → Cleanup
├── ApproachEnemy
├── Attack
└── Retreat
```

父级用 On Tick 监控 EnemyDead，子级用 Completed 推进序列。

### 模式 4：Event 驱动的状态机

```
Idle
├── Transition: On Event "PlayerSpotted" → Combat
├── Transition: On Event "ItemFound" → Pickup

Combat
├── Transition: On Event "PlayerLost" → Idle
├── Transition: On State Failed → Flee
```

### 模式 5：紧急打断

```
Root
└── Transition: On Tick + Health<=0 → Death  [Priority: Critical]

Combat
├── Attack
└── Defend
```

无论何时何地，只要血量归零，立即跳到 Death。

---

## 十一、Transition 在源码中的关键数据结构

### `FCompactStateTransition`（编译后形态）

每条 Transition 在 StateTree 编译后变成 `FCompactStateTransition`，包含：

```cpp
struct FCompactStateTransition
{
    EStateTreeTransitionTrigger Trigger;
    FStateTreeStateHandle TargetState;
    EStateTreeTransitionPriority Priority;
    FStateTreeIndex16 ConditionsBegin;
    int32 ConditionsNum;
    FStateTreeEventDesc RequiredEvent;
    FStateTreeDelegateDispatcher RequiredDelegateDispatcher;
    EStateTreeSelectionFallback Fallback;
    float Delay;
    float DelayVariance;
    bool bConsumeEventOnSelect;
    // ...
};
```

### `FStateTreeTransitionResult`（运行时传递）

调用 EnterState/ExitState 时传给 Task 的：

```cpp
struct FStateTreeTransitionResult
{
    FStateTreeStateHandle TargetState;
    EStateTreeRunStatus CurrentRunStatus;
    EStateTreeStateChangeType ChangeType;  // ⭐ Changed / Sustained
    FStateTreeTransitionSource Source;     // 触发源
    // ...
};
```

---

## 十二、Transition 评估的副作用

### 1. Event 消费

`OnEvent` 类 Transition 默认会"消费"事件（处理后从队列移除）。可以通过 `bConsumeEventOnSelect` 控制是否消费。

### 2. 延迟队列

带 Delay 的 Transition 会被放入 `Exec.DelayedTransitions` 队列，每帧检查是否到时间。

### 3. 状态被 Reselect

如果 Transition 命中后选中链与原链有共同祖先，共同祖先以下的状态会被 Exit/Enter，但**带 Sustained ChangeType**（除非 Task 关掉了 Reselect）。

---

## 十三、关键问答

### Q1: 同一帧多个 Transition 都命中，谁赢？

**A**: 评估到的第一个赢（叶子→根、上→下顺序），除非其他 Transition 有更高 Priority。

### Q2: On Tick 和 On State Completed 同时挂在一个状态上，谁优先？

**A**: 它们是不同的事件，不会"同时"评估。On Tick 每帧评估；On Completed 仅在 Task 完成时评估。如果同一帧 Task 完成且 Tick 条件也满足，两者都会被收集，然后按编辑器顺序、优先级决定哪个赢。

### Q3: 如果 Transition 目标的 Enter Condition 失败，会怎样？

**A**: 取决于 Fallback 设置：
- `None`：本次 Transition 失败，继续评估下一条
- `NextSelectableSibling`：尝试目标的下一个兄弟

### Q4: Transition 命中后，源状态的 Tick 还会继续吗？

**A**: 不会。命中后立即开始 ExitState → SelectState → EnterState 流程。本帧的 Tick 已经完成，无法回滚。

### Q5: 我能从 Task 内部"取消" Transition 吗？

**A**: 不能直接取消。但你可以通过 `Context.RequestTransition` 请求另一个目标，配合 Priority 覆盖原来的请求。

### Q6: On Event 和 On Delegate 有什么区别？

**A**:
- **Event**：基于 `FGameplayTag`，跨系统传播好（任何地方都能发送）
- **Delegate**：基于 `FStateTreeDelegateDispatcher`，结构化、类型化，但需要显式绑定

实践中 Event 更常用。

---

## 十四、最佳实践

1. **优先用 `On State Completed` 推进序列**，少用 `On Tick`
2. **`On Tick` 用于打断和监控**，不用于等待完成
3. **Transition 顺序很重要**：具体的（如 OnFailed）放上面，通用的（如 OnCompleted）放下面
4. **NextState vs NextSelectableState**：默认用后者
5. **父级放兜底 Transition**：让所有子状态共享
6. **Critical Priority 用于紧急打断**：死亡、退出等
7. **Delay 避免闪烁**：UI、对话等场景特别有用
8. **不要嵌套太深的 Transition**：超过 4 层就考虑用 Subtree 拆分

---

下一章我们将讲解 **数据流与绑定**。

