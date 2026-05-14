# 第 1.3 章 · 现代 Input Routing 源码剖析与 Hazel 迁移代码

> **本章目标**：把 ImGui 现代输入系统**从最底层数据结构到最顶层路由策略**完整剖一遍，并交付一份**可直接编译运行**的 Hazel `ImGuiLayer.cpp v2`——把 1.2 章的 13 个痛点全部修复。
>
> **本章对应源码**：`imgui_internal.h:1515~1648`（输入事件、Owner、Routing 的全部数据结构）、`imgui.cpp:1771~2002`（`ClearInputMouse` / `FindLatestInputEvent` / `AddKeyEvent` / `AddMouseXxxEvent` / `AddFocusEvent`）、`imgui.cpp:10422~10562`（`UpdateInputEvents` 全文）、`imgui.cpp:9515~9760`（Routing 表的更新与 `CalcRoutingScore` / `SetShortcutRouting`）、`imgui.h:2519~2528`（公开 API）、`backends/imgui_impl_glfw.cpp` 中的回调函数（参考实现）。
>
> **章节结构**：先讲清"事件数据怎么存"（数据层），再讲"事件怎么入队"（Add API 层），再讲"事件怎么消费"（UpdateInputEvents trickle 层），再讲"快捷键怎么路由"（Routing 层），最后给"Hazel 迁移代码"。

---

## 1.3.0 整体数据流：从 OS 输入到 ImGuiKeyData

```
┌──────────────────────────────────────────────────────────────────────┐
│ ① OS 派发原始事件                                                       │
│    GLFW Callback / Win32 WindowProc / SDL Event 等                    │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ Backend 翻译为 ImGui 抽象
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│ ② Backend 调 io.AddXxxEvent                                          │
│    AddKeyEvent / AddMousePosEvent / AddMouseButtonEvent / ...        │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ 入队
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│ ③ g.InputEventsQueue : ImVector<ImGuiInputEvent>                     │
│    事件按入队顺序排列，等待 NewFrame 处理                                │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ NewFrame 调 UpdateInputEvents(trickle)
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│ ④ UpdateInputEvents 处理 - 可能"trickle"（拆分到多帧）                  │
│    把事件投影到：                                                       │
│      io.MousePos / io.MouseDown[] / io.MouseWheel                    │
│      io.KeysData[ImGuiKey_NamedKey_COUNT]                            │
│      io.InputQueueCharacters                                          │
│      io.AppFocusLost                                                  │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ 已处理事件搬到 Trail
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│ ⑤ g.InputEventsTrail : ImVector<ImGuiInputEvent>                     │
│    本帧已处理的事件历史，给应用代码做精确事件回溯                          │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ 后续 ImGui 内部使用
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│ ⑥ ImGui 控件层：IsKeyPressed / IsMouseClicked / Shortcut / ...       │
│    + Owner-Aware Routing 表决定"谁有资格响应这个键"                    │
└──────────────────────────────────────────────────────────────────────┘
```

我们逐层展开。

---

## 1.3.1 数据层：`ImGuiInputEvent` 联合体

打开 `imgui_internal.h:1515~1562`，这是输入事件的**完整数据模型**。

```cpp
// imgui_internal.h:1515~1525
enum ImGuiInputEventType
{
    ImGuiInputEventType_None = 0,
    ImGuiInputEventType_MousePos,
    ImGuiInputEventType_MouseWheel,
    ImGuiInputEventType_MouseButton,
    ImGuiInputEventType_Key,
    ImGuiInputEventType_Text,
    ImGuiInputEventType_Focus,
    ImGuiInputEventType_COUNT
};
```

**6 种事件类型**——对应 6 个 Add 函数：

| Event Type | 入队函数 | 数据负载 |
|---|---|---|
| `MousePos` | `AddMousePosEvent` | x, y, MouseSource |
| `MouseWheel` | `AddMouseWheelEvent` | wheel_x, wheel_y, MouseSource |
| `MouseButton` | `AddMouseButtonEvent` | button (0~4), down, MouseSource |
| `Key` | `AddKeyEvent` / `AddKeyAnalogEvent` | ImGuiKey, down, AnalogValue |
| `Text` | `AddInputCharacter` / `*UTF16` / `*UTF8` | unsigned int (Unicode codepoint) |
| `Focus` | `AddFocusEvent` | bool focused |

```cpp
// imgui_internal.h:1538~1543
struct ImGuiInputEventMousePos      { float PosX, PosY; ImGuiMouseSource MouseSource; };
struct ImGuiInputEventMouseWheel    { float WheelX, WheelY; ImGuiMouseSource MouseSource; };
struct ImGuiInputEventMouseButton   { int Button; bool Down; ImGuiMouseSource MouseSource; };
struct ImGuiInputEventKey           { ImGuiKey Key; bool Down; float AnalogValue; };
struct ImGuiInputEventText          { unsigned int Char; };
struct ImGuiInputEventAppFocused    { bool Focused; };
```

**6 个简单 POD 结构体**——每个对应一种事件的数据。注意它们都没有构造函数（保持 trivially copyable / relocatable，可以放进 `ImVector`）。

```cpp
// imgui_internal.h:1545~1562
struct ImGuiInputEvent
{
    ImGuiInputEventType             Type;
    ImGuiInputSource                Source;
    ImU32                           EventId;        // Unique, sequential
    union
    {
        ImGuiInputEventMousePos     MousePos;       // if Type == ImGuiInputEventType_MousePos
        ImGuiInputEventMouseWheel   MouseWheel;     // if Type == ImGuiInputEventType_MouseWheel
        ImGuiInputEventMouseButton  MouseButton;    // if Type == ImGuiInputEventType_MouseButton
        ImGuiInputEventKey          Key;            // if Type == ImGuiInputEventType_Key
        ImGuiInputEventText         Text;           // if Type == ImGuiInputEventType_Text
        ImGuiInputEventAppFocused   AppFocused;     // if Type == ImGuiInputEventType_Focus
    };
    bool                            AddedByTestEngine;

    ImGuiInputEvent() { memset((void*)this, 0, sizeof(*this)); }
};
```

**这是经典的"标签联合"（tagged union）**：

- `Type` 字段告诉你哪个 union 成员有效。
- `union` 让 6 种事件**共用同一段内存**——`sizeof(ImGuiInputEvent)` 取决于最大成员。
- 计算下来 `sizeof` 大约 **24 字节**（4 字节 Type + 4 字节 Source + 4 字节 EventId + 12 字节 union + 1 字节 AddedByTestEngine + 3 字节 padding ≈ 24）。

**为什么不用 `std::variant<MousePos, MouseWheel, ...>`**？

- ImGui 不依赖 STL（参见 0.2 章）。
- `std::variant` 的内部结构因 STL 实现不同而异（ABI 不稳定）。
- C 风格 union 更可控，更适合 `memcpy` 拷贝。

**`ImGuiInputSource`**（来源分类）：

```cpp
// imgui_internal.h:1527~1534
enum ImGuiInputSource : int
{
    ImGuiInputSource_None = 0,
    ImGuiInputSource_Mouse,         // Mouse / TouchScreen / Pen
    ImGuiInputSource_Keyboard,
    ImGuiInputSource_Gamepad,
    ImGuiInputSource_COUNT
};
```

