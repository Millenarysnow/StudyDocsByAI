# 第 3.2 章 · `ImDrawList` 数据布局与 Path 系统

> **本章目标**：把 `ImDrawList` 这个 ImGui 核心绘制结构的**每个字段**剖完——它们的语义、协作机制、内存占用。然后讲清 **Path API** 的工作原理（`PathLineTo` / `PathArcTo` / `PathArcToFast` / `PathBezierCubicCurveTo`）。读完后你应该能：(1) 直接用 `ImDrawList` 的低级 API（`PrimReserve` + `PrimWriteVtx`）写自定义图元；(2) 实现自己的曲线绘制；(3) 理解为什么 ImGui 画一个圆的速度比 OpenGL 立即模式快几十倍。
>
> **本章对应源码**：`imgui.h:3287~3440`（`ImDrawList` 完整定义）、`imgui_internal.h:856~885`（`ImDrawListSharedData`）、`imgui_internal.h:832~854`（圆形细分宏）、`imgui_draw.cpp:504~770`（`AddDrawCmd / _PopUnusedDrawCmd / AddCallback / _TryMergeDrawCmds / _OnChangedClipRect / PrimReserve / PrimRect`）、`imgui_draw.cpp:1149~1270`（Path 系列：`_PathArcToFastEx / _PathArcToN / PathArcToFast / PathArcTo`）、`imgui_draw.cpp:1500~1640`（`AddRectFilled / AddCircle / AddCircleFilled`）。
>
> **前置阅读**：第 0.4 章（GPU 管线）、第 2.1 章（ImVector）、第 3.1 章（一帧旅程）。

---

## 3.2.0 `ImDrawList` 是什么

回到第 0.4 章的"心智模型 ③"——顶点缓冲是 ImGui 与 GPU 的**唯一对外契约**。`ImDrawList` 是这个契约的**生产者**：

```
  user → ImGui → window->DrawList → ImDrawData → backend → GPU
                ─────────────────              ────────
                  本章主题                     第 0.4 章已讲
```

`ImDrawList` 字面意思是"绘制命令列表"——但它的内部存储的不只是命令（`CmdBuffer`），还包括顶点（`VtxBuffer`）和索引（`IdxBuffer`）。**一个 ImDrawList = 一份独立的"网格 + 命令"集合**。

**典型的 ImDrawList 数量**：

- 每个 ImGuiWindow 自带一个：`window->DrawList`。
- 每个 viewport 的 `BgFgDrawLists[2]`（背景 + 前景）。
- 用户通过 `IM_NEW(ImDrawList)(&shared_data)` 也可以自己创建。

一个中等编辑器一帧大约 **20~50 个 ImDrawList**。

---

## 3.2.1 完整字段清单

打开 `imgui.h:3287`：

```cpp
struct ImDrawList
{
    // ===== 用户消费的数据 =====
    ImVector<ImDrawCmd>     CmdBuffer;          // 绘制命令数组
    ImVector<ImDrawIdx>     IdxBuffer;          // 索引缓冲
    ImVector<ImDrawVert>    VtxBuffer;          // 顶点缓冲
    ImDrawListFlags         Flags;              // 抗锯齿等开关

    // ===== Internal: 构建时使用 =====
    unsigned int            _VtxCurrentIdx;     // 当前下一个顶点的索引
    ImDrawListSharedData*   _Data;              // 共享数据（圆形采样表等）
    ImDrawVert*             _VtxWritePtr;       // 当前 VtxBuffer 写入位置
    ImDrawIdx*              _IdxWritePtr;       // 当前 IdxBuffer 写入位置
    ImVector<ImVec2>        _Path;              // 临时 path 顶点
    ImDrawCmdHeader         _CmdHeader;         // 当前 cmd 模板（ClipRect/TexRef/VtxOffset）
    ImDrawListSplitter      _Splitter;          // Channels API 用
    ImVector<ImVec4>        _ClipRectStack;     // PushClipRect 栈
    ImVector<ImTextureRef>  _TextureStack;      // PushTexture 栈
    ImVector<ImU8>          _CallbacksDataBuf;  // AddCallback userdata 缓冲
    float                   _FringeScale;       // AA fringe 缩放
    const char*             _OwnerName;         // 调试用：宿主窗口名
    // ...
};
```

**字段分 3 组**：

1. **公开数据（`CmdBuffer / IdxBuffer / VtxBuffer / Flags`）**：backend 消费的对外契约。
2. **构建时游标（`_VtxCurrentIdx / _VtxWritePtr / _IdxWritePtr`）**：性能优化——避免每次 push_back。
3. **状态栈（`_ClipRectStack / _TextureStack / _CmdHeader`）**：决定下一个 cmd 的属性。

`sizeof(ImDrawList)` 约 **150~200 字节**（不算容器内部数据）。容器扩容后总共占 KB ~ MB 级别（取决于顶点数）。

---

## 3.2.2 `_VtxCurrentIdx / _VtxWritePtr / _IdxWritePtr`：写入游标

这 3 个字段是 ImDrawList 的**热路径优化**。

### 为什么不直接用 `VtxBuffer.push_back`

```cpp
// 朴素写法（慢）
for (int i = 0; i < n; i++) {
    ImDrawVert v;
    v.pos = pos[i];
    v.uv = uv[i];
    v.col = col;
    VtxBuffer.push_back(v);   // 每次检查 capacity / memcpy / Size++
}
```

`push_back` 每次：
1. 检查 `Size == Capacity`（分支预测）。
2. `memcpy(&Data[Size], &v, sizeof(v))`。
3. `Size++`。

虽然每步都很快，但一帧有上万次顶点写入，累计开销可观。

### `PrimReserve + PrimWriteVtx` 的优化

