# 第 4.1 章 · ID Stack 的哈希算法与 ID 冲突排错

> **本章目标**：把"ID Stack 系统"——immediate mode 的灵魂机制——的全部源码剖完。读完后你应该能：(1) 在白板上画出 `ImGui::Button("Save")` 的 ID 是怎么被哈希出来的；(2) 看到 ID 冲突 bug 时秒级定位（不需要 Item Picker）；(3) 设计自己的 ID 命名空间策略。
>
> **本章对应源码**：`imgui.cpp:2360~2479`（CRC32 表 + `ImHashData` + `ImHashStr`）、`imgui.cpp:9144~9197`（`ImGuiWindow::GetID*` 系列）、`imgui.cpp:9199~9300`（`PushID / PopID / GetID / PushOverrideID / GetIDWithSeed`）、`imgui.cpp:11286`（IM_ASSERT 阻止"空 ID 在窗口根"）、`imgui.cpp:11290~11301`（`IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS`）。
>
> **前置阅读**：第 0.1 章（心智模型 ②：ID Stack 即逻辑路径）、第 2.2 章（`ImGuiStorage`）。

---

## 4.1.0 为什么 ID 是 immediate mode 的"灵魂"

回顾第 0.1 章的"心智模型 ②"：

> ImGui 的全部代码都活在**一根时间轴**上，没有 widget 对象树。但你显然又需要一种方法把"昨天那个 Slider 的值"和"今天这个 Slider 的值"对应起来。这个方法就是 **ID**。

具体来说，ImGui 用 **`ImGuiID`（即 `ImU32`）**作为每个 widget 的"持久身份"。所有跨帧持久化的状态都靠 ID 索引：

```
Hovered/Active/Focus 状态        → g.HoveredId / g.ActiveId / g.NavId 都是 ImGuiID
窗口位置 / 折叠状态                → g.Windows 用 ID 标识
TreeNode 展开状态                 → ImGuiStorage 用 ID 作 key
InputText 光标位置 / Undo Stack   → g.InputTextState 用 ID 标识
拖拽 payload 持有                 → g.DragDropPayload.SourceId
快捷键路由                        → routing 表用 ID 作 key
表格列宽 / 列顺序                 → ImGuiTable 用 ID 作 key
所有"看起来是 widget"的状态        → 几乎全部用 ID 索引
```

**没有 ID 就没有 ImGui**——因为 immediate mode 永远不持有 widget 对象。

ID 必须满足两个要求：

1. **跨帧稳定**：今天的 Button 和昨天的 Button 必须哈希出**同一个 ID**——否则 hover/active 状态丢失。
2. **跨控件唯一**：不同 Button 必须有不同 ID——否则状态串台。

ImGui 怎么做到这两点？答案就是 **ID Stack + ImHashStr**。本章详解。

---

## 4.1.1 一个最小例子：`Button("Save")` 的 ID 是怎么算出来的

```cpp
ImGui::Begin("Editor");
ImGui::Button("Save");
ImGui::End();
```

我们追踪 `Button("Save")` 内部会算出什么 ID：

```cpp
// imgui_widgets.cpp 中 Button 的开头（简化）
bool ImGui::Button(const char* label, const ImVec2& size_arg) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;
    
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);   // ← 关键这里
    // ...
}
```

`window->GetID(label)` 的实现：

```cpp
// imgui.cpp:9144
ImGuiID ImGuiWindow::GetID(const char* str, const char* str_end)
{
    ImGuiID seed = IDStack.back();   // ← 当前 ID 栈顶
    ImGuiID id = ImHashStr(str, str_end ? (str_end - str) : 0, seed);
    return id;
}
```

**两步**：

1. 取 `IDStack.back()` 作为 seed（种子）。
2. 用 `ImHashStr` 把 label 字节流和 seed 一起哈希。

那么 `IDStack` 当前栈顶是什么？让我们看 `Begin("Editor")` 是怎么初始化它的。

```cpp
// imgui.cpp 中 Begin 内（简化）
ImGuiWindow* window = FindWindowByName("Editor");
if (!window) {
    window = CreateNewWindow("Editor");
    window->ID = ImHashStr("Editor", 0, 0);   // ← 用 seed=0 哈希窗口名
}
window->IDStack.resize(0);
window->IDStack.push_back(window->ID);   // ← 栈底就是窗口 ID
```

所以：