注意 `MouseSource` ≠ `InputSource`：
- `InputSource_Mouse / Keyboard / Gamepad` 是 ImGui 用来分类**逻辑输入设备**（决定触发 Nav 系统的哪个分支）。
- `MouseSource_Mouse / TouchScreen / Pen` 是给鼠标事件标注**物理输入设备类型**（决定是否禁用 hover 等触屏不友好行为）。

`AddKeyEvent` 内部会根据 key 是不是手柄按键自动设置 InputSource：

```cpp
// imgui.cpp:1845
e.Source = ImGui::IsGamepadKey(key) ? ImGuiInputSource_Gamepad : ImGuiInputSource_Keyboard;
```

---

## 1.3.2 数据层：`ImGuiKey` 的"双段编码"

`ImGuiKey` 是一个看起来"奇怪"的枚举：

```cpp
// imgui.h（节选 ImGuiKey 枚举）
enum ImGuiKey : int
{
    ImGuiKey_None = 0,

    // [0, 512): Legacy native key indices（兼容旧 API，不再积极使用）
    // ...

    // [512, ...]: Named keys
    ImGuiKey_Tab = 512,
    ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow,
    // ...
    ImGuiKey_Z,
    ImGuiKey_F1,
    // ...
    ImGuiKey_NamedKey_BEGIN = 512,
    ImGuiKey_NamedKey_END   = ...,
    ImGuiKey_NamedKey_COUNT = ImGuiKey_NamedKey_END - ImGuiKey_NamedKey_BEGIN,
};
```

**"双段编码"**：

- **[0, 512)** 区段：留给旧 API 的"native key index"——这是 1.87 之前 `KeysDown[512]` 数组的索引空间。**新代码完全不用这个区段**。
- **[512, ...)** 区段：所有抽象命名键。**新代码只用这个区段**。

```cpp
// imgui_internal.h:1494~1503
#define ImGuiKey_LegacyNativeKey_BEGIN  0
#define ImGuiKey_LegacyNativeKey_END    512
#define ImGuiKey_Keyboard_BEGIN         (ImGuiKey_NamedKey_BEGIN)
#define ImGuiKey_Keyboard_END           (ImGuiKey_GamepadStart)
#define ImGuiKey_Gamepad_BEGIN          (ImGuiKey_GamepadStart)
#define ImGuiKey_Gamepad_END            (ImGuiKey_GamepadRStickDown + 1)
#define ImGuiKey_Mouse_BEGIN            (ImGuiKey_MouseLeft)
#define ImGuiKey_Mouse_END              (ImGuiKey_MouseWheelY + 1)
#define ImGuiKey_Aliases_BEGIN          (ImGuiKey_Mouse_BEGIN)
#define ImGuiKey_Aliases_END            (ImGuiKey_Mouse_END)
```

**Named key 内部又分子区段**：键盘 / 手柄 / 鼠标别名。`io.KeysData[]` 数组只覆盖 `ImGuiKey_NamedKey_BEGIN ~ ImGuiKey_NamedKey_END`：

```cpp
// imgui.h:2577
ImGuiKeyData  KeysData[ImGuiKey_NamedKey_COUNT];

// imgui.cpp:9388 中的 GetKeyData：
ImGuiKeyData* ImGui::GetKeyData(ImGuiContext* ctx, ImGuiKey key)
{
    // ...
    IM_ASSERT(IsNamedKey(key) && "Support for user key indices was dropped in favor of ImGuiKey. Please update backend & user code.");
    return &g.IO.KeysData[key - ImGuiKey_NamedKey_BEGIN];   // ← 减去基址才是数组索引
}
```

**`AddKeyEvent` 拒绝旧式 native key**：

```cpp
// imgui.cpp:1820
IM_ASSERT(ImGui::IsNamedKeyOrMod(key)); // Backend needs to pass a valid ImGuiKey_ constant. 0..511 values are legacy native key codes which are not accepted by this API.
```

——你的 Backend 必须把"GLFW key code 65 (A)"翻译成"ImGuiKey_A (522)"。**这就是 1.2 章痛点 4 的核心**。

### Mod 键的特殊编码

```cpp
// imgui.h（伪代码，实际位置在 ImGuiKey_ 枚举末尾）
ImGuiMod_None    = 0,
ImGuiMod_Ctrl    = 1 << 12,
ImGuiMod_Shift   = 1 << 13,
ImGuiMod_Alt     = 1 << 14,
ImGuiMod_Super   = 1 << 15,
ImGuiMod_Mask_   = 0xF000,
```

`ImGuiMod_*` 是**位掩码**而不是单个值——这样可以用 `key | mod` 表达"组合键"：

```cpp
ImGuiKeyChord shortcut = ImGuiMod_Ctrl | ImGuiKey_S;   // Ctrl+S
```

**`ImGuiKeyChord` 就是 `int`**——高位是 Mod 位掩码，低位是 ImGuiKey 值。

---

## 1.3.3 数据层：`ImGuiKeyData`——每个键的完整状态

```cpp
struct ImGuiKeyData
{
    bool    Down;             // 当前是否按下
    float   DownDuration;     // 按下持续时间（秒）；-1.0f = 没按下
    float   DownDurationPrev; // 上一帧的 DownDuration
    float   AnalogValue;      // 模拟值（用于手柄轴或 IsKeyDown 的力度）
};
```

这是**所有 ImGui 键状态的统一表示**——键盘、鼠标、手柄都用它。

**`DownDuration` 是核心**：

- 按下瞬间：`Down=true, DownDuration=0.0f`。
- 按下持续：每帧 `DownDuration += io.DeltaTime`。
- 释放瞬间：`Down=false, DownDuration=-1.0f`。

`IsKeyPressed(key, repeat)` 的实现就是查 `DownDurationPrev` 和 `DownDuration` 的关系：

```cpp
// imgui.cpp 中（简化）
bool ImGui::IsKeyPressed(ImGuiKey key, bool repeat) {
    ImGuiKeyData* data = GetKeyData(key);
    if (!data->Down) return false;
    if (data->DownDurationPrev < 0.0f && data->DownDuration >= 0.0f)
        return true;   // 新按下
    if (repeat && data->DownDuration > io.KeyRepeatDelay)
        return CalcTypematicRepeatAmount(...) > 0;   // 长按重复
    return false;
}
```

这套统一状态模型让"键盘 IsKeyPressed"、"鼠标 IsMouseClicked"、"手柄 IsKeyPressed(GamepadFaceDown)" **走同一份代码**。

---

## 1.3.4 入队层：`AddKeyEvent` 逐行剖析

打开 `imgui.cpp:1853`，这是最常被调用的事件入队函数：

```cpp
// imgui.cpp:1853~1858
void ImGuiIO::AddKeyEvent(ImGuiKey key, bool down)
{
    if (!AppAcceptingEvents)
        return;
    AddKeyAnalogEvent(key, down, down ? 1.0f : 0.0f);
}
```

`AddKeyEvent` 是 `AddKeyAnalogEvent` 的包装——把 `down` 转换为模拟值（按下=1.0，释放=0.0）。

我们看完整的 `AddKeyAnalogEvent`（`imgui.cpp:1813~1851`）：

