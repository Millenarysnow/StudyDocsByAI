# 第 4.2 章 · `ImGuiWindow` 结构与 Z-Order 排序

> **本章目标**：把 `ImGuiWindow` 这个 ImGui 中**最复杂的内部结构**（100+ 字段）剖完。读完后你应该能：(1) 在调试器里看着任一 `ImGuiWindow` 字段说出它的用途；(2) 理解 `Begin()` 内部窗口创建/复用的全流程；(3) 画出 `g.Windows` / `WindowsFocusOrder` / `Windows.SortBuffer` 三个数组的协作关系；(4) 明白 `RootWindow` / `RootWindowPopupTree` / `RootWindowForNav` 等 5 个 root 链各自的语义。
>
> **本章对应源码**：`imgui_internal.h:2597~2648`（`ImGuiWindowTempData`）、`imgui_internal.h:2651~2779`（`ImGuiWindow` 完整定义，130 行字段）、`imgui.cpp:[SECTION] WINDOW FOCUS`（`FocusWindow / FocusTopMostWindowUnderOne / BringWindowToFocusFront`）、`imgui.cpp:[SECTION] MAIN CODE` 内的 `Begin()` 实现（约 6500+ 行）、`imgui.cpp:[SECTION] INITIALIZATION, SHUTDOWN` 中的 `Shutdown()` 见第 2.3 章。
>
> **前置阅读**：第 4.1 章（ID Stack）、第 2.2 章（ImGuiStorage）、第 3.1 章（一帧旅程）。

---

## 4.2.0 一图概览：ImGuiWindow 在 ImGui 中的位置

```
┌─────────────────────────────────────────────────────────────────────┐
│                       ImGuiContext                                   │
│                                                                      │
│  ImVector<ImGuiWindow*>     Windows                                  │  ← 所有窗口（绘制顺序）
│  ImVector<ImGuiWindow*>     WindowsFocusOrder                        │  ← Root 窗口（焦点顺序）
│  ImVector<ImGuiWindow*>     WindowsTempSortBuffer                    │  ← EndFrame 排序临时
│  ImGuiStorage               WindowsById                               │  ← ID → 窗口* 映射
│  ImGuiWindow*               CurrentWindow                            │  ← 当前 Begin 中的窗口
│  ImGuiWindow*               HoveredWindow                            │  ← 鼠标 hover 的窗口
│  ImGuiWindow*               NavWindow                                 │  ← 当前焦点窗口
│  ImGuiWindow*               MovingWindow                              │  ← 用户正在拖动的窗口
│  ImGuiWindow*               WheelingWindow                           │  ← 滚轮锁定的窗口
│  ImVector<ImGuiWindowStackData> CurrentWindowStack                   │  ← Begin/End 嵌套栈
└─────────────────────────────────────────────────────────────────────┘
```

ImGui 用 **5 个全局指针 + 4 个 ImVector + 1 个 Storage** 管理所有窗口。每个 `ImGuiWindow` 既出现在 `Windows`（绘制顺序）也可能出现在 `WindowsFocusOrder`（焦点顺序）。

---

## 4.2.1 `ImGuiWindow` 字段全景：8 个职能分组

打开 `imgui_internal.h:2651`。`ImGuiWindow` 有约 130 个字段——直接读会窒息。按职能分组：

### 分组 1：身份与 Flags（约 10 字段）

```cpp
ImGuiContext*       Ctx;                 // 所属 Context
char*               Name;                 // 窗口名（IM_ALLOC 复制的，独立所有权）
ImGuiID             ID;                   // == ImHashStr(Name, 0, 0)
ImGuiWindowFlags    Flags;                // ImGuiWindowFlags_NoTitleBar 等
ImGuiChildFlags     ChildFlags;           // 子窗口特有 flag
ImGuiViewportP*     Viewport;             // 所属 viewport（Multi-Viewport 模式下可能是副 viewport）
int                 NameBufLen;
ImGuiID             MoveId;               // == GetID("#MOVE")，用于拖动判定
ImGuiID             ChildId;              // 在父窗口中的 item ID
ImGuiID             PopupId;
```

**最关键的 3 个**：

- `Name`：窗口名字串，IM_ALLOC 复制，窗口生命周期内有效。
- `ID`：`ImHashStr(Name, 0, 0)`——身份。Window 第一次创建时算一次，跨帧不变。
- `MoveId`：用于"拖动标题栏"判定。`Hash("#MOVE", window_id)`。

### 分组 2：几何（约 25 字段）

