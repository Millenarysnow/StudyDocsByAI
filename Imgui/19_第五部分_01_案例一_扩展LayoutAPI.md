# 第 5.1 章 · 案例一：扩展 Layout API（自定义"网格"与"Flex Row"）

> **本章目标**：基于第 4 部分的"三段式"模板和 internal API，**实现两个真实有用的 Layout 扩展**——`BeginFlexRow / EndFlexRow`（CSS Flexbox 风格的水平按权重分配）+ `BeginGrid / EndGrid`（自动断行的网格）。读完后你应该能：(1) 在白板上画出 ImGui 现有 Layout 系统的能力边界；(2) 自己写任何"现有 API 不直接支持"的布局（环形菜单、瀑布流、星标网格…）。
>
> **本章对应源码**：`imgui.cpp:[SECTION] LAYOUT]`（约 11321 行起）—— `ItemSize / SameLine / Indent / BeginGroup / EndGroup` 全文剖析；`imgui_internal.h:[SECTION] ImGuiWindowTempData]` —— `DC.CursorPos / CurrLineSize / Indent / GroupOffset` 等字段；`imgui_internal.h:1377`（`ImGuiLastItemData`）。
>
> **前置阅读**：第 4.2 章（`ImGuiWindow::DC`）、第 4.3 章（三段式模板）。

---

## 5.1.0 ImGui 现有 Layout 系统的能力边界

ImGui 的"内置 Layout"由几个原语组合：

| API | 作用 |
|---|---|
| `ItemSize(size)` | 报告 item 尺寸，推进 cursor |
| `SameLine(offset, spacing)` | 退回上一行末尾，水平继续 |
| `Indent(w) / Unindent(w)` | 左缩进 |
| `Spacing()` / `Dummy(size)` | 留空 |
| `Separator()` | 横线 |
| `BeginGroup() / EndGroup()` | 把多个 item 视作一个整体 |
| `BeginChild() / EndChild()` | 子窗口（独立 scroll region） |
| `Columns(N)` / `BeginTable()` | 多列布局 |

**能做什么**：

- 垂直堆叠（默认）。
- 水平堆叠（用 SameLine）。
- 简单缩进 / 折叠树结构（TreeNode）。
- 等宽列（Columns / Tables）。

**不能直接做**：

- **按权重分配宽度**（CSS Flexbox 的 `flex: 1` / `flex: 2` 风格）。
- **自适应换行**（item 超出宽度自动到下一行）。
- **居中 / 右对齐**（垂直方向无对齐 API）。
- **网格按行优先 vs 列优先**。

这些"高级布局"需要你自己写——本章给两个最常用的实现。

---

## 5.1.1 Layout 心智模型回顾

打开 `imgui_internal.h:[SECTION] ImGuiWindowTempData]`，关注 Layout 相关字段：

```cpp
struct ImGuiWindowTempData {
    // ===== Layout 核心字段 =====
    ImVec2  CursorPos;              // 下个 item 起点（屏幕坐标）
    ImVec2  CursorPosPrevLine;      // 上一行 cursor 末位置
    ImVec2  CursorStartPos;          // 窗口内容起点（Begin 后初始）
    ImVec2  CursorMaxPos;            // cursor 到过的最右下点（用于自动调整窗口大小）
    ImVec2  IdealMaxPos;
    ImVec2  CurrLineSize;            // 当前行的 widget 累计大小
    ImVec2  PrevLineSize;
    float   CurrLineTextBaseOffset;  // 当前行的文本基线偏移
    float   PrevLineTextBaseOffset;
    bool    IsSameLine;
    bool    IsSetPos;
    ImVec1  Indent;                  // 当前行首缩进（从 window 左边）
    ImVec1  ColumnsOffset;           // Columns 引入的额外偏移
    ImVec1  GroupOffset;             // BeginGroup 引入的偏移
    
    // ===== Layout 类型 =====
    ImGuiLayoutType LayoutType;     // Horizontal / Vertical
    ImGuiLayoutType ParentLayoutType;
};
```

### 一行 widget 的完整生命周期

