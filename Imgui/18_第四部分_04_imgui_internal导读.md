# 第 4.4 章 · `imgui_internal.h` 导读：突破公开 API 的底盘

> **本章目标**：把 `imgui_internal.h` 这个 4500+ 行的"私有但开放"头文件**结构化梳理**——按 SECTION 巡回、给出 50 个最常用 internal 函数清单、画出 `ImGuiContext` 字段全景。然后落到**4 条魔改铁律 + 4 个版本漂移实战案例**。读完后你应该能：(1) 在 internal.h 里精准找到任何你需要的私有 API；(2) 写出"经得起 ImGui 升级"的魔改代码；(3) 对"版本漂移"主动防御。
>
> **本章对应源码**：`imgui_internal.h:10~42`（SECTION 索引）、`imgui_internal.h:[SECTION] ImGuiContext]`（约 2192 行起）、`imgui_internal.h:[SECTION] ImGui internal API]`（核心 internal 函数声明）、`imgui.h:104`（`IMGUI_CHECKVERSION`）、`imgui.h:33`（`IMGUI_VERSION_NUM`）。
>
> **前置阅读**：第 0.2 章（C++ 子集 / 私有但开放的 API 哲学）、第 4.1~4.3 章（ID Stack / Window / 状态机）。

---

## 4.4.0 `imgui_internal.h` 是什么

`imgui.h` 是**对应用层的稳定承诺**——所有公开 API 跨版本保持兼容。
`imgui_internal.h` 是**对自身实现的私有接口**——所有内部数据结构 + 内部函数。

但是！`imgui_internal.h` **不是私有的**——任何人都可以 `#include`。它的"私有"含义是：

> **官方明确不保证 ABI / API 跨版本稳定**。任何字段、函数、enum 都可能在版本之间被改名、改类型、改语义。

**这意味着**：

- 你**可以**用它的所有内容做魔改 / 高级控件。
- 你**必须**为你的代码加版本守护，避免 ImGui 升级时神秘崩溃。
- 你**应该**了解哪些字段比较稳定、哪些字段经常变。

本章给你这份"导览图 + 防御指南"。

---

## 4.4.1 28 个 SECTION 全景巡回

打开 `imgui_internal.h:10~42`，所有 SECTION 索引：

```cpp
// [SECTION] Header mess              ← 编译器 pragma / 警告关闭
// [SECTION] Forward declarations     ← 所有内部 struct 的前置声明
// [SECTION] Context pointer          ← GImGui 全局 + IMGUI_SET_CURRENT_CONTEXT_FUNC 宏
// [SECTION] STB libraries includes   ← imstb_textedit.h 等
// [SECTION] Macros                   ← IM_ALLOC / IM_NEW / IM_ASSERT_PARANOID 等
// [SECTION] Generic helpers          ← ImBitArray / ImSpan / ImPool / ImChunkStream + 数学
// [SECTION] ImDrawList support       ← ImDrawListSharedData / SetCircleTessellationMaxError
// [SECTION] Style support            ← ImGuiStyleVarInfo / ImGuiColorMod / ImGuiStyleMod
// [SECTION] Data types support       ← ImGuiDataTypeInfo
// [SECTION] Widgets support          ← ImGuiButtonFlags / ImGuiSliderFlags / ItemFlags / LastItemData
// [SECTION] Popup support            ← ImGuiPopupData
// [SECTION] Inputs support           ← ImGuiInputEvent / ImGuiKeyOwnerData / ImGuiKeyRoutingTable
// [SECTION] Clipper support          ← ImGuiListClipperData
// [SECTION] Navigation support       ← ImGuiNavItemData / ImGuiNavMoveFlags
// [SECTION] Typing-select support
// [SECTION] Columns support          ← ImGuiOldColumns
// [SECTION] Box-select support
// [SECTION] Multi-select support
// [SECTION] Docking support          ← ImGuiDockNode（Docking 分支独有）
// [SECTION] Viewport support         ← ImGuiViewportP
// [SECTION] Settings support         ← ImGuiWindowSettings / ImGuiSettingsHandler
// [SECTION] Localization support     ← ImGuiLocKey / ImGuiLocEntry
// [SECTION] Error handling           ← ImGuiErrorRecoveryState
// [SECTION] Metrics, Debug tools     ← ImGuiDebugLogFlags / ImGuiIDStackTool
// [SECTION] Generic context hooks    ← ImGuiContextHook
// [SECTION] ImGuiContext             ← 主 Context 结构（>4500 字节）
// [SECTION] ImGuiWindowTempData, ImGuiWindow   ← 第 4.2 章重点
// [SECTION] Tab bar, Tab item support
// [SECTION] Table support            ← ImGuiTable / ImGuiTableColumn
// [SECTION] ImGui internal API       ← 所有内部函数声明（>500 个）
// [SECTION] ImFontLoader             ← 字体加载器接口
// [SECTION] ImFontAtlas internal API
// [SECTION] Test Engine specific hooks
```