```cpp
ImVec2  Pos;                       // 屏幕位置（已四舍五入到像素）
ImVec2  Size;                      // 当前大小（折叠时只是标题栏高）
ImVec2  SizeFull;                  // 非折叠时的完整大小
ImVec2  ContentSize;               // 内容大小（用于 scroll 范围）
ImVec2  ContentSizeIdeal;
ImVec2  ContentSizeExplicit;
ImVec2  WindowPadding;
float   WindowRounding;
float   WindowBorderSize;
float   TitleBarHeight, MenuBarHeight;
float   DecoOuterSizeX1, DecoOuterSizeY1;   // 左/上装饰大小
float   DecoOuterSizeX2, DecoOuterSizeY2;   // 右/下装饰大小
float   DecoInnerSizeX1, DecoInnerSizeY1;
ImVec2  Scroll, ScrollMax;
ImVec2  ScrollTarget;
ImVec2  ScrollTargetCenterRatio;
ImVec2  ScrollTargetEdgeSnapDist;
ImVec2  ScrollbarSizes;
bool    ScrollbarX, ScrollbarY;
// 多个 Rect 字段（见下方）
ImRect  OuterRectClipped;
ImRect  InnerRect;
ImRect  InnerClipRect;
ImRect  WorkRect;
ImRect  ParentWorkRect;
ImRect  ClipRect;
ImRect  ContentRegionRect;
```

**核心几何字段**：

- `Pos / Size`：窗口左上角 + 整体尺寸。
- `SizeFull`：折叠 vs 展开时区分——`Collapsed=true` 时 `Size.y` 只是标题栏高，`SizeFull.y` 是真正高度。
- `ContentSize`：内容大小（可滚动区域），用于自动调整大小 + scroll bar 计算。

**6 个 Rect 字段**（窗口的"嵌套矩形"）：

```
┌─ OuterRectClipped ────────────────────┐  ← 整个窗口（含装饰，被父裁剪）
│  ┌─ InnerRect ──────────────────────┐ │  ← 减去标题栏 / 菜单栏 / 滚动条
│  │  ┌─ InnerClipRect ──────────┐    │ │  ← InnerRect 缩 WindowPadding/2
│  │  │  ┌─ WorkRect ──────────┐ │    │ │  ← InnerClipRect 缩 WindowPadding（内容区）
│  │  │  │  控件正常画在这里    │ │    │ │
│  │  │  └────────────────────┘ │    │ │
│  │  └─────────────────────────┘    │ │
│  └─────────────────────────────────┘ │
│  Scrollbar / Title bar / Menu bar    │
└──────────────────────────────────────┘
```

注释 `imgui_internal.h:2722` 说：

> `// The best way to understand what those rectangles are is to use the 'Metrics->Tools->Show Windows Rectangles' viewer.`

——在 Demo → Metrics 里把它们可视化，比读字段名直观 10 倍。

### 分组 3：状态 Flags（约 15 字段）

```cpp
bool    Active;                    // 本帧调用了 Begin()
bool    WasActive;                  // 上帧 Active
bool    WriteAccessed;              // 任何 widget 调用了 GetCurrentWindow() 或类似
bool    Collapsed;                  // 折叠
bool    WantCollapseToggle;
bool    SkipItems;                  // 本帧 widget 都跳过（窗口不可见或折叠）
bool    SkipRefresh;
bool    Appearing;                  // 本帧首次出现
bool    Hidden;                     // HiddenFramesXxx > 0
bool    IsFallbackWindow;           // == 隐式 "Debug##Default"
bool    IsExplicitChild;
bool    HasCloseButton;
signed char ResizeBorderHovered;
signed char ResizeBorderHeld;
short   BeginCount;
ImS8    HiddenFramesCanSkipItems;
ImS8    HiddenFramesCannotSkipItems;
ImS8    HiddenFramesForRenderOnly;
ImS8    DisableInputsFrames;
```

**最重要的 2 个**：

- **`SkipItems`**：每个 widget 函数开头检查这个 flag——`true` 时直接 return，跳过所有计算。窗口被 collapse、隐藏、太小等情况下会设置。
- **`Active` vs `WasActive`**：`Active` 是本帧"是否被 Begin"，`WasActive` 是上帧的——用于检测"窗口刚刚关闭"。

```cpp
// 典型 widget 函数开头
bool ImGui::Button(const char* label) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;   // 早退
    // ...
}
```

### 分组 4：层级与焦点链（5 个 root 指针）

```cpp
ImGuiWindow*    ParentWindow;                       // 直接父
ImGuiWindow*    ParentWindowInBeginStack;
ImGuiWindow*    RootWindow;                          // 跨子窗口的根
ImGuiWindow*    RootWindowPopupTree;                 // 跨 popup 的根
ImGuiWindow*    RootWindowForTitleBarHighlight;      // 标题高亮的根
ImGuiWindow*    RootWindowForNav;                    // 导航的根
ImGuiWindow*    ParentWindowForFocusRoute;           // 焦点路由的"逻辑父"
```