```cpp
// 假设当前 cursor 在 (10, 50)，行高 0
ImGui::Button("A");   // size = (40, 22)
   ↓ ItemAdd 把 LastItemData.Rect 设为 [(10,50), (50,72)]
   ↓ ItemSize(size=(40,22)) 推进 cursor：
      DC.CursorPosPrevLine = (50, 50)
      DC.CursorPos.x = window->Pos.x + DC.Indent.x + DC.ColumnsOffset.x = (10)
      DC.CursorPos.y = 50 + line_height (=22) + ItemSpacing.y (=4) = (76)
   ↓ DC.PrevLineSize.y = 22
   ↓ DC.CurrLineSize = (0, 0)

ImGui::Button("B");   // size = (40, 22)
   ↓ 在 (10, 76) 画
   ↓ cursor 推进到 (10, 102)
```

**默认垂直堆叠**——每个 ItemSize 后 cursor.y 推进，cursor.x 重置到行首。

### `SameLine` 的"魔法"

```cpp
ImGui::Button("A");
ImGui::SameLine();
ImGui::Button("B");
```

`SameLine()` 内部（`imgui.cpp:11396`）：

```cpp
window->DC.CursorPos.x = window->DC.CursorPosPrevLine.x + spacing_w;   // 退回上行末尾
window->DC.CursorPos.y = window->DC.CursorPosPrevLine.y;                // 退回上行 y
window->DC.CurrLineSize = window->DC.PrevLineSize;                       // 恢复行高
window->DC.IsSameLine = true;
```

——把 `CursorPos` 强制设回"上一个 ItemSize 之前"，让下一个 widget 紧跟在上一个之后。

**关键洞察**：`CursorPosPrevLine` 是 `CursorPos` 的"备份"——专门给 `SameLine` 用。这就是 ImGui 实现"水平布局"的核心。

### `BeginGroup / EndGroup` 的"打包"

```cpp
ImGui::BeginGroup();
ImGui::Button("A");
ImGui::Button("B");
ImGui::EndGroup();
// EndGroup 后，整个 group 被当作一个 item
if (ImGui::IsItemHovered()) { ... }
```

`BeginGroup`（`imgui.cpp:11652`）做的事：

```cpp
g.GroupStack.resize(g.GroupStack.Size + 1);
ImGuiGroupData& g = g.GroupStack.back();
g.BackupCursorPos = window->DC.CursorPos;       // 备份所有相关字段
g.BackupCursorMaxPos = window->DC.CursorMaxPos;
// ...
window->DC.GroupOffset.x = window->DC.CursorPos.x - window->Pos.x - window->DC.ColumnsOffset.x;
window->DC.Indent = window->DC.GroupOffset;     // group 内的 cursor 缩进到 group 起点
window->DC.CursorMaxPos = window->DC.CursorPos; // 重置 max（仅跟踪 group 内）
```

`EndGroup`（`imgui.cpp:11682`）：

```cpp
ImRect group_bb(group_data.BackupCursorPos, ImMax(window->DC.CursorMaxPos, ...));
window->DC.CursorPos = group_data.BackupCursorPos;        // 恢复
window->DC.CursorMaxPos = ImMax(group_data.BackupCursorMaxPos, group_bb.Max);
window->DC.Indent = group_data.BackupIndent;
// ...
ItemSize(group_bb.GetSize());                              // 把 group 当作一个 item
ItemAdd(group_bb, 0, NULL, ImGuiItemFlags_NoTabStop);
// 转发 ActiveId / Edited / Hovered 状态
```

**"先备份 → 让 group 内 widget 自由布局 → 算出整体 bbox → 恢复 → 把 bbox 当 ItemSize 提交"**——这正是我们扩展 Layout 时要复用的模式。

---

## 5.1.2 案例 1：`BeginFlexRow / EndFlexRow`

### 想达到的效果

```cpp
// CSS Flex 风格：3 个 item 按 1:2:1 分配可用宽度
ImGui::BeginFlexRow();
    ImGui::FlexItem(1.0f);  // 占 25%
    ImGui::Button("A");
    
    ImGui::FlexItem(2.0f);  // 占 50%
    ImGui::Button("B");
    
    ImGui::FlexItem(1.0f);  // 占 25%
    ImGui::Button("C");
ImGui::EndFlexRow();

// 结果：
// |  [   A   ]  [          B          ]  [   C   ]  |
//      25%               50%                  25%
```

