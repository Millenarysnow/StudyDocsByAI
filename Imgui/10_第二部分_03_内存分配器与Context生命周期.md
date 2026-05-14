# 第 2.3 章 · 内存分配器与 `ImGuiContext` 生命周期

> **本章目标**：把 ImGui 的"系统级生命周期"全链路讲清——从你调 `SetAllocatorFunctions` 那一刻起，到 `CreateContext` 创建出 4KB+ 的 Context、到 `NewFrame/EndFrame` 的逐帧清理、到 `DestroyContext` 释放一切——中间发生了什么。读完后你应该能：(1) 在多 DLL 引擎里**正确**集成 ImGui 不踩 GImGui 副本陷阱；(2) 实现 Hot Reload 时知道 Context 该如何挪窝；(3) 用 N Context 模型搞自动化测试或多窗口实例。
>
> **本章对应源码**：`imgui.cpp:1414~1453`（CONTEXT AND MEMORY ALLOCATORS 全 SECTION）、`imgui.cpp:4062~4116`（GetCurrentContext / SetCurrentContext / SetAllocatorFunctions / CreateContext / DestroyContext）、`imgui.cpp:4133~4350`（`ImGuiContext::ImGuiContext` 巨型构造器）、`imgui.cpp:4369~4427`（`ImGui::Initialize`）、`imgui.cpp:4430~4555`（`ImGui::Shutdown`）、`imgui_internal.h:[SECTION] ImGuiContext`（约 2192 行起的 4KB+ 字段定义）。
>
> **前置阅读**：第 0.3 章（内存模型 / IM_ALLOC 4 件套）、第 2.1 章（ImVector）、第 2.2 章（ImPool / ImChunkStream / ImGuiStorage）。

---

## 2.3.0 这一章解决的实际工程问题

很多读者在 Hazel 这种"应用 + 引擎 DLL + 编辑器 DLL + 游戏 DLL"的多模块工程里集成 ImGui 时，会撞到下面这些**经典坑**：

1. **集成完毕但崩**：`CreateContext` 在引擎 DLL 里调，应用层调 `ImGui::Begin` 就崩——因为应用层看到的 `GImGui` 是 NULL。
2. **Hot Reload 后崩**：编辑器 DLL 重新加载后，旧 ImGui 对象的指针还在引用已被 unload 的内存。
3. **内存分配器双重计数**：你给 `SetAllocatorFunctions` 注册了引擎的内存池，但发现某些分配仍走 `malloc`。
4. **多视口下 ImGui 调你 `Platform_CreateWindow` 时引擎已经在退出**——崩在不知所谓的位置。

每个问题都有"教科书答案"，但答案的前提是**你理解 Context 的生命周期**。这一章就讲清这些。

---

## 2.3.1 全景图：ImGui 系统的"生命线"

```
应用启动
    │
    ▼
┌──────────────────────────────────────────────┐
│  ① SetAllocatorFunctions(my_alloc, my_free)  │ ◀─ 必须在 CreateContext 之前
│     - 改三个 static 函数指针                    │
└──────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────┐
│  ② CreateContext()                            │
│     - IM_NEW(ImGuiContext)                    │ ◀─ 调 4000+ 字段构造器
│     - SetCurrentContext(ctx)                  │
│     - Initialize()                            │ ◀─ 注册 INI handler / 创建主 viewport
└──────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────┐
│  ③ Backend Init                                │
│     - ImGui_ImplXxx_Init                      │
│     - 设置 io.BackendFlags                     │
│     - 注册 PlatformIO 回调                      │
└──────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────┐
│  ④ 主循环（每一帧）                              │
│     - NewFrame()        ← 重置逐帧状态           │
│     - 用户构建 UI                                │
│     - EndFrame() / Render()                    │
│     - Backend RenderDrawData                  │
│     - swap chain present                       │
└──────────────────────────────────────────────┘
    │
    │ ... 大量帧循环 ...
    │
    ▼
┌──────────────────────────────────────────────┐
│  ⑤ Backend Shutdown                            │
│     - ImGui_ImplXxx_Shutdown                  │
│     - 必须在 DestroyContext 之前                 │
└──────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────┐
│  ⑥ DestroyContext()                            │
│     - SaveIniSettingsToDisk                    │
│     - Shutdown() ← 释放所有内部状态              │
│     - IM_DELETE(ctx)                           │
└──────────────────────────────────────────────┘
    │
    ▼
应用退出
```

**关键时序**：

- `SetAllocatorFunctions` 必须在 `CreateContext` **之前**——否则 Context 用 default malloc 分配，但你后面的代码用你的 free 释放，混乱。
- Backend Init 必须在 `CreateContext` **之后**——Backend 会读 `ImGui::GetIO()`。
- Backend Shutdown 必须在 `DestroyContext` **之前**——`Shutdown` 内有 IM_ASSERT 检查 `BackendXxxUserData == NULL`。

---

## 2.3.2 内存分配器：3 个 static 函数指针

打开 `imgui.cpp:1441~1453`，这是 ImGui 内存分配器的**全部基础设施**：

```cpp
// imgui.cpp:1441
// Memory Allocator functions. Use SetAllocatorFunctions() to change them.
// - You probably don't want to modify that mid-program, and if you use global/static e.g. ImVector<> instances you may need to keep them accessible during program destruction.
// - DLL users: read comments above.
#ifndef IMGUI_DISABLE_DEFAULT_ALLOCATORS
static void* MallocWrapper(size_t size, void* user_data) { IM_UNUSED(user_data); return malloc(size); }
static void  FreeWrapper(void* ptr, void* user_data)     { IM_UNUSED(user_data); free(ptr); }
#else
static void* MallocWrapper(size_t size, void* user_data) { IM_UNUSED(user_data); IM_UNUSED(size); IM_ASSERT(0); return NULL; }
static void  FreeWrapper(void* ptr, void* user_data)     { IM_UNUSED(user_data); IM_UNUSED(ptr); IM_ASSERT(0); }
#endif

static ImGuiMemAllocFunc    GImAllocatorAllocFunc = MallocWrapper;
static ImGuiMemFreeFunc     GImAllocatorFreeFunc  = FreeWrapper;
static void*                GImAllocatorUserData  = NULL;
```

**3 个 `static` 变量**：函数指针 + 函数指针 + 用户数据。`static` 意味着它们的可见范围**仅限于当前翻译单元（imgui.cpp）**。

### `IMGUI_DISABLE_DEFAULT_ALLOCATORS` 模式

```cpp
#ifndef IMGUI_DISABLE_DEFAULT_ALLOCATORS
static void* MallocWrapper(...) { return malloc(size); }
#else
static void* MallocWrapper(...) { IM_ASSERT(0); return NULL; }
#endif
```

