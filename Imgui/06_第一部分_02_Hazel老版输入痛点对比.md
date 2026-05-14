# 第 1.2 章 · Cherno Hazel 老版输入处理痛点对比分析

> **本章目标**：把 Cherno Hazel 教程里那份"看似简单实则地雷遍布"的 ImGui 集成代码**逐行剥皮**，找出它在现代 ImGui (1.92+) 下**编译失败 / 行为异常 / 隐性 Bug** 的全部点位。读完后你会清楚地知道：迁移到现代 API 不是"可选优化"，而是"必须做"。
>
> **本章对应源码**：`imgui.h:2563~2624`（IO 中"Input"区段，含废弃字段注释）、`imgui.h:2606~2609`（Legacy 字段的"墓碑注释"）、`imgui.h:2519~2528`（现代 `AddXxxEvent` API）、`imgui.cpp:[SECTION] INPUTS`（输入处理流程）、`backends/imgui_impl_glfw.cpp` 中的回调函数（参考实现）。

---

## 1.2.0 时代背景：Hazel 教程录制时的 ImGui 是什么样

Cherno 在 2019~2020 年间录制 Hazel 教程的"ImGui 集成"视频时，使用的是 **ImGui 1.74~1.84** 区间的版本。彼时的 ImGui 输入 API 长这样：

```cpp
// 1.84 时代的 ImGuiIO（节选关键部分）
struct ImGuiIO {
    int     KeyMap[ImGuiKey_COUNT];        // ImGuiKey → 你的 native key code
    bool    KeysDown[512];                 // [native_key_code] → 是否按下
    bool    MouseDown[5];                  // [button] → 是否按下
    ImVec2  MousePos;                      // 鼠标位置
    float   MouseWheel, MouseWheelH;       // 鼠标滚轮
    bool    KeyCtrl, KeyShift, KeyAlt, KeySuper;   // Mod 键状态
    float   NavInputs[ImGuiNavInput_COUNT];        // 手柄轴向值

    void    AddInputCharacter(unsigned int c);     // 字符输入（这一个保留至今）
    // ...
};
```

**核心模式**：用户每帧把"OS 输入快照"**直接写进 `io.KeysDown[]` / `io.MouseDown[]` 等数组**。ImGui 内部读这些数组，与上一帧对比，得出 click / press / release 等高级事件。

Cherno 的 `Hazel/src/Hazel/ImGui/ImGuiBuild.cpp` 和 `ImGuiLayer.cpp` 完全建立在这个模式上。**这套模式 ImGui 1.87 (2022年) 起被彻底废弃**。

---

## 1.2.1 解剖样本：Hazel 老版 `ImGuiLayer::OnEvent` 全文

下面这段是从 Cherno 的 Hazel 视频 `Episode 32: ImGui Events` 提炼出来的代表性代码（略有简化，真实 GitHub 仓库代码大同小异）：

```cpp
// Hazel/src/Hazel/ImGui/ImGuiLayer.cpp (老版本 - 不能在现代 ImGui 编译)

void ImGuiLayer::OnAttach() {
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

    // ❌ 痛点 1：手工填 KeyMap（已废除）
    io.KeyMap[ImGuiKey_Tab]         = HZ_KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow]   = HZ_KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow]  = HZ_KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow]     = HZ_KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow]   = HZ_KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp]      = HZ_KEY_PAGE_UP;
    io.KeyMap[ImGuiKey_PageDown]    = HZ_KEY_PAGE_DOWN;
    io.KeyMap[ImGuiKey_Home]        = HZ_KEY_HOME;
    io.KeyMap[ImGuiKey_End]         = HZ_KEY_END;
    io.KeyMap[ImGuiKey_Insert]      = HZ_KEY_INSERT;
    io.KeyMap[ImGuiKey_Delete]      = HZ_KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace]   = HZ_KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Space]       = HZ_KEY_SPACE;
    io.KeyMap[ImGuiKey_Enter]       = HZ_KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape]      = HZ_KEY_ESCAPE;
    io.KeyMap[ImGuiKey_A]           = HZ_KEY_A;
    io.KeyMap[ImGuiKey_C]           = HZ_KEY_C;
    io.KeyMap[ImGuiKey_V]           = HZ_KEY_V;
    io.KeyMap[ImGuiKey_X]           = HZ_KEY_X;
    io.KeyMap[ImGuiKey_Y]           = HZ_KEY_Y;
    io.KeyMap[ImGuiKey_Z]           = HZ_KEY_Z;

    ImGui_ImplOpenGL3_Init("#version 410");
}

void ImGuiLayer::OnEvent(Event& event) {
    EventDispatcher dispatcher(event);
    dispatcher.Dispatch<MouseButtonPressedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseButtonPressedEvent));
    dispatcher.Dispatch<MouseButtonReleasedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseButtonReleasedEvent));
    dispatcher.Dispatch<MouseMovedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseMovedEvent));
    dispatcher.Dispatch<MouseScrolledEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnMouseScrolledEvent));
    dispatcher.Dispatch<KeyPressedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyPressedEvent));
    dispatcher.Dispatch<KeyTypedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyTypedEvent));
    dispatcher.Dispatch<KeyReleasedEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnKeyReleasedEvent));
    dispatcher.Dispatch<WindowResizeEvent>(HZ_BIND_EVENT_FN(ImGuiLayer::OnWindowResizeEvent));
}

bool ImGuiLayer::OnMouseButtonPressedEvent(MouseButtonPressedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    // ❌ 痛点 2：直接写 MouseDown[] 数组（不再推荐，但暂时还能编译）
    io.MouseDown[e.GetMouseButton()] = true;
    return false;
}

bool ImGuiLayer::OnMouseButtonReleasedEvent(MouseButtonReleasedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDown[e.GetMouseButton()] = false;
    return false;
}

bool ImGuiLayer::OnMouseMovedEvent(MouseMovedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    // ❌ 痛点 3：直接写 MousePos（多视口下错位）
    io.MousePos = ImVec2(e.GetX(), e.GetY());
    return false;
}

bool ImGuiLayer::OnMouseScrolledEvent(MouseScrolledEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseWheelH += e.GetXOffset();
    io.MouseWheel += e.GetYOffset();
    return false;
}

bool ImGuiLayer::OnKeyPressedEvent(KeyPressedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    // ❌ 痛点 4：直接写 KeysDown[]（已删除）
    io.KeysDown[e.GetKeyCode()] = true;

    // ❌ 痛点 5：手动派生 Mod 键（已被自动化）
    io.KeyCtrl  = io.KeysDown[HZ_KEY_LEFT_CONTROL]  || io.KeysDown[HZ_KEY_RIGHT_CONTROL];
    io.KeyShift = io.KeysDown[HZ_KEY_LEFT_SHIFT]    || io.KeysDown[HZ_KEY_RIGHT_SHIFT];
    io.KeyAlt   = io.KeysDown[HZ_KEY_LEFT_ALT]      || io.KeysDown[HZ_KEY_RIGHT_ALT];
    io.KeySuper = io.KeysDown[HZ_KEY_LEFT_SUPER]    || io.KeysDown[HZ_KEY_RIGHT_SUPER];
    return false;
}

bool ImGuiLayer::OnKeyReleasedEvent(KeyReleasedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    io.KeysDown[e.GetKeyCode()] = false;
    return false;
}

bool ImGuiLayer::OnKeyTypedEvent(KeyTypedEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    int keycode = e.GetKeyCode();
    if (keycode > 0 && keycode < 0x10000)
        io.AddInputCharacter((unsigned short)keycode);
    return false;
}
```