```cpp
// imgui_draw.cpp:719
void ImDrawList::PrimReserve(int idx_count, int vtx_count)
{
    // ① 大网格支持（VtxOffset 跳转）
    if (sizeof(ImDrawIdx) == 2 && (_VtxCurrentIdx + vtx_count >= (1 << 16)) && (Flags & ImDrawListFlags_AllowVtxOffset))
    {
        _CmdHeader.VtxOffset = VtxBuffer.Size;
        _OnChangedVtxOffset();
    }

    // ② 累加 cmd 的 ElemCount（这一组顶点的索引数）
    ImDrawCmd* draw_cmd = &CmdBuffer.Data[CmdBuffer.Size - 1];
    draw_cmd->ElemCount += idx_count;

    // ③ 一次性扩容 VtxBuffer / IdxBuffer
    int vtx_buffer_old_size = VtxBuffer.Size;
    VtxBuffer.resize(vtx_buffer_old_size + vtx_count);
    _VtxWritePtr = VtxBuffer.Data + vtx_buffer_old_size;

    int idx_buffer_old_size = IdxBuffer.Size;
    IdxBuffer.resize(idx_buffer_old_size + idx_count);
    _IdxWritePtr = IdxBuffer.Data + idx_buffer_old_size;
}
```

**做的事**：

1. 一次性扩容 VtxBuffer 和 IdxBuffer 到目标大小。
2. 把 `_VtxWritePtr` 指向新分配区域的起点。
3. 把 `_IdxWritePtr` 指向新分配区域的起点。
4. 更新当前 cmd 的 `ElemCount`。

```cpp
// imgui.h:3412~3414
inline void PrimWriteVtx(const ImVec2& pos, const ImVec2& uv, ImU32 col) {
    _VtxWritePtr->pos = pos;
    _VtxWritePtr->uv = uv;
    _VtxWritePtr->col = col;
    _VtxWritePtr++;            // 指针推进
    _VtxCurrentIdx++;          // 索引值推进
}
inline void PrimWriteIdx(ImDrawIdx idx) {
    *_IdxWritePtr = idx;
    _IdxWritePtr++;
}
```

**`PrimWriteVtx` 的实质**：直接通过指针写——不检查边界、不调用任何函数。等价于**朴素 C 数组**的写法：

```cpp
*p++ = v;
```

——CPU 一条指令。

**这就是 ImDrawList 性能的核心**：先一次性扩容到位（一次分配），然后通过裸指针填数据（最快）。

### `_VtxCurrentIdx` 与 `VtxBuffer.Size` 的关系

```cpp
unsigned int _VtxCurrentIdx;   // 注释：generally == VtxBuffer.Size unless we are past 64K vertices
```

通常 `_VtxCurrentIdx == VtxBuffer.Size`——它就是"下一个写入的顶点的索引"。

但是当**16-bit 索引溢出时**（`VtxBuffer.Size > 65535`）：

- 如果 backend 支持 `RendererHasVtxOffset`：在 `PrimReserve` 里设置 `_CmdHeader.VtxOffset = VtxBuffer.Size`，让后续索引值**从 0 重新计数**——但实际顶点位置在 `VtxOffset` 之后。
- `_VtxCurrentIdx` 在这种情况下被重置为 0。

backend 渲染时：

```cpp
// 16-bit 索引 + VtxOffset：
glDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices, vtx_offset);
```

——索引仍然是 16-bit（值最大 65535），但实际顶点 = `VtxBuffer[index + vtx_offset]`。这样**理论顶点数无上限**（只受 `IdxBuffer` 的 32-bit 大小限制）。

### `PrimVtx`：组合写法

```cpp
// imgui.h:3414
inline void PrimVtx(const ImVec2& pos, const ImVec2& uv, ImU32 col) {
    PrimWriteIdx((ImDrawIdx)_VtxCurrentIdx);
    PrimWriteVtx(pos, uv, col);
}
```

——写一个独立顶点（不复用）。每个顶点用**唯一**索引。适合简单几何。

### 复用顶点的写法

```cpp
// imgui_draw.cpp:756 PrimRect
void ImDrawList::PrimRect(const ImVec2& a, const ImVec2& c, ImU32 col)
{
    ImVec2 b(c.x, a.y), d(a.x, c.y), uv(_Data->TexUvWhitePixel);
    ImDrawIdx idx = (ImDrawIdx)_VtxCurrentIdx;

    // 6 个索引，复用 4 个顶点
    _IdxWritePtr[0] = idx;     _IdxWritePtr[1] = (ImDrawIdx)(idx+1); _IdxWritePtr[2] = (ImDrawIdx)(idx+2);
    _IdxWritePtr[3] = idx;     _IdxWritePtr[4] = (ImDrawIdx)(idx+2); _IdxWritePtr[5] = (ImDrawIdx)(idx+3);

    // 4 个顶点（左上、右上、右下、左下）
    _VtxWritePtr[0].pos = a; _VtxWritePtr[0].uv = uv; _VtxWritePtr[0].col = col;
    _VtxWritePtr[1].pos = b; _VtxWritePtr[1].uv = uv; _VtxWritePtr[1].col = col;
    _VtxWritePtr[2].pos = c; _VtxWritePtr[2].uv = uv; _VtxWritePtr[2].col = col;
    _VtxWritePtr[3].pos = d; _VtxWritePtr[3].uv = uv; _VtxWritePtr[3].col = col;

    _VtxWritePtr += 4;
    _VtxCurrentIdx += 4;
    _IdxWritePtr += 6;
}
```

**对比 PrimVtx**：

- `PrimVtx × 6` = 6 顶点 + 6 索引（无复用）。
- `PrimRect` = 4 顶点 + 6 索引（顶点 1 和 2 被两个三角形共享）。

每个矩形省 2 个顶点（40 字节）。一帧上千个矩形 = 几十 KB 节省。

---

## 3.2.3 `_CmdHeader`：下一个 cmd 的"模板"

打开 `imgui.h:3216`：

```cpp
struct ImDrawCmdHeader
{
    ImVec4          ClipRect;
    ImTextureRef    TexRef;
    unsigned int    VtxOffset;
};
```

**注意 `_CmdHeader` 与 `ImDrawCmd` 的关系**：

```cpp
struct ImDrawCmd {
    ImVec4          ClipRect;
    ImTextureRef    TexRef;
    unsigned int    VtxOffset;
    unsigned int    IdxOffset;
    unsigned int    ElemCount;
    ImDrawCallback  UserCallback;
    // ...
};
```

`_CmdHeader` 的 3 个字段是 `ImDrawCmd` **前 3 个字段**——**精心选择的子集**。