如果你定义了 `IMGUI_DISABLE_DEFAULT_ALLOCATORS`，ImGui 不会 link `malloc` / `free`——`MallocWrapper` 变成"调用就 assert"的桩函数。这强制用户在 `CreateContext` 前调 `SetAllocatorFunctions`，否则一分配就崩。

**用途**：嵌入式平台 / 没有 `malloc` 的环境（虽然现代游戏机一般都有）/ 想强制审计所有分配。

### `SetAllocatorFunctions` 的实现

```cpp
// imgui.cpp:4081
void ImGui::SetAllocatorFunctions(ImGuiMemAllocFunc alloc_func, ImGuiMemFreeFunc free_func, void* user_data)
{
    GImAllocatorAllocFunc = alloc_func;
    GImAllocatorFreeFunc = free_func;
    GImAllocatorUserData = user_data;
}
```

**就这 3 行**。改 3 个 static 变量。无锁。

### `GetAllocatorFunctions`：跨 DLL 同步用

```cpp
// imgui.cpp:4089
void ImGui::GetAllocatorFunctions(ImGuiMemAllocFunc* p_alloc_func, ImGuiMemFreeFunc* p_free_func, void** p_user_data)
{
    *p_alloc_func = GImAllocatorAllocFunc;
    *p_free_func = GImAllocatorFreeFunc;
    *p_user_data = GImAllocatorUserData;
}
```

注释 `imgui.cpp:4088`：

> `// This is provided to facilitate copying allocators from one static/DLL boundary to another (e.g. retrieve default allocator of your executable address space)`

——**这是 DLL 多副本同步的关键 API**。后面会展开。

### 为什么用全局函数指针而不是抽象基类

OO 风格：

```cpp
class IAllocator {
    virtual void* Alloc(size_t) = 0;
    virtual void  Free(void*) = 0;
};
ImGui::SetAllocator(IAllocator* alloc);
```

ImGui 风格：

```cpp
ImGui::SetAllocatorFunctions(MyAlloc, MyFree, my_userdata);
```

**ImGui 选择函数指针的原因**：

1. 不需要 vtable —— 兼容 `-fno-rtti` 工程。
2. 不需要 `<memory>` / 不需要继承 —— 减少 STL 依赖。
3. 函数指针 + `void* user_data` 是 C 经典模式——任何语言（包括 C / Rust / Python）都能传过来。
4. 简单——3 行 setter 就解决。

代价：API 不太"现代 C++"。但在工业实用主义视角下完全可接受。

---

## 2.3.3 `ImGui::MemAlloc / MemFree`：用户暴露的入口

```cpp
// imgui.cpp 中（位置约 1455 起）
void* ImGui::MemAlloc(size_t size)
{
    void* ptr = (*GImAllocatorAllocFunc)(size, GImAllocatorUserData);
#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (ImGuiContext* ctx = GImGui)
        DebugAllocHook(&ctx->DebugAllocInfo, ctx->FrameCount, ptr, size);
#endif
    return ptr;
}

void ImGui::MemFree(void* ptr)
{
#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (ptr != NULL)
        if (ImGuiContext* ctx = GImGui)
            DebugAllocHook(&ctx->DebugAllocInfo, ctx->FrameCount, ptr, (size_t)-1);
#endif
    (*GImAllocatorFreeFunc)(ptr, GImAllocatorUserData);
}
```

**做的事**：

1. 转发到 `GImAllocatorAllocFunc` / `GImAllocatorFreeFunc`。
2. **如果有当前 Context**，调 `DebugAllocHook` 记录这次分配——给 Metrics 窗口的内存视图用。
3. `MemFree(NULL)` 是合法的（兼容 `free(NULL)` 语义）。

### `DebugAllocHook` 的工作

`DebugAllocInfo` 是 `ImGuiContext` 内的字段（参见 internal）。它记录"过去 N 帧的分配/释放"——给 Metrics 窗的"Memory" 子标签显示：

```
Frame 100: 23 allocs, 18 frees, +5 active
Frame 101: 18 allocs, 22 frees, -4 active
Frame 102: ...
```

如果你看到某帧"+1000 active"突然飙升，能直接定位到哪一帧出了什么操作。

### 为什么把 hook 放这里

把 hook 放在 `MemAlloc/MemFree` 而不是分配器函数指针内：
- **统一**：无论用户用什么分配器，hook 都能被调用。
- **避免污染分配器接口**：用户写自己的分配器函数时不需要懂 hook。

### 调试技巧：watch `DebugMemAllocCount` / `DebugMemFreeCount`

```cpp
// 在你的引擎主循环 Debug 构建里
ImGuiContext& g = *ImGui::GetCurrentContext();
HZ_CORE_TRACE("ImGui: alloc={} free={} active={}",
    g.DebugMemAllocCount,
    g.DebugMemFreeCount,
    g.DebugMemAllocCount - g.DebugMemFreeCount);
```

健康指标：

- 启动后前几帧 `active` 上涨（容器扩容、字体加载）。
- 稳定后 `active` 应**几乎不变**（< 10/秒级别的浮动）。
- 持续上涨 → 泄漏。

---

## 2.3.4 `IM_NEW / IM_DELETE` 的整合

回顾第 0.3 章的 4 件套：

```cpp
// imgui.h:2189~2193
#define IM_ALLOC(_SIZE)                     ImGui::MemAlloc(_SIZE)
#define IM_FREE(_PTR)                       ImGui::MemFree(_PTR)
#define IM_PLACEMENT_NEW(_PTR)              new(ImNewWrapper(), _PTR)
#define IM_NEW(_TYPE)                       new(ImNewWrapper(), ImGui::MemAlloc(sizeof(_TYPE))) _TYPE
template<typename T> void IM_DELETE(T* p)   { if (p) { p->~T(); ImGui::MemFree(p); } }
```

**完整的内存调用链**：

```
源码：IM_NEW(ImGuiWindow)("name")
  ↓ 宏展开
new (ImNewWrapper(), ImGui::MemAlloc(sizeof(ImGuiWindow))) ImGuiWindow("name")
  ↓ 编译器解析
  1. ImGui::MemAlloc(sizeof(ImGuiWindow))
     ↓
     (*GImAllocatorAllocFunc)(size, GImAllocatorUserData)
     ↓
     默认是 MallocWrapper → malloc(size)
     或用户的 HazelImGuiAlloc → 引擎内存池
  2. operator new(size, ImNewWrapper, void* ptr) → 返回 ptr（不分配）
  3. 在 ptr 上调用 ImGuiWindow::ImGuiWindow("name") 构造函数
```

