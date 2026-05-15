# 04. C++ Task 完整指南

> 基于 UE5.7 源码 `StateTreeTaskBase.h` 和 `StateTreeExecutionContext.cpp`

---

## 一、Task 类的继承体系

```
FStateTreeNodeBase
  └─ FStateTreeTaskBase           ← 真正的 Task 基类，所有虚函数定义在这里
       ├─ FStateTreeTaskCommonBase ← "通用 Task" 标记类
       │    └─ 你的自定义 Task
       │
       ├─ FStateTreeAITaskBase     ← AI 专用 Task
       │    └─ FStateTreeAITaskCommonBase
       │
       ├─ FMassStateTreeTaskBase   ← Mass 专用
       ├─ FGameplayInteractionStateTreeTask  ← 智能对象交互
       └─ FStateTreeBlueprintTaskWrapper      ← 蓝图 Task 的 C++ 包装
```

### 各基类的差异

| 基类 | Schema 兼容 | 适用场景 |
|------|-------------|---------|
| `FStateTreeTaskCommonBase` | 几乎所有 | 通用 Task（推荐起点）|
| `FStateTreeAITaskBase` | 仅 AI 类 Schema | 需要 AIController 上下文 |
| `FMassStateTreeTaskBase` | Mass Schema | Mass 实体处理 |

**记住**：选择基类决定了你的 Task 能在哪些 StateTree 中使用。

---

## 二、最小可工作 Task 模板

### 头文件 `MyTask.h`

```cpp
#pragma once

#include "StateTreeTaskBase.h"
#include "MyTask.generated.h"

/** Task 的实例数据（每个 Task 实例独立的运行时数据） */
USTRUCT()
struct FSTT_MyTaskInstanceData
{
    GENERATED_BODY()

    // 输入：从外部绑定
    UPROPERTY(EditAnywhere, Category = "Parameter")
    float Duration = 1.0f;

    // 输入（可选）：上下文 Actor
    UPROPERTY(EditAnywhere, Category = "Context")
    TObjectPtr<AActor> ContextActor = nullptr;

    // 输出：其他 Task 可以绑定到此
    UPROPERTY(VisibleAnywhere, Category = "Output")
    bool bResult = false;

    // 内部状态：仅运行时使用，不需要序列化
    UPROPERTY()
    float ElapsedTime = 0.0f;
};

/** 你的 Task 本体 */
USTRUCT(meta = (DisplayName = "My Task"))
struct MYGAME_API FSTT_MyTask : public FStateTreeTaskCommonBase
{
    GENERATED_BODY()

    using FInstanceDataType = FSTT_MyTaskInstanceData;

    FSTT_MyTask();

    virtual const UStruct* GetInstanceDataType() const override
    {
        return FInstanceDataType::StaticStruct();
    }

    virtual EStateTreeRunStatus EnterState(
        FStateTreeExecutionContext& Context,
        const FStateTreeTransitionResult& Transition) const override;

    virtual EStateTreeRunStatus Tick(
        FStateTreeExecutionContext& Context,
        const float DeltaTime) const override;

    virtual void ExitState(
        FStateTreeExecutionContext& Context,
        const FStateTreeTransitionResult& Transition) const override;
};
```

### 实现文件 `MyTask.cpp`

```cpp
#include "MyTask.h"
#include "StateTreeExecutionContext.h"

FSTT_MyTask::FSTT_MyTask()
{
    // 默认配置（Task 基类默认值）
    bShouldCallTick = true;                  // 启用 Tick（5.7+ 默认就是 true）
    bShouldStateChangeOnReselect = true;     // 默认 true
    // 其他可选配置：
    // bShouldCallTickOnlyOnEvents = false;
    // bShouldCopyBoundPropertiesOnTick = true;
    // bShouldAffectTransitions = false;
}

EStateTreeRunStatus FSTT_MyTask::EnterState(
    FStateTreeExecutionContext& Context,
    const FStateTreeTransitionResult& Transition) const
{
    // 获取实例数据
    FInstanceDataType& Data = Context.GetInstanceData(*this);
    Data.ElapsedTime = 0.0f;
    Data.bResult = false;

    UE_LOG(LogTemp, Log, TEXT("MyTask Enter, Duration=%f"), Data.Duration);

    return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FSTT_MyTask::Tick(
    FStateTreeExecutionContext& Context,
    const float DeltaTime) const
{
    FInstanceDataType& Data = Context.GetInstanceData(*this);
    Data.ElapsedTime += DeltaTime;

    if (Data.ElapsedTime >= Data.Duration)
    {
        Data.bResult = true;
        return EStateTreeRunStatus::Succeeded;
    }
    return EStateTreeRunStatus::Running;
}

void FSTT_MyTask::ExitState(
    FStateTreeExecutionContext& Context,
    const FStateTreeTransitionResult& Transition) const
{
    UE_LOG(LogTemp, Log, TEXT("MyTask Exit"));
}
```