### SECTION 阅读策略

按"使用频率"读：

**必读 SECTION**（90% 魔改场景都涉及）：
- Generic helpers（`ImBitArray` / `ImSpan` 等）
- Widgets support（`ImGuiButtonFlags` / `LastItemData`）
- ImGui internal API（核心私有函数）
- ImGuiContext / ImGuiWindow

**按需读 SECTION**（特定场景）：
- ImDrawList support（自定义绘制）
- Inputs support（自定义快捷键）
- Settings support（持久化扩展）
- Tables support（深度自定义表格）

**很少读 SECTION**：
- STB libraries includes
- Test Engine hooks
- ImFontLoader（自定义字体后端）

---

## 4.4.2 50 个高频 internal 函数清单

按"使用频率"排序——前 10 个你写魔改代码几乎天天用。

### Tier 1：每天用（前 10）

| 函数 | 用途 |
|---|---|
| `ImGui::GetCurrentWindow()` | 取当前 ImGuiWindow* |
| `ImGui::GetCurrentContext()` | 取当前 Context（公开但常用） |
| `ImGui::ItemAdd(bb, id)` | 注册一个 widget 到布局 |
| `ImGui::ItemSize(size)` | 推进 cursor |
| `ImGui::ItemHoverable(bb, id, flags)` | 检查 hover |
| `ImGui::ButtonBehavior(bb, id, ...)` | 检查 click / held |
| `ImGui::PushOverrideID(id)` | 跳过哈希直接 push ID |
| `ImGui::KeepAliveID(id)` | 保活 ActiveId |
| `ImGui::SetActiveID(id, window)` | 设置 ActiveId |
| `ImGui::ClearActiveID()` | 清 ActiveId |

### Tier 2：高频（11~25）

| 函数 | 用途 |
|---|---|
| `ImGui::FocusWindow(window)` | 让窗口获得焦点 |
| `ImGui::BringWindowToFocusFront(window)` | 窗口到 focus 顶 |
| `ImGui::BringWindowToDisplayFront(window)` | 窗口到绘制顶 |
| `ImGui::IsClippedEx(bb, id)` | 是否被裁剪 |
| `ImGui::PushClipRect(min, max, intersect)` | 入裁剪栈 |
| `ImGui::PopClipRect()` | 出裁剪栈 |
| `ImGui::FindWindowByID(id)` | 按 ID 找窗口 |
| `ImGui::FindWindowByName(name)` | 按名找窗口 |
| `ImGui::SetHoveredID(id)` | 设置 HoveredId |
| `ImGui::GetID(label)` / `GetIDWithSeed` | 算 ID |
| `ImGui::RenderFrame(min, max, col, ...)` | 画带 padding 的矩形 |
| `ImGui::RenderText(pos, text)` | 画文字 |
| `ImGui::RenderNavCursor(bb, id)` | 画键盘焦点框 |
| `ImGui::CalcItemWidth()` | 算当前 item 宽 |
| `ImGui::CalcWrapWidthForPos(pos, wrap_pos)` | 算文字换行宽 |

### Tier 3：偶尔用（26~50）