### 为什么是这个子集

这 3 个字段**决定是否需要新建 cmd**：

- `ClipRect` 变 → 必须新 cmd（scissor 变化）。
- `TexRef` 变 → 必须新 cmd（绑定纹理变化）。
- `VtxOffset` 变 → 必须新 cmd（base vertex 变化）。

`IdxOffset` 不需要（它就是 `IdxBuffer.Size` 的当前值，自动跟随）。
`ElemCount` 不需要（每次 PrimReserve 累加）。
`UserCallback` 不需要（普通图元都是 NULL）。

### `ImDrawCmd_HeaderCompare`：用 memcmp 一次比较 3 字段

`imgui_draw.cpp:564~568`：

```cpp
#define ImDrawCmd_HeaderSize                            (offsetof(ImDrawCmd, VtxOffset) + sizeof(unsigned int))
#define ImDrawCmd_HeaderCompare(CMD_LHS, CMD_RHS)       (memcmp(CMD_LHS, CMD_RHS, ImDrawCmd_HeaderSize))    // Compare ClipRect, TexRef, VtxOffset
#define ImDrawCmd_HeaderCopy(CMD_DST, CMD_SRC)          (memcpy(CMD_DST, CMD_SRC, ImDrawCmd_HeaderSize))    // Copy ClipRect, TexRef, VtxOffset
```

**为什么用 `memcmp`**：3 个字段独立比较 = 3 次比较 + 3 次分支。`memcmp` = 一次连续内存比较 = 编译器可向量化（SSE/AVX）。

`imgui.h:3179` 的 ImDrawCmd 注释：

> `// - The ClipRect/TexRef/VtxOffset fields must be contiguous as we memcmp() them together (this is asserted for).`

——这就是为什么 `ImDrawCmd` 的字段顺序是固定的。如果你改了 `ImDrawCmd` 字段顺序破坏 contiguous，整个 ImGui 会编译失败（IM_ASSERT）。

### `_CmdHeader` 的更新流程

```
用户调 PushClipRect
   ↓
_ClipRectStack.push_back(rect)
   ↓
_CmdHeader.ClipRect = rect      ← 更新模板
   ↓
_OnChangedClipRect()             ← 决定是否新建 cmd
```

```
用户调 PushTexture
   ↓
_TextureStack.push_back(tex)
   ↓
_CmdHeader.TexRef = tex
   ↓
_OnChangedTexture()
```

`_OnChangedClipRect / _OnChangedTexture` 的逻辑见第 3.4 章——它们决定"扩展当前 cmd"还是"新建 cmd"。

---

## 3.2.4 `CmdBuffer` 的"恒不变量"

ImDrawList 的一个关键设计：**`CmdBuffer.Size > 0` 永远成立**（除非刚刚 ResetForNewFrame）。

```cpp
// imgui_draw.cpp 中的注释 (line 583)
// Our scheme may appears a bit unusual, basically we want the most-common calls AddLine AddRect etc.
// to not have to perform any check so we always have a command ready in the stack.
// The cost of figuring out if a new command has to be added or if we can merge is paid in those Update** functions only.
```

**翻译**：ImGui 想让 `AddLine / AddRect` 等热路径**不做检查**。所以：

- `_ResetForNewFrame` 后立刻 `AddDrawCmd()` 一个空 cmd（ElemCount=0）。
- 用户写顶点时直接累加到这个 cmd 的 `ElemCount`。
- 状态变化时（PushClipRect / PushTexture / PushFont），调 `_OnChangedXxx` 决定是新建还是合并。
- Render 前调 `_PopUnusedDrawCmd` 清掉末尾的空 cmd。

**好处**：99% 的图元提交不需要检查"cmd 是否为空"——直接写入。

### `AddDrawCmd`

```cpp
// imgui_draw.cpp:504
void ImDrawList::AddDrawCmd()
{
    ImDrawCmd draw_cmd;
    draw_cmd.ClipRect = _CmdHeader.ClipRect;
    draw_cmd.TexRef = _CmdHeader.TexRef;
    draw_cmd.VtxOffset = _CmdHeader.VtxOffset;
    draw_cmd.IdxOffset = IdxBuffer.Size;

    IM_ASSERT(draw_cmd.ClipRect.x <= draw_cmd.ClipRect.z && draw_cmd.ClipRect.y <= draw_cmd.ClipRect.w);
    CmdBuffer.push_back(draw_cmd);
}
```

把 `_CmdHeader` 的内容 + 当前 `IdxBuffer.Size`（作为 `IdxOffset`）打包成一个新 cmd 加入 `CmdBuffer`。

### `_PopUnusedDrawCmd`

```cpp
// imgui_draw.cpp:518
void ImDrawList::_PopUnusedDrawCmd()
{
    while (CmdBuffer.Size > 0)
    {
        ImDrawCmd* curr_cmd = &CmdBuffer.Data[CmdBuffer.Size - 1];
        if (curr_cmd->ElemCount != 0 || curr_cmd->UserCallback != NULL)
            return;
        CmdBuffer.pop_back();
    }
}
```

**做的事**：从尾部弹出所有"空"cmd（ElemCount==0 且不是 callback）。在 Render 前调，避免给 backend 一堆空 cmd 导致它徒劳遍历。

---

## 3.2.5 `_ClipRectStack` 与 `_TextureStack`

```cpp
ImVector<ImVec4>        _ClipRectStack;     // Stack of clip rectangles
ImVector<ImTextureRef>  _TextureStack;      // Stack of textures
```

**为什么是栈**：`Push/Pop` 配对——支持嵌套。

```cpp
draw_list->PushClipRect(min1, max1);
    draw_list->PushClipRect(min2, max2, /*intersect=*/true);   // 嵌套裁剪
        // ... 绘制 ...
    draw_list->PopClipRect();
    // 回到 (min1, max1) 裁剪
draw_list->PopClipRect();
```

```cpp
draw_list->PushTexture(my_texture);
    draw_list->AddImage(my_texture, ...);   // 实际上 PushTexture + AddImage 顺序无所谓
    // ... 用 my_texture 绘制 ...
draw_list->PopTexture();
```