---

## 三、所有可重写的虚函数

### 1. `EnterState` —— 状态进入时调用

```cpp
virtual EStateTreeRunStatus EnterState(
    FStateTreeExecutionContext& Context,
    const FStateTreeTransitionResult& Transition) const;
```

**调用时机**：状态被选中并激活时
**返回值含义**：
- `Running`：Task 正在执行，进入 Tick 循环
- `Succeeded` / `Failed`：立即结束 Task，触发 Transition 评估
- `Stopped` / `Unset`：异常情况，一般不返回

**Transition 参数包含什么**：
```cpp
struct FStateTreeTransitionResult
{
    FStateTreeStateHandle TargetState;          // 目标状态
    EStateTreeRunStatus CurrentRunStatus;       // 之前的 RunStatus
    EStateTreeStateChangeType ChangeType;       // ⭐ Changed / Sustained
    FStateTreeTransitionSource Source;          // 触发源
    // ...
};
```

**`ChangeType` 是关键判断**：
- `Changed`：状态真的从未激活变激活
- `Sustained`：状态在父→子重选中保持激活（你大概率不想响应这个）

### 2. `Tick` —— 每帧调用

```cpp
virtual EStateTreeRunStatus Tick(
    FStateTreeExecutionContext& Context,
    const float DeltaTime) const;
```

**前提**：`bShouldCallTick = true`（5.7 默认 true） 或 `bShouldCallTickOnlyOnEvents = true` 且有事件

**返回值**：同 EnterState

**性能提示**：如果不需要 Tick（比如纯监听型 Task），关闭 `bShouldCallTick` 可以节省遍历开销。

### 3. `ExitState` —— 状态退出时调用

```cpp
virtual void ExitState(
    FStateTreeExecutionContext& Context,
    const FStateTreeTransitionResult& Transition) const;
```

**调用时机**：状态从激活变非激活，或被切换到新状态
**返回值**：void（你不能在这里改变结果）
**用途**：清理资源、注销监听器、停止动画等

### 4. `StateCompleted` —— 状态完成、新状态选择前

```cpp
virtual void StateCompleted(
    FStateTreeExecutionContext& Context,
    const EStateTreeRunStatus CompletionStatus,
    const FStateTreeActiveStates& CompletedActiveStates) const;
```

**调用时机**：旧状态已完成，但新状态还没选好
**特殊性**：这个函数被**反向调用**（leaf → root），用于让父 Task 也能感知子状态完成
**用途**：记录日志、保存最终状态等"事后"逻辑

### 5. `TriggerTransitions` —— Task 主动请求过渡

```cpp
virtual void TriggerTransitions(FStateTreeExecutionContext& Context) const;
```

**前提**：`bShouldAffectTransitions = true`（默认 false）

**用途**：在 Tick 之外的特殊时机主动请求状态切换，比如：
```cpp
void FMyWatchdogTask::TriggerTransitions(FStateTreeExecutionContext& Context) const
{
    const FInstanceDataType& Data = Context.GetInstanceData(*this);
    if (Data.ShouldEmergencyExit)
    {
        Context.RequestTransition(EmergencyState, EStateTreeTransitionPriority::Critical);
    }
}
```

### 6. `Link` —— 资产编译时调用，绑定外部数据

```cpp
virtual bool Link(FStateTreeLinker& Linker);
```

**用途**：声明这个 Task 需要哪些外部数据（External Data），让框架帮你解析。

```cpp
bool FMyTask::Link(FStateTreeLinker& Linker)
{
    Linker.LinkExternalData(NavSystemHandle);
    return Super::Link(Linker);
}
```

### 7. `GetInstanceDataType` —— 必须重写

```cpp
virtual const UStruct* GetInstanceDataType() const override
{
    return FInstanceDataType::StaticStruct();
}
```

让框架知道你的 Task 需要多大的实例数据。**忘记重写会导致 Task 无实例数据**。

---

## 四、所有标志位详解（基于 5.7 源码）

源码定义（`StateTreeTaskBase.h`）：

```cpp
FStateTreeTaskBase()
    : bShouldStateChangeOnReselect(true)         // ① 默认 true
    , bShouldCallTick(true)                       // ② 默认 true（5.7）
    , bShouldCallTickOnlyOnEvents(false)         // ③ 默认 false
    , bShouldCopyBoundPropertiesOnTick(true)     // ④ 默认 true
    , bShouldCopyBoundPropertiesOnExitState(true) // ⑤ 默认 true
    , bShouldAffectTransitions(false)            // ⑥ 默认 false
    , bConsideredForScheduling(true)             // ⑦ 默认 true
    , bTaskEnabled(true)
#if WITH_EDITORONLY_DATA
    , bConsideredForCompletion(true)             // ⑧ 默认 true（5.6+）
    , bCanEditConsideredForCompletion(true)
#endif
{}
```