**5 层间接**：宏 → 模板/inline 函数 → 全局函数 → 函数指针 → 用户实现。

但这 5 层在 Release 构建里被编译器**逐层内联**——最终只剩用户的 `malloc`/自定义实现的开销。

---

## 2.3.5 `CreateContext`：4 步走

```cpp
// imgui.cpp:4096
ImGuiContext* ImGui::CreateContext(ImFontAtlas* shared_font_atlas)
{
    ImGuiContext* prev_ctx = GetCurrentContext();
    ImGuiContext* ctx = IM_NEW(ImGuiContext)(shared_font_atlas);   // ① 分配 + 构造
    SetCurrentContext(ctx);                                         // ② 设当前
    Initialize();                                                   // ③ 子系统初始化
    if (prev_ctx != NULL)
        SetCurrentContext(prev_ctx); // Restore previous context if any, else keep new one.
    return ctx;
}
```

**4 个有效步骤**：

### ① `IM_NEW(ImGuiContext)(shared_font_atlas)`

- `IM_ALLOC(sizeof(ImGuiContext))` 走当前的全局分配器。
- 在该地址 placement new `ImGuiContext::ImGuiContext(shared_font_atlas)`。

`sizeof(ImGuiContext)` 在 1.92 大约 **4500~5500 字节**（取决于 docking / 平台）——这是你"从分配器拿走"的最大单块内存。

### ② `SetCurrentContext(ctx)`

```cpp
// imgui.cpp:4072
void ImGui::SetCurrentContext(ImGuiContext* ctx)
{
#ifdef IMGUI_SET_CURRENT_CONTEXT_FUNC
    IMGUI_SET_CURRENT_CONTEXT_FUNC(ctx); // For custom thread-based hackery you may want to have control over this.
#else
    GImGui = ctx;
#endif
}
```

**默认实现**：`GImGui = ctx`——一个全局指针赋值。

**`IMGUI_SET_CURRENT_CONTEXT_FUNC` 钩子**：用户在 `imconfig.h` 定义这个宏可以接管"如何设置当前 Context"。这正是 thread_local 模式下需要的：

```cpp
// imconfig.h
extern thread_local ImGuiContext* MyImGuiTLS;
#define GImGui MyImGuiTLS
#define IMGUI_SET_CURRENT_CONTEXT_FUNC(ctx) (MyImGuiTLS = ctx)
```

详见第 6 章多线程主题。

### ③ `ImGui::Initialize()`

`imgui.cpp:4369~4427` 是 `Initialize` 完整实现：

```cpp
void ImGui::Initialize()
{
    ImGuiContext& g = *GImGui;
    IM_ASSERT(!g.Initialized && !g.SettingsLoaded);

    // Add .ini handle for ImGuiWindow type
    {
        ImGuiSettingsHandler ini_handler;
        ini_handler.TypeName = "Window";
        ini_handler.TypeHash = ImHashStr("Window");
        ini_handler.ClearAllFn = WindowSettingsHandler_ClearAll;
        ini_handler.ReadOpenFn = WindowSettingsHandler_ReadOpen;
        ini_handler.ReadLineFn = WindowSettingsHandler_ReadLine;
        ini_handler.ApplyAllFn = WindowSettingsHandler_ApplyAll;
        ini_handler.WriteAllFn = WindowSettingsHandler_WriteAll;
        AddSettingsHandler(&ini_handler);
    }
    TableSettingsAddSettingsHandler();

    // Setup default localization table
    LocalizeRegisterEntries(GLocalizationEntriesEnUS, IM_COUNTOF(GLocalizationEntriesEnUS));

    // Setup default ImGuiPlatformIO clipboard/IME handlers.
    g.PlatformIO.Platform_GetClipboardTextFn = Platform_GetClipboardTextFn_DefaultImpl;
    g.PlatformIO.Platform_SetClipboardTextFn = Platform_SetClipboardTextFn_DefaultImpl;
    g.PlatformIO.Platform_OpenInShellFn      = Platform_OpenInShellFn_DefaultImpl;
    g.PlatformIO.Platform_SetImeDataFn       = Platform_SetImeDataFn_DefaultImpl;

    // Create default viewport
    ImGuiViewportP* viewport = IM_NEW(ImGuiViewportP)();
    viewport->ID = IMGUI_VIEWPORT_DEFAULT_ID;
    g.Viewports.push_back(viewport);
    g.TempBuffer.resize(1024 * 3 + 1, 0);

    // Build KeysMayBeCharInput[] lookup table (1 bit per named key)
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ...)
        if (...)
            g.KeysMayBeCharInput.SetBit(key);

    ImFontAtlas* atlas = g.IO.Fonts;
    g.DrawListSharedData.Context = &g;
    RegisterFontAtlas(atlas);

    g.Initialized = true;
}
```

**做了 6 件大事**：

1. **注册 `.ini` 处理器**：让 `Window` 和 `Table` 类型的 settings 能被序列化/反序列化。
2. **加载默认本地化表**：英文字符串。
3. **挂默认 PlatformIO 回调**：clipboard / IME / OpenInShell 的 Win32/Cocoa/Linux 默认实现。
4. **创建默认 Viewport**：主 viewport（ID = `IMGUI_VIEWPORT_DEFAULT_ID`）。`ImGuiViewportP` 是 internal 的子类。
5. **构造 `KeysMayBeCharInput` 位图**：哪些键按下时可能产生字符（A-Z / 0-9 / 标点等）。这个位图在 `UpdateInputEvents` 的 trickle 算法里用。
6. **注册字体图集**：把 `g.IO.Fonts` 加到 `g.FontAtlases` 列表里。

`g.Initialized = true` 后，Context 就准备好接收 `NewFrame` 调用了。

### ④ 恢复之前的 Context

```cpp
if (prev_ctx != NULL)
    SetCurrentContext(prev_ctx);
```

——如果你**已经有**一个 Context 在用，`CreateContext` 不会偷偷把它顶掉。新创建的 Context 你需要显式 `SetCurrentContext` 切换过去。

如果之前没有 Context（`prev_ctx == NULL`），新创建的就保持为当前 Context。

---

## 2.3.6 `ImGuiContext` 字段全景图（按职能分组）

打开 `imgui_internal.h:2192` 的 `struct ImGuiContext { ... }`——它有**接近 100 个字段**。直接读会昏。我们按"职能"分组讲。

### 分组 1：核心标识与帧时序（约 20 字段）