### 设计思路

ImGui 的 widget 调用是**立即模式**——`Button("A")` 调用时**还不知道**后面有几个 item、各自的权重是多少。怎么办？

**方案**：分两次"提交"——

1. **第一次（BeginFlexRow ~ EndFlexRow 之间）**：用户调用 `FlexItem(weight) + 真实 widget`。我们**不**真的画，只**测量** widget 的 _最小_ 尺寸 + 累计权重。
2. **第二次（EndFlexRow 时）**：基于已知总宽度 + 各 item 权重，算出每个 item 应分配的宽度。然后**用 SetCursorPos + 重新调用** widget 的真实绘制？

但这需要"把 widget 调用包起来再延迟执行"——在 immediate mode 中很难（每个 widget 立即写顶点）。

**另一个方案**（更实际）：让用户**自己**用 `SetNextItemWidth` 配合，我们只负责**算每个槽位的 x 位置 + 宽度**，用户自己用 `Button(label, ImVec2(w, 0))` 调用。

```cpp
struct FlexRowState {
    ImVec2  StartPos;
    float   AvailWidth;
    float   Spacing;
    int     ItemCount;
    float   TotalWeight;
    float   CurrX;
};

void BeginFlexRow(float spacing = 0.0f);
float FlexNextSlot(float weight);   // 返回该 slot 应有的宽度
void  EndFlexRow();
```

用法：

```cpp
ImGui::BeginFlexRow();
    float w = ImGui::FlexNextSlot(1.0f);
    ImGui::Button("A", ImVec2(w, 0));
    
    w = ImGui::FlexNextSlot(2.0f);
    ImGui::Button("B", ImVec2(w, 0));
    
    w = ImGui::FlexNextSlot(1.0f);
    ImGui::Button("C", ImVec2(w, 0));
ImGui::EndFlexRow();
```

但是！我们仍然面临"调用 FlexNextSlot 时不知道总权重"的问题——因为后面的 `FlexNextSlot` 调用还没发生。

**解决**：**两遍渲染**——但**只有在第一帧/权重变化帧**重画。把状态缓存到 `ImGuiStorage`：

1. 第一帧：让所有 item 用 `weight=0`（占位），同时累计权重。EndFlexRow 时记录 TotalWeight。
2. 第二帧：用上一帧的 TotalWeight 给每个 slot 分配宽度。

但 1 帧延迟通常用户感知不到——只在 weight 变化时才有 1 帧"塌缩"。

**更优雅的方案**：用 `ImGuiStorage` 跨帧记录权重，**在 EndFlexRow 时一次性计算**——但那时 widget 已经画完了。所以我们需要让 widget **延迟绘制**。

### 真正可行的方案：基于"两遍 Begin"

```
帧 N:
  第一遍（用户代码）：
    BeginFlexRow();
    FlexNextSlot(1.0f); Button("A");        // 假设 weight 总和已知（从上帧 cache）
    FlexNextSlot(2.0f); Button("B");
    FlexNextSlot(1.0f); Button("C");
    EndFlexRow();
  
  EndFlexRow 内部：
    把本帧的 (weight 序列) 存入 ImGuiStorage
    下一帧 BeginFlexRow 时读取
```

只有**第一帧**显示错乱——可以让第一帧 `BeginFlexRow` 直接 IM_ASSERT 不绘制（"warm up"），或者第一帧用平均分配。

下面给一个**基于"上帧权重"**的简洁实现：

### 完整代码

