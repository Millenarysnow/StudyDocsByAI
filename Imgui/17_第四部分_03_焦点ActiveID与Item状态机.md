# 第 4.3 章 · 窗口焦点、Active ID 与 Item 状态机

> **本章目标**：把 ImGui 中**最微妙的状态机**剖完——`HoveredId / ActiveId / NavId` 三大 ID 的状态流转 + `LastItemData` 的"上一个 item"哲学 + `ItemAdd / ItemHoverable / ButtonBehavior` 的链式调用。读完后你应该能写出**任何**自定义 widget——按钮、滑块、节点端口、时间线 keyframe——都用同一套"三段式"模板。
>
> **本章对应源码**：`imgui.cpp:4655~4763`（`SetActiveID / ClearActiveID / SetHoveredID / MarkItemEdited`）、`imgui.cpp:4811~5003`（`IsItemHovered / IsWindowContentHoverable / ItemHoverable`）、`imgui.cpp:5018~5050`（`SetLastItemData`）、`imgui.cpp:11195~11400`（`KeepAliveID / ItemAdd / ItemSize`）、`imgui_internal.h:1377~1390`（`ImGuiLastItemData`）、`imgui_internal.h:1046`（`ImGuiButtonFlags_`）、`imgui_widgets.cpp` 中的 `ButtonBehavior`。
>
> **前置阅读**：第 4.1 章（ID Stack）、第 4.2 章（ImGuiWindow）。

---

## 4.3.0 三大 ID 速览

```
HoveredId   = 鼠标悬停的 widget ID                  ← 每帧重置 + 重新计算
ActiveId    = 当前激活的 widget ID                  ← 跨帧持久（直到释放）
NavId       = 当前焦点的 widget ID（键盘/手柄）       ← 跨帧持久（Tab 移动）
```

每个都是 `ImGuiID`（`ImU32`）。每帧只有最多一个 widget 占据每个角色。

```
                时间轴 →

Frame 1: HoveredId=0,    ActiveId=0,    NavId=0
            ↓ 鼠标移到 ButtonA 上
Frame 2: HoveredId=A,    ActiveId=0,    NavId=0     ← Hovered
            ↓ 用户点击 ButtonA
Frame 3: HoveredId=A,    ActiveId=A,    NavId=A     ← Active + Focused
            ↓ 用户松开
Frame 4: HoveredId=A,    ActiveId=0,    NavId=A     ← Released, Click 触发
            ↓ 鼠标移走
Frame 5: HoveredId=0,    ActiveId=0,    NavId=A     ← 仍有焦点
            ↓ 用户按 Tab
Frame 6: HoveredId=0,    ActiveId=0,    NavId=B     ← 焦点移到 ButtonB
```

每个状态变化都触发 `ImGui::IsItemActivated() / IsItemDeactivated()` 等"边沿事件"。

---

## 4.3.1 `g.HoveredId`：鼠标悬停目标

### 字段

```cpp
// imgui_internal.h:2249~2256
ImGuiID  HoveredId;                          // 当前帧 hover 目标
ImGuiID  HoveredIdPreviousFrame;
int      HoveredIdPreviousFrameItemCount;    // 上帧用同 ID 的 item 数（>1 = 冲突）
float    HoveredIdTimer;                     // 悬停连续时间
float    HoveredIdNotActiveTimer;            // 悬停且不 active 的连续时间
bool     HoveredIdAllowOverlap;
bool     HoveredIdIsDisabled;
```

### 状态流转（每帧）

```
NewFrame:
   HoveredIdPreviousFrame = HoveredId   ← 备份
   HoveredId = 0                         ← 清零
   HoveredIdAllowOverlap = false
   HoveredIdIsDisabled = false
   HoveredIdPreviousFrameItemCount = 0

用户构建 UI:
   每个 widget 调 ItemHoverable(bb, id)：
     - 检查鼠标位置在 bb 内
     - 检查 g.HoveredWindow == window
     - 检查 g.HoveredId == 0（先到先得）
     - 通过则 SetHoveredID(id)

EndFrame:
   ConfigDebugHighlightIdConflicts 检查 HoveredIdPreviousFrameItemCount
```

### `SetHoveredID`

```cpp
// imgui.cpp:4725~4732
void ImGui::SetHoveredID(ImGuiID id)
{
    ImGuiContext& g = *GImGui;
    g.HoveredId = id;
    g.HoveredIdAllowOverlap = false;
    if (id != 0 && g.HoveredIdPreviousFrame != id)
        g.HoveredIdTimer = g.HoveredIdNotActiveTimer = 0.0f;
}
```

——重置 timer 当**新** widget 接管 hover 时。这就是 tooltip 延时机制的基础。

### `HoveredIdTimer` 的用途