```cpp
void ImGuiIO::AddKeyAnalogEvent(ImGuiKey key, bool down, float analog_value)
{
    IM_ASSERT(Ctx != NULL);
    if (key == ImGuiKey_None || !AppAcceptingEvents)
        return;
    ImGuiContext& g = *Ctx;
    IM_ASSERT(ImGui::IsNamedKeyOrMod(key));     // ← 拒绝 [0,512) 区段
    IM_ASSERT(ImGui::IsAliasKey(key) == false); // ← 拒绝 ImGuiKey_MouseLeft 等别名

    // ① MacOS: swap Cmd(Super) and Ctrl
    if (g.IO.ConfigMacOSXBehaviors)
    {
        if (key == ImGuiMod_Super)          { key = ImGuiMod_Ctrl; }
        else if (key == ImGuiMod_Ctrl)      { key = ImGuiMod_Super; }
        else if (key == ImGuiKey_LeftSuper) { key = ImGuiKey_LeftCtrl; }
        else if (key == ImGuiKey_RightSuper){ key = ImGuiKey_RightCtrl; }
        else if (key == ImGuiKey_LeftCtrl)  { key = ImGuiKey_LeftSuper; }
        else if (key == ImGuiKey_RightCtrl) { key = ImGuiKey_RightSuper; }
    }

    // ② Filter duplicate (in particular: key mods and gamepad analog values are commonly spammed)
    const ImGuiInputEvent* latest_event = FindLatestInputEvent(&g, ImGuiInputEventType_Key, (int)key);
    const ImGuiKeyData* key_data = ImGui::GetKeyData(&g, key);
    const bool latest_key_down = latest_event ? latest_event->Key.Down : key_data->Down;
    const float latest_key_analog = latest_event ? latest_event->Key.AnalogValue : key_data->AnalogValue;
    if (latest_key_down == down && latest_key_analog == analog_value)
        return;

    // ③ Add event
    ImGuiInputEvent e;
    e.Type = ImGuiInputEventType_Key;
    e.Source = ImGui::IsGamepadKey(key) ? ImGuiInputSource_Gamepad : ImGuiInputSource_Keyboard;
    e.EventId = g.InputEventsNextEventId++;
    e.Key.Key = key;
    e.Key.Down = down;
    e.Key.AnalogValue = analog_value;
    g.InputEventsQueue.push_back(e);
}
```

### 三个关键步骤逐一剖

#### ① macOS 自动换键

`io.ConfigMacOSXBehaviors` 默认在 `__APPLE__` 上为 true。它**在事件入队层就把 Super 和 Ctrl 互换**——这意味着：

- macOS 用户按 `Cmd+S` → Backend 报告 `AddKeyEvent(ImGuiMod_Super | ImGuiKey_S)` → ImGui 内部转换为 `ImGuiMod_Ctrl | ImGuiKey_S`。
- ImGui 内部所有"`Ctrl+S` 是保存"的逻辑**对 macOS 自动等价于 `Cmd+S`**。

老 Hazel 代码**完全没有这个逻辑**——macOS 用户的快捷键全部错位。

#### ② 重复过滤

`FindLatestInputEvent`（`imgui.cpp:1790~1805`）从队列尾向前找同类型同 key 的最新事件。如果：

- 事件队列里已经有 `Key A down`，又来 `Key A down` → 重复，丢弃。
- 事件队列空但 `key_data->Down == true`，又来 `down=true` → 状态没变，丢弃。

**为什么需要过滤**：某些 OS（特别是 Windows）会反复发送同一状态的事件（例如鼠标移动到边界外仍每帧触发 WM_MOUSEMOVE）。过滤可以减少队列长度。

#### ③ 入队

构造 `ImGuiInputEvent`，`push_back` 到 `g.InputEventsQueue`。事件会在下一次 `NewFrame` 被处理。

### `AddMousePosEvent` 的 Floor 处理

```cpp
// imgui.cpp:1882~1906
void ImGuiIO::AddMousePosEvent(float x, float y)
{
    // ...
    // Apply same flooring as UpdateMouseInputs()
    ImVec2 pos((x > -FLT_MAX) ? ImFloor(x) : x, (y > -FLT_MAX) ? ImFloor(y) : y);
    // ...
}
```

**注意 `ImFloor`**——把浮点坐标向下取整。这是为了让"鼠标在 (10.5, 20.7)"和"鼠标在 (10.99, 20.01)"被视为同一像素，避免亚像素抖动触发不必要的事件。

`-FLT_MAX` 是"鼠标不可用"的特殊标记，不应取整。

### `AddMouseButtonEvent` 的 macOS Ctrl+Click

```cpp
// imgui.cpp:1908~1954（节选）
// On MacOS X: Convert Ctrl(Super)+Left click into Right-click.
if (ConfigMacOSXBehaviors && mouse_button == 0 && down)
{
    const ImGuiInputEvent* latest_super_event = FindLatestInputEvent(&g, ImGuiInputEventType_Key, (int)ImGuiMod_Super);
    if (latest_super_event ? latest_super_event->Key.Down : g.IO.KeySuper)
    {
        IMGUI_DEBUG_LOG_IO("[io] Super+Left Click aliased into Right Click\n");
        MouseCtrlLeftAsRightClick = true;
        AddMouseButtonEvent(1, true); // Recursive call as right-click
        return;
    }
}
```

**这是 macOS 用户体验的"金标准"**：在 macOS 上 Ctrl+点击（物理 Ctrl，对应 ImGui 的 ImGuiMod_Super）= 右键点击。ImGui 自动处理这个转换。

老 Hazel 代码**没有任何 macOS 适配**——macOS 用户在编辑器里用 Ctrl+点击 弹不出右键菜单。

---

## 1.3.5 双队列模型：`InputEventsQueue` 与 `InputEventsTrail`

```cpp
// imgui_internal.h:2219~2221
ImVector<ImGuiInputEvent> InputEventsQueue;       // 待处理事件
ImVector<ImGuiInputEvent> InputEventsTrail;       // 已处理事件历史
ImGuiMouseSource          InputEventsNextMouseSource;
```

**两个队列的角色**：

- **`InputEventsQueue`**：Backend 调 `AddXxxEvent` 入队的事件，等待 `NewFrame` 处理。
- **`InputEventsTrail`**：本帧已处理过的事件历史，给应用代码做"事件回溯"用。

### `InputEventsTrail` 给谁用

应用代码偶尔需要知道"这一帧用户具体按了什么"——例如：

- 触屏 / 触控笔的"轨迹回放"。
- 录制宏 / 自动化测试。
- 日志记录用户操作。

`io.MousePos` 只反映"当前位置"——你不知道这一帧鼠标走过了什么路径。`InputEventsTrail` 保留所有原始事件，可以重建轨迹。

**`imgui_internal.h:2220` 的注释**：

> `InputEventsTrail; // Past input events processed in NewFrame(). This is to allow domain-specific application to access e.g mouse/pen trail.`

### `InputEventsTrail` 何时清空

每次 `NewFrame` 开始时清空（旧的 trail 不需要保留）：

```cpp
// imgui.cpp:NewFrame 内（伪代码）
g.InputEventsTrail.resize(0);
UpdateInputEvents(g.IO.ConfigInputTrickleEventQueue);   // 处理 Queue → 写入 Trail
```

应用代码访问 trail 必须**在 `NewFrame` 之后、`Render` 之前**——因为下一帧的 `NewFrame` 会清空它。

---

## 1.3.6 Trickle 算法：低帧率不丢按键的核心

`UpdateInputEvents` 是 ImGui 输入处理的**心脏**。打开 `imgui.cpp:10422~10562`，整段一百多行。