| 函数 | 用途 |
|---|---|
| `ImGui::IsKeyPressedMap(key, repeat)` | 老式 key 检查 |
| `ImGui::SetKeyOwner(key, owner_id, flags)` | 占用按键 |
| `ImGui::TestKeyOwner(key, owner_id)` | 测试占用 |
| `ImGui::Shortcut(key_chord, flags, owner_id)` | 路由快捷键 |
| `ImGui::SetShortcutRouting(...)` | 注册路由 |
| `ImGui::PushFocusScope(id)` / `PopFocusScope()` | 焦点 scope |
| `ImGui::SetWindowPos(window, pos, cond)` | 设位置（带 window 参数） |
| `ImGui::SetWindowSize(window, size, cond)` | 设大小 |
| `ImGui::SetNextWindowScroll(scroll)` | 设下窗滚动 |
| `ImGui::ScrollToBringRectIntoView(bb)` | 滚到可见 |
| `ImGui::OpenPopupEx(id, flags)` | 内部开 popup |
| `ImGui::ClosePopupToLevel(remaining, ...)` | 关到层 |
| `ImGui::IsPopupOpen(id, flags)` | 查 popup |
| `ImGui::GetTopMostPopupModal()` | 取顶 modal |
| `ImGui::BeginTabBarEx(tab_bar, bb, flags)` | 内部 TabBar |
| `ImGui::TabBarFindTabByID(tb, id)` | 查 tab |
| `ImGui::TableBeginRow(table)` | 内部 Table 行 |
| `ImGui::DataTypeFormatString(buf, ...)` | 数字格式化 |
| `ImGui::SetItemKeyOwner(key, flags)` | 给 item 占按键 |
| `ImGui::SplitterBehavior(bb, id, axis, ...)` | 分隔条交互 |
| `ImGui::DragBehavior(id, type, value, ...)` | 拖拽数字 |
| `ImGui::SliderBehavior(bb, id, type, ...)` | 滑块 |
| `ImGui::ImFontAtlasUpdateNewFrame(...)` | 字体新帧 |
| `ImGui::DebugLog(fmt, ...)` | 写 Debug Log |
| `ImGui::DebugLocateItem(id)` | 高亮 item |

---

## 4.4.3 `ImGuiContext` 字段全景图（按 12 类职能）

打开 `imgui_internal.h:2192`。这是 ImGui 中最大的结构（约 4500 字节）。