```cpp
// 用法：延时显示 tooltip
if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    // 等价于：g.HoveredIdNotActiveTimer >= style.HoverDelayNormal
    ImGui::SetTooltip("提示");
}
```

`io.HoverDelayNormal`（默认 0.5 秒）—— 鼠标静止超过这个时间才显示 tooltip。

---

## 4.3.2 `g.ActiveId`：当前激活目标

### 字段（约 15 个）

```cpp
// imgui_internal.h:2257~2270
ImGuiID  ActiveId;                          // 当前 active 目标
ImGuiID  ActiveIdIsAlive;                   // 本帧是否被 widget 主张
float    ActiveIdTimer;
bool     ActiveIdIsJustActivated;            // 本帧刚激活
bool     ActiveIdAllowOverlap;
bool     ActiveIdNoClearOnFocusLoss;         // 焦点丢失时不清 ActiveId
bool     ActiveIdHasBeenPressedBefore;
bool     ActiveIdHasBeenEditedBefore;
bool     ActiveIdHasBeenEditedThisFrame;
bool     ActiveIdFromShortcut;
ImS8     ActiveIdMouseButton;                // 哪个鼠标按钮触发的
ImGuiID  ActiveIdDisabledId;
ImVec2   ActiveIdClickOffset;                // 点击点距 widget 左上的偏移
ImGuiInputSource ActiveIdSource;             // Mouse / Keyboard / Gamepad
ImGuiWindow* ActiveIdWindow;                  // ActiveId 所在的窗口
ImGuiID  ActiveIdPreviousFrame;
```

### `SetActiveID`：激活 widget

```cpp
// imgui.cpp:4655 (核心部分)
void ImGui::SetActiveID(ImGuiID id, ImGuiWindow* window)
{
    ImGuiContext& g = *GImGui;

    // ① 清旧 ActiveId
    if (g.ActiveId != 0)
    {
        // 把旧 ActiveId 信息存进 DeactivatedItemData（给 IsItemDeactivated 用）
        g.DeactivatedItemData.ID = g.ActiveId;
        g.DeactivatedItemData.ElapseFrame = ...;
        g.DeactivatedItemData.HasBeenEditedBefore = g.ActiveIdHasBeenEditedBefore;
        // ...
        
        // 特殊：InputText 失活钩子
        if (g.InputTextState.ID == g.ActiveId)
            InputTextDeactivateHook(g.ActiveId);
        
        // 特殊：取消窗口移动
        if (g.MovingWindow != NULL && g.ActiveId == g.MovingWindow->MoveId)
            StopMouseMovingWindow();
    }

    // ② 激活新
    g.ActiveIdIsJustActivated = (g.ActiveId != id);
    if (g.ActiveIdIsJustActivated) {
        g.ActiveIdTimer = 0.0f;
        g.ActiveIdHasBeenPressedBefore = false;
        g.ActiveIdHasBeenEditedBefore = false;
        // ...
    }
    g.ActiveId = id;
    g.ActiveIdAllowOverlap = false;
    g.ActiveIdWindow = window;
    g.ActiveIdSource = ...;
    
    // 清 widget 已声明的输入
    g.ActiveIdUsingNavDirMask = 0x00;
    g.ActiveIdUsingAllKeyboardKeys = false;
}
```

**做的事**：

1. 旧 ActiveId 信息备份到 `DeactivatedItemData`——后续 `IsItemDeactivated()` 用。
2. 处理"特殊 ActiveId"——例如 InputText 的失活钩子（保存 undo 状态）、窗口移动的取消。
3. 设置新 ActiveId + 重置一系列字段。

### `ClearActiveID`

```cpp
void ImGui::ClearActiveID() {
    SetActiveID(0, NULL);
}
```

——其实就是 `SetActiveID(0, NULL)`。

### `KeepAliveID`：保活机制

```cpp
// imgui.cpp:11202
void ImGui::KeepAliveID(ImGuiID id)
{
    ImGuiContext& g = *GImGui;
    if (g.ActiveId == id)
        g.ActiveIdIsAlive = id;
    if (g.DeactivatedItemData.ID == id)
        g.DeactivatedItemData.IsAlive = true;
}
```

每个 `ItemAdd` 内部会调一次。**`ActiveIdIsAlive` 的作用**：跟踪"当前 ActiveId 对应的 widget 在本帧是否还存在"。

`NewFrame` 中的检查（`imgui.cpp:5564`）：

```cpp
if (g.ActiveId != 0 && g.ActiveIdIsAlive != g.ActiveId && g.ActiveIdPreviousFrame == g.ActiveId) {
    // ActiveId 对应的 widget 上帧 alive 但本帧没出现 → 自动 ClearActiveID
    ClearActiveID();
}
```