**这 5 个 root 是 ImGuiWindow 最容易混淆的地方**——下面专门展开。

### 分组 5：Z-Order 与焦点（约 5 字段）

```cpp
short   BeginCount;                          // 本帧 Begin 次数
short   BeginCountPreviousFrame;
short   BeginOrderWithinParent;              // 在父窗口里的 Begin 序号
short   BeginOrderWithinContext;             // 在整个 Context 里的 Begin 序号
short   FocusOrder;                           // 在 WindowsFocusOrder[] 里的索引
```

`FocusOrder` 是 `WindowsFocusOrder` 数组里的索引——`FocusWindow` 调用时会更新这个值，让窗口"提升"到栈顶。

### 分组 6：Settings 与持久化（约 5 字段）

```cpp
int             SettingsOffset;       // SettingsWindows[] 中的偏移
ImGuiStorage    StateStorage;         // 跨帧 KV 存储（TreeNode 展开等）
ImVector<ImGuiOldColumns> ColumnsStorage;
float           FontWindowScale;
int             LastFrameActive;       // 上次 Active 的帧号
float           LastTimeActive;        // 上次 Active 的时间戳
```

`StateStorage` 是第 2.2 章讲的 `ImGuiStorage`——每个窗口自带一个 KV 存储。TreeNode 的展开状态、CollapsingHeader 的状态、自定义控件的状态都存这里。

### 分组 7：DrawList 与 ID Stack

```cpp
ImVector<ImGuiID>   IDStack;
ImGuiWindowTempData DC;                  // 逐帧重置的临时数据
ImDrawList*         DrawList;            // == &DrawListInst
ImDrawList          DrawListInst;        // 实际的 ImDrawList 实例
```

**`IDStack` 是第 4.1 章的核心**——窗口的 ID 栈。

**`DrawList` vs `DrawListInst`**：`DrawList` 是指针，指向 `&DrawListInst`。为什么不直接用 `DrawListInst`？因为某些**内部代码**已经用 `window->DrawList->XXX` 的形式（指针）写——为了兼容性保留指针。

### 分组 8：导航 (Nav) 与 Misc

```cpp
ImGuiWindow*    NavLastChildNavWindow;
ImGuiID         NavLastIds[ImGuiNavLayer_COUNT];   // 每层最后一个有焦点的 widget
ImRect          NavRectRel[ImGuiNavLayer_COUNT];
ImVec2          NavPreferredScoringPosRel[ImGuiNavLayer_COUNT];
ImGuiID         NavRootFocusScopeId;

int             MemoryDrawListIdxCapacity;
int             MemoryDrawListVtxCapacity;
bool            MemoryCompacted;
```

**`NavLastIds[2]`**：每层（Main / Menu）最后聚焦的 widget ID——切换窗口回来时恢复焦点位置。

---

## 4.2.2 `ImGuiWindowTempData`（DC）：逐帧临时数据

`imgui_internal.h:2597~2648` 定义了 `ImGuiWindowTempData`。它存放**只在当前帧有效**的窗口状态。

```cpp
struct ImGuiWindowTempData {
    // ===== Layout =====
    ImVec2  CursorPos;              // 下一个 widget 的位置
    ImVec2  CursorPosPrevLine;
    ImVec2  CursorStartPos;          // Begin() 后的初始位置
    ImVec2  CursorMaxPos;            // 用于自动调整大小
    ImVec2  IdealMaxPos;
    ImVec2  CurrLineSize;            // 当前行的尺寸
    ImVec2  PrevLineSize;
    float   CurrLineTextBaseOffset;
    float   PrevLineTextBaseOffset;
    bool    IsSameLine;
    bool    IsSetPos;
    ImVec1  Indent;                  // 缩进量
    ImVec1  ColumnsOffset;
    ImVec1  GroupOffset;
    
    // ===== Nav =====
    ImGuiNavLayer NavLayerCurrent;
    short   NavLayersActiveMask;
    short   NavLayersActiveMaskNext;
    bool    NavIsScrollPushableX;
    bool    NavHideHighlightOneFrame;
    bool    NavWindowHasScrollY;
    
    // ===== Misc =====
    bool    MenuBarAppending;
    ImVec2  MenuBarOffset;
    ImGuiMenuColumns MenuColumns;
    int     TreeDepth;
    ImU32   TreeHasStackDataDepthMask;
    ImU32   TreeRecordsClippedNodesY2Mask;
    ImVector<ImGuiWindow*> ChildWindows;
    ImGuiStorage* StateStorage;     // 通常 == &window->StateStorage
    ImGuiOldColumns* CurrentColumns;
    int     CurrentTableIdx;
    ImGuiLayoutType LayoutType;     // Horizontal / Vertical
    ImGuiLayoutType ParentLayoutType;
    
    // ===== Stack =====
    float   ItemWidth;
    float   ItemWidthDefault;
    float   TextWrapPos;
    ImVector<float> ItemWidthStack;
    ImVector<float> TextWrapPosStack;
};
```