```
IDStack 状态：
  Begin("Editor")
    push_back(window->ID = Hash("Editor", seed=0) = 0xA1B2C3D4)
    IDStack = [0xA1B2C3D4]

  Button("Save")
    seed = IDStack.back() = 0xA1B2C3D4
    id = Hash("Save", seed=0xA1B2C3D4) = 0xE5F6A7B8

  End()
    （IDStack 栈顶在 EndFrame 时被清，但 Button 已经用过 id）
```

**ID = 0xE5F6A7B8**——这就是这个 Button 的"持久身份"。下一帧仍然 `Begin("Editor") + Button("Save")`，会算出同一个 0xE5F6A7B8。

### 嵌套 PushID 的累积

```cpp
ImGui::Begin("Editor");
ImGui::PushID("Tools");
    ImGui::Button("Save");          // ID = Hash("Save", Hash("Tools", Hash("Editor", 0)))
ImGui::PopID();
ImGui::End();
```

每个 `PushID` 把"当前栈顶的 hash 结果"作为 seed，哈希新 label，结果再 push 回栈。最终的 ID 就是**整个嵌套路径的累积哈希**。

```
栈状态变化：
1. Begin("Editor")             → IDStack = [Hash("Editor", 0)]
2. PushID("Tools")              → IDStack = [Hash("Editor", 0), Hash("Tools", Hash("Editor", 0))]
3. Button("Save") 内部计算       → id = Hash("Save", Hash("Tools", Hash("Editor", 0)))
4. PopID()                      → IDStack = [Hash("Editor", 0)]
5. End()
```

**这就是 ID Stack 的全部魔法**——ID 是路径的累积哈希。

---

## 4.1.2 `ImHashStr` 源码逐行剖析

打开 `imgui.cpp:2434`：

```cpp
// Zero-terminated string hash, with support for ### to reset back to seed value.
// e.g. "label###id" outputs the same hash as "id" (and "label" is generally displayed by the UI functions)
ImGuiID ImHashStr(const char* data_p, size_t data_size, ImGuiID seed)
{
    seed = ~seed;                                        // ① 反转 seed
    ImU32 crc = seed;
    const unsigned char* data = (const unsigned char*)data_p;
#ifndef IMGUI_ENABLE_SSE4_2_CRC
    const ImU32* crc32_lut = GCrc32LookupTable;
#endif
    if (data_size != 0)
    {
        while (data_size-- > 0)
        {
            unsigned char c = *data++;
            if (c == '#' && data_size >= 2 && data[0] == '#' && data[1] == '#')
            {
                crc = seed;                              // ② "###" 重置
                data += 2;
                data_size -= 2;
                continue;
            }
#ifndef IMGUI_ENABLE_SSE4_2_CRC
            crc = (crc >> 8) ^ crc32_lut[(crc & 0xFF) ^ c];   // ③ CRC32 步进
#else
            crc = _mm_crc32_u8(crc, c);                  // SSE 4.2 硬件 CRC
#endif
        }
    }
    else
    {
        // 没指定长度，按 0 终止处理
        while (unsigned char c = *data++)
        {
            if (c == '#' && data[0] == '#' && data[1] == '#')
            {
                crc = seed;
                data += 2;
                continue;
            }
            // ... 同上 CRC 步进
        }
    }
    return ~crc;                                         // ④ 反转回来
}
```

### 三个关键算法点

#### ① 起始反转 `seed = ~seed`

CRC32 标准实现要求初始化为 `0xFFFFFFFF`（全 1）。但 ImGui 把 `seed` 作为初始值——为了和 CRC32 标准等价，需要先反转一下。

最后输出时再 `~crc` 反转回来——使输出符合"标准 CRC32 协议"。

为什么这么做：让"seed=0 时哈希一个空字符串"得到 `Hash("", 0) = 0`——这是一个有用的不动点性质。

#### ② "###" 重置

```cpp
if (c == '#' && data_size >= 2 && data[0] == '#' && data[1] == '#') {
    crc = seed;
    data += 2;
    continue;
}
```

**遇到三个连续的 `#` 时，把当前 CRC 状态**重置回起始值**——等价于"忽略 `###` 之前的所有字符"**。

这是 ImGui ID 系统中**最关键的特性之一**。让我们看用法：

```cpp
ImGui::Button("Save###CommonId");   // 显示 "Save"，ID = Hash("###CommonId")
ImGui::Button("Load###CommonId");   // 显示 "Load"，ID = Hash("###CommonId")
// 两个 Button 显示文字不同，但 ID 完全相同！
```