```cpp
bool      Initialized;                           // Initialize() 之后为 true
bool      WithinFrameScope;                      // NewFrame ~ EndFrame 之间
bool      WithinFrameScopeWithImplicitWindow;
bool      TestEngineHookItems;                   // imgui_test_engine 钩子
int       FrameCount;                             // 已调用 NewFrame 次数
int       FrameCountEnded;                        // 已调用 EndFrame 次数
int       FrameCountRendered;
double    Time;                                   // 累计时间
char      ContextName[16];                        // Debug 时区分多 Context
ImGuiIO   IO;                                     // 嵌入的 IO 结构（不是指针！）
ImGuiPlatformIO PlatformIO;
ImGuiStyle Style;
// ...
```

**注意**：`IO` 和 `PlatformIO` 是**嵌入式字段**而不是指针——它们的字节是 Context 的一部分。`sizeof(ImGuiContext)` 包含了它们。

### 分组 2：字体（约 6 字段）

```cpp
ImVector<ImFontAtlas*> FontAtlases;       // 这个 Context 用的字体图集列表
ImFont*       Font;                       // 当前绑定的字体
ImFontBaked*  FontBaked;                  // 当前绑定的字体在当前大小下的烘焙
float         FontSize, FontSizeBase;
float         FontBakedScale;
float         FontRasterizerDensity;
```

### 分组 3：输入事件（4 字段）

```cpp
ImVector<ImGuiInputEvent> InputEventsQueue;       // 见第 1.3 章
ImVector<ImGuiInputEvent> InputEventsTrail;
ImGuiMouseSource         InputEventsNextMouseSource;
ImU32                    InputEventsNextEventId;
```

### 分组 4：窗口管理（约 15 字段）

```cpp
ImVector<ImGuiWindow*> Windows;                  // 所有窗口，绘制顺序
ImVector<ImGuiWindow*> WindowsFocusOrder;        // Root 窗口，焦点顺序
ImVector<ImGuiWindow*> WindowsTempSortBuffer;
ImVector<ImGuiWindowStackData> CurrentWindowStack;
ImGuiStorage           WindowsById;              // ID → ImGuiWindow*
int                    WindowsActiveCount;
ImGuiWindow*           CurrentWindow;            // 正在 Begin 的窗口
ImGuiWindow*           HoveredWindow;
ImGuiWindow*           HoveredWindowUnderMovingWindow;
ImGuiWindow*           MovingWindow;
ImGuiWindow*           WheelingWindow;
// ... + 一堆滚轮相关状态
```

### 分组 5：Item / Widget 状态（约 30 字段）

```cpp
// Hovered/Active/Focus 三大 ID
ImGuiID  HoveredId;
ImGuiID  HoveredIdPreviousFrame;
ImGuiID  ActiveId;
ImGuiID  ActiveIdPreviousFrame;
ImGuiWindow* ActiveIdWindow;
// + 一堆 ActiveId 相关 timer / flag

// LastItemData 是新创建的"上一个 Item 的全部信息"
ImGuiLastItemData LastItemData;
ImGuiNextItemData NextItemData;
ImGuiNextWindowData NextWindowData;
// ...
```

这是第 4 章会重点剖析的部分。

### 分组 6：Layout / Stack（约 10 字段）

```cpp
ImVector<ImGuiColorMod>      ColorStack;          // PushStyleColor 栈
ImVector<ImGuiStyleMod>      StyleVarStack;       // PushStyleVar 栈
ImVector<ImGuiFontStackData> FontStack;           // PushFont 栈
ImVector<ImGuiID>            FocusScopeStack;     // PushFocusScope 栈
ImVector<ImGuiTreeNodeStackData> TreeNodeStack;   // TreeNode 栈
ImVector<ImGuiID>            OpenPopupStack;
ImVector<ImGuiID>            BeginPopupStack;
// ...
```

### 分组 7：Tables / TabBars / Multi-Select / Drag&Drop / Nav

```cpp
ImPool<ImGuiTabBar>          TabBars;             // 所有 TabBar，按 ID
ImPool<ImGuiTable>           Tables;              // 所有 Table
ImVector<ImGuiTableTempData> TablesTempData;
// ...

// Multi-Select 状态
ImGuiMultiSelectState MultiSelectStorage;

// Drag&Drop 状态
bool            DragDropActive;
ImGuiPayload    DragDropPayload;
// ...

// Nav 状态
ImGuiID         NavId, NavWindow, NavFocusScopeId;
// ...
```

### 分组 8：Settings（INI）

```cpp
bool                                    SettingsLoaded;
float                                   SettingsDirtyTimer;
ImGuiTextBuffer                         SettingsIniData;
ImVector<ImGuiSettingsHandler>          SettingsHandlers;
ImChunkStream<ImGuiWindowSettings>      SettingsWindows;
ImChunkStream<ImGuiTableSettings>       SettingsTables;
```

注意 `SettingsWindows` 是 `ImChunkStream<ImGuiWindowSettings>`——回到第 2.2 章讲过的"变长记录顺序流"。每个窗口的 settings 包含变长字符串名字，所以用 ChunkStream 而不是 ImVector。

### 分组 9：Viewports（Multi-Viewport）

```cpp
ImVector<ImGuiViewportP*>  Viewports;             // 所有 viewport
float                      CurrentDpiScale;
// ...
```

### 分组 10：Debug / Metrics

```cpp
int     DebugMemAllocCount;
int     DebugMemFreeCount;
ImGuiDebugLogFlags  DebugLogFlags;
ImGuiTextBuffer     DebugLogBuf;
ImGuiTextIndex      DebugLogIndex;
ImGuiIDStackTool    DebugIDStackTool;
// ...
```

### 总尺寸预算

```
sizeof(ImGuiContext) ≈ 4500 ~ 5500 字节
其中：
  ImGuiIO            ≈ 3000 字节
  ImGuiPlatformIO    ≈  500 字节
  ImGuiStyle         ≈  500 字节
  其他字段           ≈  500 ~ 1500 字节
```

——一个 Context 约 5KB。在嵌入式平台上要注意，但桌面平台无关紧要。

---

## 2.3.7 `ImGuiContext::ImGuiContext` 巨型构造器

打开 `imgui.cpp:4133~4350`，这是 ImGuiContext 的构造函数——**接近 200 行**的字段初始化。