**`DC.CursorPos` 是 Layout 系统的核心**——下个 widget 该画在哪里。每个 widget 提交后通过 `ItemSize` 推进 CursorPos。

### "DC" 名字的由来

注释：

> `// Transient per-window data, reset at the beginning of the frame. This used to be called ImGuiDrawContext, hence the DC variable name in ImGuiWindow.`

历史名字 `ImGuiDrawContext`——简写 DC。第 5.1 章会专题讲 Layout 系统时再深入。

---

## 4.2.3 `Begin()` 内部：窗口创建与复用

`Begin()` 的全流程超过 800 行——这里给一个简化版本：

```cpp
bool ImGui::Begin(const char* name, bool* p_open, ImGuiWindowFlags flags)
{
    ImGuiContext& g = *GImGui;
    
    // ① 找窗口 / 创建窗口
    ImGuiWindow* window = FindWindowByName(name);
    bool window_just_created = (window == NULL);
    if (window_just_created)
        window = CreateNewWindow(name, flags);
    
    // ② 应用 flags
    if (flags != window->Flags)
        window->Flags = flags;
    
    // ③ 检查是否首帧 / 重新激活
    if (!window->WasActive) {
        window->Appearing = true;
        // 应用持久化的 Pos/Size 等
        ApplyWindowSettings(window, ...);
    }
    
    // ④ 处理 Parent / Root 链
    ImGuiWindow* parent_window = ...;   // 来自 g.CurrentWindowStack
    SetupParents(window, parent_window);   // ParentWindow / RootWindow / ...
    
    // ⑤ 把窗口加入 g.Windows（如果是首帧）
    if (window_just_created) {
        g.Windows.push_back(window);
        g.WindowsById.SetVoidPtr(window->ID, window);
    }
    
    // ⑥ 推入 CurrentWindowStack
    g.CurrentWindowStack.push_back({ window, ... });
    g.CurrentWindow = window;
    
    // ⑦ 重置 DC（逐帧数据）
    window->IDStack.resize(1);
    window->IDStack[0] = window->ID;   // 栈底是窗口 ID
    window->DC.CursorPos = window->Pos + window->WindowPadding;
    window->DC.CursorMaxPos = window->DC.CursorPos;
    // ... 其他 DC 字段重置
    
    // ⑧ 更新窗口位置/大小（处理 Move / Resize）
    UpdateWindowManualResize(window, ...);
    
    // ⑨ 计算 OuterRectClipped / InnerRect / WorkRect 等
    UpdateWindowRects(window);
    
    // ⑩ 如果折叠/隐藏，设置 SkipItems
    if (window->Collapsed || ...) window->SkipItems = true;
    
    // ⑪ 画窗口背景 + 标题栏 + 边框
    if (!window->SkipItems) {
        RenderWindowDecorations(window, ...);
    }
    
    return !window->SkipItems;
}
```

### `FindWindowByName`

```cpp
ImGuiWindow* FindWindowByName(const char* name) {
    ImGuiID id = ImHashStr(name, 0);
    return (ImGuiWindow*)g.WindowsById.GetVoidPtr(id);
}
```

——用 `ImGuiStorage` 的 ID→ptr 映射 O(log N) 查找。

### `CreateNewWindow`

```cpp
ImGuiWindow* CreateNewWindow(const char* name, ImGuiWindowFlags flags) {
    ImGuiWindow* window = IM_NEW(ImGuiWindow)(g, name);
    window->Flags = flags;
    window->ID = ImHashStr(name, 0);
    g.WindowsById.SetVoidPtr(window->ID, window);
    
    // 加载持久化的 Pos / Size 等
    if (ImGuiWindowSettings* settings = FindWindowSettings(window->ID)) {
        window->Pos = ImVec2(settings->Pos);
        window->Size = ImVec2(settings->Size);
        window->Collapsed = settings->Collapsed;
        // ...
    }
    
    return window;
}
```

`IM_NEW(ImGuiWindow)`——分配 + 构造。`ImGuiWindow::ImGuiWindow` 会零初始化所有字段 + 默认 Pos/Size。

随后 `FindWindowSettings` 查 `g.SettingsWindows`（`ImChunkStream<ImGuiWindowSettings>`）找该 ID 的持久化数据。如果上次会话保存了这个窗口的位置，恢复它。

### `g.WindowsById`：ID → ImGuiWindow*

```cpp
ImGuiStorage WindowsById;   // ImGuiID → void* (实际是 ImGuiWindow*)
```