**用途**：动态改变按钮显示文字而保持状态稳定。

```cpp
const char* label = is_recording ? "Stop###RecordButton" : "Record###RecordButton";
if (ImGui::Button(label)) {
    is_recording = !is_recording;
}
// 文字在 "Record" 和 "Stop" 之间切换，但 hover/active/animation 状态保留
```

如果不用 `###`，每次切换文字时 ID 会变 → 上一帧的 active 状态丢失 → 鼠标按下 "Record"，按住时切到 "Stop" → ImGui 认为新按钮没被按下，松开不触发 click。

#### ③ CRC32 步进

```cpp
crc = (crc >> 8) ^ crc32_lut[(crc & 0xFF) ^ c];
```

**经典的查表法 CRC32 算法**：

- 取 CRC 的低 8 位 XOR 当前字节 → 索引到 `crc32_lut`。
- CRC 右移 8 位 XOR 查表结果。

**`GCrc32LookupTable[256]`** 是 1024 字节的预计算表（`imgui.cpp:2364~2403`）。每个表项是"对当前字节做 8 次 CRC 单 bit 处理后的结果"——一次查表代替 8 次循环。

注释 `imgui.cpp:2360~2363` 说明：

> `// CRC32 needs a 1KB lookup table (not cache friendly)`
> `// Although the code to generate the table is simple and shorter than the table itself, using a const table allows us to:`
> `// - avoid an unnecessary branch/memory tap, - keep the ImHashXXX functions usable by static constructors, - make it thread-safe.`

——表是 const 的，所以**线程安全**——多线程同时调用 `ImHashStr` 安全。

#### ④ SSE 4.2 硬件加速

```cpp
#ifdef IMGUI_ENABLE_SSE4_2_CRC
    crc = _mm_crc32_u8(crc, c);
#endif
```

Intel SSE 4.2 引入了 `CRC32` 指令——一条指令完成步进。比查表快 5~10×。

**但是**：

- 它用的是 **CRC32C**（Castagnoli 多项式 0x1EDC6F41），与查表用的"通用 CRC32"（0xEDB88320）**不同**。
- 两者哈希结果不同——切换 `IMGUI_ENABLE_SSE4_2_CRC` 会让所有 ID 变。

`imgui.cpp:2386` 提供了 SSE 兼容版的查表（`#else` 分支）——保证两条路径**默认**情况下输出一致：

```cpp
#ifdef IMGUI_USE_LEGACY_CRC32_ADLER
    // 1.91.6 之前的旧表（兼容老 .ini）
#else
    // SSE 4.2 兼容的 CRC32c 表（新默认）
```

**1.91.6 起 ImGui 切换到 SSE 兼容表作为默认**——所以你的 .ini 文件里的窗口 ID 应该用新版 ImGui 重新写一次。如果你坚持兼容老 .ini，定义 `IMGUI_USE_LEGACY_CRC32_ADLER`。

### 为什么用 CRC32 而非 FNV-1a / 其他

注释 `imgui.cpp:2408` 说：

> `// FIXME-OPT: Replace with e.g. FNV1a hash? CRC32 pretty much randomly access 1KB. Need to do proper measurements.`

ImGui 自己也在质疑这个选择——FNV-1a 不需要查表（更 cache 友好）。但 CRC32 已经选了多年，切换会破坏所有 ID 兼容性。考虑到 CRC32 也有硬件加速（SSE 4.2），保留是合理的。

---

## 4.1.3 `ImHashData`：定长字节哈希

```cpp
// imgui.cpp:2409
ImGuiID ImHashData(const void* data_p, size_t data_size, ImGuiID seed)
{
    ImU32 crc = ~seed;
    const unsigned char* data = (const unsigned char*)data_p;
    const unsigned char *data_end = (const unsigned char*)data_p + data_size;
#ifndef IMGUI_ENABLE_SSE4_2_CRC
    const ImU32* crc32_lut = GCrc32LookupTable;
    while (data < data_end)
        crc = (crc >> 8) ^ crc32_lut[(crc & 0xFF) ^ *data++];
    return ~crc;
#else
    while (data + 4 <= data_end)
    {
        crc = _mm_crc32_u32(crc, *(ImU32*)data);   // ← 4 字节一次
        data += 4;
    }
    while (data < data_end)
        crc = _mm_crc32_u8(crc, *data++);          // 末尾不足 4 字节
    return ~crc;
#endif
}
```