```cpp
// HazelImGuiExt.h

namespace ImGui {

struct FlexRowState {
    ImVec2  StartPos;       // BeginFlexRow 时 cursor 位置
    float   AvailWidth;     // 总可用宽度
    float   Spacing;        // 槽位间隙
    float   TotalWeightLastFrame;
    float   CurrSumThisFrame;
    int     SlotCountThisFrame;
};

// 一个 FlexRow 的全部状态都存进 window storage
inline FlexRowState* GetFlexRowState() {
    static FlexRowState* state = nullptr;
    return state;   // 简化：用 static 单例。生产环境用 storage map
}

void BeginFlexRow(float spacing = 4.0f) {
    ImGuiContext& g = *GetCurrentContext();
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return;
    
    // 状态从 window storage 取（按 ID 区分多个 FlexRow）
    ImGuiID id = window->GetID("##FlexRow");
    FlexRowState* state = (FlexRowState*)window->StateStorage.GetVoidPtr(id);
    if (!state) {
        state = IM_NEW(FlexRowState)();
        memset(state, 0, sizeof(*state));
        window->StateStorage.SetVoidPtr(id, state);
    }
    
    state->StartPos    = window->DC.CursorPos;
    state->AvailWidth  = ImGui::GetContentRegionAvail().x;
    state->Spacing     = spacing;
    state->CurrSumThisFrame  = 0.0f;
    state->SlotCountThisFrame = 0;
    
    // 把 state 存到 g 里供 FlexNextSlot 用
    PushOverrideID(id);
}

float FlexNextSlot(float weight) {
    ImGuiContext& g = *GetCurrentContext();
    ImGuiWindow* window = GetCurrentWindow();
    
    ImGuiID id = window->IDStack.back();
    FlexRowState* state = (FlexRowState*)window->StateStorage.GetVoidPtr(id);
    IM_ASSERT(state && "FlexNextSlot called outside BeginFlexRow");
    
    state->SlotCountThisFrame++;
    
    if (state->SlotCountThisFrame > 1) {
        // 不是第一个 slot —— SameLine
        ImGui::SameLine(0.0f, state->Spacing);
    }
    
    // 如果上一帧记录了 TotalWeight，用它分配宽度
    if (state->TotalWeightLastFrame > 0.0f) {
        // 从可用宽度减去 spacing × (slot_count-1)
        // 但我们不知道当前帧的总 slot 数...
        // 用上一帧的总 slot 数（从 SlotCountLastFrame，简化省略）
        float w = (state->AvailWidth - state->Spacing * (state->SlotCountThisFrame - 1)) 
                  * (weight / state->TotalWeightLastFrame);
        state->CurrSumThisFrame += weight;
        return w;
    }
    
    // 第一帧 —— 用平均分配作占位
    state->CurrSumThisFrame += weight;
    return state->AvailWidth * 0.25f;   // 占位值，下帧会矫正
}

void EndFlexRow() {
    ImGuiContext& g = *GetCurrentContext();
    ImGuiWindow* window = GetCurrentWindow();
    
    ImGuiID id = window->IDStack.back();
    FlexRowState* state = (FlexRowState*)window->StateStorage.GetVoidPtr(id);
    if (state) {
        state->TotalWeightLastFrame = state->CurrSumThisFrame;
    }
    
    PopID();
    
    // 强制换行（如果 widget 调用没自动换行）
    NewLine();
}

}  // namespace ImGui
```

**核心机制**：

- 每个 FlexRow 用 ID（`Hash("##FlexRow", window_id)`）作 key，把状态缓存到 `window->StateStorage`。
- 第一帧权重总和未知 → 用占位值。
- 第二帧起读取上帧记录的总权重，按比例分配宽度。
- 用 `SameLine` 让所有 slot 处于同一行。

**用户调用**：

```cpp
ImGui::BeginFlexRow();
    float w1 = ImGui::FlexNextSlot(1.0f); ImGui::Button("A", ImVec2(w1, 0));
    float w2 = ImGui::FlexNextSlot(2.0f); ImGui::Button("B", ImVec2(w2, 0));
    float w3 = ImGui::FlexNextSlot(1.0f); ImGui::Button("C", ImVec2(w3, 0));
ImGui::EndFlexRow();
```

### 优化：每个 FlexRow 独立 ID

实际生产代码应该让 `BeginFlexRow` 接受 `const char* str_id` 参数 + 用 `PushOverrideID` 隔离多个 FlexRow：

```cpp
void BeginFlexRow(const char* str_id, float spacing = 4.0f) {
    ImGuiID id = GetID(str_id);
    PushOverrideID(id);
    // ...
}
```

否则同一窗口内多个 FlexRow 会共享同一个 state，互相覆盖。

### 更进一步：嵌套 FlexRow 支持