——`O(log N)` 查找。每次 `FindWindowByName` 都通过它。

---

## 4.2.4 5 个 Root 指针的语义

`imgui_internal.h:2746~2750`：

```cpp
ImGuiWindow*    ParentWindow;
ImGuiWindow*    ParentWindowInBeginStack;
ImGuiWindow*    RootWindow;                     // ← 跨 child 的根
ImGuiWindow*    RootWindowPopupTree;             // ← 跨 popup 的根
ImGuiWindow*    RootWindowForTitleBarHighlight;
ImGuiWindow*    RootWindowForNav;
ImGuiWindow*    ParentWindowForFocusRoute;
```

每个的语义：

### `ParentWindow`

**直接父窗口**——子窗口的"调用栈父"。

```cpp
Begin("Outer");          // window = Outer，ParentWindow = NULL
    Begin("InnerChild", ChildWindow);  // window = InnerChild，ParentWindow = Outer
    End();
End();
```

### `ParentWindowInBeginStack`

通常和 `ParentWindow` 相同——但 `Popup` 窗口有时不一样。

### `RootWindow`：跨 child 的根

**只跨"BeginChild"的链**——遇到 popup 不跳过。

```cpp
Begin("Outer");                            // RootWindow = Outer
    BeginChild("Child1");                   // RootWindow = Outer
        BeginChild("Grandchild");            // RootWindow = Outer
        EndChild();
    EndChild();
End();

Begin("OtherWindow");                       // RootWindow = OtherWindow
    OpenPopup("Popup");                      // 触发 popup
    BeginPopup("Popup");                     // RootWindow = Popup（popup 是独立 root）
    EndPopup();
End();
```

——主要用于"窗口 Z-Order 排序"和"焦点"。每个 root 是一个独立的"焦点链"。

### `RootWindowPopupTree`：跨 popup 的根

**跨 child + popup 都跳过**——只到顶级窗口。

```cpp
Begin("Outer");                              // RootWindowPopupTree = Outer
    OpenPopup("Popup");
    BeginPopup("Popup");                       // RootWindowPopupTree = Outer（跨 popup）
        BeginChild("PopupChild");
        EndChild();                              // RootWindowPopupTree = Outer
    EndPopup();
End();
```

用途：菜单链导航（`Outer → Popup → PopupChild`）共享一个 root，键盘 Tab 不跳出菜单组。

### `RootWindowForTitleBarHighlight`

——决定"标题栏哪个窗口显示 Active 颜色"。例如有 Modal 时，子 modal 的标题应该用 Active 颜色，但**它的父**（背景中的主 modal）也应该高亮。这个字段指向"应该高亮"的根。

### `RootWindowForNav`

——导航（Tab / 方向键）的"边界"。Tab 不跨过这个根。

### `ParentWindowForFocusRoute`

——`Shortcut(...)` 路由用的"逻辑父"。**手动设置**，让"工具窗口"逻辑上属于"文档窗口"——这样 Ctrl+S 等快捷键能从工具窗传到文档窗。

```cpp
ImGui::Begin("ToolPanel");
ImGui::SetNextWindowParentOnFocus(document_window);   // 把 ToolPanel 链到 document
ImGui::End();
```

---

## 4.2.5 `g.Windows`：绘制顺序

`imgui_internal.h:2225~2227`：

```cpp
ImVector<ImGuiWindow*> Windows;                  // 所有窗口，绘制顺序（背景到前景）
ImVector<ImGuiWindow*> WindowsFocusOrder;        // Root 窗口，焦点顺序（背景到前景）
ImVector<ImGuiWindow*> WindowsTempSortBuffer;    // EndFrame 排序临时
```

### `g.Windows` 的特性

- 包含**所有**窗口（包括 child / popup / tooltip）。
- 顺序 = 绘制顺序——后画的盖在前画的上面。
- `Render` 时按这个顺序遍历。
- `EndFrame` 中重新排序，保证父窗口在子窗口之前。

### `EndFrame` 中的排序逻辑

`imgui.cpp` 中的简化伪代码：

```cpp
g.WindowsTempSortBuffer.resize(0);
g.WindowsTempSortBuffer.reserve(g.Windows.Size);

// 遍历"焦点顺序"中的 root，递归把它们的 child 加入 temp buffer
for (ImGuiWindow* root : g.WindowsFocusOrder) {
    if (!root->Active || root->RootWindow != root) continue;
    AddWindowToSortBuffer(&g.WindowsTempSortBuffer, root);
}

g.Windows.swap(g.WindowsTempSortBuffer);   // 把排好序的赋回去
```

`AddWindowToSortBuffer` 递归：