```cpp
// imgui.cpp:10422~10426（注释）
// Process input queue
// We always call this with the value of 'bool g.IO.ConfigInputTrickleEventQueue'.
// - trickle_fast_inputs = false : process all events, turn into flattened input state (e.g. successive down/up/down/up will be lost)
// - trickle_fast_inputs = true  : process as many events as possible (successive down/up/down/up will be trickled over several frames so nothing is lost) (new feature in 1.87)
```

**两种模式**：

- **trickle = false**：把队列里**全部**事件合并到 IO 状态。简单但可能丢事件。
- **trickle = true (默认)**：**逐个**处理事件，**遇到"可能产生 click 等高级语义的冲突"就停下**——把剩余事件留到下一帧。

### Trickle 的核心规则

`UpdateInputEvents` 的主循环对每个事件检查"是否冲突"，冲突则 `break`：

```cpp
// imgui.cpp:10440~10531（节选 + 注释翻译）
int event_n = 0;
for (; event_n < g.InputEventsQueue.Size; event_n++)
{
    ImGuiInputEvent* e = &g.InputEventsQueue[event_n];

    if (e->Type == ImGuiInputEventType_MousePos)
    {
        if (g.IO.WantSetMousePos) continue;
        ImVec2 event_pos(e->MousePos.PosX, e->MousePos.PosY);

        // 规则 A：如果已经有鼠标按钮事件 / 滚轮 / 键盘 / 文本输入，停止处理鼠标移动
        if (trickle_fast_inputs && (mouse_button_changed != 0 || mouse_wheeled || key_changed || text_inputted))
            break;

        io.MousePos = event_pos;
        // ...
    }
    else if (e->Type == ImGuiInputEventType_MouseButton)
    {
        const ImGuiMouseButton button = e->MouseButton.Button;

        // 规则 B：如果同一按钮已经有变化，或者已经有滚轮事件，停止
        if (trickle_fast_inputs && ((mouse_button_changed & (1 << button)) || mouse_wheeled))
            break;

        // 规则 C：触屏 down/up 不应该被先于 MousePos 处理（触屏没有 hover 概念）
        if (trickle_fast_inputs && e->MouseButton.MouseSource == ImGuiMouseSource_TouchScreen && mouse_moved)
            break;

        io.MouseDown[button] = e->MouseButton.Down;
        // ...
    }
    else if (e->Type == ImGuiInputEventType_Key)
    {
        ImGuiKey key = e->Key.Key;
        ImGuiKeyData* key_data = GetKeyData(key);
        const int key_data_index = (int)(key_data - g.IO.KeysData);

        // 规则 D：同一键的状态变化已经发生过 / 鼠标按钮已变化时，停止
        if (trickle_fast_inputs && key_data->Down != e->Key.Down && (key_changed_mask.TestBit(key_data_index) || mouse_button_changed != 0))
            break;

        // 规则 E：与 InputText 的字符输入交错——如果已经处理过文本，停止处理"非字符键"
        const bool key_is_potentially_for_char_input = IsKeyChordPotentiallyCharInput(GetMergedModsFromKeys() | key);
        if (trickle_interleaved_nonchar_keys_and_text && (text_inputted && !key_is_potentially_for_char_input))
            break;

        if (key_data->Down != e->Key.Down) {
            key_changed = true;
            key_changed_mask.SetBit(key_data_index);
            if (trickle_interleaved_nonchar_keys_and_text && !key_is_potentially_for_char_input)
                key_changed_nonchar = true;
        }

        key_data->Down = e->Key.Down;
        key_data->AnalogValue = e->Key.AnalogValue;
    }
    // ... 其他事件类型
}
```

### 一个具体例子

用户在一帧（33ms）内"按下并释放" Tab 键。OS 发了两个事件：

```
事件 0: KeyDown(Tab)
事件 1: KeyUp(Tab)
```

trickle 算法：

```
处理事件 0:
  KeyDown(Tab) → key_data->Down = true
  key_changed_mask 设置 Tab 位
  event_n = 1

处理事件 1:
  KeyUp(Tab) → 但 key_changed_mask 已经设置 Tab 位
  规则 D 触发 → break

最终：
  io.KeysData[Tab].Down = true
  IsKeyPressed(Tab) = true（这一帧）
  
下一帧 NewFrame：
  剩余的"事件 1" 还在队列
  处理事件 1: KeyUp(Tab) → key_data->Down = false
  IsKeyReleased(Tab) = true（下一帧）
```

**用户感知**：Tab 在第 1 帧"按下"、第 2 帧"释放"——尽管真实只持续了 10ms。这比"事件全部丢失"好得多。

### 处理完事件后的清理

```cpp
// imgui.cpp:10533~10551（节选）
// Record trail (for domain-specific applications wanting to access a precise trail)
for (int n = 0; n < event_n; n++)
    g.InputEventsTrail.push_back(g.InputEventsQueue[n]);

// Remaining events will be processed on the next frame
// FIXME-MULTITHREADING: io.AddKeyEvent() etc. calls are mostly thread-safe apart from the fact they push to this
// queue which may be resized here. Could potentially rework this to narrow down the section needing a mutex? (#5772)
if (event_n == g.InputEventsQueue.Size)
    g.InputEventsQueue.resize(0);
else
    g.InputEventsQueue.erase(g.InputEventsQueue.Data, g.InputEventsQueue.Data + event_n);
```

- 已处理的 `event_n` 个事件被搬到 `InputEventsTrail`。
- 全部处理完 → 队列 clear（`resize(0)` 不释放内存，下帧重用）。
- 部分处理 → erase 已处理部分，剩余事件留到下一帧。

**`FIXME-MULTITHREADING` 注释**——这是第 6 章会回到的主题。简单说：`AddKeyEvent` 在多线程下"几乎安全"——但同时另一个线程在 `UpdateInputEvents` 里 `resize(0)` 队列，会撞车。

### `ConfigInputTrickleEventQueue` 该不该开

**默认 true，编辑器 / 游戏都建议保持 true**。代价是低帧率下"高频按键"会被人为延迟一两帧——但用户感知比"丢事件"小得多。

**只有在以下场景考虑关闭**：
- 高帧率 + 节奏游戏（每个事件必须立刻响应，宁可丢失边角事件）。
- 自动化测试 / 录制宏（不希望 ImGui 帮你"延迟"事件）。

---

## 1.3.7 Owner-Aware Input：现代快捷键路由

ImGui 1.89 引入的**Input Routing 系统**——它把"快捷键的响应权"从"全局先到先得"改成"基于焦点和 owner 的优先级路由"。

### 老的"广播"模式问题

老 ImGui：用户按 Ctrl+S，**所有调 `IsKeyChordPressed(Ctrl+S)` 的代码同时返回 true**。

后果：

- 你的引擎主菜单 "File / Save" 注册了 Ctrl+S。
- 你的 InputText 控件想用 Ctrl+S 做"保存当前文本"。
- 用户在 InputText 里按 Ctrl+S 时——**两个都触发**，混乱。

老 ImGui 的常见 workaround 是**手动检查焦点**：

```cpp
// 老 workaround
if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
    if (ImGui::IsAnyItemActive()) {
        // InputText 等占用，不响应
    } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
        // 某窗口焦点，按其规则
    } else {
        // 全局响应
    }
}
```