**与 `ImHashStr` 的差异**：

- 不处理 `###` 重置——纯字节流。
- SSE 路径 4 字节一次（`_mm_crc32_u32`），更快。

**用途**：哈希非字符串数据，例如 `void*` 指针、`int`、自定义结构体：

```cpp
// imgui.cpp:9156~9178
ImGuiID ImGuiWindow::GetID(const void* ptr) {
    ImGuiID seed = IDStack.back();
    return ImHashData(&ptr, sizeof(void*), seed);    // ← 哈希指针的 8 字节
}

ImGuiID ImGuiWindow::GetID(int n) {
    ImGuiID seed = IDStack.back();
    return ImHashData(&n, sizeof(n), seed);          // ← 哈希 int 的 4 字节
}
```

---

## 4.1.4 三种 ID 来源：字符串 / 整数 / 指针

### 字符串 ID（最常用）

```cpp
ImGui::Button("Save");          // ID = Hash("Save", seed)
ImGui::Begin("MyWindow");        // 窗口 ID = Hash("MyWindow", 0)
ImGui::TreeNode("Section A");    // ID = Hash("Section A", seed)
```

### 整数 ID（循环用）

```cpp
for (int i = 0; i < 100; i++) {
    ImGui::PushID(i);
        ImGui::Button("Delete");   // 100 个 Button，每个 ID 不同
    ImGui::PopID();
}
```

### 指针 ID（对象关联）

```cpp
for (Item* item : items) {
    ImGui::PushID(item);            // 用对象地址作 seed
        ImGui::Button("Edit");
    ImGui::PopID();
}
```

**注意**：指针只在对象生命周期内有效——如果 item 被删除然后另一个 item 被分配到同一地址，会有 ID 冲突。所以**业务对象 ID 优先用业务标识符**（数据库 id 等），而不是内存地址。

---

## 4.1.5 `PushID / PopID` 实现

```cpp
// imgui.cpp:9199
void ImGui::PushID(const char* str_id) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID(str_id);
    window->IDStack.push_back(id);
}

void ImGui::PushID(int int_id) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID(int_id);
    window->IDStack.push_back(id);
}

void ImGui::PushID(const void* ptr_id) { /* 类似 */ }

void ImGui::PopID() {
    ImGuiContext& g = *GImGui;
    g.CurrentWindow->IDStack.pop_back();
}
```

——3 行实现。`PushID` 计算 ID 并入栈、`PopID` 弹栈。

`window->IDStack` 是 `ImVector<ImGuiID>`——`push_back` / `pop_back` 都是 O(1) 摊销。

### `PushOverrideID`：跳过哈希

```cpp
// imgui.cpp:9232
void ImGui::PushOverrideID(ImGuiID id) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    window->IDStack.push_back(id);
}
```

**直接把 `id` 入栈**——跳过 `Hash(str, seed)` 步骤。

用途：你有一个**预先算好的 ID**（例如从外部系统接进来的对象 ID），想直接拿来用：

```cpp
ImGuiID my_id = my_engine->GetEntityImGuiId(entity);
ImGui::PushOverrideID(my_id);
    ImGui::Button("Edit");   // ID = Hash("Edit", my_id)
ImGui::PopID();
```

`PushID(...)` 等价于 `PushOverrideID(GetID(...))`——前者更直观，后者跳过一次哈希。

### `GetIDWithSeed`：自定义 seed 哈希

```cpp
// imgui.cpp:9246
ImGuiID ImGui::GetIDWithSeed(const char* str, const char* str_end, ImGuiID seed) {
    ImGuiID id = ImHashStr(str, str_end ? (str_end - str) : 0, seed);
    return id;
}
```

**绕过 IDStack**——直接给一个 seed 算 ID。

用途：跨上下文组合 ID。例如：

```cpp
// 我想算"在 Window A 里画的某个 Button"的 ID，但当前 IDStack 是 Window B 的
ImGuiID seed = window_a->ID;
ImGuiID button_id = ImGui::GetIDWithSeed("Save", NULL, seed);
```

---

## 4.1.6 ID 冲突的 4 种典型场景

ID 冲突 = 两个不同的逻辑 widget 被算出同一个 ImGuiID。后果：

- 一个被点击，两个都显示"按下"动画。
- 焦点在它们之间错乱跳跃。
- TreeNode 展开状态串台。
- InputText 文本输入到错的 widget。

下面是 4 种典型场景。

### 场景 1：循环里没 PushID