```cpp
void AddWindowToSortBuffer(ImVector<ImGuiWindow*>* out, ImGuiWindow* window) {
    out->push_back(window);   // 父先加入
    for (ImGuiWindow* child : window->DC.ChildWindows) {
        // 排序 child（按 BeginOrderWithinParent）
        if (child->Active)
            AddWindowToSortBuffer(out, child);
    }
}
```

——**深度优先 + 父先 + 子按 Begin 顺序**。最终绘制顺序：父 → child 1 → child 2 → ... → 下一个父。

### `g.WindowsFocusOrder`

只包含 **Root 窗口**（即 `RootWindow == this` 的窗口）。child 不进入。

排序：**最后聚焦的在末尾**——最末是"最前景"。

`FocusWindow(window)` 调用时：

```cpp
void FocusWindow(ImGuiWindow* window) {
    if (window != g.NavWindow) {
        g.NavWindow = window;
        // ...
    }
    // 把 window 移到 WindowsFocusOrder 末尾
    int order = window->FocusOrder;
    if (order != g.WindowsFocusOrder.Size - 1) {
        g.WindowsFocusOrder.erase(g.WindowsFocusOrder.Data + order);
        g.WindowsFocusOrder.push_back(window);
        // 更新所有受影响窗口的 FocusOrder 字段
    }
}
```

---

## 4.2.6 不同窗口类型的 Z-Order 优先级

ImGui 内部的"绘制顺序"由几个机制叠加：

1. **WindowsFocusOrder**：基础顺序——后聚焦的在上。
2. **TopMost flag**：某些窗口（Modal / Tooltip）强制置顶。
3. **`g.Windows` 排序**：父先 → 子后。
4. **DrawData 的 layer**（第 3.1 章）：Background → Default → Foreground → Tooltip。

### 各类窗口的实际 Z 行为

| 窗口类型 | 标志 | Z 行为 |
|---|---|---|
| 普通窗口 | （无） | 按 Focus Order |
| 子窗口 | `_ChildWindow` | 紧跟父之后 |
| 弹出菜单 | `_Popup` | 在打开它的父之上，关闭时栈式弹出 |
| 模态对话框 | `_Modal` | 强制置顶 + 其他窗口被 Dim |
| Tooltip | `_Tooltip` | 始终最前（Tooltip Layer） |
| Background DrawList | (BgFgDrawLists[0]) | 主窗口之下 |
| Foreground DrawList | (BgFgDrawLists[1]) | 主窗口之上 |

### Modal 的"染色"机制

```cpp
g.DimBgRatio = 1.0f;   // Modal 激活时
```

EndFrame 中如果有 Modal，所有非 Modal 窗口下面画一层半透明黑——视觉上"模态"。

---

## 4.2.7 `MovingWindow / WheelingWindow / HoveredWindow` 三个特殊指针

```cpp
ImGuiWindow* HoveredWindow;
ImGuiWindow* HoveredWindowUnderMovingWindow;
ImGuiWindow* HoveredWindowBeforeClear;
ImGuiWindow* MovingWindow;
ImGuiWindow* WheelingWindow;
```

### `HoveredWindow`：鼠标悬停的窗口

每帧 NewFrame 中重新计算——遍历 `g.Windows`（**逆序**！前景到背景），找第一个鼠标矩形包含的窗口。

为什么逆序：前景窗口优先 hover。

### `MovingWindow`：用户正在拖动的窗口

```cpp
// imgui.cpp:5234 中
void ImGui::StartMouseMovingWindow(ImGuiWindow* window) {
    FocusWindow(window);
    SetActiveID(window->MoveId, window);
    g.ActiveIdClickOffset = g.IO.MouseClickedPos[0] - window->RootWindow->Pos;
    g.MovingWindow = window;
}
```

用户点击标题栏 → `MovingWindow` 被设为该窗口。

每帧 `UpdateMouseMovingWindowNewFrame`（`imgui.cpp:5273`）处理拖动：

```cpp
if (g.IO.MouseDown[0] && IsMousePosValid(&g.IO.MousePos)) {
    ImVec2 pos = g.IO.MousePos - g.ActiveIdClickOffset;
    SetWindowPos(moving_window, pos, ImGuiCond_Always);
    FocusWindow(g.MovingWindow);
} else {
    StopMouseMovingWindow();
    ClearActiveID();
}
```

——鼠标按住：更新位置 + 维持焦点。鼠标松开：清掉 `MovingWindow` 和 `ActiveId`。

### `WheelingWindow`：滚轮锁定窗口

用户用滚轮滚动一个窗口——即使滚动过程中鼠标移到了相邻窗口，仍然滚原来那个。这是 ImGui 的"反误操作"特性：

```cpp
ImGuiWindow* WheelingWindow;
ImVec2       WheelingWindowRefMousePos;
int          WheelingWindowStartFrame;
float        WheelingWindowReleaseTimer;
```