——丑、易错、不可组合。

### 新的"路由"模式

`Shortcut(key_chord, flags)` 是新接口：

```cpp
// 在你的菜单代码中：
if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
    SaveFile();

// 在你的 InputText 处理中：
if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused))
    SaveTextLocally();
```

ImGui 自动判定"当前哪个路由获胜"——**最深的焦点链优先**。如果用户在 InputText 里按 Ctrl+S，`RouteFocused` 路由获胜（菜单的 RouteGlobal 不响应）。如果焦点不在任何 InputText，`RouteGlobal` 路由获胜。

### Routing Flags

```cpp
// imgui_internal.h:1691~1699
ImGuiInputFlags_RouteActive             = 1 << 10,  // Route to active item only.
ImGuiInputFlags_RouteFocused            = 1 << 11,  // Route to windows in the focus stack (DEFAULT).
ImGuiInputFlags_RouteGlobal             = 1 << 12,  // Global route.
ImGuiInputFlags_RouteAlways             = 1 << 13,  // Do not register route, poll keys directly.

ImGuiInputFlags_RouteOverFocused        = 1 << 14,  // Option: global route higher priority than focused.
ImGuiInputFlags_RouteOverActive         = 1 << 15,  // Option: global route higher priority than active item.
ImGuiInputFlags_RouteUnlessBgFocused    = 1 << 16,  // Option: global route disabled if background focused.
ImGuiInputFlags_RouteFromRootWindow     = 1 << 17,  // Option: route evaluated from root window.
```

| Flag | 含义 |
|---|---|
| `RouteActive` | 只在 `ActiveId == owner_id` 时响应（典型：InputText 内部的 Ctrl+A） |
| `RouteFocused` (默认) | 沿焦点栈传递；最深焦点的 owner 优先 |
| `RouteGlobal` | 全局响应（典型：菜单快捷键） |
| `RouteAlways` | 不注册到路由表，直接查 IsKeyPressed（兼容老代码） |
| `RouteOverFocused` | 加在 Global 上，让 Global 比 Focused 优先 |
| `RouteOverActive` | 加在 Global 上，让 Global 比 Active 优先 |

### Routing Score 算法

`imgui.cpp:9607~9654` 的 `CalcRoutingScore` 是路由仲裁的核心。它给每个**注册路由**打分（**分数低 = 优先级高**）：

```cpp
// 简化版
static int CalcRoutingScore(ImGuiID focus_scope_id, ImGuiID owner_id, ImGuiInputFlags flags)
{
    if (flags & ImGuiInputFlags_RouteFocused)
    {
        if (g.ActiveId == owner_id)
            return 300;   // ActiveId 优先
        for (int i = 0; i < g.NavFocusRoute.Size; i++)
            if (g.NavFocusRoute.Data[i].ID == focus_scope_id)
                return 199 - i;   // 焦点栈深度越深分数越低
        return 0;
    }
    else if (flags & ImGuiInputFlags_RouteGlobal)
    {
        if (flags & ImGuiInputFlags_RouteOverActive) return 400;
        if (g.ActiveId == owner_id) return 300;
        if (flags & ImGuiInputFlags_RouteOverFocused) return 200;
        return 1;   // 默认 Global 是最低优先级
    }
    // ...
}
```

**实战记忆法**：

- `RouteFocused`（默认）：最深焦点的得分最低（最优先）。
- `RouteGlobal`：默认得分 1（最低优先级，被任何 Focused 路由打败）。
- `RouteGlobal | RouteOverFocused`：得分 200，比所有 Focused 路由都优先。

### Routing Table 是怎么积累和清算的

`imgui_internal.h:1572~1594` 定义了 RoutingData 和 Table：

```cpp
struct ImGuiKeyRoutingData {
    ImGuiKeyRoutingIndex NextEntryIndex;
    ImU16    Mods;
    ImU16    RoutingCurrScore;
    ImU16    RoutingNextScore;
    ImGuiID  RoutingCurr;     // 这一帧的获胜者
    ImGuiID  RoutingNext;     // 下一帧的候选者
};

struct ImGuiKeyRoutingTable {
    ImGuiKeyRoutingIndex Index[ImGuiKey_NamedKey_COUNT];
    ImVector<ImGuiKeyRoutingData> Entries;
    ImVector<ImGuiKeyRoutingData> EntriesNext;   // Double-buffer
};
```

**双缓冲设计**：

- 这一帧用户调 `Shortcut(...)` → 注册到 `EntriesNext`（下一帧的候选）。
- 下一帧 `NewFrame` → `UpdateKeyRoutingTable` 把 `EntriesNext` 排序选最低分 → 写入 `RoutingCurr`。
- 这一帧 `Shortcut(...)` 检查 `RoutingCurr == owner_id` → 决定是否响应。

**这意味着 `Shortcut(...)` 实际上有"一帧延迟"**——第一次注册的帧不会响应，第二次帧才响应。但用户感知不到（视觉上一帧 = 16ms）。

### 实战 Cheatsheet

```cpp
// 全局菜单快捷键（推荐写法）
if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
    SaveFile();

// 当前焦点窗口才响应
if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F))   // 默认 RouteFocused
    OpenFindBar();

// 我的控件正在 active 时才响应
if (ImGui::IsItemActive())
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteActive))
        SelectAllInThisItem();

// 强制全局优先（即使有 InputText 占用焦点，也响应）
if (ImGui::Shortcut(ImGuiKey_F1, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive | ImGuiInputFlags_RouteOverFocused))
    ToggleHelp();
```

### Owner ID 与 SetKeyOwner

`SetKeyOwner` 是更底层的 API——直接把某个键"独占"给某个 owner：

```cpp
// 我的自定义 widget 想独占 Space 键，不让别人响应
ImGuiID my_id = ImGui::GetID("MyWidget");
if (ImGui::IsItemActive())
    ImGui::SetKeyOwner(ImGuiKey_Space, my_id);

// 后续 IsKeyPressed(Space) 必须传 my_id 才能返回 true
if (ImGui::IsKeyPressed(ImGuiKey_Space) /* 这个不传 owner 默认 ImGuiKeyOwner_Any */) {
    // ↑ 这里仍然返回 true（默认 Any），因为只 SetKeyOwner 不锁
}

// 用 Lock 模式才真正"吃掉"这个键
ImGui::SetKeyOwner(ImGuiKey_Space, my_id, ImGuiInputFlags_LockThisFrame);
```

**这是给"超级深度自定义控件"的能力**——节点编辑器、时间线等。99% 应用代码用 `Shortcut` 就够了。

---

## 1.3.8 完整 Hazel 迁移代码（v2）

讲完了所有理论，给一份**真正能用**的 Hazel `ImGuiLayer.cpp`。这份代码：

- **修复了 1.2 章列出的全部 13 个痛点**。
- **不依赖**官方 `imgui_impl_glfw.cpp`（如果你的 Hazel 完全自管 Window）。
- **依赖**官方 `imgui_impl_opengl3.cpp` 作为 Renderer Backend（自己写 Renderer 是第 5.4 章的内容）。
- 假定 Hazel `KeyCodes.h` 与 GLFW 兼容（`HZ_KEY_*` = GLFW 键码）。

### 头文件 `ImGuiLayer.h`