**总共 6 个痛点**。我们逐个剖。

---

## 1.2.2 痛点 1：`io.KeyMap[]` 整体废除

### 老 API 的逻辑

老版 ImGui 的 `KeyMap` 是这样工作的：

```cpp
// ImGui 1.84 中
enum ImGuiKey_ {                    // ImGui 抽象键（只有 22 个）
    ImGuiKey_Tab = 0,
    ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow,
    // ... 仅 ~22 个
    ImGuiKey_COUNT
};

struct ImGuiIO {
    int  KeyMap[ImGuiKey_COUNT];   // KeyMap[ImGuiKey_Tab] = 你的 native_key_code(258)
    bool KeysDown[512];             // KeysDown[258] = true 表示 Tab 按下
};

// ImGui 内部检测 Tab 是否按下：
bool ImGui::IsKeyPressed(ImGuiKey key) {
    return io.KeysDown[io.KeyMap[key]];   // 二级查表
}
```

**老 API 的"两层映射"**：
- 用户用 `ImGuiKey_Tab` 表达"我说的是 Tab 键"。
- ImGui 通过 `KeyMap` 把 `ImGuiKey_Tab` 转成"用户引擎中的 native key code"（GLFW 中是 258）。
- ImGui 用 native key code 在 `KeysDown[]` 里查状态。

这个设计的初衷是**让 ImGui 不依赖具体的键盘平台代码**——你的引擎用 GLFW 的 258，别人的引擎可能用 SDL 的 SDLK_TAB（9），ImGui 不在乎，它只通过你给的 KeyMap 翻译。

### 现代 API 的"颠覆"

```cpp
// imgui.h:2606~2607（注释段，证明被删除）
//int       KeyMap[ImGuiKey_COUNT];     // [LEGACY] Input: map of indices into the KeysDown[512] entries array which represent your "native" keyboard state. The first 512 are now unused and should be kept zero. Legacy backend will write into KeyMap[] using ImGuiKey_ indices which are always >512.
//bool      KeysDown[ImGuiKey_COUNT];   // [LEGACY] Input: Keyboard keys that are pressed (ideally left in the "native" order your engine has access to keyboard keys, so you can use your own defines/enums for keys). This used to be [512] sized. It is now ImGuiKey_COUNT to allow legacy io.KeysDown[GetKeyIndex(...)] to work without an overflow.
```

`KeyMap[]` 数组**整个被删除**。任何代码引用 `io.KeyMap[ImGuiKey_Tab]` —— **编译失败**。

### 为什么删

1.87 的 ImGuiKey 枚举从 22 个**扩展到 150+ 个**——包含所有键盘键（A-Z、F1-F24、Numpad、控制键、符号键、Mod键等）+ 鼠标键 + 手柄按钮：

```cpp
// 现代 imgui.h 中的 ImGuiKey 枚举（节选）
enum ImGuiKey : int {
    ImGuiKey_None = 0,
    ImGuiKey_Tab = 512,                  // ← 注意从 512 开始（避开旧的 [0,512) native 区段）
    ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow,
    // ... 字母 A-Z
    ImGuiKey_A,
    // ...
    // ... 数字 0-9
    ImGuiKey_0,
    // ...
    // ... 功能键 F1-F24
    ImGuiKey_F1,
    // ...
    // ... 符号键
    ImGuiKey_Apostrophe,
    ImGuiKey_Comma,
    // ...
    // ... 手柄按钮
    ImGuiKey_GamepadStart,
    ImGuiKey_GamepadFaceDown,
    // ...
    // ... 鼠标按钮（作为别名）
    ImGuiKey_MouseLeft,
    // ...
    // ... Mod 键
    ImGuiMod_Ctrl,
    ImGuiMod_Shift,
    // ...
};
```