```cpp
ImGuiContext::ImGuiContext(ImFontAtlas* shared_font_atlas)
{
    IO.Ctx = this;                               // 让 IO 知道自己的 Context（AddXxxEvent 要用）
    InputTextState.Ctx = this;

    Initialized = false;                         // ← 等 Initialize() 才置 true
    WithinFrameScope = WithinFrameScopeWithImplicitWindow = false;
    // ...
    FrameCount = 0;
    FrameCountEnded = FrameCountRendered = -1;
    Time = 0.0f;
    memset(ContextName, 0, sizeof(ContextName));

    Font = NULL;
    FontBaked = NULL;
    // ...

    IO.Fonts = shared_font_atlas ? shared_font_atlas : IM_NEW(ImFontAtlas)();
    if (shared_font_atlas == NULL)
        IO.Fonts->OwnerContext = this;

    // ... 接下来 150+ 行各字段初始化
    HoveredId = HoveredIdPreviousFrame = 0;
    ActiveId = 0;
    // ... 滚轮 / Nav / DragDrop / Tables / ...

    // 所有 Tab/Ctrl 等 mod 键映射
    ConfigNavWindowingKeyNext = IO.ConfigMacOSXBehaviors ? (ImGuiMod_Super | ImGuiKey_Tab) : (ImGuiMod_Ctrl | ImGuiKey_Tab);
    // ...
}
```

### 几个值得注意的点

**`shared_font_atlas` 参数**：

- 不传（NULL）→ `IM_NEW(ImFontAtlas)`——创建新的字体图集，归这个 Context 拥有。
- 传一个已存在的 → 共享。

**何时共享 Font Atlas**：

- 你想在多个 Context 之间共享字体（节省内存 / 避免重复加载）。
- 多视口模式下不需要——同 Context 的多 viewport 自动共享。
- 多 Context 模式下（例如编辑器 + 游戏运行时各有一个 Context）建议共享。

**所有字段都被显式初始化**——没有"未初始化字段依赖编译器零初始化"。这是工程纪律。

`memset` 大量被使用（例如 `memset(&DeactivatedItemData, 0, sizeof(DeactivatedItemData))`）——继续 ImGui 的"trivially copyable + memset 替代构造"模式。

---

## 2.3.8 `Shutdown` 与 `DestroyContext`：清理顺序

```cpp
// imgui.cpp:4107
void ImGui::DestroyContext(ImGuiContext* ctx)
{
    ImGuiContext* prev_ctx = GetCurrentContext();
    if (ctx == NULL)
        ctx = prev_ctx;
    SetCurrentContext(ctx);
    Shutdown();                                          // ① 清理子系统
    SetCurrentContext((prev_ctx != ctx) ? prev_ctx : NULL);
    IM_DELETE(ctx);                                      // ② 释放 Context 内存
}
```

### `Shutdown` 完整剖

`imgui.cpp:4430~4555`：

```cpp
void ImGui::Shutdown()
{
    ImGuiContext& g = *GImGui;

    // ① Backend 必须先 Shutdown
    IM_ASSERT_USER_ERROR(g.IO.BackendPlatformUserData == NULL, "Forgot to shutdown Platform backend?");
    IM_ASSERT_USER_ERROR(g.IO.BackendRendererUserData == NULL, "Forgot to shutdown Renderer backend?");

    // ② 清理字体图集
    for (ImFontAtlas* atlas : g.FontAtlases) {
        UnregisterFontAtlas(atlas);
        if (atlas->RefCount == 0) {
            atlas->Locked = false;
            IM_DELETE(atlas);
        }
    }
    g.DrawListSharedData.TempBuffer.clear();

    // ③ 没 Initialize 过就直接退出
    if (!g.Initialized) return;

    // ④ 保存 .ini
    if (g.SettingsLoaded && g.IO.IniFilename != NULL)
        SaveIniSettingsToDisk(g.IO.IniFilename);

    // ⑤ 调用 Shutdown 钩子
    CallContextHooks(&g, ImGuiContextHookType_Shutdown);

    // ⑥ 清理所有窗口（含析构）
    g.Windows.clear_delete();                            // ← clear_delete 调 IM_DELETE
    g.WindowsFocusOrder.clear();
    g.WindowsTempSortBuffer.clear();
    g.CurrentWindow = NULL;
    g.CurrentWindowStack.clear();
    g.WindowsById.Clear();
    g.NavWindow = NULL;

    // ⑦ 清理 routing 表
    g.KeysRoutingTable.Clear();

    // ⑧ 清理各种栈
    g.ColorStack.clear();
    g.StyleVarStack.clear();
    g.FontStack.clear();
    g.OpenPopupStack.clear();
    g.BeginPopupStack.clear();
    g.TreeNodeStack.clear();

    // ⑨ 清理 viewports
    g.Viewports.clear_delete();

    // ⑩ 清理 TabBars / Tables / MultiSelect
    g.TabBars.Clear();
    g.Tables.Clear();
    g.MultiSelectStorage.Clear();
    // ...

    // ⑪ 清理剪贴板缓存 / InputText 状态
    g.ClipboardHandlerData.clear();
    g.InputTextState.ClearFreeMemory();
    // ...
}
```

### 清理顺序的"哲学"

**为什么要这个顺序**：

1. **Backend 在前**：Backend 可能持有指向 Context 内字段的指针（例如 ImGui_ImplOpenGL3_Data 含 Context 指针）。如果 Context 先析构，Backend 析构时会引用悬挂指针。所以 ImGui 用 IM_ASSERT 强制用户先 Backend Shutdown。
2. **保存 ini 在清窗口之前**：因为保存逻辑要遍历窗口的 settings。
3. **窗口在 viewport 之前**：窗口持有 viewport 指针引用。
4. **逐组清理**：每组数据互相独立，没有跨组依赖。

### `clear_delete` 在哪用

回顾第 2.1 章：

```cpp
inline void clear_delete() { for (int n = 0; n < Size; n++) IM_DELETE(Data[n]); clear(); }
```

——`Data[n]` 是 `T*` 类型，对每个调 `IM_DELETE`（析构 + free）。

`g.Windows.clear_delete()` 因为 `g.Windows` 是 `ImVector<ImGuiWindow*>`——每个 window 是堆分配的，需要逐个 delete。

`g.Viewports.clear_delete()` 同理。

### `IM_ASSERT_USER_ERROR` 的"软"特性

```cpp
IM_ASSERT_USER_ERROR(g.IO.BackendPlatformUserData == NULL, "Forgot to shutdown Platform backend?");
```

`IM_ASSERT_USER_ERROR` 与 `IM_ASSERT` 的差异：

- 默认行为：触发 `IM_ASSERT`（崩溃）。
- 但可以通过 `io.ConfigErrorRecoveryEnableAssert = false` 关闭——只记 log，不 abort。

这是 ImGui 的"错误恢复"特性——给非程序员用户（设计师/QA）的 Release 构建提供"软失败"路径。第 0.2 章讨论过。

---

## 2.3.9 DLL 边界陷阱

### 问题场景

你的 Hazel 工程结构：