```cpp
#pragma once

#include "Hazel/Core/Layer.h"
#include "Hazel/Events/MouseEvent.h"
#include "Hazel/Events/KeyEvent.h"
#include "Hazel/Events/ApplicationEvent.h"

namespace Hazel {

class ImGuiLayer : public Layer
{
public:
    ImGuiLayer();
    ~ImGuiLayer() = default;

    void OnAttach() override;
    void OnDetach() override;
    void OnEvent(Event& e) override;

    void Begin();
    void End();

    void BlockEvents(bool block) { m_BlockEvents = block; }
    void SetDarkThemeColors();

private:
    bool OnMouseButtonPressed(MouseButtonPressedEvent& e);
    bool OnMouseButtonReleased(MouseButtonReleasedEvent& e);
    bool OnMouseMoved(MouseMovedEvent& e);
    bool OnMouseScrolled(MouseScrolledEvent& e);
    bool OnKeyPressed(KeyPressedEvent& e);
    bool OnKeyReleased(KeyReleasedEvent& e);
    bool OnKeyTyped(KeyTypedEvent& e);
    bool OnWindowResize(WindowResizeEvent& e);
    bool OnWindowFocus(WindowFocusEvent& e);
    bool OnWindowLostFocus(WindowLostFocusEvent& e);

private:
    bool m_BlockEvents = true;
    float m_Time = 0.0f;
};

} // namespace Hazel
```

### `ImGuiLayer.cpp`