**这个机制处理了什么**：用户代码执行了一个分支让某个 active widget 不再被提交（例如某个按钮 active 时切换面板，按钮消失）—— ImGui 自动释放 ActiveId 避免"幽灵 active"。

### `g.DeactivatedItemData`：刚 deactivate 的 widget

```cpp
struct ImGuiDeactivatedItemData {
    ImGuiID ID;
    int     ElapseFrame;
    bool    HasBeenEditedBefore;
    bool    IsAlive;
};
```

存"上一个被 deactivate 的 widget 信息"——专门给 `IsItemDeactivated()` / `IsItemDeactivatedAfterEdit()` 用。

---

## 4.3.3 `g.NavId`：键盘/手柄焦点

`NavId` 与 ActiveId 的关键区别：

- **ActiveId** = 鼠标按下时激活；松开时清零。
- **NavId** = Tab / 方向键导航的当前目标；**跨帧持久**（直到用户按 Tab 移动）。

```cpp
// imgui_internal.h（ImGuiContext 中）
ImGuiID         NavId;
ImGuiWindow*    NavWindow;
ImGuiID         NavFocusScopeId;
ImGuiID         NavActivateId;
ImGuiID         NavActivateDownId;
ImGuiID         NavActivatePressedId;
ImGuiID         NavNextActivateId;
// ...
```

### NavId 的转移

```
用户按 Tab：
   UpdateNav() 找下一个可聚焦 widget
   NavId = next_id
   下次该 widget 提交时被高亮

用户按 Enter / Space（Activate 键）：
   NavActivateId = NavId
   该 widget 内部检测：
     if (g.NavActivateId == id) 触发 click
```

### 为什么要分开 NavId 和 ActiveId

考虑：用户用键盘聚焦到 Slider，按方向键调整值。

- `NavId` = Slider 的 ID（持续焦点）。
- `ActiveId` = 0（Slider 没有"按下"动作，只有调整）。

如果用 `ActiveId` 跟踪键盘焦点：会与"鼠标 active"互相干扰。所以分两个状态。

---

## 4.3.4 `g.LastItemData`：上一个提交的 item

```cpp
// imgui_internal.h:1377
struct ImGuiLastItemData
{
    ImGuiID                 ID;
    ImGuiItemFlags          ItemFlags;
    ImGuiItemStatusFlags    StatusFlags;
    ImRect                  Rect;
    ImRect                  NavRect;
    ImRect                  DisplayRect;
    ImRect                  ClipRect;
    ImGuiKeyChord           Shortcut;
};
```

**所有 `IsItemXXX()` API 都通过 `g.LastItemData` 工作**：

```cpp
ImGui::Button("X");                       // ItemAdd 设置 g.LastItemData
if (ImGui::IsItemHovered()) { ... }       // 读 g.LastItemData.ID + 检查 g.HoveredId
if (ImGui::IsItemActive()) { ... }        // 读 g.LastItemData.ID + 检查 g.ActiveId
if (ImGui::IsItemEdited()) { ... }        // 读 g.LastItemData.StatusFlags 中 _Edited 位
```

### `ImGuiItemStatusFlags_`

```cpp
enum ImGuiItemStatusFlags_ {
    ImGuiItemStatusFlags_None              = 0,
    ImGuiItemStatusFlags_HoveredRect       = 1 << 0,   // 几何上 hover（不一定逻辑 hover）
    ImGuiItemStatusFlags_HasDisplayRect    = 1 << 1,
    ImGuiItemStatusFlags_Edited            = 1 << 2,   // 由 MarkItemEdited 设置
    ImGuiItemStatusFlags_ToggledSelection  = 1 << 3,
    ImGuiItemStatusFlags_ToggledOpen       = 1 << 4,
    ImGuiItemStatusFlags_HasDeactivated    = 1 << 5,
    ImGuiItemStatusFlags_Deactivated       = 1 << 6,
    ImGuiItemStatusFlags_HoveredWindow     = 1 << 7,
    ImGuiItemStatusFlags_Visible           = 1 << 8,
    ImGuiItemStatusFlags_HasClipRect       = 1 << 9,
    ImGuiItemStatusFlags_HasShortcut       = 1 << 10,
    // ...
};
```

每个 widget 在交互过程中根据状态设置 StatusFlags。后续 `IsItemEdited()` 等读取它。

### `MarkItemEdited`：标记编辑

```cpp
// imgui.cpp:4740
void ImGui::MarkItemEdited(ImGuiID id)
{
    ImGuiContext& g = *GImGui;
    if (g.LastItemData.ItemFlags & ImGuiItemFlags_NoMarkEdited) return;
    if (g.ActiveId == id || g.ActiveId == 0) {
        g.ActiveIdHasBeenEditedThisFrame = true;
        g.ActiveIdHasBeenEditedBefore = true;
        // ...
    }
    g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_Edited;
}
```