| # | 标志位 | 默认 | 作用 |
|---|--------|------|------|
| ① | `bShouldStateChangeOnReselect` | true | 重选时是否重发 Enter/Exit（**关掉可避免反直觉 #4**）|
| ② | `bShouldCallTick` | true | 是否调用 Tick |
| ③ | `bShouldCallTickOnlyOnEvents` | false | 仅事件来时才 Tick（性能优化）|
| ④ | `bShouldCopyBoundPropertiesOnTick` | true | Tick 前复制绑定属性 |
| ⑤ | `bShouldCopyBoundPropertiesOnExitState` | true | ExitState 前复制绑定 |
| ⑥ | `bShouldAffectTransitions` | false | 是否调用 TriggerTransitions |
| ⑦ | `bConsideredForScheduling` | true | 是否参与 ScheduledTick 计算 |
| ⑧ | `bConsideredForCompletion`（5.6+ 编辑器) | true | 是否参与 State 完成判定 |

### 实战标志位组合

#### 标准动作型 Task（默认）

```cpp
FMyActionTask() {
    // 全用默认值
}
```

#### 父状态的资源持有 Task

```cpp
FResourceHolderTask() {
    bShouldStateChangeOnReselect = false;  // 不响应子状态切换
    bShouldCallTick = false;               // 不需要 Tick
}
```

#### 监听型 Task（仅事件触发）

```cpp
FListenerTask() {
    bShouldCallTick = false;
    bShouldCallTickOnlyOnEvents = true;
    bShouldStateChangeOnReselect = false;
}
```

#### 仅观察、不影响完成的 Task

```cpp
FObserverTask() {
    bConsideredForCompletion = false;  // 5.6+
    // 即使返回 Succeeded，也不会触发 State 完成
}
```

#### 主动请求过渡的 Task

```cpp
FWatchdogTask() {
    bShouldAffectTransitions = true;
    // 重写 TriggerTransitions(...)
}
```

---

## 五、生命周期完整时序图

```
                        Task 生命周期
        ═══════════════════════════════════════════
        
        State 被选中
           │
           ▼
        [Phase: EnterStates]
        ┌─────────────────────────────────────┐
        │ 1. 拷贝 Bindings 到 InstanceData     │
        │ 2. 调用 EnterState(Context, T)      │
        │    - 返回 Running → 进入 Tick        │
        │    - 返回 Succeeded/Failed → 立即触发 Transition │
        └─────────────────────────────────────┘
           │
           ▼
        [Phase: TickingTasks] ←───────┐
        ┌─────────────────────────────┐│
        │ 1. if bShouldCopyBoundPropertiesOnTick: │
        │      拷贝 Bindings           ││
        │ 2. 调用 Tick(DeltaTime)      ││
        │    - 返回 Running → 继续      ││
        │    - 返回 Succeeded/Failed → 触发 │
        └─────────────────────────────┘│
           │ Running                   │
           └───────────────────────────┘
           │ 完成 / Transition 命中
           ▼
        [Phase: TriggerTransitions] (可选)
        if bShouldAffectTransitions:
            调用 TriggerTransitions()
           │
           ▼
        [Phase: StateCompleted] (反向)
        如果状态完成，从叶子→根调用 StateCompleted
           │
           ▼
        [Phase: ExitStates]
        ┌─────────────────────────────────────┐
        │ 1. if bShouldCopyBoundPropertiesOnExitState: │
        │      拷贝 Bindings                   │
        │ 2. 调用 ExitState(Context, T)        │
        │    （叶→根的顺序）                    │
        └─────────────────────────────────────┘
           │
           ▼
        Task 销毁
```

---

## 六、ExecutionContext 中常用 API

### 数据访问

```cpp
// 获取实例数据
FInstanceDataType& Data = Context.GetInstanceData(*this);

// 获取 Owner（StateTree 所在的 Object）
UObject* Owner = Context.GetOwner();

// 获取 World
UWorld* World = Context.GetWorld();

// 获取外部数据（需要先在 Link 中声明）
const UNavigationSystemV1& NavSys = Context.GetExternalData(NavSystemHandle);
```

### Task 完成

```cpp
// 方式 1：Tick 中返回
return EStateTreeRunStatus::Succeeded;

// 方式 2：异步回调中调用
Context.FinishTask(*this, EStateTreeFinishTaskType::Succeeded);
```

### 主动请求过渡

```cpp
Context.RequestTransition(
    TargetStateHandle,
    EStateTreeTransitionPriority::High,
    EStateTreeSelectionFallback::None);
```