用 `g.GroupStack` 风格的栈结构而不是 storage，能支持任意嵌套。但 95% 用例只需要单层——上面代码够用。

---

## 5.1.3 案例 2：`BeginGrid / EndGrid` 自动断行网格

### 想达到的效果

```cpp
ImGui::BeginGrid("Items", 4, ImVec2(80, 80));   // 4 列，每个 cell 80×80
    for (Item* item : items) {
        ImGui::PushID(item->id);
        DrawItemCard(item);     // 内部画 cell（图标 + 名字）
        ImGui::PopID();
    }
ImGui::EndGrid();

// 结果（如果有 10 个 item）：
//  [I0] [I1] [I2] [I3]
//  [I4] [I5] [I6] [I7]
//  [I8] [I9]
```

——超过 4 列自动换行。

### 设计思路

每提交一个 cell，检查"是否够画在当前行"：

- 够 → 用 SameLine 留在当前行。
- 不够 → 自然换行（不调 SameLine，cursor 自动到下一行）。

```cpp
struct GridState {
    int     ColumnCount;
    int     CurrentColumn;
    ImVec2  CellSize;
    float   ColumnSpacing;
    float   RowSpacing;
};
```

### 完整代码

```cpp
namespace ImGui {

void BeginGrid(const char* str_id, int column_count, ImVec2 cell_size, 
               float col_spacing = 4.0f, float row_spacing = 4.0f) {
    ImGuiContext& g = *GetCurrentContext();
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return;
    
    // 用 ID 区分多个 Grid
    ImGuiID id = window->GetID(str_id);
    PushOverrideID(id);
    
    // 状态栈（用 static 数组简化；生产环境用 ImVector<GridState> 栈）
    static thread_local ImVector<GridState> grid_stack;
    grid_stack.push_back({});
    GridState& state = grid_stack.back();
    state.ColumnCount    = column_count;
    state.CurrentColumn  = 0;
    state.CellSize       = cell_size;
    state.ColumnSpacing  = col_spacing;
    state.RowSpacing     = row_spacing;
    
    // 我们需要在每个 cell 提交后插入"是否换行"的判断
    // 简单做法：用户代码里每 cell 后调 ImGui::NextGridCell()
    // 这里只是初始化
}

// 用户在每个 cell 之后调用（即在 ImGui::Button() 等之后）
void NextGridCell() {
    ImGuiContext& g = *GetCurrentContext();
    if (g.CurrentWindow->SkipItems) return;
    
    static thread_local ImVector<GridState>* p_grid_stack;   // 简化
    GridState& state = (*p_grid_stack).back();
    
    state.CurrentColumn++;
    if (state.CurrentColumn < state.ColumnCount) {
        // 同行，下一格
        ImGui::SameLine(0.0f, state.ColumnSpacing);
    } else {
        // 换行
        state.CurrentColumn = 0;
        // 默认 ImGui 推进 cursor 已经把 y 推到下一行
        // 如果我们想要自定义 row_spacing，可以 SetCursorPosY:
        // float y = ImGui::GetCursorPosY();
        // ImGui::SetCursorPosY(y - g.Style.ItemSpacing.y + state.RowSpacing);
    }
}

void EndGrid() {
    ImGuiContext& g = *GetCurrentContext();
    if (g.CurrentWindow->SkipItems) {
        PopID();
        return;
    }
    
    // 弹出栈
    static thread_local ImVector<GridState> grid_stack;
    grid_stack.pop_back();
    
    PopID();
}

}  // namespace ImGui
```

### 用户调用

```cpp
ImGui::BeginGrid("ItemGrid", 4, ImVec2(80, 80));
for (Item* item : items) {
    ImGui::PushID(item->id);
    DrawItemCard(item);
    ImGui::PopID();
    ImGui::NextGridCell();
}
ImGui::EndGrid();
```

或者用 RAII 风格 wrapper 让 `NextGridCell` 自动调用：

```cpp
class GridCellScope {
public:
    GridCellScope() {}
    ~GridCellScope() { ImGui::NextGridCell(); }
};

#define IMGUI_GRID_CELL() GridCellScope HZ_CONCAT(_grid_cell_, __LINE__)

// 用户代码：
ImGui::BeginGrid("ItemGrid", 4, ImVec2(80, 80));
for (Item* item : items) {
    IMGUI_GRID_CELL();
    ImGui::PushID(item->id);
    DrawItemCard(item);
    ImGui::PopID();
}
ImGui::EndGrid();
```