现在 `ImGuiKey` 已经"自带所有键的标识"——**不需要中间映射表**。Backend 直接告诉 ImGui "ImGuiKey_Tab 按下了"，ImGui 直接记录在 `io.KeysData[ImGuiKey_Tab - ImGuiKey_NamedKey_BEGIN]` 里。

`imgui.cpp:9396` 的 IM_ASSERT 给了一个明确的"墓碑信息"：

```cpp
IM_ASSERT(IsNamedKey(key) && "Support for user key indices was dropped in favor of ImGuiKey. Please update backend & user code.");
```

——你看到这个 assert，就是引用了已废弃的 native key 索引。

### 痛点 1 的具体后果

把老 Hazel 代码原样放到现代 ImGui 编译：

```
error: ‘struct ImGuiIO’ has no member named ‘KeyMap’
   io.KeyMap[ImGuiKey_Tab] = HZ_KEY_TAB;
      ^~~~~~
```

**编译就过不去**。无法绕过——`KeyMap` 不存在了。

---

## 1.2.3 痛点 2：`io.MouseDown[]` 直接写入仍能编译，但不推荐

```cpp
io.MouseDown[e.GetMouseButton()] = true;
```

这一行**在现代 ImGui 仍能编译**——`io.MouseDown[5]` 字段还在（`imgui.h:2566`）：

```cpp
bool        MouseDown[5];           // Mouse buttons: 0=left, 1=right, 2=middle + extras
```

但 `imgui.h:2563` 的注释明确说：

> `(this block used to be written by backend, since 1.87 it is best to NOT write to those directly, call the AddXXX functions above instead)`

### 为什么"能编译但不推荐"

**直接写**的问题：

1. **绕过事件队列**：直接写跳过了 `g.InputEventsQueue` 的处理。下面会讲到，事件队列让 ImGui 能做"trickle"（事件分摊）—— low fps 下保证 click/release 不被合并丢失。直接写丢失这个优化。

2. **绕过事件元数据**：现代事件还携带 `MouseSource`（鼠标 / 触屏 / 触控笔）。直接写无法告知 ImGui 这是触屏事件还是鼠标事件——某些控件（如 InputText 长按弹出菜单）依赖这个区分。

3. **绕过 Multi-Viewport 投影**：在 Multi-Viewport 模式下，鼠标点击事件需要被 ImGui 的"viewport 路由"逻辑处理——直接写绕过这个逻辑，副 viewport 上的点击会被错算成主 viewport。

4. **未来兼容性**：注释说 "since 1.87 it is best to NOT write"——这是官方的"软废弃"信号。可能某天会硬废弃。

### 痛点 2 的解法（预告，详见 1.3 章）

```cpp
// 老
io.MouseDown[e.GetMouseButton()] = true;

// 现代
io.AddMouseButtonEvent(e.GetMouseButton(), true);
```

`AddMouseButtonEvent` 内部会把事件入队，等 NewFrame 时再写入 `io.MouseDown[]`。

---

## 1.2.4 痛点 3：`io.MousePos = ImVec2(...)` 在 Multi-Viewport 下崩溃

```cpp
io.MousePos = ImVec2(e.GetX(), e.GetY());
```

**单视口下能用，Multi-Viewport 下完全错乱**。

### 不开 Multi-Viewport 时

`e.GetX() / e.GetY()` 通常是**鼠标在 OS 窗口客户区中的坐标**——左上角 (0,0)，右下角 (width, height)。

ImGui 内部把所有 UI 元素也用同一坐标系——也是 (0,0) 在主窗口左上角。所以这一行是对的。

### 开了 Multi-Viewport 后

ImGui 的内部坐标系**变成"虚拟桌面坐标"**——多显示器拼接在一起，左上角的显示器有 (0,0)，第二显示器可能是 (1920, 0)，依此类推。每个 ImGuiViewport 在虚拟桌面中有自己的 `Pos`。

**问题来了**：

`e.GetX()` 仍然是 OS 窗口客户区局部坐标。如果用户把 ImGui 窗口拖到第二显示器，并把鼠标移到那个 ImGui 窗口上：
- OS 给 Hazel 主窗口发的鼠标移动事件包含的是"主窗口客户区"的鼠标坐标——但鼠标已经离开了主窗口。
- 副显示器上有自己的 OS 窗口（被 ImGui 创建用作副 viewport），它的鼠标事件 OS 单独发送。

如果用 `io.MousePos = ImVec2(e.GetX(), e.GetY())` 笼统地写：
- 来自主窗口的鼠标事件覆盖了来自副 viewport 的事件。
- 鼠标在副 viewport 上时，主窗口可能根本没收到事件，`io.MousePos` 是上次主窗口事件的位置（陈旧）。

**症状**：副窗口上的鼠标完全无响应，或者鼠标在副窗口上"瞬移"。

### 现代 API 怎么解决

```cpp
// AddMousePosEvent 不要 OS 局部坐标，而是要"虚拟桌面坐标"
io.AddMousePosEvent(virtual_desktop_x, virtual_desktop_y);
```

Backend 必须做坐标转换：把"OS 窗口局部坐标 + 窗口的虚拟桌面位置" 加起来，得到虚拟桌面坐标。`imgui_impl_glfw.cpp` 的现代实现：

```cpp
// 简化伪代码
static void ImGui_ImplGlfw_CursorPosCallback(GLFWwindow* window, double x, double y) {
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // 把局部坐标加上窗口的虚拟桌面位置
        int wx, wy;
        glfwGetWindowPos(window, &wx, &wy);
        x += wx;
        y += wy;
    }
    io.AddMousePosEvent((float)x, (float)y);
}
```