**症状**：一组按钮只有第一个能点。

```cpp
// ❌ 错误
for (int i = 0; i < 5; i++) {
    if (ImGui::Button("Delete"))   // 5 个 button 都是 Hash("Delete", same_seed) → 同 ID
        DeleteItem(i);
}
```

**修复**：

```cpp
// ✅ 用 PushID(int)
for (int i = 0; i < 5; i++) {
    ImGui::PushID(i);
    if (ImGui::Button("Delete"))
        DeleteItem(i);
    ImGui::PopID();
}

// ✅ 或用 "##" 显式 ID
for (int i = 0; i < 5; i++) {
    char label[32];
    snprintf(label, sizeof(label), "Delete##%d", i);
    if (ImGui::Button(label))
        DeleteItem(i);
}
```

### 场景 2：标签相同的两个根级控件

**症状**：两个按钮其中一个不响应。

```cpp
// ❌ 错误
ImGui::Begin("Window");
ImGui::Button("OK");   // ID = Hash("OK", window_id)
// ... 一些代码 ...
ImGui::Button("OK");   // 同 ID！
ImGui::End();
```

**修复**：用 `###` 显式后缀区分。

```cpp
ImGui::Button("OK###OkButton1");   // ID = Hash("###OkButton1", window_id)
// ...
ImGui::Button("OK###OkButton2");   // 不同 ID
```

### 场景 3：在 TreeNode 内部和外部用相同标签

```cpp
// ❌ 错误
ImGui::Button("Apply");    // ID_a = Hash("Apply", window_id)
if (ImGui::TreeNode("Settings")) {
    ImGui::Button("Apply");    // ID_b = Hash("Apply", Hash("Settings", window_id)) — 不同
    ImGui::TreePop();
}
```

实际上**这不冲突**——TreeNode 自动在 IDStack 上 push 一层。所以 ID_a ≠ ID_b。

**真正冲突的版本**：

```cpp
// ❌ 用 BeginGroup 而非 TreeNode（BeginGroup 不影响 IDStack）
ImGui::Button("Apply");
ImGui::BeginGroup();
ImGui::Button("Apply");   // 同 ID！
ImGui::EndGroup();
```

修复：手工 PushID。

### 场景 4：动态生成的标签

```cpp
// ❌ 危险
char buf[32];
for (Item* item : items) {
    snprintf(buf, sizeof(buf), "%s", item->name.c_str());
    ImGui::Button(buf);   // 如果两个 item 同名，ID 冲突
}
```

修复：用 item 的唯一标识：

```cpp
for (Item* item : items) {
    ImGui::PushID(item->uuid);    // 数据库 uuid
    ImGui::Button(item->name.c_str());
    ImGui::PopID();
}
```

---

## 4.1.7 ImGui 自带的"空 ID 检测"防御

打开 `imgui.cpp:11286`：

```cpp
// imgui.cpp:11283~11286
// [DEBUG] People keep stumbling on this problem and using "" as identifier in the root of a window instead of "##something".
// Empty identifier are valid and useful in a small amount of cases, but 99.9% of the time you want to use "##something".
// READ THE FAQ: https://dearimgui.com/faq
IM_ASSERT(id != window->ID && "Cannot have an empty ID at the root of a window. If you need an empty label, use ## and read the FAQ about how the ID Stack works!");
```

**解读**：在 `ItemAdd` 中如果 `id == window->ID`——意味着用户提交了一个**没标签**的 widget 在窗口根：

```cpp
ImGui::Begin("Window");
ImGui::Button("");   // label = ""，Hash("", window_id) = window_id
ImGui::End();
```

`ImHashStr("", 0, window_id)` 的结果就是 `window_id` 本身（空字符串不改变 CRC 状态）——和窗口 ID 冲突！

ImGui 直接 IM_ASSERT 失败——给你最早的警告。修复：

```cpp
ImGui::Button("##unique_id");   // 显式 ID，不与窗口 ID 冲突
```

---

## 4.1.8 `ConfigDebugHighlightIdConflicts`：实时冲突检测

```cpp
// imgui.h:2484
bool ConfigDebugHighlightIdConflicts;          // = true   // Highlight and show error popup on multiple items with same ID

// imgui.cpp:5544~5545
if (g.IO.ConfigDebugHighlightIdConflicts && g.HoveredIdPreviousFrameItemCount > 1)
    g.DebugDrawIdConflictsId = g.HoveredIdPreviousFrame;
```