——widget 的"值变了"时调。例如 Slider 拖动改变值时调 `MarkItemEdited`。

---

## 4.3.5 `IsItemHovered` 全剖

```cpp
// imgui.cpp:4811 (简化)
bool ImGui::IsItemHovered(ImGuiHoveredFlags flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (g.NavHighlightItemUnderNav)
        if (g.LastItemData.ID == g.NavId && !(flags & ImGuiHoveredFlags_NoNavOverride))
            // 键盘焦点也算 hover
            ;
    
    // 1. 几何 hover 检查
    if (!(g.LastItemData.StatusFlags & ImGuiItemStatusFlags_HoveredRect))
        return false;
    if (g.HoveredId != g.LastItemData.ID && !(flags & ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        if (g.LastItemData.ID != g.ActiveId)
            return false;

    // 2. 窗口 hover 检查（如果 widget 在被遮挡的窗口里 → 不 hover）
    if (!(flags & ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        if (g.NavWindow && (focused_root != window->RootWindow))
            if (focused_root->Flags & ImGuiWindowFlags_Modal)
                return false;
    
    // 3. Disabled 检查
    if ((g.LastItemData.ItemFlags & ImGuiItemFlags_Disabled) && !(flags & ImGuiHoveredFlags_AllowWhenDisabled))
        return false;
    
    // 4. 延时检查（DelayShort / DelayNormal）
    if (flags & ImGuiHoveredFlags_DelayMask_) {
        // 如果是新 hover，记录 HoverItemDelayId
        if (g.HoverItemUnlockedStationaryId != g.LastItemData.ID)
            return false;
        // 检查 timer
        float delay = (flags & ImGuiHoveredFlags_DelayShort) ? g.Style.HoverDelayShort : g.Style.HoverDelayNormal;
        if (g.HoverItemDelayTimer < delay)
            return false;
    }
    
    return true;
}
```

**5 层检查**：

1. 几何上鼠标在 widget rect 内（`HoveredRect` flag）。
2. 没有更前景的 widget 抢走 hover（`HoveredId == LastItemData.ID`）。
3. 没有更前景的窗口（modal / popup）阻挡。
4. widget 没被禁用（除非 flag 允许）。
5. 如果要求延时，timer 已达到。

---

## 4.3.6 `ItemHoverable` 全剖

```cpp
// imgui.cpp:4915
bool ImGui::ItemHoverable(const ImRect& bb, ImGuiID id, ImGuiItemFlags item_flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    // ① ID 冲突检测（"白送"的检查）
#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (id != 0 && g.HoveredIdPreviousFrame == id && (item_flags & ImGuiItemFlags_AllowDuplicateId) == 0)
    {
        g.HoveredIdPreviousFrameItemCount++;
        if (g.DebugDrawIdConflictsId == id)
            window->DrawList->AddRect(bb.Min - ImVec2(1,1), bb.Max + ImVec2(1,1), IM_COL32(255, 0, 0, 255), 0.0f, ImDrawFlags_None, 2.0f);
    }
#endif

    // ② 窗口 hover 检查
    if (g.HoveredWindow != window) return false;
    
    // ③ 几何检查
    if (!IsMouseHoveringRect(bb.Min, bb.Max)) return false;

    // ④ 其他 widget 抢占检查
    if (g.HoveredId != 0 && g.HoveredId != id && !g.HoveredIdAllowOverlap) return false;
    if (g.ActiveId != 0 && g.ActiveId != id && !g.ActiveIdAllowOverlap) {
        if (!g.ActiveIdFromShortcut)
            return false;
    }

    // ⑤ 窗口内容 hover 检查（modal / popup 遮挡）
    if (!(item_flags & ImGuiItemFlags_NoWindowHoverableCheck) && !IsWindowContentHoverable(window, ImGuiHoveredFlags_None)) {
        g.HoveredIdIsDisabled = true;
        return false;
    }

    // ⑥ 设置 HoveredId（先到先得）
    if (id != 0) {
        if (g.DragDropActive && g.DragDropPayload.SourceId == id && ...)
            return false;
        SetHoveredID(id);
        // AllowOverlap 模式特殊处理
        if (item_flags & ImGuiItemFlags_AllowOverlap) {
            g.HoveredIdAllowOverlap = true;
            if (g.HoveredIdPreviousFrame != id)
                return false;
        }
    }

    // ⑦ Disabled 处理
    if (item_flags & ImGuiItemFlags_Disabled) {
        if (g.ActiveId == id && id != 0) ClearActiveID();
        g.HoveredIdIsDisabled = true;
        return false;
    }

    // ⑧ 调试钩子（Item Picker / 断点）
#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (id != 0) {
        if (g.DebugItemPickerActive && g.HoveredIdPreviousFrame == id)
            GetForegroundDrawList()->AddRect(bb.Min, bb.Max, IM_COL32(255, 255, 0, 255));
        if (g.DebugItemPickerBreakId == id)
            IM_DEBUG_BREAK();
    }
#endif

    // ⑨ Nav 高亮模式下不 hover
    if (g.NavHighlightItemUnderNav && (item_flags & ImGuiItemFlags_NoNavDisableMouseHover) == 0)
        return false;

    return true;
}
```