```cpp
struct ImGuiContext {
    // ═══ 1. 核心标识 ═══
    bool        Initialized;
    int         FrameCount;
    double      Time;
    char        ContextName[16];
    
    // ═══ 2. 嵌入子结构 ═══
    ImGuiIO     IO;                    // ~3000 字节
    ImGuiPlatformIO PlatformIO;         // ~500 字节
    ImGuiStyle  Style;                  // ~500 字节
    
    // ═══ 3. 字体子系统 ═══
    ImVector<ImFontAtlas*> FontAtlases;
    ImFont*     Font;
    ImFontBaked* FontBaked;
    float       FontSize, FontSizeBase, FontBakedScale;
    float       FontRasterizerDensity;
    float       CurrentDpiScale;
    ImDrawListSharedData DrawListSharedData;
    
    // ═══ 4. 输入事件 ═══
    ImVector<ImGuiInputEvent> InputEventsQueue;
    ImVector<ImGuiInputEvent> InputEventsTrail;
    ImGuiMouseSource          InputEventsNextMouseSource;
    ImU32                     InputEventsNextEventId;
    
    // ═══ 5. 窗口管理 ═══
    ImVector<ImGuiWindow*>     Windows;
    ImVector<ImGuiWindow*>     WindowsFocusOrder;
    ImVector<ImGuiWindow*>     WindowsTempSortBuffer;
    ImVector<ImGuiWindowStackData> CurrentWindowStack;
    ImGuiStorage               WindowsById;
    int                        WindowsActiveCount;
    ImGuiWindow*               CurrentWindow;
    ImGuiWindow*               HoveredWindow;
    ImGuiWindow*               MovingWindow;
    ImGuiWindow*               WheelingWindow;
    ImGuiWindow*               NavWindow;
    
    // ═══ 6. Item / Widget 状态 ═══
    ImGuiID    HoveredId, HoveredIdPreviousFrame, ActiveId, ActiveIdPreviousFrame;
    float      HoveredIdTimer, ActiveIdTimer, LastActiveIdTimer;
    bool       ActiveIdIsAlive, ActiveIdIsJustActivated, ActiveIdHasBeenEditedBefore;
    ImGuiID    ActiveIdWindow;
    ImGuiInputSource ActiveIdSource;
    ImGuiID    LastActiveId;
    ImGuiDeactivatedItemData DeactivatedItemData;
    ImGuiLastItemData LastItemData, NextItemData;
    ImGuiNextWindowData NextWindowData;
    
    // ═══ 7. Stack 系列 ═══
    ImVector<ImGuiColorMod>      ColorStack;
    ImVector<ImGuiStyleMod>      StyleVarStack;
    ImVector<ImGuiFontStackData> FontStack;
    ImVector<ImGuiID>            FocusScopeStack;
    ImVector<ImGuiTreeNodeStackData> TreeNodeStack;
    ImVector<ImGuiID>            OpenPopupStack;
    ImVector<ImGuiID>            BeginPopupStack;
    
    // ═══ 8. 控件子系统状态 ═══
    ImPool<ImGuiTabBar>          TabBars;
    ImPool<ImGuiTable>           Tables;
    ImVector<ImGuiTableTempData> TablesTempData;
    ImGuiTable*                  CurrentTable;
    ImGuiTabBar*                 CurrentTabBar;
    ImGuiMultiSelectState        MultiSelectStorage;
    
    // ═══ 9. Drag & Drop ═══
    bool         DragDropActive, DragDropWithinSource, DragDropWithinTarget;
    ImGuiPayload DragDropPayload;
    ImGuiID      DragDropTargetId, DragDropAcceptIdCurr, DragDropAcceptIdPrev;
    // ...
    
    // ═══ 10. 导航 (Nav) ═══
    ImGuiID         NavId, NavFocusScopeId, NavActivateId;
    bool            NavCursorVisible, NavInitRequest, NavMoveSubmitted;
    ImGuiDir        NavMoveDir;
    ImGuiKeyChord   NavMoveKeyMods;
    // ... ~ 40 个 Nav 字段
    
    // ═══ 11. Settings & 输入路由 ═══
    bool                                SettingsLoaded;
    float                               SettingsDirtyTimer;
    ImGuiTextBuffer                     SettingsIniData;
    ImVector<ImGuiSettingsHandler>      SettingsHandlers;
    ImChunkStream<ImGuiWindowSettings>  SettingsWindows;
    ImChunkStream<ImGuiTableSettings>   SettingsTables;
    ImGuiKeyRoutingTable                KeysRoutingTable;
    ImBitArrayForNamedKeys              KeysMayBeCharInput;
    
    // ═══ 12. Viewport / Debug ═══
    ImVector<ImGuiViewportP*> Viewports;
    int       DebugMemAllocCount;
    int       DebugMemFreeCount;
    ImGuiDebugLogFlags DebugLogFlags;
    ImGuiTextBuffer    DebugLogBuf;
    ImGuiTextIndex     DebugLogIndex;
    ImGuiIDStackTool   DebugIDStackTool;
    // ...
    
    // ═══ Misc ═══
    ImVector<char>  TempBuffer;
    char            TempKeychordName[64];
    ImVector<ImGuiContextHook> Hooks;
    // ...
};
```

### 总尺寸预算

```
~4500~5500 字节（取决于 docking / 平台）

其中最大块：
  IO                    ~3000 (含 KeysData[154] × 16 bytes)
  PlatformIO            ~500 (函数指针表 + viewports vector)
  Style                 ~500 (~100 字段 × 4-16 bytes)
  其他动态数据           ~500-1500 (含若干 ImVector / ImPool 头)

但是 ImVector 内部数据是 heap 分配，不计入 sizeof
```

---

## 4.4.4 4 条魔改铁律

### 铁律 1：不要持有 `ImGuiWindow*` 跨帧

**反例**：

```cpp
class MyEditor {
    ImGuiWindow* m_ViewportWindow = nullptr;
    
    void OnImGuiRender() {
        if (!m_ViewportWindow)
            m_ViewportWindow = ImGui::FindWindowByName("Viewport");
        
        // ❌ 危险：m_ViewportWindow 可能已失效
        if (m_ViewportWindow->Active) { ... }
    }
};
```

**为什么危险**：