**默认开启**——检测到鼠标悬停的 ID 在上一帧被多于一个 item 使用，就标红。

实战中你会看到：

- 多个按钮 hover 都同时高亮 → 立刻知道有 ID 冲突。
- 弹出 error tooltip 提示 ID 信息。
- 提供 "Item Picker" 按钮帮你定位。

**这是免费的开发期保险**——开发时绝对不要关。

### `IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS`：硬核检测

```cpp
// imgui.cpp:11290~11301
#ifdef IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS
if ((g.LastItemData.ItemFlags & ImGuiItemFlags_AllowDuplicateId) == 0)
{
    int* p_alive = g.DebugDrawIdConflictsAliveCount.GetIntRef(id, -1);
    int* p_highlight = g.DebugDrawIdConflictsHighlightSet.GetIntRef(id, -1);
    if (*p_alive == g.FrameCount)
        *p_highlight = g.FrameCount;
    *p_alive = g.FrameCount;
    if (*p_highlight >= g.FrameCount - 1)
        window->DrawList->AddRect(bb.Min - ImVec2(1, 1), bb.Max + ImVec2(1, 1), IM_COL32(255, 0, 0, 255), 0.0f, ImDrawFlags_None, 2.0f);
}
#endif
```

**编译期开关**——把所有冲突的 widget 都画上红框，**无论是否 hover**。

代价：每帧 ItemAdd 都查 `ImGuiStorage` 两次——慢。所以注释 `imgui.cpp:11288` 强调：

> `// THIS WILL SLOW DOWN DEAR IMGUI. DON'T KEEP ACTIVATED.`

仅在排查"哪里都有 ID 冲突"时临时开。

---

## 4.1.9 `imgui_internal.h:160` 的 `ImGuiLastItemData`

```cpp
// imgui_internal.h:1377
struct ImGuiLastItemData {
    ImGuiID                 ID;            // 上一个 item 的 ID
    ImGuiItemFlags          ItemFlags;
    ImGuiItemStatusFlags    StatusFlags;   // Hovered / Edited / Activated 等
    ImRect                  Rect;
    ImRect                  NavRect;
    ImRect                  DisplayRect;
    ImRect                  ClipRect;
    ImGuiKeyChord           Shortcut;
};
```

**`g.LastItemData.ID`** 是上一个被 `ItemAdd` 提交的 widget 的 ID——`IsItemHovered` / `IsItemActive` / `IsItemEdited` 等 API 都通过它读取。

```cpp
ImGui::Button("X");
if (ImGui::IsItemHovered()) {   // 内部读 g.LastItemData.ID
    // 鼠标悬停
}
```

第 4.3 章会专题展开 `LastItemData` 与 ActiveId / HoveredId 的协作。

---

## 4.1.10 ID Stack Tool：反查 ID 来源

ImGui 自带 **ID Stack Tool**（Demo → Tools → ID Stack Tool）。

### 用法

1. 用 Item Picker 选中一个 widget（鼠标光标变成十字时点击）。
2. ID Stack Tool 显示这个 widget 的 ID 是怎么哈希出来的。

例如你点击了一个嵌套的 Button：

```
Window: "Editor" (id=0xA1B2C3D4)
└─ PushID: "Tools" (id=0xC3D4E5F6)
   └─ PushID: 5 (id=0x12345678)
      └─ Item: "Save" (id=0xABCD1234)
```

清楚显示 ID 的"路径"——4 层嵌套，每层贡献了什么。

### 实现原理

```cpp
// imgui.cpp:9148~9152 (在 GetID 内部)
#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (g.DebugHookIdInfoId == id)
        ImGui::DebugHookIdInfo(id, ImGuiDataType_String, str, str_end);
#endif
```

每次 `GetID` 计算出一个 ID 时——如果该 ID 等于 `g.DebugHookIdInfoId`（用户通过 Item Picker 选中的目标 ID）——记录这一次"贡献"。

`DebugHookIdInfo` 把 (label, type, parent_seed) 入队。最后 ID Stack Tool 显示整个队列——就是 ID 的"贡献链"。

**性能**：默认 `g.DebugHookIdInfoId == 0`，比较通常失败——零开销。仅在用户启用 ID Stack Tool 时才有效。

---

## 4.1.11 一些特殊的 ID 计算

### `GetIDFromPos`：用屏幕位置作 ID