**这是 ImGui 中最复杂的函数之一**。9 步检查决定一个 widget 是否能"被鼠标 hover"。

### `IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS` 的低成本检测

注释 `imgui.cpp:4920~4922`：

> `Detect ID conflicts (this is specifically done here by comparing on hover because it allows us a detection of duplicates that is algorithmically extra cheap, 1 u32 compare per item. No O(log N) lookup whatsoever)`

ImGui 在 `ItemHoverable` 里**顺便**做 ID 冲突检测——比较 `g.HoveredIdPreviousFrame == id`。如果上帧某个 ID 已经被 hover，本帧又有 widget 用同 ID + 在 hover 区域 → 计数。

**只检测"鼠标位置附近"的冲突**——成本 O(1)。如果想要全量检测，开 `IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS`（参见第 4.1 章）。

---

## 4.3.7 `ItemAdd`：widget 注册的核心

```cpp
// imgui.cpp:11216 (简化)
bool ImGui::ItemAdd(const ImRect& bb, ImGuiID id, const ImRect* nav_bb_arg, ImGuiItemFlags extra_flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    // ① 设置 LastItemData
    g.LastItemData.ID = id;
    g.LastItemData.Rect = bb;
    g.LastItemData.NavRect = nav_bb_arg ? *nav_bb_arg : bb;
    g.LastItemData.ItemFlags = g.CurrentItemFlags | g.NextItemData.ItemFlags | extra_flags;
    g.LastItemData.StatusFlags = ImGuiItemStatusFlags_None;

    // ② Nav 处理
    if (id != 0) {
        KeepAliveID(id);
        if (!(g.LastItemData.ItemFlags & ImGuiItemFlags_NoNav)) {
            window->DC.NavLayersActiveMaskNext |= (1 << window->DC.NavLayerCurrent);
            if (g.NavId == id || g.NavAnyRequest)
                if (g.NavWindow->RootWindowForNav == window->RootWindowForNav)
                    if (window == g.NavWindow || ((window->ChildFlags | g.NavWindow->ChildFlags) & ImGuiChildFlags_NavFlattened))
                        NavProcessItem();
        }
        if (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasShortcut)
            ItemHandleShortcut(id);
    }

    // ③ 清 NextItemData
    g.NextItemData.HasFlags = ImGuiNextItemDataFlags_None;
    g.NextItemData.ItemFlags = ImGuiItemFlags_None;

    // ④ Test Engine 钩子
#ifdef IMGUI_ENABLE_TEST_ENGINE
    if (id != 0)
        IMGUI_TEST_ENGINE_ITEM_ADD(id, g.LastItemData.NavRect, &g.LastItemData);
#endif

    // ⑤ 裁剪检查
    const bool is_rect_visible = bb.Overlaps(window->ClipRect);
    if (!is_rect_visible) {
        if (id == 0 || (id != g.ActiveId && id != g.ActiveIdPreviousFrame && id != g.NavId && id != g.NavActivateId))
            if (!g.ItemUnclipByLog)
                return false;
    }

    // ⑥ 调试钩子
    // 5.4.1 节会展开
    
    // ⑦ 标记 visible
    g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_Visible;
    if (is_rect_visible)
        g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_Visible | ImGuiItemStatusFlags_HoveredRect;

    return true;
}
```

**关键步骤**：

1. **设置 LastItemData**：所有 IsItemXXX 的数据源。
2. **Nav 处理**：如果当前 widget 是 NavId 目标，记录它的 NavRect。
3. **NextItemData 消费**：`SetNextItemWidth` / `SetNextItemOpen` 等的临时状态。
4. **裁剪检查**：不可见且非 ActiveId/NavId 直接返回 false。

`ItemAdd` 返回 `false` 表示"widget 不可见，可以 skip"——widget 函数 early return：

```cpp
bool ImGui::Button(const char* label) {
    // ...
    if (!ItemAdd(bb, id))
        return false;   // ← 不可见，跳过
    
    // ... 渲染 + 行为检测
}
```

---

## 4.3.8 `ItemSize`：推进 cursor