### `PushClipRect`

```cpp
// imgui_draw.cpp 中（简化）
void ImDrawList::PushClipRect(const ImVec2& cr_min, const ImVec2& cr_max, bool intersect_with_current_clip_rect)
{
    ImVec4 cr(cr_min.x, cr_min.y, cr_max.x, cr_max.y);
    if (intersect_with_current_clip_rect) {
        ImVec4 current = _CmdHeader.ClipRect;
        if (cr.x < current.x) cr.x = current.x;
        if (cr.y < current.y) cr.y = current.y;
        if (cr.z > current.z) cr.z = current.z;
        if (cr.w > current.w) cr.w = current.w;
    }
    cr.z = ImMax(cr.x, cr.z);
    cr.w = ImMax(cr.y, cr.w);

    _ClipRectStack.push_back(cr);
    _CmdHeader.ClipRect = cr;
    _OnChangedClipRect();
}
```

**`intersect_with_current_clip_rect` 参数**：

- `true`：把新 rect 与当前 rect **求交集**——常用，例如子窗口被父窗口裁剪。
- `false`：直接用新 rect——较少用（强制裁剪到指定区域）。

### `PopClipRect`

```cpp
void ImDrawList::PopClipRect()
{
    _ClipRectStack.pop_back();
    _CmdHeader.ClipRect = (_ClipRectStack.Size == 0) ? _Data->ClipRectFullscreen : _ClipRectStack.Data[_ClipRectStack.Size - 1];
    _OnChangedClipRect();
}
```

弹出栈顶后，`_CmdHeader.ClipRect` 设为新栈顶（或 fullscreen 如果栈空）。

---

## 3.2.6 `_FringeScale`：抗锯齿 fringe 缩放

```cpp
float _FringeScale;   // anti-alias fringe is scaled by this value
```

**默认 1.0f**——表示 AA fringe（抗锯齿过渡区）宽度为 1 像素。

### 高 DPI / 高缩放场景

如果你在编辑器里实现"画布缩放"（用户缩放 ImGui 显示，例如节点编辑器拖到 200% 大小），AA fringe 也应该缩放——否则 fringe 永远是 1 像素显得太细。

```cpp
ImDrawList* dl = ImGui::GetWindowDrawList();
float old_fringe = dl->_FringeScale;
dl->_FringeScale = 1.0f / current_zoom;   // 缩小 fringe 让它在屏幕上保持 1 像素
// ... 绘制 ...
dl->_FringeScale = old_fringe;
```

如果 zoom = 2.0，`_FringeScale = 0.5`——AA 顶点扩展量减半，最终在屏幕上仍是 1 像素。

### 注释说明

`imgui.h:3306` 的注释：

> `// [Internal] anti-alias fringe is scaled by this value, this helps to keep things sharp while zooming at vertex buffer content`

——专门给"放大画布"场景用的。

---

## 3.2.7 `_CallbacksDataBuf`：回调数据缓冲

```cpp
ImVector<ImU8> _CallbacksDataBuf;
```

回顾 `AddCallback`（`imgui_draw.cpp:529`）：

```cpp
void ImDrawList::AddCallback(ImDrawCallback callback, void* userdata, size_t userdata_size)
{
    // ...
    if (userdata_size == 0) {
        // 直接存指针
        curr_cmd->UserCallbackData = userdata;
        curr_cmd->UserCallbackDataSize = 0;
        curr_cmd->UserCallbackDataOffset = -1;
    } else {
        // 复制数据到缓冲
        curr_cmd->UserCallbackData = NULL;   // Render 时由 Render 函数填充
        curr_cmd->UserCallbackDataSize = (int)userdata_size;
        curr_cmd->UserCallbackDataOffset = _CallbacksDataBuf.Size;
        _CallbacksDataBuf.resize(_CallbacksDataBuf.Size + (int)userdata_size);
        memcpy(_CallbacksDataBuf.Data + curr_cmd->UserCallbackDataOffset, userdata, userdata_size);
    }
    AddDrawCmd();   // Force a new command after us
}
```

**两种模式**：

- `userdata_size == 0`：用户传的 `void*` 直接存进 cmd——用户自己保证指针生命周期。
- `userdata_size > 0`：ImGui 把 N 字节复制到 `_CallbacksDataBuf`——用户传的指针可以是栈上临时数据。

**为什么需要后一种**：

```cpp
// 用户场景：在 lambda 里弹出回调
struct MyData { int a, b, c; };
MyData d = { 1, 2, 3 };

draw_list->AddCallback(MyCallback, &d, sizeof(d));   // 复制
// 此时 d 离开作用域，但 ImGui 已经存了副本

// Render 时 callback 看到副本的指针
```

### Render 时如何拿到 userdata

`backends/imgui_impl_*.cpp` 的 RenderDrawData 内部：

```cpp
// 简化伪代码
for (ImDrawCmd& cmd : pcmd->CmdBuffer) {
    if (cmd.UserCallback) {
        // 如果是从 buf 拷贝过来的，要 fix 一下指针
        if (cmd.UserCallbackDataOffset != -1)
            cmd.UserCallbackData = draw_list->_CallbacksDataBuf.Data + cmd.UserCallbackDataOffset;
        cmd.UserCallback(draw_list, &cmd);
    } else {
        // 普通 draw call
    }
}
```

`UserCallbackData` 在 cmd 提交时是 NULL（因为 `_CallbacksDataBuf` 可能扩容导致 Data 指针变）；Render 时才修正。

---

## 3.2.8 `ImDrawListSharedData`：共享只读数据

`imgui_internal.h:860`：

```cpp
struct IMGUI_API ImDrawListSharedData
{
    ImVec2          TexUvWhitePixel;            // 字体图集中的纯白像素 UV
    const ImVec4*   TexUvLines;                 // AA lines 纹理 UV 表
    ImFontAtlas*    FontAtlas;
    ImFont*         Font;
    float           FontSize;
    float           FontScale;
    float           CurveTessellationTol;       // Bezier 容差
    float           CircleSegmentMaxError;
    float           InitialFringeScale;
    ImDrawListFlags InitialFlags;
    ImVec4          ClipRectFullscreen;
    ImVector<ImVec2>      TempBuffer;           // 临时计算 buffer（AA 算法用）
    ImVector<ImDrawList*> DrawLists;
    ImGuiContext*   Context;

    // 预计算 lookup tables
    ImVec2          ArcFastVtx[IM_DRAWLIST_ARCFAST_TABLE_SIZE];   // 48 个圆周采样点
    float           ArcFastRadiusCutoff;
    ImU8            CircleSegmentCounts[64];                       // 半径 → 段数
};
```