```
HazelApp.exe                    ← 应用入口
  ├ HazelEngine.dll              ← 引擎核心
  │  └ 链接 imgui.cpp            ← ImGui 在引擎里
  ├ HazelEditor.dll              ← 编辑器（hot reloadable）
  │  └ 调 ImGui::Begin / Button
  └ HazelGame.dll                ← 游戏脚本
     └ 也调 ImGui::Begin / ...
```

**问题**：每个 DLL **各自看到一份** `imgui.h`。如果**只有 HazelEngine.dll 链接 imgui.cpp**，那么：

- HazelEditor.dll 调 `ImGui::Begin` 实际跨 DLL 调 HazelEngine.dll 的代码——**OK**（动态链接成功）。
- HazelEditor.dll 直接读 `GImGui` 这个全局变量——**问题**：每个 DLL 自己有 GImGui 副本（如果 imgui.h 在头文件展开）……

等等，这要细看。

### `GImGui` 的存储位置

```cpp
// imgui.cpp:1437~1439
#ifndef GImGui
ImGuiContext* GImGui = NULL;
#endif
```

`GImGui` 是定义在 `imgui.cpp` 的全局变量。**链接 imgui.cpp 的那个二进制单元**拥有这个变量。

如果只有 `HazelEngine.dll` 链接 imgui.cpp：
- `GImGui` 存在于 `HazelEngine.dll` 的数据段。
- HazelEditor.dll 调 `ImGui::Begin`（这是 imgui.cpp 里的函数）——函数访问 `GImGui` 时访问的是 **HazelEngine.dll 的 GImGui**——**OK**。

### 真正的坑：每个 DLL 都链接 imgui.cpp

如果你**每个 DLL 都把 imgui.cpp 编译进去**（静态链接到每个 DLL）：

- HazelEngine.dll 有自己的 GImGui（设为 ctx）。
- HazelEditor.dll 有自己的 GImGui（NULL！）。

HazelEditor.dll 调 `ImGui::Begin` 时调的是 **HazelEditor.dll 自己**的 imgui.cpp 的实现，访问的是 **HazelEditor.dll 自己的 GImGui = NULL** —— 立刻崩溃。

### 解决方案 1：只让一个 DLL 链接 imgui.cpp

最简单：把 imgui.cpp 仅放在 HazelEngine.dll，其他 DLL 通过 IMGUI_API 导入函数。

```cpp
// imconfig.h
#define IMGUI_API __declspec(dllimport)   // 编译 HazelEditor.dll 时

// 编译 HazelEngine.dll 时改为：
#define IMGUI_API __declspec(dllexport)
```

这样 HazelEditor.dll 调 `ImGui::Begin` 是真的跨 DLL 调到 HazelEngine.dll。

### 解决方案 2：每个 DLL 链接 imgui.cpp，启动时同步

如果你想把 imgui.cpp 静态链接进每个 DLL（编译更快、调试器跳转更直接），那么每个 DLL 启动时必须**同步 GImGui 和分配器**：

```cpp
// HazelEditor.dll 加载时调用
extern "C" void HazelEditor_OnLoad(ImGuiContext* shared_ctx, ImGuiMemAllocFunc alloc, ImGuiMemFreeFunc free, void* user_data) {
    ImGui::SetCurrentContext(shared_ctx);
    ImGui::SetAllocatorFunctions(alloc, free, user_data);
}

// HazelEngine.dll 加载 HazelEditor.dll 后调用
ImGuiMemAllocFunc alloc;
ImGuiMemFreeFunc  free;
void*             user_data;
ImGui::GetAllocatorFunctions(&alloc, &free, &user_data);
HazelEditor_OnLoad(ImGui::GetCurrentContext(), alloc, free, user_data);
```

这正是 `imgui.cpp:1417~1422` 的注释所说：

```cpp
// DLL users:
// - Heaps and globals are not shared across DLL boundaries!
// - You will need to call SetCurrentContext() + SetAllocatorFunctions() for each static/DLL boundary you are calling from.
// - Same applies for hot-reloading mechanisms that are reliant on reloading DLL.
```

### 解决方案 3：`thread_local GImGui`（多 Context / 多线程）

这是第 6 章的主题。简单提一下：

```cpp
// imconfig.h
extern thread_local ImGuiContext* MyImGuiTLS;
#define GImGui MyImGuiTLS
```

每个**线程**有自己的 GImGui。这种模式下 DLL 边界仍然要解决（每个 DLL 仍有自己的 thread_local 实例）。

### 我应该选哪个？

| 你的场景 | 推荐方案 |
|---|---|
| 单 EXE / 静态库 | 不需要做什么 |
| EXE + 几个工具 DLL，所有 ImGui 调用集中在 EXE | 方案 1（IMGUI_API dllimport/export） |
| Hazel-style 引擎 + 多 hot-reload DLL | 方案 2（每个 DLL 启动时手工同步） |
| N 线程 N Context 并行测试 | 方案 3（thread_local） |

**Cherno Hazel 默认是单 EXE**——不需要做这些。但如果你扩展成多 DLL 架构，这是你必须读的章节。

---

## 2.3.10 Hot Reload：Context 怎么活过 DLL 重新加载

### 场景

你的引擎结构：

```
HazelApp.exe              ← 主进程，常驻
  └ ImGui Context           ← Context 在 EXE 的内存里
HazelEditor.dll            ← Hot reloadable
  └ 调 ImGui::Begin
  └ 注册一些 ImGuiSettingsHandler
  └ 注册 PlatformIO 回调（如果做 Multi-Viewport）
```

用户改了 HazelEditor.cpp，引擎热重载该 DLL。**问题**：DLL unload 时它注册到 ImGui 的回调指针**全部失效**——下次 Render 时调到失效函数 → 崩溃。

### 安全 Hot Reload 的步骤

**Unload 旧 DLL 之前**：

```cpp
void HazelEditor_OnUnload() {
    // 1. 移除注册的 SettingsHandler
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = g.SettingsHandlers.Size - 1; i >= 0; i--)
        if (g.SettingsHandlers[i].UserData == /* 我的标识 */)
            g.SettingsHandlers.erase_unsorted(&g.SettingsHandlers[i]);
    
    // 2. 清掉 PlatformIO 中我注册的回调（如果有）
    if (g.PlatformIO.Platform_CreateWindow == &MyEditorCreateWindow)
        g.PlatformIO.Platform_CreateWindow = NULL;
    // ... 其他回调
    
    // 3. 移除 ContextHooks
    ImGui::RemoveContextHook(&g, my_hook_id);
    
    // 4. 清掉所有指向我的字符串字面量
    //    ImGui 内部某些地方会 cache `const char*`——这些字符串在 .rodata 里
    //    DLL unload 后这块内存被释放，cache 的指针变成悬挂
    //    特别是窗口名字（"Editor##xxx"）—— ImGuiWindow 持有指针
    for (ImGuiWindow* w : g.Windows)
        if (w 是我创建的)
            ImGui::ClearWindowSettings(w->Name);
}
```