`UpdateMouseWheel` 内部：

```cpp
if (mouse_just_started_wheeling) {
    g.WheelingWindow = FindBestWheelingWindow(wheel);
    g.WheelingWindowRefMousePos = io.MousePos;
}
// 后续滚动事件总是发到 g.WheelingWindow
```

`g.WheelingWindowReleaseTimer` 倒计时——鼠标静止 / 滚动停止 0.x 秒后释放锁。

---

## 4.2.8 `g.CurrentWindowStack`：Begin / End 嵌套栈

```cpp
// imgui_internal.h:1426
struct ImGuiWindowStackData {
    ImGuiWindow*            Window;
    ImGuiLastItemData       ParentLastItemDataBackup;
    ImGuiErrorRecoveryState StackSizesInBegin;
    bool                    DisabledOverrideReenable;
    float                   DisabledOverrideReenableAlphaBackup;
};
```

`g.CurrentWindowStack` = `ImVector<ImGuiWindowStackData>`。每个 Begin 入栈、每个 End 出栈。

```cpp
Begin("A");           // CurrentWindowStack = [A]
    Begin("B");          // CurrentWindowStack = [A, B]
    End();                // CurrentWindowStack = [A]
End();                // CurrentWindowStack = []
```

**`StackSizesInBegin`** 用于错误恢复（参见 0.2 章）——记录 Begin 时各栈的大小，End 时检查是否 push/pop 平衡。如果用户中途忘记 PopID，End 检测到 IDStack 多了一项，自动 PopID。

---

## 4.2.9 `g.Hooks` / 窗口生命周期事件

`imgui_internal.h:2173`：

```cpp
enum ImGuiContextHookType {
    ImGuiContextHookType_NewFramePre,
    ImGuiContextHookType_NewFramePost,
    ImGuiContextHookType_EndFramePre,
    ImGuiContextHookType_EndFramePost,
    ImGuiContextHookType_RenderPre,
    ImGuiContextHookType_RenderPost,
    ImGuiContextHookType_Shutdown,
    ImGuiContextHookType_PendingRemoval_,
};
```

——**Context 级**钩子，不是 Window 级。但你可以在钩子里读 `g.Windows` 等，间接拦截窗口操作。

例如 `imgui_test_engine` 用 `NewFramePost` 收集所有窗口的 item 信息。

`g.SettingsHandlers` 是 Window 级的"持久化钩子":

```cpp
struct ImGuiSettingsHandler {
    const char* TypeName;     // 例如 "Window" / "Table"
    ImGuiID     TypeHash;
    void (*ClearAllFn)(...);
    void (*ReadOpenFn)(...);
    void (*ReadLineFn)(...);
    void (*ApplyAllFn)(...);
    void (*WriteAllFn)(...);
    void*       UserData;
};
```

`Initialize` 注册了 "Window" handler——负责把 `g.SettingsWindows` 序列化到 .ini，并在加载时恢复每个窗口的 Pos/Size/Collapsed。

---

## 4.2.10 窗口的内存压缩：`MemoryCompacted`

```cpp
bool MemoryCompacted;
int  MemoryDrawListIdxCapacity;
int  MemoryDrawListVtxCapacity;
```

ImGui 有一个"垃圾回收"机制（GC）——长时间不用的窗口被压缩内存。

`io.ConfigMemoryCompactTimer`（默认 60 秒）—— 60 秒未 Active 的窗口，下次 NewFrame 时被压缩：

```cpp
// 简化
if (window->LastTimeActive < g.Time - 60.0f && !window->MemoryCompacted) {
    // 备份 capacity（备份是为了重新激活时能 reserve 回原大小）
    window->MemoryDrawListIdxCapacity = window->DrawList->IdxBuffer.Capacity;
    window->MemoryDrawListVtxCapacity = window->DrawList->VtxBuffer.Capacity;
    
    // 释放
    window->DrawList->_ClearFreeMemory();
    window->IDStack.clear();
    window->DC.ChildWindows.clear();
    window->ColumnsStorage.clear();
    
    window->MemoryCompacted = true;
}
```

`_ClearFreeMemory` 是 `ImDrawList` 的"释放底层内存"版本（与 `Clear` 不同——后者只 resize(0) 不释放）。

下次窗口 Active 时，`reserve` 回原大小，避免几次扩容（参见 `MemoryDrawListIdxCapacity`）。

---

## 4.2.11 实战：在 Hazel 编辑器里查窗口状态