Hazel 老代码完全没有这个转换——**Multi-Viewport 必崩**。

### 痛点 3 的额外细节：MousePos 的"无效值"

现代 API 用一个特殊值表示"鼠标不可用"：

```cpp
io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
```

例如应用失焦、鼠标离开所有 ImGui 窗口范围、应用最小化。这让 ImGui 知道"现在不要假装鼠标在 (0,0)"——避免 (0,0) 处的控件误响应。

老代码`io.MousePos = ImVec2(e.GetX(), e.GetY())` 没有这个机制。失焦时鼠标位置可能停留在最后一个值，导致 hover 状态僵住。

---

## 1.2.5 痛点 4：`io.KeysDown[]` 整体废除

```cpp
io.KeysDown[e.GetKeyCode()] = true;
```

**编译失败**。

```
error: ‘struct ImGuiIO’ has no member named ‘KeysDown’
```

### 为什么删

旧 API 的 `KeysDown[512]`：
- 索引是"native key code"（你引擎中的键码，例如 GLFW 的 258 表示 Tab）。
- 大小固定 512——某些键盘（带特殊键的）超过 512。
- 与 `KeyMap[]` 联动——必须先填 KeyMap 才能用。

新 API 用 `io.KeysData[ImGuiKey_NamedKey_COUNT]`：

```cpp
// imgui.h:2577
ImGuiKeyData  KeysData[ImGuiKey_NamedKey_COUNT];
```

索引是 ImGui 自己的 `ImGuiKey_*` 抽象枚举（>512 的范围）。每个 `ImGuiKeyData` 包含：

```cpp
struct ImGuiKeyData {
    bool    Down;             // 是否按下
    float   DownDuration;     // 按下持续时间（秒）
    float   DownDurationPrev; // 上一帧的 DownDuration
    float   AnalogValue;      // 模拟值（手柄轴用）
};
```

新 API 的好处：
- 索引就是抽象键，不需要二级映射。
- 每个键的"按下持续时间"自动维护——支持 IsKeyPressed 的 `repeat` 参数无缝运行。

### 兼容层去哪了？

注释明确说"旧字段已被删除"。但官方提供了一个**临时兼容**：

```cpp
// imgui.h（伪代码示意）
// 1.87 引入的迁移函数：
IMGUI_API void  ImGui::SetKeyEventNativeData(ImGuiKey key, int native_keycode, int native_scancode);
```

它允许你**告诉 ImGui** "我的引擎里 native_keycode = 258 对应 ImGuiKey_Tab"。然后 ImGui 把 native code 也当成"非命名键"接受。

但这是**一个临时桥梁**，不是替代品。最终你还是要让 Backend 直接用 `ImGuiKey_*`。

### 痛点 4 的解法

```cpp
// 老
io.KeysDown[e.GetKeyCode()] = true;

// 现代
ImGuiKey imgui_key = HazelKeyToImGuiKey(e.GetKeyCode());  // 转换函数
io.AddKeyEvent(imgui_key, true);
```

**关键差异**：你必须写一个 `HazelKeyToImGuiKey` 转换函数——把 GLFW 键码（258）转成 ImGuiKey 枚举（ImGuiKey_Tab）。这是 1.3 章的核心代码。

---

## 1.2.6 痛点 5：手动派生 Mod 键状态

```cpp
io.KeyCtrl  = io.KeysDown[HZ_KEY_LEFT_CONTROL]  || io.KeysDown[HZ_KEY_RIGHT_CONTROL];
io.KeyShift = io.KeysDown[HZ_KEY_LEFT_SHIFT]    || io.KeysDown[HZ_KEY_RIGHT_SHIFT];
io.KeyAlt   = io.KeysDown[HZ_KEY_LEFT_ALT]      || io.KeysDown[HZ_KEY_RIGHT_ALT];
io.KeySuper = io.KeysDown[HZ_KEY_LEFT_SUPER]    || io.KeysDown[HZ_KEY_RIGHT_SUPER];
```

### 这一段在做什么

ImGui 的内部逻辑需要知道"现在 Ctrl 是否按下"——例如 Ctrl+S 的快捷键判定、Ctrl+点击 多选等。

老 API 的 `io.KeyCtrl / KeyShift / KeyAlt / KeySuper` 是 4 个 bool 字段。Backend 要在每帧更新它们——通常通过"OR 左右两个 mod 键"派生。

### 现代 API 自动派生

```cpp
// imgui.h:2570~2576（仍存在但不推荐写）
bool        KeyCtrl;    // Keyboard modifier down: Ctrl (non-macOS), Cmd (macOS)
bool        KeyShift;   // Keyboard modifier down: Shift
bool        KeyAlt;     // Keyboard modifier down: Alt
bool        KeySuper;   // Keyboard modifier down: Windows/Super

// Other state maintained from data above + IO function calls
ImGuiKeyChord KeyMods;  // Key mods flags (any of ImGuiMod_Ctrl/Shift/Alt/Super flags). Read-only, updated by NewFrame()
```

注意 `KeyMods` 注释 `"Read-only, updated by NewFrame()"`——它由 ImGui 自动从 `AddKeyEvent(ImGuiKey_LeftCtrl, ...)` 等事件**派生**出来。

`imgui.cpp` 中的 `UpdateKeyboardInputs` 会做这件事（约 9700+ 行）：

```cpp
// 简化伪代码
void ImGui::UpdateKeyboardInputs() {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    io.KeyMods = GetMergedModsFromKeys();   // 从 KeysData 推导
    io.KeyCtrl  = (io.KeyMods & ImGuiMod_Ctrl)  != 0;
    io.KeyShift = (io.KeyMods & ImGuiMod_Shift) != 0;
    io.KeyAlt   = (io.KeyMods & ImGuiMod_Alt)   != 0;
    io.KeySuper = (io.KeyMods & ImGuiMod_Super) != 0;
}
```