——离开作用域时自动 `NextGridCell`。

### 更"自动"的版本：基于 ItemSize hook

更高级的实现：用 `g.Hooks` 注册一个钩子，在每个 ItemAdd 后自动判断是否换行。但这破坏了"立即模式"的清晰性——上面"显式 NextGridCell"反而更可读。

### 自适应 cell 大小

```cpp
void BeginGridAuto(const char* str_id, int column_count, float cell_height) {
    float avail = ImGui::GetContentRegionAvail().x;
    float spacing = 4.0f;
    float cell_width = (avail - spacing * (column_count - 1)) / column_count;
    BeginGrid(str_id, column_count, ImVec2(cell_width, cell_height), spacing, spacing);
}
```

——根据可用宽度自动算 cell 宽度。这是 Visual Asset Browser 等编辑器场景的常用模式。

---

## 5.1.4 进阶：基于 `BeginGroup` 的"延迟提交"模式

某些 layout 需要"先测量再画"——例如：

- 居中对齐：先量出整个 group 宽度，再 SetCursorPos 把整体居中。
- 右对齐：把 widget 推到 ContentRegion 右侧。
- 等高分隔：知道所有 cell 的最大高度后统一对齐。

### 居中对齐示例

```cpp
namespace ImGui {

// 把若干 widget 整体居中
void BeginCenter() {
    BeginGroup();
    // 后续 widget 先正常画
}

void EndCenter() {
    EndGroup();
    // EndGroup 后 LastItemData.Rect 是整个 group 的 bbox
    ImVec2 group_size = GetItemRectSize();
    
    // 关键：但 widget 已经画了，怎么居中？
    // 答：我们其实不能"事后居中"——必须在 BeginCenter 时 SetCursorPos 到正确位置
}

}
```

——这暴露了"延迟提交"的根本问题：widget 已经写顶点了。

**正确解法**：基于上一帧的尺寸。

```cpp
struct CenterState {
    float LastFrameWidth;
};

void BeginCenter(const char* str_id) {
    ImGuiID id = GetID(str_id);
    PushOverrideID(id);
    
    CenterState* s = (CenterState*)GetCurrentWindow()->StateStorage.GetVoidPtr(id);
    if (!s) {
        s = IM_NEW(CenterState)();
        s->LastFrameWidth = 0.0f;
        GetCurrentWindow()->StateStorage.SetVoidPtr(id, s);
    }
    
    // 用上帧宽度居中
    if (s->LastFrameWidth > 0.0f) {
        float avail = GetContentRegionAvail().x;
        float offset = (avail - s->LastFrameWidth) * 0.5f;
        if (offset > 0.0f) {
            float x = GetCursorPosX();
            SetCursorPosX(x + offset);
        }
    }
    
    BeginGroup();
}

void EndCenter() {
    EndGroup();
    
    ImGuiID id = GetCurrentWindow()->IDStack.back();
    CenterState* s = (CenterState*)GetCurrentWindow()->StateStorage.GetVoidPtr(id);
    s->LastFrameWidth = GetItemRectSize().x;
    
    PopID();
}
```

第一帧不居中，第二帧起居中——通常用户感知不到。

---

## 5.1.5 边角 case 处理：剪裁 / Scroll / 子窗口

实际写 Layout 扩展时常被"边角 case"反咬：

### 1. `window->SkipItems == true` 时的早退

每个 layout API 开头都要：

```cpp
if (window->SkipItems) return;
```

——窗口被折叠 / 完全裁剪时跳过整个布局逻辑。

### 2. `Scroll` 影响 cursor

`window->DC.CursorPos` 是**屏幕坐标**——已经包含了 `-window->Scroll`。所以你**不需要**自己处理滚动偏移。

### 3. `Indent` / `ColumnsOffset` / `GroupOffset`

`SameLine(0)` 内部会重置 `cursor.x = window->Pos.x + DC.Indent.x + DC.ColumnsOffset.x`。**如果你自定义换行**，记得把这些 offset 加回去：