```cpp
// 在 OnImGuiRender 里：
ImGuiContext& g = *ImGui::GetCurrentContext();
ImGui::Begin("Window Inspector");
ImGui::Text("Total windows: %d", g.Windows.Size);
ImGui::Text("Active windows: %d", g.WindowsActiveCount);
ImGui::Text("Hovered: %s", g.HoveredWindow ? g.HoveredWindow->Name : "(none)");
ImGui::Text("Focused (NavWindow): %s", g.NavWindow ? g.NavWindow->Name : "(none)");
ImGui::Text("Moving: %s", g.MovingWindow ? g.MovingWindow->Name : "(none)");
ImGui::Text("Wheeling: %s", g.WheelingWindow ? g.WheelingWindow->Name : "(none)");

ImGui::Separator();
for (ImGuiWindow* w : g.Windows) {
    if (!w->Active) continue;
    ImGui::Text("'%s' [Active=%d, Skip=%d, Hidden=%d, Collapsed=%d]",
        w->Name, w->Active, w->SkipItems, w->Hidden, w->Collapsed);
    ImGui::Text("  Pos=(%.0f,%.0f), Size=(%.0f,%.0f)", 
        w->Pos.x, w->Pos.y, w->Size.x, w->Size.y);
    ImGui::Text("  Vtx=%d, Idx=%d", 
        w->DrawList->VtxBuffer.Size, 
        w->DrawList->IdxBuffer.Size);
}
ImGui::End();
```

——这就是 Metrics 窗的"Windows" 子标签的核心。

---

## 4.2.12 创建自定义窗口的几种姿势

### 姿势 1：标准窗口

```cpp
ImGui::Begin("MyWindow");
// ...
ImGui::End();
```

——产生一个完整的浮动窗口。

### 姿势 2：子窗口（嵌入）

```cpp
ImGui::Begin("Parent");
ImGui::BeginChild("MyChild", ImVec2(200, 100), ImGuiChildFlags_Border);
// ...
ImGui::EndChild();
ImGui::End();
```

——子窗口在父内部，独立 scroll region。

### 姿势 3：完全无边框无标题（用于自定义渲染区域）

```cpp
ImGui::SetNextWindowPos(ImVec2(0, 0));
ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
ImGui::Begin("MyCustomCanvas",
             nullptr,
             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | 
             ImGuiWindowFlags_NoBackground);
// 在全屏画布上绘制
ImGui::End();
```

——常用于"全屏 ImGui-only" 应用，或者编辑器的"主画布"层。

### 姿势 4：DockSpace（编辑器专用）

```cpp
ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
```

——把整个主 viewport 变成 docking 区。其他窗口可以拖进去。

第 5.4 章会详细讲 Hazel 编辑器布局。

---

## 4.2.13 本章小结

`ImGuiWindow` 的 8 个职能分组：

1. **身份**（Name/ID/Flags）——3 个最重要字段。
2. **几何**（Pos/Size/6 Rects）——用 Metrics → Show Rectangles 可视化。
3. **状态 Flags**（Active/SkipItems）——SkipItems 是热路径检查。
4. **层级链**（5 个 root 指针）——决定焦点 / Z-Order / Nav 边界。
5. **Z-Order 字段**（FocusOrder/BeginOrder）。
6. **Settings**（StateStorage/ColumnsStorage）。
7. **DrawList + IDStack + DC**——绘制 + ID + 逐帧临时数据。
8. **Nav + Misc**——键盘导航相关。

`g.Windows` / `g.WindowsFocusOrder` / `g.WindowsTempSortBuffer` 三个数组的协作：

- `Windows` = 所有窗口（绘制顺序，父先子后）。
- `WindowsFocusOrder` = root 窗口（焦点顺序，最近聚焦在末尾）。
- `WindowsTempSortBuffer` = EndFrame 中临时排序用。

5 个 Root 指针的语义记忆：

- `RootWindow` = 跨 child（不跨 popup）。
- `RootWindowPopupTree` = 跨 child + popup（最顶级）。
- `RootWindowForTitleBarHighlight` = 标题颜色。
- `RootWindowForNav` = 导航边界。
- `ParentWindowForFocusRoute` = 手动焦点链。

---

## 4.2.14 下一章预告

第 4.3 章 [[17_第四部分_03_焦点ActiveID与Item状态机]] 会带你看 ImGui 里**最微妙**的状态机——

- `g.HoveredId` / `g.ActiveId` / `g.NavId` 三大 ID 的状态机。
- `g.LastItemData`（IsItemHovered / IsItemActive / IsItemEdited 的底层）。
- `ItemAdd` / `ItemSize` / `ItemHoverable` 的链式调用。
- `ButtonBehavior`：万能行为函数 + 12 个 ButtonFlags 标志位。
- 自定义 widget 的"三段式"模板。

读完后你应该能写出**任何**自定义 widget——按钮、滑块、节点编辑器的端口、时间线 keyframe——都用同一套模式。