- `ImGuiWindow*` 是 IM_NEW 分配的——但 ImGui **可能销毁** window（用户关闭某个 popup、Hot Reload 等）。
- 销毁后指针变悬挂——下次访问 = UB。
- 即使没销毁，window 内部字段在每次 NewFrame 时被重置——你"昨天"读到的字段值今天可能不对。

**正确做法**：

```cpp
void OnImGuiRender() {
    // 每次访问都重新查找——便宜（O(log N)）
    ImGuiWindow* w = ImGui::FindWindowByName("Viewport");
    if (w && w->Active) { ... }
}
```

**特别警告**：跨帧持有 `ImDrawList*` 也危险——`window->DrawList` 在窗口被 Compact（GC）时被清空内存。

### 铁律 2：`ImGuiID` 不可序列化

**反例**：

```cpp
// ❌ 错误：ImGuiID 跨进程不稳定
struct MySaveFile {
    ImGuiID active_widget_id;
};
file.write(&save, sizeof(save));
// ... 启动时 read，期望恢复焦点 ...
```

**为什么不稳定**：

- `ImGuiID` 是 ID Stack 累积哈希结果。
- 哈希算法版本：1.91.6 之前用一种 CRC32 表，之后切到 SSE 兼容表——**同样的 label 哈希出不同 ID**。
- `IMGUI_USE_LEGACY_CRC32_ADLER` 配置宏会影响。
- 你的 widget 嵌套结构改变（例如多了一层 PushID）→ ID 全变。

**正确做法**：用业务级 ID 标识，临时算 ImGuiID：

```cpp
// 业务保存：
struct MySaveFile {
    int focused_entity_id;   // 业务 ID（数据库主键）
};

// 加载时重新计算 ImGuiID：
ImGuiID imgui_id = ImHashStr("Entity", 0, 0);
imgui_id = ImHashData(&entity_id, sizeof(int), imgui_id);
ImGui::SetActiveID(imgui_id, my_window);
```

**例外**：`.ini` 文件里的窗口位置——这个**是** ImGui 自己持久化 ImGuiID 的（窗口名 → ID）。但**用户代码不应模仿**——除非你愿意承担"哈希算法变化时所有保存的状态失效"的风险。

### 铁律 3：写入 internal 字段必须包裹版本守护

**反例**：

```cpp
// ❌ 危险：直接写 internal 字段
ImGui::GetCurrentWindow()->DC.CursorPos.y += 10.0f;
g.NavWindow = my_window;
```

如果 ImGui 1.93 把 `DC.CursorPos` 改名（实际历史上发生过几次），你的代码**编译不过**——好。但更糟的情况是：字段还在但**语义变了**——编译过、运行错乱。

**正确做法**：

```cpp
#if IMGUI_VERSION_NUM >= 19271 && IMGUI_VERSION_NUM < 20000
    // 1.92.7+ 但小于未来 2.0 的字段读写
    ImGui::GetCurrentWindow()->DC.CursorPos.y += 10.0f;
#else
    #error "Unsupported ImGui version - check internal field compat"
#endif
```

**写所有 internal 访问时养成"上下界守护"的习惯**——上界让你在升级时主动审视，下界防止意外回退到不兼容旧版本。

### 铁律 4：自定义控件统一走"三段式"

回顾第 4.3 章的模板：

```cpp
bool MyWidget(...) {
    // ① ID + 几何
    ImGuiID id = window->GetID(label);
    ImRect bb = ...;
    
    // ② ItemSize → ItemAdd → ButtonBehavior
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    
    // ③ 渲染
    // ...
}
```

**为什么必须**：

- `ItemAdd` 设置 `g.LastItemData` —— 后续 `IsItemHovered()` 等才能工作。
- `ButtonBehavior` 处理 12+ 种交互模式 —— 不要重新发明轮子。
- `ItemSize` 推进 cursor —— 让你的 widget 与其他 widget 自然衔接。

**反例**：

```cpp
// ❌ 跳过 ItemAdd
bool MyButton(const char* label) {
    ImGui::Dummy(ImVec2(100, 30));   // 留出空间
    if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(bb_min, bb_max))
        return true;
    return false;
}
```

后果：
- `IsItemHovered()` 调用不到——LastItemData 没设。
- ID Stack 状态不更新——Tab 导航不能聚焦它。
- ID 冲突检测失效。
- Item Picker 找不到它。