**核心是 2 个查找表**：

- `ArcFastVtx[48]`：圆的 48 个等距采样点（cos/sin 的预计算值）。
- `CircleSegmentCounts[64]`：半径 0~63 对应的"自适应段数"。

### `ArcFastVtx`：48 个角度的 cos/sin

```cpp
// 在 ImDrawListSharedData 构造时填充
for (int i = 0; i < IM_DRAWLIST_ARCFAST_TABLE_SIZE; i++) {
    const float a = ((float)i * 2 * IM_PI) / IM_DRAWLIST_ARCFAST_TABLE_SIZE;
    ArcFastVtx[i] = ImVec2(ImCos(a), ImSin(a));
}
```

48 个角度 = 360° / 48 = 7.5° 间隔。每个采样点是单位圆上的 (cos a, sin a)。

**画一个半径 r 的圆**就是：

```cpp
for (int i = 0; i < 48; i++) {
    ImVec2 p = center + ArcFastVtx[i] * r;
    _Path.push_back(p);
}
```

——零三角函数调用！48 次乘法 + 48 次加法。

### `CircleSegmentCounts[64]`：半径自适应段数

数学：画一个半径 r 的圆，要让"折线与真圆"的最大误差不超过 `max_error`，需要的段数 N：

```
N = π / acos(1 - max_error / r)
```

`imgui_internal.h:832~844` 的宏：

```cpp
#define IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_CALC(_RAD,_MAXERROR)   \
    ImClamp(IM_ROUNDUP_TO_EVEN((int)ImCeil(IM_PI / ImAcos(1 - ImMin((_MAXERROR), (_RAD)) / (_RAD)))), \
            IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_MIN, IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_MAX)
```

**为什么 `IM_ROUNDUP_TO_EVEN`**：注释里说：

> `Rendering circles with an odd number of segments, while mathematically correct will produce asymmetrical results on the raster grid. Therefore we're rounding N to next even number.`

奇数段数会导致圆的左右两边光栅化不对称。强制偶数。

### 预计算的查表

```cpp
// SetCircleTessellationMaxError 中
for (int i = 0; i < 64; i++) {
    const float radius = (float)i;
    CircleSegmentCounts[i] = (radius > 0.0f) ? IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_CALC(radius, max_error) : 0;
}
ArcFastRadiusCutoff = /* 半径阈值 */;
```

调 `_CalcCircleAutoSegmentCount` 时：

```cpp
// imgui_draw.cpp（简化）
int ImDrawList::_CalcCircleAutoSegmentCount(float radius) const
{
    const int radius_idx = (int)(radius + 0.999999f);
    if (radius_idx < IM_ARRAYSIZE(_Data->CircleSegmentCounts))
        return _Data->CircleSegmentCounts[radius_idx];
    return IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_CALC(radius, _Data->CircleSegmentMaxError);
}
```

——半径 < 64 直接查表（O(1)）；> 64 才计算（罕见的大圆）。

---

## 3.2.9 Path API：从点序列到几何

Path API 是 ImDrawList 的"高级图元层"。基本流程：

```cpp
draw_list->PathLineTo(p0);
draw_list->PathLineTo(p1);
draw_list->PathLineTo(p2);
draw_list->PathFillConvex(col);   // 或 PathStroke(col, ImDrawFlags_Closed, thickness)
```

**两步**：

1. 用 `PathLineTo / PathArcTo / PathBezier...` 累积顶点到 `_Path`（`ImVector<ImVec2>`）。
2. 用 `PathFillConvex / PathStroke` 把 `_Path` 转成实际几何（最终调 `AddConvexPolyFilled / AddPolyline`），然后清空 `_Path`。

### 简单路径函数

```cpp
// imgui.h:3366~3371
inline void PathClear() { _Path.Size = 0; }
inline void PathLineTo(const ImVec2& pos) { _Path.push_back(pos); }
inline void PathLineToMergeDuplicate(const ImVec2& pos) {
    if (_Path.Size == 0 || memcmp(&_Path.Data[_Path.Size - 1], &pos, 8) != 0)
        _Path.push_back(pos);
}
inline void PathFillConvex(ImU32 col) {
    AddConvexPolyFilled(_Path.Data, _Path.Size, col);
    _Path.Size = 0;
}
inline void PathStroke(ImU32 col, ImDrawFlags flags = 0, float thickness = 1.0f) {
    AddPolyline(_Path.Data, _Path.Size, col, flags, thickness);
    _Path.Size = 0;
}
```

**全部都是 inline**——`PathLineTo` 在编译后等于 `_Path.push_back(pos)`，零开销。

`PathLineToMergeDuplicate` 用 `memcmp` 比较两个 ImVec2 的字节——如果新点和上一个点完全相同，跳过。避免重复点导致退化三角形。

### `PathArcToFast`：12 等分的快速圆弧

```cpp
// imgui_draw.cpp:1260
void ImDrawList::PathArcToFast(const ImVec2& center, float radius, int a_min_of_12, int a_max_of_12)
{
    if (radius < 0.5f) {
        _Path.push_back(center);
        return;
    }
    _PathArcToFastEx(center, radius, a_min_of_12 * IM_DRAWLIST_ARCFAST_SAMPLE_MAX / 12, a_max_of_12 * IM_DRAWLIST_ARCFAST_SAMPLE_MAX / 12, 0);
}
```

**`a_min_of_12` / `a_max_of_12`**：用"12 等分"表示角度——0 是 0°、3 是 90°、6 是 180°、9 是 270°、12 是 360°。

例：画一个完整圆：

```cpp
draw_list->PathArcToFast(center, radius, 0, 12);
draw_list->PathStroke(col, ImDrawFlags_Closed, thickness);
```