**Backend 只需要 `AddKeyEvent(ImGuiKey_LeftCtrl, true)`，所有派生 ImGui 自动做**。

### macOS 特殊处理

```cpp
// imgui_internal.h:1664（节选）
// On macOS, we swap Cmd(Super) and Ctrl keys at the time of the io.AddKeyEvent() call.
```

当 `io.ConfigMacOSXBehaviors == true`（macOS 默认开），`AddKeyEvent(ImGuiKey_LeftSuper, true)` 会被 ImGui 内部理解为 "Cmd 按下"——并设置 `io.KeyCtrl = true` 而不是 `KeySuper`（因为 Mac 的 Cmd 在交互意义上等于 PC 的 Ctrl）。

老 Hazel 代码**没有任何 macOS 适配**——在 macOS 上 Ctrl+C 等快捷键全错位。新 API 自动处理。

### 痛点 5 的解法

**完全删掉这 4 行**。`AddKeyEvent(ImGuiKey_LeftCtrl, true)` 会让 ImGui 自动派生 Mod 状态。

---

## 1.2.7 痛点 6：`AddInputCharacter` 与 `KeyTypedEvent` 的语义错配

```cpp
bool ImGuiLayer::OnKeyTypedEvent(KeyTypedEvent& e) {
    int keycode = e.GetKeyCode();
    if (keycode > 0 && keycode < 0x10000)
        io.AddInputCharacter((unsigned short)keycode);
    return false;
}
```

### `AddInputCharacter` 是干什么的

`AddInputCharacter(unsigned int c)` 是给 InputText 等文本输入控件**字符流**的 API。`c` 是一个 **Unicode codepoint**——例如：

- 用户按 'A' 键 → c = 65（ASCII / Unicode）
- 用户用拼音输入法输入"中" → c = 0x4E2D（U+4E2D，"中"的 Unicode）
- 用户按 Tab 键 → **不应**调 AddInputCharacter（Tab 不是字符）

### Hazel 老代码错在哪

Hazel 的 `KeyTypedEvent` 是 GLFW `glfwSetCharCallback` 的包装：

```cpp
// Hazel/Window.cpp（老代码）
glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int keycode) {
    KeyTypedEvent event(keycode);
    callback(event);
});
```

GLFW 的 `glfwSetCharCallback` 给的 `keycode` **就是 Unicode codepoint**——这一段没问题。

但是！老代码 `io.AddInputCharacter((unsigned short)keycode)` **强制 cast 成 `unsigned short`（16-bit）**——把 Unicode 码点超过 0xFFFF 的字符（emoji、扩展符号、某些罕见汉字）**截断**！

### Unicode 平面

Unicode 码点空间分为 17 个 "plane"：
- Plane 0 (BMP, Basic Multilingual Plane): 0x0000 ~ 0xFFFF——大多数常用字符。
- Plane 1 (SMP): 0x10000 ~ 0x1FFFF——emoji、稀有汉字。
- Plane 2 (SIP): 0x20000 ~ 0x2FFFF——更稀有的 CJK 汉字。
- ...

强 cast 成 `unsigned short` = **截断到 16 位 = 只支持 BMP**。emoji（U+1F600 → 0xF600 错误编码）输入失败。

### 现代 API 的修复

```cpp
// imgui.h:2526~2528
IMGUI_API void  AddInputCharacter(unsigned int c);              // 完整 32-bit Unicode
IMGUI_API void  AddInputCharacterUTF16(ImWchar16 c);             // UTF-16 surrogate pair 支持
IMGUI_API void  AddInputCharactersUTF8(const char* str);         // UTF-8 字符串
```

**正确做法**：

```cpp
io.AddInputCharacter(keycode);   // 不要 cast
```

或者如果 Backend 的字符回调给的是 UTF-16 surrogate：

```cpp
io.AddInputCharacterUTF16(c);   // ImGui 内部组合 surrogate pair
```

### 同时还要配合 `IMGUI_USE_WCHAR32`

如果你想支持完整 Unicode（包括 emoji 显示），需要在 `imconfig.h` 定义：

```cpp
#define IMGUI_USE_WCHAR32
```

这让 ImGui 内部的 `ImWchar` 是 32-bit 而非默认 16-bit——能存放完整 Unicode 码点。

---

## 1.2.8 痛点 7（隐藏）：完全没有 IME 支持

老 Hazel 代码**完全没有 IME 处理**——这意味着：

- 中文 / 日文 / 韩文 输入法**无法在 ImGui InputText 中正常工作**：
  - 拼音输入"shijie"，ImGui 会把 "s/h/i/j/i/e" 6 个字符**直接显示**为 InputText 内容。
  - 真正的"世界"汉字组字过程对 ImGui 不可见。
  - 用户按回车选词，IME 关闭，但 InputText 里已经堆了 6 个英文字母。

### 现代 API 的 IME 接缝

```cpp
// imgui.h 中（节选）
struct ImGuiPlatformImeData {
    bool    WantVisible;          // ImGui 想让 IME 显示
    ImVec2  InputPos;             // IME 候选窗口应该出现的位置
    float   InputLineHeight;
    ImGuiViewport* ViewportId;
};

// ImGuiPlatformIO 中：
void  (*Platform_SetImeDataFn)(ImGuiContext* ctx, ImGuiViewport* viewport, ImGuiPlatformImeData* data);
```

**Backend 必须实现 `Platform_SetImeDataFn`**——当 ImGui 想"开启 IME 候选窗口"时，回调用 Win32 `ImmSetCompositionWindow` / Cocoa `markedTextRange:` 等 OS API 把 IME 候选位置定到 ImGui 文本输入光标处。