```cpp
// 自定义换行
ImGuiWindow* w = GetCurrentWindow();
w->DC.CursorPos.x = w->Pos.x + w->DC.Indent.x + w->DC.ColumnsOffset.x;
w->DC.CursorPos.y += /* row height */;
```

### 4. `BeginChild` 内部不影响外部

如果你的 layout 用 `BeginChild` 创建子窗口——子窗口有自己的 cursor / Indent / Scroll。`BeginGrid` 内部嵌套 `BeginChild` 不会破坏外部布局。

---

## 5.1.6 调试 Layout 的实用技巧

### 1. `Style.DebugShowGroupRects`

```cpp
ImGui::GetCurrentContext()->DebugShowGroupRects = true;
```

每个 BeginGroup/EndGroup 自动画**紫色矩形**（`imgui.cpp:11744`）：

```cpp
if (g.DebugShowGroupRects)
    window->DrawList->AddRect(group_bb.Min, group_bb.Max, IM_COL32(255,0,255,255));
```

——立刻看出 group 的实际边界。

### 2. Alt 键悬停显示几何

`imgui.cpp:11370` 的注释代码：

```cpp
//if (g.IO.KeyAlt) window->DrawList->AddRect(window->DC.CursorPos, window->DC.CursorPos + ImVec2(size.x, line_height), IM_COL32(255,0,0,200));
```

——取消注释后，按住 Alt 键，每个 widget 显示红色 bbox。强大的"看哪里出问题"工具。

### 3. 可视化 cursor 路径

```cpp
ImDrawList* fg = ImGui::GetForegroundDrawList();
fg->AddCircleFilled(window->DC.CursorPos, 3.0f, IM_COL32(0, 255, 0, 255));   // 当前 cursor
fg->AddCircleFilled(window->DC.CursorPosPrevLine, 3.0f, IM_COL32(255, 0, 0, 255));   // 上行末尾
fg->AddCircle(window->DC.CursorMaxPos, 5.0f, IM_COL32(255, 255, 0, 255));    // 历史最远点
```

——绿色 = 下一个 widget 起点，红色 = SameLine 回退点，黄色圈 = CursorMaxPos（决定窗口自动大小）。

---

## 5.1.7 案例 3（高级）：`BeginRowAligned`：竖直方向居中对齐

不同高度的 widget 在同一行时，默认是顶部对齐（baseline）。如果想竖直居中：

```cpp
ImGui::Text("Label:");
ImGui::SameLine();
ImGui::Image(my_tex, ImVec2(64, 64));   // 默认顶部对齐——Text 偏上
```

期望：

```
[Label:] [   Image   ]
            居中对齐
```

实现：

```cpp
namespace ImGui {

struct AlignedRowState {
    float MaxHeightThisFrame;
    float MaxHeightLastFrame;
    int   ItemIndex;
    ImVec2 BackupCursorPos;
};

void BeginAlignedRow(const char* str_id) {
    ImGuiID id = GetID(str_id);
    PushOverrideID(id);
    
    AlignedRowState* s = ...;   // 从 storage 取
    s->MaxHeightThisFrame = 0;
    s->ItemIndex = 0;
    s->BackupCursorPos = GetCurrentWindow()->DC.CursorPos;
}

void NextAlignedItem() {
    AlignedRowState* s = ...;
    
    if (s->ItemIndex > 0)
        SameLine();
    
    if (s->MaxHeightLastFrame > 0) {
        // 用上帧 max height 把当前 item 偏移到中心
        // SetCursorPosY 把 cursor 向下推（item 实际画的位置）
        // 但 item 的高度未知——我们简化：假设所有 item 用 frame line height 对齐
        float my_height = GetTextLineHeight();   // 占位估算
        float offset = (s->MaxHeightLastFrame - my_height) * 0.5f;
        if (offset > 0.0f) {
            float y = GetCursorPosY();
            SetCursorPosY(y + offset);
        }
    }
    s->ItemIndex++;
}

void EndAlignedRow() {
    AlignedRowState* s = ...;
    
    // 测量本帧实际 max height（用 LastItemData）
    // 简化：用 GetCursorMaxPos
    // ... 复杂，省略
    
    PopID();
}

}
```