**Load 新 DLL 之后**：

```cpp
void HazelEditor_OnLoad(ImGuiContext* shared_ctx, ...) {
    ImGui::SetCurrentContext(shared_ctx);
    ImGui::SetAllocatorFunctions(...);
    
    // 重新注册回调
    // 重新注册 SettingsHandler
}
```

### 字符串字面量的隐性问题

```cpp
ImGui::Begin("Editor");   // "Editor" 是字符串字面量，存在 .rodata
```

`ImGuiWindow::Name` 内部 `strdup` 了一份吗？看 `imgui_internal.h` 中 `ImGuiWindow::Name` 字段：

```cpp
char* Name;   // Window name
```

——是 `char*`，用 IM_ALLOC 分配（`strdup` 风格）。所以**窗口名是安全的**——ImGui 自己复制了。

但 ImGui 内部某些其他位置可能 cache `const char*` 不复制——例如 `g.ContextName[16]` 是 char 数组，但 `g.IO.IniFilename` 是 `const char*`。**这些指针的生命周期由你保证**。

如果你给 `IniFilename = "settings.ini"` 用字符串字面量——OK，字面量在 EXE 的 .rodata，永远活着。

如果你给 `IniFilename = mydll_function_returning_static_string()`——危险，DLL unload 后字符串失效。

### Hot Reload 的设计建议

1. **Context 永远在 EXE 中创建**——不要在 DLL 里 `CreateContext`。
2. **DLL 注册的回调在 unload 时清理**——别假装 ImGui 会自动清。
3. **不要让 ImGui 持有 DLL 内的字符串指针**——所有 `const char*` 必须来自 EXE 或者用 ImGui::MemAlloc 复制。
4. **Hot Reload 的窗口位置自动恢复**——ImGui 的 `.ini` 系统会保存窗口位置/大小。重载后调 `LoadIniSettingsFromDisk` 恢复。

---

## 2.3.11 多 Context 模式

### 何时需要多 Context

- **Headless 测试**：单元测试同时跑 N 个独立 ImGui 实例。
- **多游戏窗口**：编辑器主窗口 + 独立的"运行时预览"窗口，各有自己的 UI。
- **分屏渲染**：每个屏幕一个 Context。

### 创建多 Context

```cpp
ImGuiContext* ctx_a = ImGui::CreateContext();
ImGuiContext* ctx_b = ImGui::CreateContext();   // 创建第二个

// 在 A 上工作
ImGui::SetCurrentContext(ctx_a);
ImGui::NewFrame();
ImGui::Begin("A Window");
ImGui::End();
ImGui::Render();

// 切到 B
ImGui::SetCurrentContext(ctx_b);
ImGui::NewFrame();
// ...
ImGui::Render();

// 销毁
ImGui::DestroyContext(ctx_a);
ImGui::DestroyContext(ctx_b);
```

### Context 与 Backend 的对应

每个 Backend（Platform / Renderer）的 init 是**当前 Context 的**：

```cpp
ImGui::SetCurrentContext(ctx_a);
ImGui_ImplGlfw_InitForOpenGL(window_a, true);
ImGui_ImplOpenGL3_Init();

ImGui::SetCurrentContext(ctx_b);
ImGui_ImplGlfw_InitForOpenGL(window_b, true);   // 第二个 GLFW 窗口
ImGui_ImplOpenGL3_Init();
```

**注意**：`io.BackendPlatformUserData` 是 per-Context 的（因为 IO 嵌入在 Context 里）——所以两个 Context 的 Backend 各自独立。

### 字体共享

```cpp
ImFontAtlas* shared_atlas = new ImFontAtlas();
shared_atlas->AddFontDefault();
// ...

ImGuiContext* ctx_a = ImGui::CreateContext(shared_atlas);
ImGuiContext* ctx_b = ImGui::CreateContext(shared_atlas);
```

——传同一个 `ImFontAtlas` 给两个 Context，节省字体图集内存（通常 1~4 MB）。

`shared_atlas->RefCount` 会被 ImGui 自动维护——两个 Context 都引用时 = 2，第一个 DestroyContext 后 = 1，第二个 DestroyContext 后 = 0 时被释放。

### 多线程 N Context

```cpp
// imconfig.h
extern thread_local ImGuiContext* MyImGuiTLS;
#define GImGui MyImGuiTLS

// MyImGui.cpp
thread_local ImGuiContext* MyImGuiTLS = NULL;
```

每个线程通过 `SetCurrentContext` 把自己的 Context 设进去——`GImGui`（=`MyImGuiTLS`）每个线程独立。

完整内容参见第 6 章。

---

## 2.3.12 实战：Hazel 集成的"完美"版本

把这一章所有知识整合成一个完美的 `ImGuiLayer.cpp` 初始化序列：

```cpp
// HazelImGui_Init.cpp（伪代码）

// ① 主程序在 HazelApp.exe 里
void Application::InitImGui() {
    // ① 设置自定义分配器（必须在 CreateContext 之前）
    ImGui::SetAllocatorFunctions(
        [](size_t size, void* user_data) {
            return ((Hazel::MemoryManager*)user_data)->Allocate(size, "ImGui");
        },
        [](void* ptr, void* user_data) {
            ((Hazel::MemoryManager*)user_data)->Free(ptr);
        },
        &m_MemoryManager
    );
    
    // ② 创建 Context
    IMGUI_CHECKVERSION();
    m_ImGuiContext = ImGui::CreateContext();
    
    // ③ 配置 IO
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | 
                       ImGuiConfigFlags_DockingEnable |
                       ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = "imgui.ini";   // 字符串字面量，永远有效
    
    // ④ Backend Init
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();
}

// ② DLL 加载时调用（从 EXE 传 Context）
extern "C" __declspec(dllexport)
void HazelEditor_OnDllLoad(ImGuiContext* ctx) {
    // 同步 Context
    ImGui::SetCurrentContext(ctx);
    
    // 同步分配器（关键！）
    ImGuiMemAllocFunc alloc_fn;
    ImGuiMemFreeFunc  free_fn;
    void*             user_data;
    ImGui::GetAllocatorFunctions(&alloc_fn, &free_fn, &user_data);
    ImGui::SetAllocatorFunctions(alloc_fn, free_fn, user_data);
    
    // 注册 DLL 自己的 SettingsHandler / Hooks（如有）
}

// ③ DLL 卸载时调用
extern "C" __declspec(dllexport)
void HazelEditor_OnDllUnload() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    
    // 移除我们注册的 SettingsHandlers
    for (int i = g.SettingsHandlers.Size - 1; i >= 0; i--) {
        if (g.SettingsHandlers[i].TypeHash == ImHashStr("MyEditor"))
            g.SettingsHandlers.erase_unsorted(&g.SettingsHandlers[i]);
    }
    
    // 移除 ContextHooks
    ImGui::RemoveContextHook(&g, my_hook_id);
    
    // 清窗口（让 ImGui 释放它们的 IM_ALLOC 内存）
    // 注意：窗口名是 ImGui 自己 strdup 的，本身不会悬挂
}

// ④ Application::Shutdown
void Application::ShutdownImGui() {
    // 必须先 Backend Shutdown
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    
    // 再 DestroyContext
    ImGui::DestroyContext(m_ImGuiContext);
    m_ImGuiContext = NULL;
}
```