```cpp
// imgui.cpp:11354
void ImGui::ItemSize(const ImVec2& size, float text_baseline_y)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window->SkipItems) return;

    const float line_height = ImMax(window->DC.CurrLineSize.y, /* 当前行高 */ size.y);

    const float line_y1 = window->DC.IsSameLine ? window->DC.CursorPosPrevLine.y : window->DC.CursorPos.y;
    
    // 推进 CursorPos
    window->DC.CursorPosPrevLine.x = window->DC.CursorPos.x + size.x;
    window->DC.CursorPosPrevLine.y = line_y1;
    window->DC.CursorPos.x = IM_TRUNC(window->Pos.x + window->DC.Indent.x + window->DC.ColumnsOffset.x);
    window->DC.CursorPos.y = IM_TRUNC(line_y1 + line_height + g.Style.ItemSpacing.y);
    window->DC.CursorMaxPos.x = ImMax(window->DC.CursorMaxPos.x, window->DC.CursorPosPrevLine.x);
    window->DC.CursorMaxPos.y = ImMax(window->DC.CursorMaxPos.y, window->DC.CursorPos.y - g.Style.ItemSpacing.y);
    // ...
    window->DC.PrevLineSize.y = line_height;
    window->DC.CurrLineSize.y = 0.0f;
    // ...
}
```

**做的事**：

1. 把 `CursorPos.y` 推进到下一行（当前行高 + ItemSpacing.y）。
2. 把 `CursorPos.x` 重置到行首（含 Indent / ColumnsOffset）。
3. 更新 `CursorMaxPos`（用于自动调整窗口大小）。
4. 处理 `IsSameLine`（如果上一项调用了 `SameLine`，cursor 留在同一行）。

第 5.1 章会专题展开 Layout 系统。

---

## 4.3.9 `ButtonBehavior`：万能行为函数

`ButtonBehavior` 是 ImGui 中**最重要的"行为函数"**——所有"可点击 widget"的交互逻辑全在这里。

### 签名

```cpp
// imgui_internal.h:3658
IMGUI_API bool ButtonBehavior(const ImRect& bb, ImGuiID id, bool* out_hovered, bool* out_held, ImGuiButtonFlags flags = 0);
```

输入：bb（点击区域）、id、flags。
输出：`out_hovered`（是否 hover）、`out_held`（是否按住）、返回值（是否点击）。

### `ImGuiButtonFlags_`

```cpp
// imgui.h + imgui_internal.h
ImGuiButtonFlags_None           = 0,
ImGuiButtonFlags_MouseButtonLeft        = 1 << 0,   // 默认
ImGuiButtonFlags_MouseButtonRight       = 1 << 1,
ImGuiButtonFlags_MouseButtonMiddle      = 1 << 2,

// imgui_internal.h 私有 flag
ImGuiButtonFlags_PressedOnClick         = 1 << 4,   // 按下立刻触发
ImGuiButtonFlags_PressedOnClickRelease  = 1 << 5,   // 按下+释放（默认）
ImGuiButtonFlags_PressedOnClickReleaseAnywhere = 1 << 6,
ImGuiButtonFlags_PressedOnRelease       = 1 << 7,
ImGuiButtonFlags_PressedOnDoubleClick   = 1 << 8,
ImGuiButtonFlags_PressedOnDragDropHold  = 1 << 9,
ImGuiButtonFlags_Repeat                 = 1 << 10,  // 长按重复触发
ImGuiButtonFlags_FlattenChildren        = 1 << 11,
ImGuiButtonFlags_AllowOverlap           = 1 << 12,
ImGuiButtonFlags_DontClosePopups        = 1 << 13,
ImGuiButtonFlags_NoNavFocus             = 1 << 18,
// ... 12+ 个 flag
```

### 行为模式（PressedOn...）

| Flag | 行为 |
|---|---|
| `_PressedOnClickRelease`（默认） | 按下不触发，**按下后在 widget 内松开**才触发。Win/Mac 标准。 |
| `_PressedOnClick` | 按下立刻触发。"硬"按钮（射击游戏）。 |
| `_PressedOnRelease` | 不需要按下时在 widget 内，松开时位置不重要。罕见。 |
| `_PressedOnClickReleaseAnywhere` | 按下时在 widget 内，松开时位置无所谓。Drag 后下放场景。 |
| `_PressedOnDoubleClick` | 双击触发。 |
| `_PressedOnDragDropHold` | 拖拽悬停 0.7 秒触发。 |

### 简化版实现（伪代码）