实际生产中往往直接用 `BeginGroup` + 手动 `SetCursorPos` 实现——不必造个新 API。但理解原理后能在需要时快速实现。

---

## 5.1.8 实战：Hazel Inspector 中的"标签 + 控件"对齐

Hazel 编辑器的属性面板（Inspector）常需要：

```
[Position    ] [____X____] [____Y____] [____Z____]
[Rotation    ] [____X____] [____Y____] [____Z____]
[Scale       ] [____X____] [____Y____] [____Z____]
```

——左侧标签固定宽度、右侧 3 个数字输入按权重分配。

```cpp
namespace Hazel::UI {

void DrawVec3Control(const char* label, glm::vec3& values, float reset_value = 0.0f, float column_width = 120.0f) {
    ImGui::PushID(label);
    
    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, column_width);
    ImGui::Text("%s", label);
    ImGui::NextColumn();
    
    // 第二列：3 个输入
    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.8f, 0.1f, 0.15f, 1.0f });
    if (ImGui::Button("X"))
        values.x = reset_value;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::DragFloat("##X", &values.x, 0.1f);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    
    // ... 类似 Y / Z ...
    
    ImGui::Columns(1);
    ImGui::PopID();
}

}
```

**`PushMultiItemsWidths(3, total_width)`** 是 internal API（`imgui.cpp` 中），把 `total_width` 平均分配给后续 3 个 item——比手算简单。

---

## 5.1.9 性能特性

每个 BeginXxx / EndXxx 的开销大致：

| 操作 | 微秒级开销 |
|---|---|
| `BeginGroup / EndGroup` | < 0.1μs（修改 stack 字段） |
| `SameLine` | < 0.05μs |
| 一次 ImGuiStorage 读 | < 0.1μs（O(log N)） |
| 一次 ItemAdd | < 0.5μs |
| 自定义 BeginFlexRow + 4 slots | < 1μs |

**1000 个 widget 用 BeginFlexRow 包装**：约 1 ms 开销。可忽略。

---

## 5.1.10 本章小结

ImGui Layout 的核心原语：

1. `ItemSize` 推进 cursor 到下一行。
2. `SameLine` 把 cursor 退回上行末尾。
3. `Indent` / `ColumnsOffset` / `GroupOffset` 三个偏移叠加成行首位置。
4. `BeginGroup` / `EndGroup` 备份 + 测量 + 恢复——把多个 item 当一个。
5. `CursorPosPrevLine / CurrLineSize / PrevLineSize` 是 SameLine 的备份。

扩展 Layout 的 3 类模式：

| 模式 | 用法 | 限制 |
|---|---|---|
| **直接计算 + SameLine** | FlexRow / Grid | 不能在测量后调整布局 |
| **BeginGroup 测量** | 居中 / 右对齐 | 1 帧延迟 |
| **基于 ImGuiStorage 跨帧缓存** | 自适应宽度 | 第一帧错位 |

**核心规则**：

- 所有跨帧状态用 `window->StateStorage` + ID 隔离。
- 修改 `DC.CursorPos` 前**总是**保存现状（用 BeginGroup 或手动 backup）。
- `SkipItems` 早退是必须。
- Indent / ColumnsOffset / GroupOffset 三个 offset 在自定义换行时要加回去。

---

## 5.1.11 下一章预告

第 5.2 章 [[20_第五部分_02_案例二_高性能节点编辑器]] 会带你做一个**完整的节点编辑器**——

- pan / zoom 画布（基于自定义 viewport + ClipRect）。
- 节点拖动（基于 ButtonBehavior + IsMouseDragging）。
- 端口（Pin）的连接交互（drag 起点 + 命中目标）。
- 连线 Bezier 渲染（PathBezierCubicCurveTo + AA）。
- **`ImDrawListSplitter` 多通道**：背景网格 / 连线 / 节点框 / 节点内部 / 选中框 5 通道，最终合并成 5 个 cmd 而不是 5000。
- ID Stack 命名空间设计：避免 1000 节点 ID 冲突。
- 性能优化目标：1000 节点 60fps。

读完后你应该能写出比 imnodes / imgui-node-editor 性能更好的实现。