### `Win32` Backend 的现代实现

```cpp
// backends/imgui_impl_win32.cpp（伪代码节选）
static void ImGui_ImplWin32_SetImeData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    HWND hwnd = (HWND)viewport->PlatformHandleRaw;
    if (HIMC himc = ImmGetContext(hwnd)) {
        if (data->WantVisible) {
            COMPOSITIONFORM cf = { CFS_FORCE_POSITION, { (LONG)data->InputPos.x, (LONG)data->InputPos.y }, {} };
            ImmSetCompositionWindow(himc, &cf);
        }
        ImmReleaseContext(hwnd, himc);
    }
}
```

### Hazel 的现状

Hazel 老教程的 ImGui 集成**完全没有这部分代码**——所以中文用户在编辑器里**输入不了中文**。

迁移到现代 API 时，Hazel 需要**新增**一个 IME 桥接层。如果用 GLFW 作为 platform backend，可以照搬 `backends/imgui_impl_glfw.cpp` 的 `ImGui_ImplGlfw_SetImeDataFn` 实现。

---

## 1.2.9 痛点 8（隐藏）：失焦时输入状态不被清理

老 Hazel 代码**没有处理 `WindowFocus` 事件**：

```cpp
// 老代码缺失这一段
void ImGuiLayer::OnWindowFocus(WindowFocusEvent& e) { ... }
void ImGuiLayer::OnWindowLostFocus(WindowLostFocusEvent& e) { ... }
```

### 失焦时的灾难场景

用户按住 Alt 切到另一个应用（Alt+Tab）：

1. Alt 按下事件 → `io.KeysDown[HZ_KEY_LEFT_ALT] = true`。
2. Tab 按下事件 → ImGui 收到，可能切到下一个控件。
3. 用户切到了别的应用。
4. **Alt 释放事件被新应用捕获，Hazel 没收到**。
5. 用户切回 Hazel 窗口。
6. **`io.KeysDown[HZ_KEY_LEFT_ALT]` 仍然是 true**——ImGui 认为 Alt 还按着！
7. 任何后续按键都被 ImGui 当作"Alt+XXX"组合键处理。
8. 用户按 'A' 想输入字母，结果触发了 Alt+A 菜单激活。

### 现代 API 的解决

```cpp
// imgui.h:2525
IMGUI_API void  AddFocusEvent(bool focused);
```

Backend 在窗口失焦时调 `io.AddFocusEvent(false)`。`imgui.cpp:10557~10561` 处理这个事件：

```cpp
if (g.IO.AppFocusLost) {
    g.IO.ClearInputKeys();
    g.IO.ClearInputMouse();
}
```

**自动清空所有键盘 / 鼠标状态**——避免上面的"Alt 卡死"问题。

### Hazel 的迁移

需要新增：

```cpp
// 现代 Hazel ImGuiLayer
bool ImGuiLayer::OnWindowFocusEvent(WindowFocusEvent& e) {
    ImGui::GetIO().AddFocusEvent(e.IsFocused());
    return false;
}
```

并在 Window 类的 GLFW Focus Callback 中触发这个事件。

---

## 1.2.10 痛点 9（隐藏）：`ConfigInputTrickleEventQueue` 与低帧率

`io.ConfigInputTrickleEventQueue` 默认为 `true`（`imgui.h:2432`）。它的作用是处理**低帧率下的输入丢失**问题。

### 问题场景

用户应用跑 30fps（每帧 33 ms）。用户在一帧的间隙内**快速按下并释放** Tab 键——只用了 10 ms。

**没有 trickle**：
1. 一帧内 OS 派发 KeyDown(Tab) + KeyUp(Tab) 两个事件。
2. Backend 直接写：`io.KeysDown[Tab] = true; ... io.KeysDown[Tab] = false;`
3. NewFrame 看到 `io.KeysDown[Tab] = false`——**它认为 Tab 这一帧从未被按过**！
4. `IsKeyPressed(ImGuiKey_Tab)` 返回 false——Tab 键事件**完全丢失**。

**有了 trickle（现代默认行为）**：
1. 一帧内 OS 派发 2 个事件 → 2 个 ImGuiInputEvent 入队。
2. NewFrame 时检查队列：发现"同一帧内同一键的 down + up"，**把 down 处理在这一帧、up 推迟到下一帧**。
3. 这一帧 `IsKeyPressed(ImGuiKey_Tab) = true`。
4. 下一帧 `IsKeyReleased(ImGuiKey_Tab) = true`。
5. 用户**至少能感知**这次按键。

### 老代码的代价

老 Hazel 代码**直接写数组**——绕过了事件队列——绕过了 trickle 机制。低帧率下事件经常丢失。

迁移到 `AddXxxEvent` 后自动获得这个保护。

---

## 1.2.11 痛点 10（隐藏）：手柄输入的"代际更替"

老 ImGui (1.87 之前) 的手柄输入用 `io.NavInputs[ImGuiNavInput_COUNT]`：

```cpp
// 1.87 之前
io.NavInputs[ImGuiNavInput_Activate] = (gamepad_a_pressed ? 1.0f : 0.0f);
io.NavInputs[ImGuiNavInput_Cancel]   = (gamepad_b_pressed ? 1.0f : 0.0f);
io.NavInputs[ImGuiNavInput_LStickLeft] = -lstick_x;   // 模拟摇杆
// ...
```

**1.88 起这一切被废除**（`imgui.h:2608` 注释段）：