```cpp
bool ButtonBehavior(const ImRect& bb, ImGuiID id, bool* out_hovered, bool* out_held, ImGuiButtonFlags flags)
{
    ImGuiContext& g = *GImGui;
    
    if (flags & ImGuiButtonFlags_Disabled)
        // 处理禁用
    
    if (!(flags & ImGuiButtonFlags_MouseButtonMask))
        flags |= ImGuiButtonFlags_MouseButtonLeft;
    
    bool pressed = false;
    bool hovered = ItemHoverable(bb, id, ItemFlags);
    
    // 1. 鼠标按下检查
    if (hovered && (g.IO.MouseClicked[mouse_button] || ...)) {
        if (flags & _PressedOnClick) {
            pressed = true;
            ClearActiveID();
        } else {
            SetActiveID(id, window);
        }
    }
    
    // 2. 释放 / 持续按下处理
    bool held = false;
    if (g.ActiveId == id) {
        if (g.IO.MouseDown[mouse_button]) {
            held = true;
        } else {
            // 鼠标松开
            bool release_in = hovered && IsMouseReleased(mouse_button);
            if (release_in && (flags & _PressedOnClickRelease))
                pressed = true;
            ClearActiveID();
        }
    }
    
    // 3. 键盘 Activate
    if (g.NavActivateId == id && !held) {
        pressed = true;
        ClearActiveID();
    }
    
    *out_hovered = hovered;
    *out_held = held;
    return pressed;
}
```

——根据 flags 组合，处理几十种交互模式。

### 真实代码量

`ButtonBehavior` 在 `imgui_widgets.cpp` 中接近 200 行——包含所有边界情况：

- 鼠标按钮分发（左/右/中）。
- 拖拽与点击的区分（鼠标移动距离 < threshold 才算 click）。
- Repeat 模式（长按定时触发）。
- DoubleClick 检测（基于 `MouseClickedCount`）。
- DragDrop hover 触发。
- Nav 激活（键盘 Enter/Space）。
- AllowOverlap 模式。
- Disabled 处理。

---

## 4.3.10 自定义 Widget 的"三段式"模板

基于上面所有源码分析，自定义 widget 的标准模板：

```cpp
bool MyEditor::MyCustomWidget(const char* label, /* 参数 */)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    // ============ 阶段 1：ID + 几何 ============
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    
    // 计算 widget 边界
    const ImVec2 pos = window->DC.CursorPos;
    const ImVec2 size = ImVec2(/* 你的宽度 */, /* 你的高度 */);
    const ImRect bb(pos, pos + size);
    
    // ============ 阶段 2：注册 + 行为检测 ============
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;
    
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_None);
    
    if (pressed) {
        // 用户点击
        ImGui::MarkItemEdited(id);
    }

    // ============ 阶段 3：渲染 ============
    const ImU32 col = ImGui::GetColorU32(
        held ? ImGuiCol_ButtonActive :
        hovered ? ImGuiCol_ButtonHovered :
        ImGuiCol_Button);
    
    ImDrawList* dl = window->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, col, g.Style.FrameRounding);
    
    // 渲染 label
    ImGui::RenderText(bb.Min + ImVec2(g.Style.FramePadding.x, g.Style.FramePadding.y), label);
    
    return pressed;
}
```

**三段式**：

1. **ID + 几何**：算 ID、算 bb。
2. **注册 + 行为**：`ItemSize` + `ItemAdd` + `ButtonBehavior`。
3. **渲染**：根据 hovered/held 选颜色，往 DrawList 写图元。

**这个模板覆盖 95% 的自定义 widget 需求**。剩下 5% 是：

- 拖拽（用 `IsMouseDragging` + `GetMouseDragDelta`）。
- 输入数字（用 `InputScalar` 或自己的 `TempInputText` 模式）。
- 键盘焦点导航（用 `g.NavId == id`）。

---

## 4.3.11 `IsItemActivated / Deactivated / DeactivatedAfterEdit`

```cpp
bool IsItemActivated() {
    return g.ActiveId == g.LastItemData.ID && g.ActiveIdIsJustActivated;
}

bool IsItemDeactivated() {
    return g.DeactivatedItemData.ID == g.LastItemData.ID && g.LastItemData.ID != 0;
}

bool IsItemDeactivatedAfterEdit() {
    return IsItemDeactivated() && g.DeactivatedItemData.HasBeenEditedBefore;
}
```

**典型场景**：

```cpp
// "用户编辑完后保存"
ImGui::InputFloat("Value", &my_value);
if (ImGui::IsItemDeactivatedAfterEdit()) {
    // 用户输入完离开了 InputFloat → 保存
    SaveValue(my_value);
}
```

——`InputFloat` 在 active 时每次按键都改 `my_value`，但你不希望每次都触发"保存"。`IsItemDeactivatedAfterEdit` 只在用户**离开** widget 后触发一次。

---

## 4.3.12 实战：节点编辑器的端口（Pin）行为