```cpp
// imgui.cpp:9182
ImGuiID ImGuiWindow::GetIDFromPos(const ImVec2& p_abs) {
    ImGuiID seed = IDStack.back();
    ImVec2 p_rel = ImGui::WindowPosAbsToRel(this, p_abs);
    return ImHashData(&p_rel, sizeof(p_rel), seed);
}
```

**用窗口内相对坐标作哈希原料**——给"由位置标识的 widget"用：

- 拖拽 hotspot（不在 IDStack 中的临时区域）。
- "BackgroundChannel"（非 widget 的图元区段）。

`p_rel` 用窗口相对坐标——这样窗口移动时，ID 保持稳定（不依赖屏幕绝对位置）。

### `GetIDFromRectangle`：用矩形作 ID

```cpp
ImGuiID ImGuiWindow::GetIDFromRectangle(const ImRect& r_abs) {
    ImGuiID seed = IDStack.back();
    ImRect r_rel = ImGui::WindowRectAbsToRel(this, r_abs);
    return ImHashData(&r_rel, sizeof(r_rel), seed);
}
```

类似——给"由矩形区域标识的 widget"用。

---

## 4.1.12 实战：在 Hazel 节点编辑器里设计 ID 命名空间

考虑一个节点编辑器：每个节点有 ID + N 个 pin（输入/输出端口） + M 条连线。

### 朴素方案（容易冲突）

```cpp
ImGui::PushID(node_id);   // 节点 ID
    ImGui::Button("Header");
    for (int p = 0; p < node->pins.size(); p++) {
        ImGui::PushID(p);
        ImGui::Button("Pin");
        ImGui::PopID();
    }
ImGui::PopID();
```

问题：如果两个不同节点都有 pin index 5，它们的 pin button 被算入同一节点的 IDStack——但实际上是不同节点。**不冲突，但理由偶然**。

### 更显式的命名空间方案

```cpp
ImGui::PushID(node_id);
    ImGui::PushID("Header");
        ImGui::Button("Header");
    ImGui::PopID();
    
    ImGui::PushID("Pins");
        for (Pin* pin : node->pins) {
            ImGui::PushID(pin->id);
            ImGui::Button("Pin");
            ImGui::PopID();
        }
    ImGui::PopID();
    
    ImGui::PushID("Body");
        // 节点内部其他控件
    ImGui::PopID();
ImGui::PopID();
```

**好处**：

- 每个"语义类别"有自己的命名空间。
- pin 用 `pin->id`（持久化的）而非 index。
- 即使节点 N 的 Pin5 和节点 M 的 Header 凑巧 hash 出同一个数（极不可能但理论可能），不同的 namespace 路径让最终 ID 不会撞。

### 与外部系统的 ID 桥接

如果 pin 来自数据库或网络，它们已经有 `int64_t pin_id`：

```cpp
// 用 PushOverrideID 直接桥接
ImGuiID imgui_id = (ImGuiID)(pin_id ^ (pin_id >> 32));   // 64-bit 映射到 32-bit
ImGui::PushOverrideID(imgui_id);
ImGui::Button("Pin");
ImGui::PopID();
```

**注意 32-bit 限制**：`ImGuiID = ImU32`，理论碰撞概率 1 / 4G。对 < 100K pin 的场景，碰撞概率可忽略（生日悖论：100K² / 4G ≈ 2%）。如果你担心，64-bit ID 可以 hash 后用：

```cpp
ImGuiID imgui_id = ImHashData(&pin_id_64, sizeof(int64_t), 0);
```

---

## 4.1.13 ID 的存储与查找：`ImGuiStorage` 的回顾

每个 ImGuiWindow 有：

```cpp
ImGuiStorage StateStorage;   // 用 ImGuiID 作 key 的 KV 存储
```

第 2.2 章已经详细讲过 `ImGuiStorage` 的 sorted vector + 二分查找实现。这里只回顾用法：

```cpp
// 例如 TreeNode 的展开状态
bool ImGui::TreeNode(const char* label) {
    ImGuiID id = window->GetID(label);
    ImGuiStorage* storage = window->DC.StateStorage;
    bool* p_opened = storage->GetBoolRef(id, false);   // 默认关闭
    
    if (clicked) *p_opened = !*p_opened;
    
    if (*p_opened) {
        // ... 渲染 ...
        return true;   // 展开了
    }
    return false;
}
```

每个 widget 用自己的 ID 作 key，把"我的状态"存在 window->StateStorage。**完全无对象的状态持久化**——这是 immediate mode 的关键技巧。

---