```cpp
//float     NavInputs[ImGuiNavInput_COUNT];     // [LEGACY] Since 1.88, NavInputs[] was removed. Backends from 1.60 to 1.86 won't build. Feed gamepad inputs via io.AddKeyEvent() and ImGuiKey_GamepadXXX enums.
```

新 API 把手柄按钮作为 ImGuiKey 一等公民：

```cpp
// 现代
io.AddKeyEvent(ImGuiKey_GamepadFaceDown, gamepad_a_pressed);
io.AddKeyEvent(ImGuiKey_GamepadFaceRight, gamepad_b_pressed);
io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lstick_x < -0.1f, -lstick_x);
```

### Hazel 的现状

Cherno Hazel 教程**没有手柄支持**——所以这部分代码完全缺失。但如果你的引擎要支持手柄，需要从一开始就用现代 API。

---

## 1.2.12 痛点 11（隐藏）：字体 GlobalScale 的迁移

老代码：

```cpp
io.FontGlobalScale = 1.5f;     // 全局字体缩放 1.5 倍
```

`imgui.h:2611~2612` 明确说：

```cpp
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    float       FontGlobalScale;            // Moved io.FontGlobalScale to style.FontScaleMain in 1.92 (June 2025)
```

**1.92 起**，这个字段被**移到了 `ImGuiStyle::FontScaleMain`**。

老代码**仍能编译**（因为 obsolete 字段还在），但写它**完全无效**——实际生效的字段是 `style.FontScaleMain`。

### 解法

```cpp
// 老
ImGui::GetIO().FontGlobalScale = 1.5f;

// 现代
ImGui::GetStyle().FontScaleMain = 1.5f;
```

### `style.FontScaleDpi`

1.92 还引入了 `style.FontScaleDpi`——自动按 DPI 缩放。如果你开了 `ImGuiConfigFlags_DpiEnableScaleFonts`，ImGui 会自动设置这个字段。**编辑器开发应该开它**（HiDPI 屏才不糊）。

---

## 1.2.13 痛点 12（隐藏）：剪贴板回调的位移

老 API：

```cpp
io.SetClipboardTextFn = MyEngineSetClipboardText;
io.GetClipboardTextFn = MyEngineGetClipboardText;
io.ClipboardUserData  = my_engine_ptr;
```

`imgui.h:2614~2618` 明确说：

```cpp
// Legacy: before 1.91.1, clipboard functions were stored in ImGuiIO instead of ImGuiPlatformIO.
const char* (*GetClipboardTextFn)(void* user_data);
void        (*SetClipboardTextFn)(void* user_data, const char* text);
void*       ClipboardUserData;
```

**1.91.1 起**，剪贴板函数移到 `ImGuiPlatformIO`。老字段保留作兼容，但**官方建议**用新位置：

```cpp
// 现代
ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
platform_io.Platform_SetClipboardTextFn = MyEngineSetClipboardText;
platform_io.Platform_GetClipboardTextFn = MyEngineGetClipboardText;
platform_io.Platform_ClipboardUserData  = my_engine_ptr;
```

注意函数签名也改了（多了 `ImGuiContext*` 参数）：

```cpp
// 老
const char* GetClipboardTextFn(void* user_data);

// 现代
const char* Platform_GetClipboardTextFn(ImGuiContext* ctx);
```

### 接 OS 默认实现

`imgui_impl_glfw.cpp` / `imgui_impl_win32.cpp` 等官方 backend 默认会接 OS 剪贴板。如果你的引擎用了官方 backend 就**不需要自己实现**。但如果是自研 platform backend，必须挂这两个回调——否则 InputText 的 Ctrl+C/V 失效。

---

## 1.2.14 痛点 13（隐藏）：触屏 / 触控笔输入

老 ImGui 没有"触屏 / 触控笔"概念——所有点击都被当成"鼠标"。

新 API 引入 `ImGuiMouseSource`：

```cpp
enum ImGuiMouseSource : int {
    ImGuiMouseSource_Mouse = 0,
    ImGuiMouseSource_TouchScreen,
    ImGuiMouseSource_Pen,
    ImGuiMouseSource_COUNT
};

// imgui.h:2524
IMGUI_API void  AddMouseSourceEvent(ImGuiMouseSource source);
```

Backend 在产生鼠标事件前调 `AddMouseSourceEvent` 标注来源——后续的 `AddMouseButtonEvent` 等会带上这个来源信息。

### 为什么需要

某些 ImGui 控件（拖动、悬停延时显示 tooltip 等）在触屏下行为应不同：

- 触屏没有"悬停"概念（手指要么按下要么松开），所以 hover-based tooltip 应禁用。
- 触屏的"长按"应被理解为右键点击。
- 触屏的拖动距离阈值应不同（手指比鼠标抖）。

### Hazel 的现状

Hazel 的 Window 抽象现在主要面向桌面（GLFW）。如果将来要做手机/平板版本，必须从 OS 拿到"输入源"信息并调 `AddMouseSourceEvent`。**老代码完全没这能力**。

---

## 1.2.15 完整痛点清单

把上面所有讨论汇总成一张表：