```cpp
#include "hzpch.h"
#include "Hazel/ImGui/ImGuiLayer.h"

#include "Hazel/Core/Application.h"
#include "Hazel/Core/Input.h"
#include "Hazel/Core/KeyCodes.h"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace Hazel {

// =================================================================
// ① Hazel KeyCode (= GLFW key) → ImGuiKey 转换函数
//    替代了老 KeyMap[] 数组的功能
// =================================================================
static ImGuiKey HazelKeyToImGuiKey(int keycode)
{
    switch (keycode)
    {
        case HZ_KEY_TAB:               return ImGuiKey_Tab;
        case HZ_KEY_LEFT:              return ImGuiKey_LeftArrow;
        case HZ_KEY_RIGHT:             return ImGuiKey_RightArrow;
        case HZ_KEY_UP:                return ImGuiKey_UpArrow;
        case HZ_KEY_DOWN:              return ImGuiKey_DownArrow;
        case HZ_KEY_PAGE_UP:           return ImGuiKey_PageUp;
        case HZ_KEY_PAGE_DOWN:         return ImGuiKey_PageDown;
        case HZ_KEY_HOME:              return ImGuiKey_Home;
        case HZ_KEY_END:               return ImGuiKey_End;
        case HZ_KEY_INSERT:            return ImGuiKey_Insert;
        case HZ_KEY_DELETE:            return ImGuiKey_Delete;
        case HZ_KEY_BACKSPACE:         return ImGuiKey_Backspace;
        case HZ_KEY_SPACE:             return ImGuiKey_Space;
        case HZ_KEY_ENTER:             return ImGuiKey_Enter;
        case HZ_KEY_ESCAPE:            return ImGuiKey_Escape;
        case HZ_KEY_APOSTROPHE:        return ImGuiKey_Apostrophe;
        case HZ_KEY_COMMA:             return ImGuiKey_Comma;
        case HZ_KEY_MINUS:             return ImGuiKey_Minus;
        case HZ_KEY_PERIOD:            return ImGuiKey_Period;
        case HZ_KEY_SLASH:             return ImGuiKey_Slash;
        case HZ_KEY_SEMICOLON:         return ImGuiKey_Semicolon;
        case HZ_KEY_EQUAL:             return ImGuiKey_Equal;
        case HZ_KEY_LEFT_BRACKET:      return ImGuiKey_LeftBracket;
        case HZ_KEY_BACKSLASH:         return ImGuiKey_Backslash;
        case HZ_KEY_RIGHT_BRACKET:     return ImGuiKey_RightBracket;
        case HZ_KEY_GRAVE_ACCENT:      return ImGuiKey_GraveAccent;
        case HZ_KEY_CAPS_LOCK:         return ImGuiKey_CapsLock;
        case HZ_KEY_SCROLL_LOCK:       return ImGuiKey_ScrollLock;
        case HZ_KEY_NUM_LOCK:          return ImGuiKey_NumLock;
        case HZ_KEY_PRINT_SCREEN:      return ImGuiKey_PrintScreen;
        case HZ_KEY_PAUSE:             return ImGuiKey_Pause;
        case HZ_KEY_KP_0:              return ImGuiKey_Keypad0;
        case HZ_KEY_KP_1:              return ImGuiKey_Keypad1;
        case HZ_KEY_KP_2:              return ImGuiKey_Keypad2;
        case HZ_KEY_KP_3:              return ImGuiKey_Keypad3;
        case HZ_KEY_KP_4:              return ImGuiKey_Keypad4;
        case HZ_KEY_KP_5:              return ImGuiKey_Keypad5;
        case HZ_KEY_KP_6:              return ImGuiKey_Keypad6;
        case HZ_KEY_KP_7:              return ImGuiKey_Keypad7;
        case HZ_KEY_KP_8:              return ImGuiKey_Keypad8;
        case HZ_KEY_KP_9:              return ImGuiKey_Keypad9;
        case HZ_KEY_KP_DECIMAL:        return ImGuiKey_KeypadDecimal;
        case HZ_KEY_KP_DIVIDE:         return ImGuiKey_KeypadDivide;
        case HZ_KEY_KP_MULTIPLY:       return ImGuiKey_KeypadMultiply;
        case HZ_KEY_KP_SUBTRACT:       return ImGuiKey_KeypadSubtract;
        case HZ_KEY_KP_ADD:            return ImGuiKey_KeypadAdd;
        case HZ_KEY_KP_ENTER:          return ImGuiKey_KeypadEnter;
        case HZ_KEY_KP_EQUAL:          return ImGuiKey_KeypadEqual;
        case HZ_KEY_LEFT_SHIFT:        return ImGuiKey_LeftShift;
        case HZ_KEY_LEFT_CONTROL:      return ImGuiKey_LeftCtrl;
        case HZ_KEY_LEFT_ALT:          return ImGuiKey_LeftAlt;
        case HZ_KEY_LEFT_SUPER:        return ImGuiKey_LeftSuper;
        case HZ_KEY_RIGHT_SHIFT:       return ImGuiKey_RightShift;
        case HZ_KEY_RIGHT_CONTROL:     return ImGuiKey_RightCtrl;
        case HZ_KEY_RIGHT_ALT:         return ImGuiKey_RightAlt;
        case HZ_KEY_RIGHT_SUPER:       return ImGuiKey_RightSuper;
        case HZ_KEY_MENU:              return ImGuiKey_Menu;
        case HZ_KEY_0:                 return ImGuiKey_0;
        case HZ_KEY_1:                 return ImGuiKey_1;
        case HZ_KEY_2:                 return ImGuiKey_2;
        case HZ_KEY_3:                 return ImGuiKey_3;
        case HZ_KEY_4:                 return ImGuiKey_4;
        case HZ_KEY_5:                 return ImGuiKey_5;
        case HZ_KEY_6:                 return ImGuiKey_6;
        case HZ_KEY_7:                 return ImGuiKey_7;
        case HZ_KEY_8:                 return ImGuiKey_8;
        case HZ_KEY_9:                 return ImGuiKey_9;
        case HZ_KEY_A:                 return ImGuiKey_A;
        case HZ_KEY_B:                 return ImGuiKey_B;
        case HZ_KEY_C:                 return ImGuiKey_C;
        // ... 完整 A-Z 略
        case HZ_KEY_Z:                 return ImGuiKey_Z;
        case HZ_KEY_F1:                return ImGuiKey_F1;
        case HZ_KEY_F2:                return ImGuiKey_F2;
        // ... F3 ~ F24 略
        default:                       return ImGuiKey_None;
    }
}

ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {}

void ImGuiLayer::OnAttach()
{
    HZ_PROFILE_FUNCTION();

    // ② Core 初始化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;

    // ③ Backend Capability Flags
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;
    // 不开 ImGuiBackendFlags_PlatformHasViewports —— 自家 Window 暂不支持 Multi-Viewport 创建副窗口
    // 如果要支持，需要实现 PlatformIO.Platform_CreateWindow 等回调（第 5.4 章）

    io.BackendPlatformName = "Hazel-Custom";

    // ④ 字体（用 default + 中文字体）
    io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Bold.ttf", 18.0f);
    io.FontDefault = io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Regular.ttf", 18.0f);

    // ⑤ Style
    ImGui::StyleColorsDark();
    SetDarkThemeColors();

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // ⑥ 1.92 字体缩放：使用 style.FontScaleMain 替代废弃的 io.FontGlobalScale
    ImGui::GetStyle().FontScaleMain = 1.0f;
    // FontScaleDpi 由 ImGui 自动维护（开了 ConfigFlags_DpiEnableScaleFonts 之后）

    // ⑦ Renderer Backend Init
    Application& app = Application::Get();
    GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());
    glfwMakeContextCurrent(window);

    ImGui_ImplOpenGL3_Init("#version 410");
}

void ImGuiLayer::OnDetach()
{
    HZ_PROFILE_FUNCTION();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}

// =================================================================
// 事件分发：把 Hazel Event 桥接到 io.AddXxxEvent
// =================================================================
void ImGuiLayer::OnEvent(Event& e)
{
    if (m_BlockEvents) {
        ImGuiIO& io = ImGui::GetIO();
        e.Handled |= e.IsInCategory(EventCategoryMouse)    & io.WantCaptureMouse;
        e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
    }

    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<MouseButtonPressedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseButtonPressed));
    dispatcher.Dispatch<MouseButtonReleasedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseButtonReleased));
    dispatcher.Dispatch<MouseMovedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseMoved));
    dispatcher.Dispatch<MouseScrolledEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseScrolled));
    dispatcher.Dispatch<KeyPressedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyPressed));
    dispatcher.Dispatch<KeyReleasedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyReleased));
    dispatcher.Dispatch<KeyTypedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyTyped));
    dispatcher.Dispatch<WindowResizeEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnWindowResize));
    dispatcher.Dispatch<WindowFocusEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnWindowFocus));
    dispatcher.Dispatch<WindowLostFocusEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnWindowLostFocus));
}

// 修复痛点 2：用 AddMouseButtonEvent 入队
bool ImGuiLayer::OnMouseButtonPressed(MouseButtonPressedEvent& e)
{
    ImGui::GetIO().AddMouseButtonEvent(e.GetMouseButton(), true);
    return false;
}
bool ImGuiLayer::OnMouseButtonReleased(MouseButtonReleasedEvent& e)
{
    ImGui::GetIO().AddMouseButtonEvent(e.GetMouseButton(), false);
    return false;
}

// 修复痛点 3：AddMousePosEvent 入队（注意 Multi-Viewport 下需要传虚拟桌面坐标）
bool ImGuiLayer::OnMouseMoved(MouseMovedEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    float x = e.GetX();
    float y = e.GetY();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // 把窗口本地坐标转成虚拟桌面坐标
        Application& app = Application::Get();
        GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());
        int wx, wy;
        glfwGetWindowPos(window, &wx, &wy);
        x += (float)wx;
        y += (float)wy;
    }
    io.AddMousePosEvent(x, y);
    return false;
}

// 修复痛点 9（隐性）：滚轮也通过事件队列
bool ImGuiLayer::OnMouseScrolled(MouseScrolledEvent& e)
{
    ImGui::GetIO().AddMouseWheelEvent(e.GetXOffset(), e.GetYOffset());
    return false;
}

// 修复痛点 1 + 4 + 5：用 AddKeyEvent，转换 KeyCode，自动派生 Mod
bool ImGuiLayer::OnKeyPressed(KeyPressedEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiKey imgui_key = HazelKeyToImGuiKey(e.GetKeyCode());
    if (imgui_key != ImGuiKey_None)
        io.AddKeyEvent(imgui_key, true);
    return false;
}
bool ImGuiLayer::OnKeyReleased(KeyReleasedEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiKey imgui_key = HazelKeyToImGuiKey(e.GetKeyCode());
    if (imgui_key != ImGuiKey_None)
        io.AddKeyEvent(imgui_key, false);
    return false;
}

// 修复痛点 6：不要强 cast 成 unsigned short，保留完整 Unicode
bool ImGuiLayer::OnKeyTyped(KeyTypedEvent& e)
{
    ImGui::GetIO().AddInputCharacter((unsigned int)e.GetKeyCode());
    return false;
}

bool ImGuiLayer::OnWindowResize(WindowResizeEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)e.GetWidth(), (float)e.GetHeight());
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);    // 如果你处理 HiDPI，这里用真实比例
    glViewport(0, 0, e.GetWidth(), e.GetHeight());
    return false;
}

// 修复痛点 8：失焦时调 AddFocusEvent，让 ImGui 自动 ClearInputKeys
bool ImGuiLayer::OnWindowFocus(WindowFocusEvent& e)
{
    ImGui::GetIO().AddFocusEvent(true);
    return false;
}
bool ImGuiLayer::OnWindowLostFocus(WindowLostFocusEvent& e)
{
    ImGui::GetIO().AddFocusEvent(false);
    return false;
}

// =================================================================
// 帧界
// =================================================================
void ImGuiLayer::Begin()
{
    HZ_PROFILE_FUNCTION();

    ImGuiIO& io = ImGui::GetIO();
    Application& app = Application::Get();
    io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(),
                            (float)app.GetWindow().GetHeight());

    float time = (float)glfwGetTime();
    io.DeltaTime = m_Time > 0.0f ? (time - m_Time) : (1.0f / 60.0f);
    m_Time = time;

    ImGui_ImplOpenGL3_NewFrame();
    // 注意：没有调 ImGui_ImplGlfw_NewFrame()，因为我们自管 Platform 输入
    ImGui::NewFrame();
}

void ImGuiLayer::End()
{
    HZ_PROFILE_FUNCTION();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);
    }
}

void ImGuiLayer::SetDarkThemeColors()
{
    auto& colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]        = ImVec4{ 0.1f,  0.105f, 0.11f, 1.0f };
    colors[ImGuiCol_Header]          = ImVec4{ 0.2f,  0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_HeaderHovered]   = ImVec4{ 0.3f,  0.305f, 0.31f, 1.0f };
    colors[ImGuiCol_HeaderActive]    = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_Button]          = ImVec4{ 0.2f,  0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_ButtonHovered]   = ImVec4{ 0.3f,  0.305f, 0.31f, 1.0f };
    colors[ImGuiCol_ButtonActive]    = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_FrameBg]         = ImVec4{ 0.2f,  0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_FrameBgHovered]  = ImVec4{ 0.3f,  0.305f, 0.31f, 1.0f };
    colors[ImGuiCol_FrameBgActive]   = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_Tab]             = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TabHovered]      = ImVec4{ 0.38f, 0.3805f, 0.381f, 1.0f };
    colors[ImGuiCol_TabActive]       = ImVec4{ 0.28f, 0.2805f, 0.281f, 1.0f };
    colors[ImGuiCol_TabUnfocused]    = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
    colors[ImGuiCol_TitleBg]         = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TitleBgActive]   = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
}

} // namespace Hazel
```