**对应**：你需要任何上述功能时——**永远**用三段式。

---

## 4.4.5 4 个版本漂移实战案例

### 案例 1：1.87 KeysDown[] / KeyMap[] 删除

**1.86 之前**：

```cpp
io.KeysDown[GLFW_KEY_A] = true;
if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_A])) { ... }
```

**1.87 之后**：

```cpp
io.AddKeyEvent(ImGuiKey_A, true);
if (ImGui::IsKeyPressed(ImGuiKey_A)) { ... }
```

**老代码迁移**：参见第 1.3 章——`HazelKeyToImGuiKey` 转换函数。

### 案例 2：1.91.1 剪贴板 API 位移

```cpp
// 1.91.0 及之前
io.GetClipboardTextFn = MyGet;
io.SetClipboardTextFn = MySet;
io.ClipboardUserData  = my_ptr;

// 1.91.1 之后
ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
pio.Platform_GetClipboardTextFn = MyGet;
pio.Platform_SetClipboardTextFn = MySet;
pio.Platform_ClipboardUserData  = my_ptr;
```

注意函数签名也改了：

```cpp
// 老
const char* GetClipboardTextFn(void* user_data);

// 新
const char* Platform_GetClipboardTextFn(ImGuiContext* ctx);
```

**防御**：

```cpp
#if IMGUI_VERSION_NUM >= 19110
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = MyGetWithCtx;
#else
    io.GetClipboardTextFn = MyGetOldStyle;
#endif
```

### 案例 3：1.92 `io.FontGlobalScale` 移到 style

```cpp
// 1.91.x 及之前
io.FontGlobalScale = 1.5f;

// 1.92 之后
ImGui::GetStyle().FontScaleMain = 1.5f;
```

老字段还在但**写无效**——只能读到上次设的值，不会影响实际渲染。

### 案例 4：1.92 `ImTextureID` 直接存 vs `ImTextureRef`

```cpp
// 1.91.x
ImDrawCmd* cmd;
ImTextureID tex = cmd->TextureId;   // 直接字段

// 1.92
ImTextureID tex = cmd->GetTexID();  // 必须用 getter
```

`TextureId` 字段被**删除**——因为引入了 `ImTextureRef`（包裹 `ImTextureID` + 可选 `ImTextureData*`），需要用 getter 解析。

---

## 4.4.6 防御实战：用 `IMGUI_CHECKVERSION`

```cpp
// imgui.h:104
#define IMGUI_CHECKVERSION()    ImGui::DebugCheckVersionAndDataLayout(IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx))
```

**每个使用 ImGui 的 .cpp 都应该在 init 时调一次**：

```cpp
void Application::InitImGui() {
    IMGUI_CHECKVERSION();   // ← 第一行
    ImGui::CreateContext();
    // ...
}
```

`DebugCheckVersionAndDataLayout` 内部检查：

- `sizeof(ImGuiIO)` 等关键结构体大小一致。
- `IMGUI_VERSION` 字符串一致。

不一致 → IM_ASSERT 失败。**这是检测"应用 vs ImGui 版本不一致 / IMGUI_USE_WCHAR32 等 ABI 影响宏没全工程一致"的最后防线**。

---

## 4.4.7 跟踪 ImGui 升级的工作流

### Step 1：阅读 release notes

每次升级 ImGui，先读：
- https://github.com/ocornut/imgui/releases
- `imgui.h` 顶部的 changelog（搜 `RECENT CHANGES`）

注意**所有提到的 BREAKING CHANGES**——这些一定影响你的代码。

### Step 2：grep 你的代码

```bash
grep -rn "io\\.KeyMap\\|io\\.KeysDown\\|io\\.GetClipboardTextFn\\|TextureId" src/
```

——找出所有"老 API 模式"，改成新 API。

### Step 3：编译看错误

ImGui 团队对 ABI breaking 的传统是：

- 删字段时**先标 obsolete**（`#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS`）保留几个版本。
- 加 deprecation 注释 + IM_ASSERT 提醒。

定期开 `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` 编译——会报出**所有用到老 API 的代码**：

```cpp
// imconfig.h
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
```