---

## 2.3.13 内存预算与 Telemetry

### 一个典型编辑器的 ImGui 内存

```
启动后：                  ~ 2 MB（字体图集 ~ 1 MB + Context ~ 5 KB + 初始容器）
打开几个面板后：          ~ 3 MB（多了几个 ImGuiWindow + DrawList capacity）
长时间使用后稳定：        ~ 4~6 MB（容器 capacity 增长到峰值后稳定）
```

### Telemetry 实战代码

```cpp
// 在你的引擎 Profiler 里：
void Application::OnImGuiRender() {
    if (!show_imgui_telemetry) return;
    
    ImGuiContext& g = *ImGui::GetCurrentContext();
    
    ImGui::Begin("ImGui Telemetry");
    ImGui::Text("Frame: %d", g.FrameCount);
    ImGui::Text("Mem: alloc=%d free=%d active=%d", 
        g.DebugMemAllocCount, 
        g.DebugMemFreeCount,
        g.DebugMemAllocCount - g.DebugMemFreeCount);
    ImGui::Text("Windows: %d (active=%d)",
        g.Windows.Size,
        g.WindowsActiveCount);
    ImGui::Text("Tables: %d alive=%d",
        g.Tables.GetMapSize(),
        g.Tables.GetAliveCount());
    ImGui::Text("Settings handlers: %d", g.SettingsHandlers.Size);
    ImGui::Text("Viewports: %d", g.Viewports.Size);
    ImGui::Text("Input events queued: %d trail: %d",
        g.InputEventsQueue.Size,
        g.InputEventsTrail.Size);
    
    // 顶点/索引数（来自上一帧）
    ImGui::Text("Vertices: %d  Indices: %d",
        g.IO.MetricsRenderVertices,
        g.IO.MetricsRenderIndices);
    ImGui::End();
}
```

这个面板能让你**实时**看到 ImGui 的健康度。

---

## 2.3.14 本章小结

让我们把整章浓缩成 7 条核心结论：

1. **ImGui 内存通过 3 个 static 函数指针路由**：`GImAllocatorAllocFunc / FreeFunc / UserData`，整个进程只有一组（per `imgui.cpp` 编译单元）。
2. **`SetAllocatorFunctions` 必须在 `CreateContext` 之前**——否则 Context 用默认分配器，后续混乱。
3. **`CreateContext` 5 步**：分配 Context 内存 → 调构造函数 → SetCurrentContext → Initialize（注册 ini handler / 默认 platform 回调 / 主 viewport / KeysMayBeCharInput） → 恢复之前的 Context。
4. **`ImGuiContext` 是 ~5KB 的"巨型上下文"**——含 IO / PlatformIO / Style / 4+ ImVector / ImPool 等成员，按 10 个职能分组。
5. **`Shutdown` 与 `DestroyContext` 严格 mirror**——Backend 必须先 Shutdown（IM_ASSERT 检查），然后保存 ini → 调 hook → 清窗口/viewport/各种栈/容器 → IM_DELETE Context。
6. **DLL 边界**：每个 DLL 自己的 `GImGui` 和 `GImAllocatorXxx`——必须手工同步（`SetCurrentContext` + `SetAllocatorFunctions`）或者通过 `IMGUI_API dllimport/export` 让一个 DLL 是唯一所有者。
7. **Hot Reload**：DLL unload 前必须清掉自己注册到 ImGui 的回调 / SettingsHandler / ContextHook，避免悬挂指针。

---

## 2.3.15 第二部分总结

至此第二部分（核心数据结构与零分配设计哲学）三章全部完成：

| 章 | 主题 | 核心收获 |
|---|---|---|
| 2.1 | `ImVector<T>` 源码逐字节剖析 | 3 字段紧凑布局 / 1.5x 增长 / `memcpy` 而非赋值 / 5 大与 std::vector 差异 |
| 2.2 | `ImPool` / `ImChunkStream` / `ImBitArray` 等 | 7 类专门化容器 / 容器选择决策树 / 工程实战示例 |
| 2.3 | 内存分配器与 Context 生命周期 | 内存路由全链路 / Context 字段全景 / DLL 边界陷阱 / Hot Reload |

**第二部分的根本主题**：**ImGui 是怎么做到"每帧重建 UI 但运行时近乎零分配"的**。答案是：

- 容器 capacity 跨帧保留（`resize(0)` 不释放）。
- `ImPool` 槽位复用（用空闲槽位字节当链表节点）。
- `ImChunkStream` 把变长记录连续存储。
- `ImSpanAllocator` 一次分配切多段。
- 全部基于 `ImVector` 的 trivially relocatable + memcpy 第一原则。

这套机制让 ImGui 在大型应用里**稳定状态下**每帧分配 0~10 次（绝大多数场景），而不是数万次。这是 ImGui 能成为"实时游戏 UI 替代品"的根本原因——`std::vector` / `std::map` 满天飞的库做不到这个。

---

## 2.3.16 下一部分预告

**第三部分：渲染管线与 ImDrawList 源码级解剖**

- 第 3.1 章：一帧的旅程——从 NewFrame 到 Render 的全链路（基于第 2 部分的内存底盘 + 第 1 部分的输入处理）。
- 第 3.2 章：ImDrawList 数据布局与 Path 系统（CmdBuffer / VtxBuffer / IdxBuffer / _Path）。
- 第 3.3 章：抗锯齿（AA Lines / AA Fill）算法源码精读。
- 第 3.4 章：顶点合并、Clip Stack 与渲染后端契约。

第三部分的前置基础是第 0.4 章（GPU 渲染管线 101）+ 第 2.1 章（ImVector）。读完第三部分你应该能写出**不依赖任何官方 imgui_impl_xxx**的自家 Renderer Backend——这是第 5.4 章 Hazel 综合实战的基础。

第二部分到此结束。当你说"开始第三部分"或"开始 3.1 章"时我们继续。