画右下 1/4 圆：

```cpp
draw_list->PathArcToFast(center, radius, 0, 3);
```

`12` 是历史选择——12 是 360 的常见因数（适合 30°/60°/90° 这些常用角度）。

### `_PathArcToFastEx`：核心算法

回看 `imgui_draw.cpp:1149`（之前已经摘录过）：

```cpp
void ImDrawList::_PathArcToFastEx(const ImVec2& center, float radius, int a_min_sample, int a_max_sample, int a_step)
{
    if (radius < 0.5f) {
        _Path.push_back(center);
        return;
    }

    if (a_step <= 0)
        a_step = IM_DRAWLIST_ARCFAST_SAMPLE_MAX / _CalcCircleAutoSegmentCount(radius);

    a_step = ImClamp(a_step, 1, IM_DRAWLIST_ARCFAST_TABLE_SIZE / 4);

    const int sample_range = ImAbs(a_max_sample - a_min_sample);
    int samples = sample_range + 1;
    bool extra_max_sample = false;
    if (a_step > 1) {
        samples = sample_range / a_step + 1;
        const int overstep = sample_range % a_step;
        if (overstep > 0) {
            extra_max_sample = true;
            samples++;
        }
    }

    _Path.resize(_Path.Size + samples);
    ImVec2* out_ptr = _Path.Data + (_Path.Size - samples);

    int sample_index = a_min_sample;
    if (sample_index < 0 || sample_index >= IM_DRAWLIST_ARCFAST_SAMPLE_MAX) {
        sample_index = sample_index % IM_DRAWLIST_ARCFAST_SAMPLE_MAX;
        if (sample_index < 0) sample_index += IM_DRAWLIST_ARCFAST_SAMPLE_MAX;
    }

    if (a_max_sample >= a_min_sample) {
        for (int a = a_min_sample; a <= a_max_sample; a += a_step) {
            if (sample_index >= IM_DRAWLIST_ARCFAST_SAMPLE_MAX)
                sample_index -= IM_DRAWLIST_ARCFAST_SAMPLE_MAX;
            const ImVec2 s = _Data->ArcFastVtx[sample_index];
            out_ptr->x = center.x + s.x * radius;
            out_ptr->y = center.y + s.y * radius;
            out_ptr++;
            sample_index += a_step;
        }
    }
    // ... 反向版本类似
}
```

**算法核心**：

1. 从 `_Data->ArcFastVtx[sample_index]` 取 `(cos, sin)`。
2. `position = center + (cos, sin) * radius`——一个乘法 + 一个加法。
3. `sample_index += a_step` 推进。

**关键性能数据**：画一个 `radius = 30` 的圆，自动段数大约 16~20。整个圆 16~20 次"两个浮点乘法 + 两个浮点加法"——这是一个**接近 SSE 极限**的操作。

### `PathArcTo`：通用圆弧（更慢）

```cpp
void ImDrawList::PathArcTo(const ImVec2& center, float radius, float a_min, float a_max, int num_segments)
{
    if (radius < 0.5f) {
        _Path.push_back(center);
        return;
    }
    if (num_segments > 0) {
        _PathArcToN(center, radius, a_min, a_max, num_segments);
        return;
    }
    // 优先用 ArcFast
    if (radius <= _Data->ArcFastRadiusCutoff) {
        // 转换 a_min/a_max（弧度）到 ArcFastVtx 的索引空间（0~48）
        // ... 调 _PathArcToFastEx
    } else {
        _PathArcToN(center, radius, a_min, a_max, /*auto*/);
    }
}
```

**两条路径**：

- 半径 ≤ `ArcFastRadiusCutoff`（典型 14~30）：用 `_PathArcToFastEx` 查表。
- 半径更大：用 `_PathArcToN` 实时计算 cos/sin。

`_PathArcToN`（`imgui_draw.cpp` 中）：

```cpp
void ImDrawList::_PathArcToN(const ImVec2& center, float radius, float a_min, float a_max, int num_segments)
{
    _Path.reserve(_Path.Size + num_segments + 1);
    for (int i = 0; i <= num_segments; i++) {
        const float a = a_min + ((float)i / (float)num_segments) * (a_max - a_min);
        _Path.push_back(ImVec2(center.x + ImCos(a) * radius, center.y + ImSin(a) * radius));
    }
}
```

**每个采样点都调 `ImCos / ImSin`**——比查表慢 5~10×。但精度更高（无量化误差）。

### `PathBezierCubicCurveTo`：自适应 Bezier 细分

```cpp
void ImDrawList::PathBezierCubicCurveTo(const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, int num_segments)
{
    ImVec2 p1 = _Path.back();
    if (num_segments == 0) {
        // 自适应细分（Casteljau 算法）
        PathBezierCubicCurveToCasteljau(&_Path, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, p4.x, p4.y, _Data->CurveTessellationTol, 0);
    } else {
        // 固定段数
        const float t_step = 1.0f / (float)num_segments;
        for (int i_step = 1; i_step <= num_segments; i_step++)
            _Path.push_back(ImBezierCubicCalc(p1, p2, p3, p4, t_step * i_step));
    }
}
```

**自适应细分（Casteljau）算法**：

```cpp
void PathBezierCubicCurveToCasteljau(ImVector<ImVec2>* path,
    float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
    float tess_tol, int level)
{
    float dx = x4 - x1;
    float dy = y4 - y1;
    float d2 = ((x2 - x4) * dy - (y2 - y4) * dx);
    float d3 = ((x3 - x4) * dy - (y3 - y4) * dx);
    d2 = (d2 >= 0) ? d2 : -d2;
    d3 = (d3 >= 0) ? d3 : -d3;
    if ((d2 + d3) * (d2 + d3) < tess_tol * (dx*dx + dy*dy)) {
        // 控制点足够接近直线 - 终止递归
        path->push_back(ImVec2(x4, y4));
    } else if (level < 10) {
        // 递归细分
        float x12 = (x1+x2)*0.5f, y12 = (y1+y2)*0.5f;
        float x23 = (x2+x3)*0.5f, y23 = (y2+y3)*0.5f;
        float x34 = (x3+x4)*0.5f, y34 = (y3+y4)*0.5f;
        float x123 = (x12+x23)*0.5f, y123 = (y12+y23)*0.5f;
        float x234 = (x23+x34)*0.5f, y234 = (y23+y34)*0.5f;
        float x1234 = (x123+x234)*0.5f, y1234 = (y123+y234)*0.5f;
        PathBezierCubicCurveToCasteljau(path, x1, y1, x12, y12, x123, y123, x1234, y1234, tess_tol, level + 1);
        PathBezierCubicCurveToCasteljau(path, x1234, y1234, x234, y234, x34, y34, x4, y4, tess_tol, level + 1);
    }
}
```