清掉警告 → 你的代码"主动迁移"完成。

### Step 4：跑 Demo + Metrics

升级后开 Demo / Metrics 窗——所有内部状态可视。如果显示异常（窗口飞走、字体糊、控件不响应），用 Item Picker / ID Stack Tool 排查。

---

## 4.4.8 我应该读多深？

不是所有人都需要把 internal.h 全读完。按你的角色：

| 你的角色 | 推荐阅读深度 |
|---|---|
| **应用层用户**（用 ImGui 写编辑器/工具） | 只读 imgui.h，配合 imgui_demo.cpp |
| **集成者**（把 ImGui 接到自家引擎） | imgui.h 全 + Backend 接口 + ImGuiContext 概览 |
| **扩展者**（写自定义 widget / 节点编辑器） | + ItemAdd/ItemHoverable/ButtonBehavior + Widgets support SECTION |
| **魔改者**（修改 ImGui 源码） | 全部 + 跟踪 dev branch + 参与 issues |
| **平台贡献者**（写新 backend） | + 全部 SECTION + Multi-Viewport + 字体 atlas |

90% 用户停在前两层。本系列读者大概率是第 3~4 层——所以本章给你的 50 函数清单 + 4 铁律应该够用。

---

## 4.4.9 一些"少为人知"的有用 internal API

### `BeginDisabled / EndDisabled`：批量禁用

```cpp
// 公开 API（其实在 imgui.h，但常被忽略）
ImGui::BeginDisabled(condition);
ImGui::Button("X");   // 自动 grayed out
ImGui::SliderFloat(...);
ImGui::EndDisabled();
```

——所有 widget 都自动禁用 + 半透明显示。比手动 `if (cond) ImGui::Button` 优雅 10 倍。

### `PushItemFlag(ImGuiItemFlags_AllowOverlap, true)`

让相邻 widget 可以重叠（鼠标 hover 优先后画的）：

```cpp
ImGui::PushItemFlag(ImGuiItemFlags_AllowOverlap, true);
ImGui::Button("Background");
ImGui::SetCursorPos(ImVec2(20, 20));
ImGui::Button("Foreground");   // 在 Background 上面
ImGui::PopItemFlag();
```

——节点编辑器里"节点框 + 端口图标重叠"必备。

### `SplitterBehavior`：分隔条交互

```cpp
ImRect bb(...); 
float min_size_a = 50, min_size_b = 50;
float current_size_a = ...;
if (ImGui::SplitterBehavior(bb, id, ImGuiAxis_X, &current_size_a, &current_size_b, min_size_a, min_size_b))
    OnSplitterDrag(current_size_a);
```

——画布、面板分割的官方实现。

### `RenderNavCursor(bb, id)`

画键盘焦点的虚线框：

```cpp
if (id == g.NavId)
    ImGui::RenderNavCursor(bb, id);
```

——你的自定义 widget 想支持键盘导航时，这一行让它和原生控件一致。

### `GetForegroundDrawList()` 在窗口外画

```cpp
// 在屏幕任意位置画，不受当前窗口约束
ImDrawList* fg = ImGui::GetForegroundDrawList();
fg->AddRect(world_to_screen(target_pos), ..., IM_COL32_WHITE);
```

——3D scene 上叠加的"瞄准框 / 标记 / overlay"必备。

### `LogText / LogToFile`

ImGui 自带的文本捕获——`ImGui::Text` 等的输出可以重定向到文件：

```cpp
ImGui::LogToFile(0, "output.txt");
ImGui::Begin("MyData");
ImGui::Text("X = %d", x);
ImGui::Text("Y = %d", y);
ImGui::End();
ImGui::LogFinish();
// output.txt 现在包含 "X = 5\nY = 7\n"
```

——把 ImGui UI 当作"诊断打印通道"——免去写自家 Logger 转发。

---

## 4.4.10 ImGui 上没有的 / 自己实现要谨慎的

虽然 ImGui internal 很丰富，但有些场景**没有公开的 internal API**——你得自己写。常见：

### 撤销 / 重做（Undo / Redo）

ImGui 的 InputText 内部用 `imstb_textedit.h` 的 undo——但**不暴露**给应用代码。