| # | 痛点 | 现代结局 | 严重程度 |
|---|---|---|---|
| 1 | `io.KeyMap[]` 直接写 | 字段已删除，**编译失败** | 🔴 阻塞 |
| 2 | `io.MouseDown[]` 直接写 | 软废弃，能编译但绕过队列 | 🟡 隐患 |
| 3 | `io.MousePos = ...` 直接写 | 多视口下错位 | 🟠 严重 |
| 4 | `io.KeysDown[]` 直接写 | 字段已删除，**编译失败** | 🔴 阻塞 |
| 5 | 手动派生 `KeyCtrl/Shift/Alt/Super` | 新 API 自动派生，老代码与 mac 适配冲突 | 🟠 严重 |
| 6 | `AddInputCharacter` 强 cast 截断 | 截断 emoji / SMP 字符 | 🟡 隐患 |
| 7 | 完全无 IME 支持 | 中日韩用户输入法失效 | 🔴 阻塞（CJK 用户） |
| 8 | 失焦不清状态 | Alt-Tab 后键盘卡死 | 🟠 严重 |
| 9 | 直接写数组 vs 事件队列 | 低帧率下事件丢失 | 🟡 隐患 |
| 10 | 无手柄支持 / 老 NavInputs API | 编译失败（如有手柄代码） | 🔴 阻塞（手柄） |
| 11 | `io.FontGlobalScale` | 1.92 失效（要用 `style.FontScaleMain`） | 🟡 隐患 |
| 12 | 老剪贴板 API 位置 | 软废弃，能用但建议迁移 | 🟢 提醒 |
| 13 | 无触屏 / 触控笔区分 | 触屏控件行为错乱 | 🟢 提醒 |

**两条阻塞级痛点（红色）**：`KeyMap` / `KeysDown` 删除——任何老代码**首次编译就过不去**。

**三条严重痛点（橙色）**：MousePos 多视口 / Mod 键派生 / 失焦清理——**编译过、跑起来后会出明显 bug**。

**四条隐患（黄色）**：MouseDown 直接写 / 字符截断 / 事件队列 / 字体缩放——**短期能跑，长期是技术债**。

---

## 1.2.16 还有哪些"并非痛点"的兼容点

并不是所有 Hazel 老代码都需要改。下面这些**保持原样可以**：

### 仍然能用的字段

```cpp
io.MousePos          // 仍存在；NewFrame 后只读
io.MouseDown[5]      // 仍存在；NewFrame 后只读
io.MouseWheel        // 仍存在；NewFrame 后只读
io.KeyCtrl/Shift/Alt/Super   // 仍存在；NewFrame 后只读（自动派生）
io.WantCaptureMouse / WantCaptureKeyboard   // 仍是输出反馈字段
io.Framerate
io.MetricsRenderVertices / Indices / Windows
```

读这些**没问题**——只是**写**它们已不推荐。

### 仍然能用的 API

```cpp
io.AddInputCharacter       // 一直存在
io.AddInputCharactersUTF8  // 1.86 引入
io.WantSaveIniSettings     // 一直存在
io.UserData                // 自由字段
ImGui::CreateContext / DestroyContext
ImGui::GetIO / GetStyle
ImGui::Begin / End / NewFrame / Render
ImGui::Button / Slider / etc.   // 公开控件 API 100% 兼容
```

ImGui 的**控件 API 极少变化**——所以你在 `OnImGuiRender()` 里写的 `ImGui::Button("Save")` 永远不需要改。**变化集中在 IO / Backend 接缝层**。

---

## 1.2.17 迁移工作量评估

如果你的 Hazel 仓库 fork 是 2020 年的 ImGui 1.84，要迁移到 1.92.8，**核心工作集中在两个文件**：

| 文件 | 修改量 | 主要工作 |
|---|---|---|
| `Hazel/src/Hazel/ImGui/ImGuiLayer.cpp` | ~60% 重写 | OnAttach 删 KeyMap、OnEvent 改 AddXxxEvent、新增 IME / Focus 处理 |
| `Hazel/src/Hazel/ImGui/ImGuiBuild.cpp` | 删除或大改 | 老 ImGuiBuild 是手工填字段；新版应直接用官方 imgui_impl_glfw.cpp |
| 其他文件 | 几乎不改 | 业务代码用 ImGui 公开 API，跨版本稳定 |

**推荐策略**：

- **不要尝试"保留老代码风格"** —— 与其把老 OnEvent 改成"调 AddKeyEvent 的现代版"，不如直接**删掉自家的 backend 代码**，使用官方 `imgui_impl_glfw.cpp`。
- **如果坚持自家 backend**（为了与 Hazel 统一抽象），下一章 (1.3) 给你完整迁移代码。

---

## 1.2.18 本章小结

- Hazel 老 ImGui 集成代码**至少 13 处问题**——其中 2 处编译就失败、3 处运行严重 bug、4 处隐性技术债。
- 编译失败的两处（KeyMap / KeysDown）**没有任何 workaround**——必须改。
- 运行 bug 的三处（MousePos 多视口 / Mod 键派生 / 失焦清理）虽能编译但用户体验严重受损。
- 隐性技术债（事件队列 / 字符截断 / 字体缩放 / 剪贴板位置）短期不致命，但累计起来代码"老化感"明显。
- ImGui 团队**官方推荐路径**：迁移到 `io.AddXxxEvent` 现代 API + 官方 `imgui_impl_xxx.cpp` backend。

---

## 1.2.19 下一章预告

第 1.3 章 [[07_第一部分_03_现代InputRouting与迁移]] 会**深入源码**讲清现代 Input 系统的工作原理：

- `ImGuiInputEvent` 联合体的 5 种事件类型如何统一存放（`imgui_internal.h:1545`）。
- `g.InputEventsQueue` → `g.InputEventsTrail` 的"双队列"模型源码分析。
- `io.ConfigInputTrickleEventQueue == true` 时事件如何被分摊到多帧（**这是低帧率不丢按键的关键**）。
- `io.AddKeyEvent` 等 API 的逐行实现。
- **Owner-Aware Input** 与 `ImGuiInputFlags_RouteXXX` 系统：理解为何一个快捷键不再"广播"而是"路由"。
- **完整的 Hazel 迁移代码**：一份可直接编译的 `ImGuiLayer.cpp` v2，把上述 13 个痛点全部修复。