**算法**：

1. 检查曲线"扁平度"：控制点 P2/P3 到 P1-P4 直线的距离平方和与曲线长度的平方比较。
2. 足够扁平 → 添加终点 P4，停止。
3. 否则 → De Casteljau 算法**对半分**曲线，递归处理两段。

`tess_tol`（`Style.CurveTessellationTol`）默认 1.25——曲线和折线的最大像素误差。值越小，曲线越平滑（采样点越多）。

**自适应的好处**：直线段几乎不细分（1 个点），急转弯处密集采样。比固定段数省顶点很多。

### `PathRect`：圆角矩形

```cpp
void ImDrawList::PathRect(const ImVec2& a, const ImVec2& b, float rounding, ImDrawFlags flags)
{
    if (rounding >= 0.5f) {
        // ... 4 个圆角各调一次 PathArcToFast
        flags = FixRectCornerFlags(flags);
        rounding = ImMin(rounding, ImFabs(b.x - a.x) * 0.5f - 1.0f);
        rounding = ImMin(rounding, ImFabs(b.y - a.y) * 0.5f - 1.0f);
        if (rounding >= 0.5f) {
            const float rounding_tl = (flags & ImDrawFlags_RoundCornersTopLeft)     ? rounding : 0.0f;
            const float rounding_tr = (flags & ImDrawFlags_RoundCornersTopRight)    ? rounding : 0.0f;
            const float rounding_br = (flags & ImDrawFlags_RoundCornersBottomRight) ? rounding : 0.0f;
            const float rounding_bl = (flags & ImDrawFlags_RoundCornersBottomLeft)  ? rounding : 0.0f;
            PathArcToFast(ImVec2(a.x + rounding_tl, a.y + rounding_tl), rounding_tl, 6, 9);
            PathArcToFast(ImVec2(b.x - rounding_tr, a.y + rounding_tr), rounding_tr, 9, 12);
            PathArcToFast(ImVec2(b.x - rounding_br, b.y - rounding_br), rounding_br, 0, 3);
            PathArcToFast(ImVec2(a.x + rounding_bl, b.y - rounding_bl), rounding_bl, 3, 6);
            return;
        }
    }
    PathLineTo(a);
    PathLineTo(ImVec2(b.x, a.y));
    PathLineTo(b);
    PathLineTo(ImVec2(a.x, b.y));
}
```

——4 个角各 1/4 圆，加直线段连接。

### `AddRectFilled`：用 Path API 实现

```cpp
// imgui_draw.cpp:1509
void ImDrawList::AddRectFilled(const ImVec2& p_min, const ImVec2& p_max, ImU32 col, float rounding, ImDrawFlags flags)
{
    if ((col & IM_COL32_A_MASK) == 0)
        return;
    if (rounding < 0.5f || (flags & ImDrawFlags_RoundCornersMask_) == ImDrawFlags_RoundCornersNone) {
        // 快速路径：无圆角
        PrimReserve(6, 4);
        PrimRect(p_min, p_max, col);
    } else {
        // 圆角路径
        PathRect(p_min, p_max, rounding, flags);
        PathFillConvex(col);
    }
}
```

**两条路径**：

- 矩形（无圆角）：`PrimRect` 一次性写入 4 顶点 + 6 索引——**最快**。
- 圆角矩形：`PathRect` + `PathFillConvex`——慢但漂亮。

**性能差距**：一个无圆角矩形 ≈ 几条 CPU 指令。一个圆角矩形（rounding=4）≈ 16 个采样点 + AA 填充 ≈ 几十条指令。

---

## 3.2.10 几何公式回顾：从 _Path 到三角形

### `AddConvexPolyFilled` 的索引模式

```cpp
// 简化的"无 AA"版本（_imgui_draw.cpp:1129~1146）
const int idx_count = (points_count - 2) * 3;   // N 边形 → N-2 个三角形
const int vtx_count = points_count;
PrimReserve(idx_count, vtx_count);

// 写顶点
for (int i = 0; i < vtx_count; i++) {
    _VtxWritePtr->pos = points[i];
    _VtxWritePtr->uv = uv;
    _VtxWritePtr->col = col;
    _VtxWritePtr++;
}

// 写索引（"扇形"模式）
for (int i = 2; i < points_count; i++) {
    _IdxWritePtr[0] = (ImDrawIdx)(_VtxCurrentIdx);
    _IdxWritePtr[1] = (ImDrawIdx)(_VtxCurrentIdx + i - 1);
    _IdxWritePtr[2] = (ImDrawIdx)(_VtxCurrentIdx + i);
    _IdxWritePtr += 3;
}
_VtxCurrentIdx += vtx_count;
```

**"扇形"三角化（fan triangulation）**：以第一个点为枢轴，每两个相邻外点形成一个三角形。

```
顶点：[P0, P1, P2, P3, P4]
三角形：(P0, P1, P2), (P0, P2, P3), (P0, P3, P4)
```

仅适用于**凸多边形**——凹多边形会有三角形跑出形状外。

### `AddConcavePolyFilled`：凹多边形

```cpp
void ImDrawList::AddConcavePolyFilled(const ImVec2* points, int num_points, ImU32 col)
{
    // 实现：耳切法 (ear clipping) - O(N²)
}
```

**性能差**：凹多边形需要"耳切法"等更复杂的三角化算法。`imgui.h:3350` 的注释：

> `// - Concave polygon fill is more expensive than convex one: it has O(N^2) complexity. Provided as a convenience for the user but not used by the main library.`