如果你想给所有 widget 做 undo，必须自己写——监听 `IsItemDeactivatedAfterEdit`，记录改前/改后值。

### 多语言文本布局（Bidi / 复杂文本）

ImGui 的文本布局是**单方向 LTR**——没有 BiDi（左右文本混合）支持，没有 Arabic shaping。

如果做阿拉伯语 / 希伯来语 UI——需要外部 HarfBuzz 等做布局，再画到 DrawList。

### IME 候选窗口的内容

ImGui 通过 `Platform_SetImeData` 告诉 OS"IME 候选窗显示在哪里"——但**不接管候选词内容**。Win32 / Cocoa 的原生 IME 自己渲染候选窗。

如果你想完全自定义 IME（例如做游戏内"输入法"），要在 Backend 层做大量工作——超出 ImGui 范围。

---

## 4.4.11 第四部分总结

至此第四部分（状态管理 - Immediate Mode 的底层魔法）全部完成：

| 章 | 主题 | 核心收获 |
|---|---|---|
| 4.1 | ID Stack 哈希算法 | CRC32 + ### 重置 / 4 类 ID 冲突 + 修复方法 / IDStack 累积哈希 |
| 4.2 | ImGuiWindow 与 Z-Order | 100+ 字段按 8 类分组 / 5 个 Root 指针 / Windows / WindowsFocusOrder / WindowsTempSortBuffer 三 vector 协作 |
| 4.3 | 焦点 ActiveID 与 Item 状态机 | HoveredId/ActiveId/NavId 三大 ID / LastItemData / ItemHoverable 9 步 / ButtonBehavior 万能函数 / 三段式自定义 widget |
| 4.4 | imgui_internal.h 导读 | 28 SECTION 巡回 / 50 高频函数 / 4 条魔改铁律 / 4 个版本漂移案例 |

**第四部分回答的根本问题**：在没有 widget 对象树的情况下，ImGui 是怎么追踪几千个 widget 的状态的？

10 条机制全在这 4 章里：

1. **ID = `Hash(label, IDStack.back())`**——每个 widget 有持久身份。
2. **CRC32 哈希**——跨帧稳定，碰撞概率 1/4G。
3. **`###` 后缀重置**——动态文字保持稳定 ID。
4. **ImGuiWindow 100+ 字段**——每个窗口的完整状态。
5. **`g.Windows` / `WindowsFocusOrder`**——绘制顺序 / 焦点顺序双轨。
6. **5 个 Root 指针**——焦点链 / 导航边界 / 标题高亮等独立语义。
7. **`HoveredId / ActiveId / NavId`**——3 个全局 ID 跟踪当前交互对象。
8. **`LastItemData`**——所有 IsItemXXX 的数据源。
9. **`ItemAdd + ItemHoverable + ButtonBehavior`**——每个 widget 的标准三段式。
10. **`imgui_internal.h` "私有但开放"**——给魔改者全部内部 API，但需版本守护。

读完第四部分你应该能：
- 在 ID 冲突时秒级定位（不用 Item Picker 也能猜对）。
- 写复杂的自定义控件（节点编辑器、时间线）。
- 突破 ImGui 公开 API 的限制做引擎深度集成。
- 主动防御版本升级带来的破坏。

---

## 4.4.12 下一部分预告

**第五部分：高级定制与源码魔改实战**

第四部分讲的是"行为机制"。第五部分讲"实战应用"——

- 第 5.1 章：扩展 Layout API（自定义"网格"与"Flex Row"）——用 internal API 实现 CSS Flexbox 风格的布局。
- 第 5.2 章：高性能节点编辑器（直接调用 internal API）——用 ImDrawListSplitter + 三段式 widget + AllowOverlap 实现 1000 节点 60fps。
- 第 5.3 章：拦截渲染指令——`UserCallback` 与 Shader 参数注入——把 game scene 嵌入 ImGui 窗口。
- 第 5.4 章：综合实战——把以上能力沉淀成 Hazel 引擎的 `ImGuiLayer`——完整可编译的编辑器外壳。

读完第五部分你应该有"完整自研 ImGui 集成"的全部代码——这就是本系列的终点。

第四部分到此结束。当你说"开始第五部分"或"开始 5.1 章"时我们继续。