### 发送事件

```cpp
Context.SendEvent(
    FGameplayTag::RequestGameplayTag(TEXT("Event.SomethingHappened")),
    SomePayload);
```

### 委托广播

```cpp
Context.BroadcastDelegate(MyDispatcher);
```

### 绑定/解绑监听

```cpp
// EnterState 中
Context.BindDelegate(MyListener, FSimpleDelegate::CreateLambda([](){ /* ... */ }));

// ExitState 中（自动会被清理，但显式更清晰）
Context.UnbindDelegate(MyListener);
```

---

## 七、Property 类别说明

Task 的 UPROPERTY 必须用对 Category 来让 StateTree 编辑器识别：

| Category | 含义 |
|----------|------|
| **"Context"** | 上下文数据（如 Actor、AIController），框架自动绑定 |
| **"Input"** | 必须由用户绑定的输入（编译时检查）|
| **"Output"** | 输出，可被其他 Task 绑定 |
| **"Parameter"** | 普通可配置参数（默认） |

```cpp
USTRUCT()
struct FMyTaskData
{
    GENERATED_BODY()

    // 必须绑定，不绑定会编译失败
    UPROPERTY(EditAnywhere, Category = "Input")
    FVector TargetLocation;

    // 自动绑定到 Context Actor
    UPROPERTY(EditAnywhere, Category = "Context")
    TObjectPtr<AActor> ContextActor;

    // 可被下游 Task 绑定
    UPROPERTY(VisibleAnywhere, Category = "Output")
    bool bSucceeded;

    // 普通参数，有默认值，可选绑定
    UPROPERTY(EditAnywhere, Category = "Parameter")
    float Tolerance = 50.0f;
};
```

---

## 八、PropertyRef（可读写引用）

5.4+ 引入。突破 Bindings 按值传递的限制：

```cpp
USTRUCT()
struct FMyTaskData
{
    GENERATED_BODY()

    // 写回类型的 PropertyRef
    UPROPERTY(EditAnywhere, Category = "Output")
    FStateTreePropertyRef<FVector> LocationRef;
};

void FMyTask::Tick(...) const
{
    FInstanceDataType& Data = Context.GetInstanceData(*this);

    // 通过 PropertyRef 写入到外部
    if (FVector* OutLocation = Data.LocationRef.GetMutablePtr(Context))
    {
        *OutLocation = NewLocation;  // 直接修改外部变量！
    }
}
```

蓝图版本是 `FStateTreeBlueprintPropertyRef`，但功能受限（5.5 之前只能读，不能写）。

---

## 九、调试建议

### 1. 使用 Visual Logger

```cpp
#include "VisualLogger/VisualLogger.h"

void FMyTask::Tick(...) const
{
    UE_VLOG(Context.GetOwner(), LogTemp, Verbose, 
        TEXT("MyTask: Time=%f"), Data.ElapsedTime);
}
```

StateTree 编辑器有 Trace Window，可以看到每个状态、每个 Task 的进入退出时刻、返回值，非常强大。

### 2. 使用 GameplayDebugger

```cpp
#if WITH_GAMEPLAY_DEBUGGER
virtual FString GetDebugInfo(const FStateTreeReadOnlyExecutionContext& Context) const override
{
    return FString::Printf(TEXT("Time=%.1f/%.1f"), Data.ElapsedTime, Data.Duration);
}
#endif
```

### 3. 使用 StateTree Debugger（编辑器）

打开 StateTree 资产，启用 Debugger（顶部工具栏）。运行后可以看到：
- 当前激活链
- 每个 Task 的状态
- Transition 的评估历史
- Event 队列

### 4. 开启日志

在控制台输入：

```
log LogStateTree Verbose
```

StateTree 的日志分类叫 `LogStateTree`（定义于 `StateTreeTypes.h`），默认级别是 Warning。改成 Verbose 后所有内部事件都会打印。

---

## 十、最佳实践 Checklist

- [ ] 选对基类（FStateTreeTaskCommonBase 大多数场景够用）
- [ ] 必须重写 `GetInstanceDataType()`
- [ ] InstanceData 用独立的 USTRUCT，所有运行时数据都放里面（不要用成员变量！）
- [ ] EnterState 默认返回 Running，除非真的瞬时完成
- [ ] 父级 / 资源型 Task 关闭 `bShouldStateChangeOnReselect`
- [ ] 不需要 Tick 的关闭 `bShouldCallTick`
- [ ] Property 用对 Category（Input / Output / Context / Parameter）
- [ ] 异步回调用 `Context.FinishTask` 完成
- [ ] 用 Visual Logger 和 StateTree Debugger 调试
- [ ] Schema 选对，决定 Task 在哪些 StateTree 里可用

下一章我们将深入 **Transition 的所有细节**。