ImGui **内部**从不使用 `AddConcavePolyFilled`——所有内置图元都是凸的。但**给用户作为便利**提供。

---

## 3.2.11 实战：自定义 Widget 用 Path API

让我们用 Path API 画一个**带渐变填充的圆角胶囊**：

```cpp
void DrawCapsule(ImDrawList* dl, ImVec2 center, ImVec2 size, ImU32 col_top, ImU32 col_bottom)
{
    float r = size.y * 0.5f;
    ImVec2 a = center - size * 0.5f;
    ImVec2 b = center + size * 0.5f;

    // 用 Path 构造胶囊外轮廓
    dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, IM_PI * 0.5f, IM_PI * 1.5f);
    dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, IM_PI * 1.5f, IM_PI * 2.5f);
    
    // 但 PathFillConvex 不支持渐变...
    // 所以我们手动写顶点
    int n = dl->_Path.Size;
    dl->PrimReserve((n - 2) * 3, n);
    for (int i = 0; i < n; i++) {
        ImVec2 p = dl->_Path[i];
        // 根据 y 位置插值颜色
        float t = (p.y - a.y) / size.y;
        ImU32 col = LerpColor(col_top, col_bottom, t);
        dl->_VtxWritePtr->pos = p;
        dl->_VtxWritePtr->uv = dl->_Data->TexUvWhitePixel;
        dl->_VtxWritePtr->col = col;
        dl->_VtxWritePtr++;
    }
    for (int i = 2; i < n; i++) {
        dl->_IdxWritePtr[0] = (ImDrawIdx)(dl->_VtxCurrentIdx);
        dl->_IdxWritePtr[1] = (ImDrawIdx)(dl->_VtxCurrentIdx + i - 1);
        dl->_IdxWritePtr[2] = (ImDrawIdx)(dl->_VtxCurrentIdx + i);
        dl->_IdxWritePtr += 3;
    }
    dl->_VtxCurrentIdx += n;
    dl->_Path.Size = 0;
}
```

**关键**：用 Path 累积形状的轮廓 → 自己写顶点（覆盖每个顶点的颜色） → 用扇形索引三角化。

这就是 ImGui 自家 `AddRectFilledMultiColor` 的实现思路——4 个顶点各自不同颜色，扇形三角化。

---

## 3.2.12 一个反直觉的发现：ImGui 不裁剪不可见图元

**重要警告**（`imgui.h:3286`）：

> `Important: Primitives are always added to the list and not culled (culling is done at higher-level by ImGui:: functions), if you use this API a lot consider coarse culling your drawn objects.`

**翻译**：`ImDrawList` 的 Add* 函数**不**做"是否在 ClipRect 内"的剔除——你画的所有图元都进入顶点缓冲，最终由 GPU 在 scissor 阶段丢弃。

**为什么这样设计**：

- ImGui 上层（`ItemAdd`）已经做了 CPU 端粗略剔除（不可见的 widget 不调用绘制）。
- DrawList 层做精细剔除会**减慢**热路径（每个图元都查 ClipRect）。
- 在大多数 UI 场景下，被裁掉的图元很少——浪费可控。

**但在以下场景需要你自己剔除**：

- 千行 list（用 `ImGuiListClipper`）。
- 节点编辑器（千个节点，用 quadtree 等）。
- 大量 `AddCircle / AddText` 在 ScrollRegion 中。

---

## 3.2.13 本章小结

`ImDrawList` 的设计哲学：

1. **3 个游标 (`_VtxCurrentIdx / _VtxWritePtr / _IdxWritePtr`)** 让热路径无函数调用、无边界检查。
2. **`_CmdHeader` 是下一个 cmd 的模板** —— 状态变化时新建或合并 cmd。
3. **`CmdBuffer.Size > 0` 永远成立** —— 简化热路径的 if 检查。
4. **`PushClipRect / PushTexture` 是栈式 API** —— 嵌套友好。
5. **`ImDrawListSharedData` 共享只读数据** —— 圆形采样表 / 字体白像素 UV。
6. **`ArcFastVtx[48]` 预计算 cos/sin** —— 圆形渲染零三角函数调用。
7. **`CircleSegmentCounts[64]` 半径自适应段数** —— 小圆少段、大圆多段。
8. **Path API 是分两步** —— 累积顶点 + 触发绘制。
9. **`PathBezierCubicCurveTo` 用 Casteljau 自适应** —— 直线段不细分、急弯密集采样。
10. **`AddRectFilled` 有快速/慢速两条路径** —— 无圆角直接 PrimRect、有圆角走 Path。

**记住的 4 条核心 API**（写自定义图元时）：

```cpp
draw_list->PrimReserve(idx_count, vtx_count);   // 一次扩容
draw_list->_VtxWritePtr->pos = ...;             // 直接指针写顶点
draw_list->_VtxWritePtr->uv = ...;
draw_list->_VtxWritePtr->col = ...;
draw_list->_VtxWritePtr++;
draw_list->_IdxWritePtr[i] = ...;               // 直接指针写索引
draw_list->_VtxCurrentIdx += vtx_count;          // 推进基址
```

这 4 行就是 ImGui 内部所有 `Add*` 图元的本质。

---

## 3.2.14 下一章预告

第 3.3 章 [[13_第三部分_03_抗锯齿AA算法源码]] 会**深入**抗锯齿算法的源码实现：

- `AddPolyline` 的 AA 模式：双重 fringe 顶点、闭合 vs 开放线、粗线扩展三角带。
- `AddConvexPolyFilled` 的 AA 模式：内圈 + 外圈 + alpha 渐变。
- `_FringeScale` 在 AA 算法中的精确语义。
- `IM_NORMALIZE2F_OVER_ZERO / IM_FIXNORMAL2F` 两个宏的数学意义。
- `ImDrawListFlags_AntiAliasedLinesUseTex`（用纹理 AA）的算法分支。
- 性能权衡：什么时候关 AA 是合理的。

读完后你应该能复现"为什么 ImGui 1px 线在屏幕上看起来比 OpenGL 默认 line 漂亮 10 倍"的具体原因——并且能针对自己引擎做相应优化。