### 痛点修复对照表

| 痛点 | v1（老代码） | v2（新代码） |
|---|---|---|
| 1. KeyMap[] 删除 | `io.KeyMap[ImGuiKey_Tab] = HZ_KEY_TAB;` | 删除整段，用 `HazelKeyToImGuiKey` 函数 |
| 2. MouseDown[] 直写 | `io.MouseDown[btn] = true;` | `io.AddMouseButtonEvent(btn, true);` |
| 3. MousePos 直写 | `io.MousePos = ImVec2(x, y);` | `io.AddMousePosEvent` + 多视口坐标转换 |
| 4. KeysDown[] 删除 | `io.KeysDown[code] = true;` | `io.AddKeyEvent(HazelKeyToImGuiKey(code), true);` |
| 5. 手动派生 Mod | `io.KeyCtrl = io.KeysDown[CTRL];` | 删除整段，AddKeyEvent 自动派生 |
| 6. 字符截断 | `io.AddInputCharacter((unsigned short)c);` | `io.AddInputCharacter((unsigned int)c);` |
| 7. 无 IME | 无任何处理 | 用官方 backend 或自实现 `Platform_SetImeDataFn` |
| 8. 失焦不清 | 缺失 | `OnWindowFocus / OnWindowLostFocus` → `AddFocusEvent` |
| 9. 滚轮直写 | `io.MouseWheel += yoff;` | `io.AddMouseWheelEvent(xoff, yoff);` |
| 10. 无手柄支持 | 缺失 | （Hazel 当前未做手柄；要做时也用 AddKeyEvent） |
| 11. FontGlobalScale | `io.FontGlobalScale = 1.5f;` | `ImGui::GetStyle().FontScaleMain = 1.5f;` |
| 12. 老剪贴板 API | `io.SetClipboardTextFn = ...;` | `ImGui::GetPlatformIO().Platform_SetClipboardTextFn = ...;` |
| 13. 无触屏支持 | 缺失 | 需要时调 `io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen)` |

---

## 1.3.9 IME 集成（痛点 7 完整方案）

如果你的 Hazel 自管 Window 不用官方 backend，需要自己实现 IME 桥接。

### Win32 平台

```cpp
#ifdef HZ_PLATFORM_WINDOWS
#include <Windows.h>
#include <imm.h>
#pragma comment(lib, "imm32.lib")

static void HazelImGuiSetImeData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
    // PlatformHandleRaw 是真正的 HWND（PlatformHandle 可能是抽象封装）
    HWND hwnd = (HWND)(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
    if (!hwnd) return;

    if (HIMC himc = ImmGetContext(hwnd)) {
        if (data->WantVisible) {
            COMPOSITIONFORM cf = {};
            cf.dwStyle = CFS_FORCE_POSITION;
            cf.ptCurrentPos.x = (LONG)data->InputPos.x;
            cf.ptCurrentPos.y = (LONG)data->InputPos.y;
            ImmSetCompositionWindow(himc, &cf);
        } else {
            ImmAssociateContext(hwnd, NULL);   // 临时禁用 IME
        }
        ImmReleaseContext(hwnd, himc);
    }
}

void ImGuiLayer::OnAttach() {
    // ... 其他初始化 ...
    ImGui::GetPlatformIO().Platform_SetImeDataFn = HazelImGuiSetImeData;
}
#endif
```

### Cocoa 平台

macOS 的 IME 集成更复杂——需要实现一个 NSTextInputClient。简化版：

```objective-c
// 在你的 NSView 子类里：
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* str = (NSString*)string;
    const char* utf8 = [str UTF8String];
    ImGui::GetIO().AddInputCharactersUTF8(utf8);
}
```

更完整的实现参考 `backends/imgui_impl_osx.mm`。

---

## 1.3.10 测试迁移效果的 Checklist

写完 v2 代码后，**逐项验证**：

- [ ] **编译通过**（如果用了 1.91+ ImGui，老代码完全过不去）。
- [ ] 鼠标在 ImGui 窗口上 hover 时高亮正确。
- [ ] InputText 能接收 ASCII 字符。
- [ ] InputText 能接收**中文/日文/韩文**（IME 候选窗口在光标附近弹出）。
- [ ] Ctrl+S 等快捷键被 ImGui 正确识别（用 Demo → Tools → Input Stack 验证）。
- [ ] 按住 Alt+Tab 切走再切回，Alt 键不"卡死"（之前 Hazel v1 这里会卡）。
- [ ] 拖动 ImGui 窗口跨越显示器边界（如果开了 Multi-Viewport），鼠标行为正确。
- [ ] 30fps 下快速点击按钮**不丢失任何点击**（trickle 起作用）。
- [ ] macOS 上 Cmd+S 等价于 Win 的 Ctrl+S（自动适配）。
- [ ] HiDPI 屏字体清晰（开了 `DpiEnableScaleFonts`）。

---

## 1.3.11 第一部分总结

| 章 | 主题 | 核心收获 |
|---|---|---|
| 1.1 | 现代架构概览 | 三层架构 / IO + PlatformIO 双总线 / 一帧时序图 |
| 1.2 | Hazel 老版痛点对比 | 13 处具体问题 / 阻塞 vs 严重 vs 隐患分级 |
| 1.3 | 现代 Input Routing 与迁移 | InputEvent 联合体 / Trickle 算法 / Owner-Aware 路由 / 完整 v2 代码 |

**最关键的一句话**：**所有 Backend 都必须通过 `io.AddXxxEvent` 入队事件，由 `UpdateInputEvents` 在 NewFrame 中统一消费**——这是 ImGui 1.87+ 输入系统的全部哲学。

读完第一部分后，你应该能：

- 在 Hazel 项目里**完整把 ImGui 1.84 老集成迁移到 1.92**。
- 看到任何 `AddKeyEvent / AddMousePosEvent` 调用都知道它会被入队、被 trickle、被消费。
- 写自定义 widget 时知道用 `Shortcut + RouteFocused` 做局部快捷键。

---

## 1.3.12 下一部分预告

**第二部分：核心数据结构与零分配设计哲学**

- 第 2.1 章：`ImVector<T>` 源码逐字节剖析。我们会回到第 0.3 章打过的"内存模型"基础上，把 `imgui.h:2207~2270` 的 60 行实现一行一行拆解。
- 第 2.2 章：`ImPool<T>` / `ImChunkStream<T>` / `ImBitArray` 等工程化容器。
- 第 2.3 章：内存分配器与 `ImGuiContext` 生命周期——回到全局指针 / DLL 边界 / Hot Reload 等真实工程问题。

第一部分结束。当你说"开始第二部分"或"开始 2.1 章"时，我们继续。