```cpp
class NodePin {
public:
    bool DrawAndCheckClicked(ImVec2 center, float radius, ImU32 color)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;

        // ID
        ImGuiID id = window->GetID(this);   // 用 this 指针作 ID
        
        // 几何
        ImRect bb(center - ImVec2(radius, radius), center + ImVec2(radius, radius));
        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id)) return false;

        // 行为
        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

        // 渲染
        ImDrawList* dl = window->DrawList;
        ImU32 final_col = held ? IM_COL32(255, 255, 0, 255) :
                          hovered ? IM_COL32(255, 200, 0, 255) :
                          color;
        dl->AddCircleFilled(center, radius, final_col);
        dl->AddCircle(center, radius, IM_COL32(0, 0, 0, 255), 0, 1.5f);

        return pressed;
    }
};
```

——三段式应用：ID 用对象指针、几何是圆形 bb、行为用 ButtonBehavior、渲染用 DrawList。

---

## 4.3.13 状态机的"边沿"事件清单

| API | 含义 |
|---|---|
| `IsItemHovered()` | 当前 hover |
| `IsItemActive()` | 当前 active（鼠标按下 / 键盘激活） |
| `IsItemFocused()` | 键盘焦点 |
| `IsItemClicked()` | 鼠标点击（PressedOnClick） |
| `IsItemEdited()` | 本帧值被修改 |
| `IsItemActivated()` | 本帧首次 active（边沿） |
| `IsItemDeactivated()` | 本帧失去 active（边沿） |
| `IsItemDeactivatedAfterEdit()` | 失去 active 且至少编辑过一次 |
| `IsItemToggledOpen()` | TreeNode 等折叠状态切换 |
| `IsItemVisible()` | 在 ClipRect 内 |
| `GetItemRectMin/Max/Size` | LastItemData.Rect 几何 |
| `GetItemID()` | LastItemData.ID |

**所有这些都通过 `g.LastItemData` 工作**——所以你**必须**紧跟在 widget 调用之后调它们：

```cpp
ImGui::Button("X");
bool b = ImGui::IsItemHovered();   // ✅ 紧跟

ImGui::Button("Y");
ImGui::Button("Z");
bool b = ImGui::IsItemHovered();   // ❌ 检查的是 Z 而不是 X
```

---

## 4.3.14 调试 ActiveId 的 Debug Log

打开 Demo → Debug Log → 勾上 `ActiveId` 类别：

```
[ActiveId] 0x1234ABCD (widget 'Button "Save"') → activated
[ActiveId] 0x1234ABCD → deactivated (released)
[ActiveId] 0xDEADBEEF (widget 'InputFloat "X"') → activated
```

每次 SetActiveID 调用都会打 log。**追踪"为什么这个 widget 突然失去焦点"的最直接方式**。

---

## 4.3.15 本章小结

ImGui 状态机的 7 条核心：

1. **3 大 ID**：HoveredId（每帧重置）/ ActiveId（持久直到释放）/ NavId（持久直到 Tab）。
2. **`g.LastItemData`** 是上一个 ItemAdd 的快照——所有 IsItemXXX 的数据源。
3. **`KeepAliveID`** 让 ActiveId 保活——widget 本帧仍提交则继续 active。
4. **`ItemHoverable` 9 步检查**——决定一个 widget 是否真的能被 hover。
5. **`ItemAdd` 7 步**：设置 LastItemData → KeepAliveID → Nav 处理 → 清 NextItemData → 裁剪检查 → Test 钩子 → 标记 visible。
6. **`ButtonBehavior` 万能函数**——12+ 标志位覆盖所有"可点击" widget 行为。
7. **三段式自定义 widget**：ID+几何 → ItemSize+ItemAdd+ButtonBehavior → 渲染。

记住的"边沿事件"：

```
IsItemActivated      = ActiveId == id && ActiveIdIsJustActivated
IsItemDeactivated    = DeactivatedItemData.ID == id
IsItemDeactivatedAfterEdit = IsItemDeactivated() && HasBeenEditedBefore
```

---

## 4.3.16 下一章预告

第 4.4 章 [[18_第四部分_04_imgui_internal导读]] 是第四部分的收官——

- `imgui_internal.h` 全文 SECTION 巡回（28 个 SECTION）。
- 50 个高频 internal 函数清单（按使用频率排序）。
- `ImGuiContext` 字段全景图（>4500 字节，按 12 类职能）。
- **写魔改代码的 4 条铁律**：
  1. 不要持有 `ImGuiWindow*` 跨帧。
  2. `ImGuiID` 不可序列化。
  3. 内部字段写入必须包裹 `IMGUI_VERSION_NUM` 守护。
  4. 自定义控件统一走 `ItemAdd + ItemHoverable + ButtonBehavior` 三段式。
- "版本漂移"防御实战：4 个真实案例（哪些 internal 字段在 1.89~1.92 之间被改名/改类型/语义变化）。

读完后你就拥有了"读 imgui_internal.h 的导览图"——为第 5 章魔改实战做最终准备。