## 4.1.14 性能预算：ID Stack 一帧的开销

一帧典型 5000 widget × 每个 1~3 次 GetID = 10K~15K 次哈希调用。

```
每次 ImHashStr(label_len=10, seed) 大约：
  - 表查找路径 (CRC32 lookup)：~30 cycles (cache miss 风险)
  - SSE 路径 (_mm_crc32_u8)：~15 cycles
```

15K × 30 = 450K cycles ≈ 0.15 ms（3 GHz CPU）。**不到一帧时间的 1%**——完全可忽略。

但如果你做错了——例如每帧重复算同一个 ID 100 次（不缓存 ID 而每次重新调 `GetID`），会变成 50 ms。所以 ImGui 内部会缓存 LastItemData.ID，不重复算。

---

## 4.1.15 调试 ID 的 5 个实用技巧

### 1. 看一个 widget 的 ID

```cpp
ImGui::Button("X");
ImGui::Text("Last ID: 0x%08X", ImGui::GetItemID());
```

`GetItemID` 返回 `g.LastItemData.ID`——上一个 item 的 ID。

### 2. 比较两个标签的哈希

```cpp
ImGui::Text("'X' hash: 0x%08X", ImHashStr("X", 0, 0));
ImGui::Text("'Y' hash: 0x%08X", ImHashStr("Y", 0, 0));
```

注意 seed=0——纯字符串哈希，不带窗口前缀。

### 3. 在 console 打印冲突的位置

```cpp
ImGui::Button("Foo");
ImGuiID id = ImGui::GetItemID();
HZ_CORE_TRACE("[{}] Button 'Foo' has ID 0x{:08X}", __FUNCTION__, id);
```

跑两次发现两个不同位置打出同样 ID → 冲突源头。

### 4. 用 IM_ASSERT 验证 ID 唯一

```cpp
static std::set<ImGuiID> seen_this_frame;
seen_this_frame.clear();   // 每帧开始

// 在每个关键 widget 后：
ImGui::Button("X");
auto [_, inserted] = seen_this_frame.insert(ImGui::GetItemID());
IM_ASSERT(inserted && "Duplicate ID detected!");
```

### 5. 用 Demo → Tools → ID Stack Tool

终极武器。Item Picker 选中目标 → Stack Tool 显示完整路径。

---

## 4.1.16 本章小结

ID Stack 系统的 6 条核心：

1. **ID = ImGuiID = ImU32**——32 位无符号整数。
2. **ID 计算 = `Hash(label, seed)`**——seed 是 IDStack 栈顶。
3. **`PushID` 算并入栈、`PopID` 弹栈**——栈深度 = 嵌套深度。
4. **ImHashStr 用 CRC32**——SSE 4.2 硬件加速可选，1KB 查表为兜底。
5. **`###` 后缀重置 hash 状态**——让动态文字保持稳定 ID。
6. **窗口 ID = `Hash(window_name, 0)`**——是 IDStack 的栈底。

记住的 4 个特殊 API：

```cpp
ImGui::PushID("X");                    // 字符串 push
ImGui::PushID(42);                      // 整数 push
ImGui::PushID(ptr);                     // 指针 push
ImGui::PushOverrideID(id);              // 直接 push ID
ImGui::GetID("X");                      // 算 ID 但不入栈
ImGui::GetIDWithSeed("X", NULL, seed);  // 用自定义 seed 算
```

ID 冲突的 4 类场景 + 4 类修复方法（PushID(int) / "##" 后缀 / "###" 显式 ID / 业务 ID 桥接）已经覆盖 95% 的实战需求。

---

## 4.1.17 下一章预告

第 4.2 章 [[16_第四部分_02_ImGuiWindow与Z-Order]] 会带你看 `ImGuiWindow` 这个**最复杂**的内部结构（100+ 字段）：

- 字段按 8 个职能分组（几何 / 状态 / 输入 / DrawList / 焦点链 / Settings / Docking / 调试）。
- `Begin()` 内部的窗口创建与复用流程。
- `g.Windows / WindowsFocusOrder / WindowsTempSortBuffer` 的 3 个 vector 协作。
- ChildWindow / Popup / Tooltip / DockedWindow 的 Z-Order 优先级。
- `RootWindow / RootWindowPopupTree / RootWindowForNav` 等 5 个 root 链。

读完后你应该能在调试器里看着 `ImGuiWindow` 字段一一说出"这是干什么的"——为第 5 章节点编辑器自定义 widget 做准备。
