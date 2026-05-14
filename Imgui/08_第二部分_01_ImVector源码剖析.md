# 第 2.1 章 · `ImVector<T>` 源码逐字节剖析

> **本章目标**：把 `imgui.h:2196~2270` 的 75 行 `ImVector<T>` 实现**逐方法**剖完——包括它的 5 个核心字段决策、5 个与 `std::vector` 的根本差异、`grow_capacity` 的 1.5 倍策略数学推导、以及"哪些类型不能放进 ImVector"的精确判定。
>
> **本章对应源码**：`imgui.h:2186~2194`（4 件套宏）、`imgui.h:2196~2270`（`ImVector<T>` 全文）、`imgui_internal.h:670~697`（`ImSpan<T>`）、`imgui_internal.h:107`（`-Wclass-memaccess` pragma）、`imgui.cpp:[SECTION] CONTEXT AND MEMORY ALLOCATORS`。
>
> **前置阅读**：第 0.2 章（C++ 子集 / 模板克制）、第 0.3 章（内存模型 101 / Trivially Relocatable）。

---

## 2.1.0 为什么 `ImVector` 值得专门一章

`ImVector<T>` 是 ImGui 的"中央数据结构"——`ImGuiContext` 内部直接或间接持有的 `ImVector` **超过 100 个**：

```cpp
// imgui_internal.h:[SECTION] ImGuiContext 内的部分 ImVector
ImVector<ImGuiInputEvent>  InputEventsQueue;
ImVector<ImGuiInputEvent>  InputEventsTrail;
ImVector<ImGuiWindow*>     Windows;
ImVector<ImGuiWindow*>     WindowsFocusOrder;
ImVector<ImGuiWindow*>     WindowsTempSortBuffer;
ImVector<ImGuiWindowStackData> CurrentWindowStack;
// ... 还有几十个
```

`ImDrawList` 内的 `VtxBuffer / IdxBuffer / CmdBuffer` 也是 `ImVector<ImDrawVert> / <ImDrawIdx> / <ImDrawCmd>`——一帧渲染的所有顶点都活在它里面。

**理解 `ImVector` 等于理解 ImGui 的"零分配运行时"机制**——它的几个看似简单的设计决策，让 ImGui 能在每帧重建 UI 的同时几乎不分配内存。

---

## 2.1.1 物理布局：3 个 int + 1 个指针

打开 `imgui.h:2207~2213`：

```cpp
template<typename T>
struct ImVector
{
    int                 Size;
    int                 Capacity;
    T*                  Data;
    // ...
};
```

**就这 3 个字段**。在 64 位机器上 `sizeof(ImVector<T>) = 4 + 4 + 8 = 16` 字节（带 4 字节填充对齐到 8）。

### 字段顺序为什么是 Size / Capacity / Data 而不是 Data / Size / Capacity？

注意**字段顺序**——ImGui 选择把 `int Size; int Capacity;` 放在前面，`T* Data` 放在后面。

字段顺序对二进制布局有实际影响：

```
布局 A (ImGui 选择)：
偏移 0: Size      (4 字节)
偏移 4: Capacity  (4 字节)
偏移 8: Data      (8 字节)
sizeof = 16

布局 B (假设)：
偏移 0: Data      (8 字节)
偏移 8: Size      (4 字节)
偏移 12: Capacity (4 字节)
sizeof = 16
```

两种布局总大小一样，但 ImGui 选择 A 的原因可能是：

1. **小整数常用**：`Size` 是被读得最频繁的字段（`for (int i = 0; i < v.Size; i++)` 模式遍布代码库）。把它放在偏移 0 让某些编译器能省略一次偏移计算。
2. **直觉对齐**：`Size` 在前更接近 `std::vector` 的"长度优先"心智模型。
3. **未指定的对齐细节**：64 位上 `T*` 必须 8 字节对齐。如果 Data 在前，Size/Capacity 紧随其后正好填充。如果 Size/Capacity 在前，Data 在偏移 8（自然对齐）。**两种布局对齐都对**——ImGui 选 A 是风格选择。

**实战意义不大**：无论哪种布局，`sizeof = 16`，性能几乎相同。但你在调试器里 watch `ImVector` 时看到 `[Size, Capacity, Data]` 的顺序，知道这个就行。

### `Data == NULL` 何时发生

ImGui 的"懒分配"策略：

- `ImVector()` 默认构造：`Size = Capacity = 0; Data = NULL;`——**不分配**。
- 第一次 `push_back` / `reserve(n)` 才分配。
- `clear()` 释放 `Data` 并设回 NULL。
- `resize(0)` **不**释放（capacity 保留，给下帧重用）。

这意味着一个全新创建但未使用的 `ImVector<T>` 是"零成本"的——和 3 个 int 一样便宜。

---

## 2.1.2 类的"基础设施"：构造、析构、赋值

```cpp
// imgui.h:2220~2227
inline ImVector()                                       { Size = Capacity = 0; Data = NULL; }
inline ImVector(const ImVector<T>& src)                 { Size = Capacity = 0; Data = NULL; operator=(src); }
inline ImVector<T>& operator=(const ImVector<T>& src)   { clear(); resize(src.Size); if (Data && src.Data) memcpy(Data, src.Data, (size_t)Size * sizeof(T)); return *this; }
inline ~ImVector()                                      { if (Data) IM_FREE(Data); } // Important: does not destruct anything

inline void         clear()                             { if (Data) { Size = Capacity = 0; IM_FREE(Data); Data = NULL; } }  // Important: does not destruct anything
inline void         clear_delete()                      { for (int n = 0; n < Size; n++) IM_DELETE(Data[n]); clear(); }     // Important: never called automatically! always explicit.
inline void         clear_destruct()                    { for (int n = 0; n < Size; n++) Data[n].~T(); clear(); }           // Important: never called automatically! always explicit.
```

### 拷贝构造与赋值

```cpp
inline ImVector<T>& operator=(const ImVector<T>& src) {
    clear();                                            // 释放当前内容
    resize(src.Size);                                    // 分配新空间
    if (Data && src.Data)
        memcpy(Data, src.Data, (size_t)Size * sizeof(T));  // 字节拷贝
    return *this;
}
```

**注意**：用 `memcpy` 而不是循环 `Data[i] = src.Data[i]`。这要求 `T` **trivially copyable**——和我们在 0.3 章讨论的一致。

### 析构：`~ImVector()` 不调用 `~T()`！

```cpp
inline ~ImVector() { if (Data) IM_FREE(Data); }
                                              // ↑ 注意没有遍历调用 Data[i].~T()
```

**这是 ImGui 与 STL 的根本差异之一**。`std::vector::~vector` 会先逐个调 `~T()` 再 free。`ImVector::~ImVector` **只 free 内存，不调析构**。

后果：

- 如果 `T` 是 `int` / `float` / 简单 POD —— 完全 OK，析构是 trivial 的。
- 如果 `T` 持有外部资源（堆指针 / 文件句柄 / 锁）——**资源泄漏**！

ImGui 的设计假设："放进 `ImVector` 的类型**不持有外部资源**"。这是为什么 `ImVector` 不能当 `std::vector` 替代品的根本原因。

### 三个 `clear*` 变体

```cpp
inline void clear()                  { if (Data) { Size = Capacity = 0; IM_FREE(Data); Data = NULL; } }
inline void clear_delete()           { for (int n = 0; n < Size; n++) IM_DELETE(Data[n]); clear(); }
inline void clear_destruct()         { for (int n = 0; n < Size; n++) Data[n].~T(); clear(); }
```

| 函数 | `~T()` 调用 | `IM_FREE(Data[n])` | 释放底层 buffer |
|---|---|---|---|
| `clear()` | ❌ | ❌ | ✅ |
| `clear_delete()` | ✅（通过 IM_DELETE） | ✅（通过 IM_DELETE） | ✅ |
| `clear_destruct()` | ✅ | ❌ | ✅ |

**用法约定**：

- 容器存的是 `T*` 指针、且这些指针的所有权由容器持有 → `clear_delete()`。
- 容器存的是带析构的 `T` 值 → `clear_destruct()`。
- 容器存的是 trivial 类型 → `clear()`。

注释明确说 `// Important: never called automatically!`——这两个函数**永远不会**被自动调用，必须显式手动调。这是为了"零成本析构"——如果你的 `T` 不需要析构，零开销。

---

## 2.1.3 容量管理：`_grow_capacity` 与 `reserve`

```cpp
// imgui.h:2247
inline int _grow_capacity(int sz) const {
    int new_capacity = Capacity ? (Capacity + Capacity / 2) : 8;
    return new_capacity > sz ? new_capacity : sz;
}
```

**这是 ImGui "1.5 倍增长策略"的全部代码**。

### 算法

```
new_capacity = max(Capacity * 1.5, 8, sz)
```

- 如果 `Capacity == 0`：起始扩到 8（避免几次 push_back 都触发扩容）。
- 否则：扩到 `Capacity * 1.5`（用整数乘法 `Capacity + Capacity/2` 避免浮点）。
- 如果 `sz` 比 1.5x 还大（用户一次 push 一个超大数组）：扩到 `sz` 满足需求。

### 1.5 倍 vs 2 倍：经典争论

`std::vector` 在不同 STL 实现上的扩容策略：
- MSVC: 1.5x
- libstdc++ (GCC): 2x
- libc++ (Clang): 2x

ImGui 选择 1.5x，与 MSVC 一致。**理论依据**：

- **2x 增长**：每次扩容后释放的旧块**永远小于**新块。新块**永远不能**复用之前释放的所有旧块的总和（几何级数极限），导致内存碎片。
- **1.5x 增长（黄金比近似）**：经过若干次扩容后，旧块累计可以**等于或超过**下次需要的新块。理论上分配器可以"原地复用"旧块，碎片最小。
- **代价**：1.5x 触发的扩容次数比 2x 多约 70%。`push_back` 的均摊复杂度仍是 O(1)，常数稍大。

实际差距很小。ImGui 选 1.5x **更可能是工程文化原因**（与 MSVC 一致 = 在 Windows 上 debug 体验一致）。

### `reserve` 的实现

```cpp
// imgui.h:2251
inline void reserve(int new_capacity) {
    if (new_capacity <= Capacity) return;
    T* new_data = (T*)IM_ALLOC((size_t)new_capacity * sizeof(T));
    if (Data) {
        memcpy(new_data, Data, (size_t)Size * sizeof(T));
        IM_FREE(Data);
    }
    Data = new_data;
    Capacity = new_capacity;
}
```

**4 步**：

1. `new_capacity <= Capacity` 早退（不缩容）。
2. `IM_ALLOC` 新空间。
3. 旧数据存在则 `memcpy` 过去 + `IM_FREE` 旧块。
4. 更新 `Data` 和 `Capacity`。

**注意 `memcpy` 而非循环赋值**——回到 0.3 章的 trivially relocatable 假设。如果 `T` 含自指针，`memcpy` 后新地址的对象的自指针仍指着旧地址（已被 free）→ 悬挂指针。

### `reserve_discard`：丢弃旧内容

```cpp
// imgui.h:2252
inline void reserve_discard(int new_capacity) {
    if (new_capacity <= Capacity) return;
    if (Data) IM_FREE(Data);
    Data = (T*)IM_ALLOC((size_t)new_capacity * sizeof(T));
    Capacity = new_capacity;
}
```

**与 `reserve` 唯一差异**：不调 `memcpy` 拷贝旧数据。新分配的内存内容**未定义**。

**何时用**：你要 `resize` 然后**完全重新填充**整个数组——旧内容不需要，省一次 `memcpy`。

ImGui 内部某些"重建图集"等场景会用到。

### `resize` 系列

```cpp
// imgui.h:2248~2249
inline void resize(int new_size)                { if (new_size > Capacity) reserve(_grow_capacity(new_size)); Size = new_size; }
inline void resize(int new_size, const T& v)    { if (new_size > Capacity) reserve(_grow_capacity(new_size)); if (new_size > Size) for (int n = Size; n < new_size; n++) memcpy(&Data[n], &v, sizeof(v)); Size = new_size; }
```

两个版本：

**单参 `resize(new_size)`**：
- 如果 `new_size > Capacity` → 扩容。
- 直接设置 `Size = new_size`，**不初始化新元素**。

**这意味着**：

```cpp
ImVector<int> v;
v.resize(10);
// v[0]..v[9] 是 "未初始化"的内存——内容是 IM_ALLOC 给的垃圾数据
```

如果 `T` 是 trivial 类型，"垃圾数据"也是合法的（任何 bit pattern 都是一个合法的 `int` 值，只是值未知）。如果 `T` 是含构造的类型，**这是不安全的**——但 ImGui 不在 `ImVector` 里放含构造的类型（除了 trivially copyable 的）。

**双参 `resize(new_size, v)`**：
- 用 `memcpy` 把 `v` 复制到每个新槽位。
- `T` 必须 trivially copyable。

注意双参版本**不调用 `T::T()`** 也不调用 `T::T(const T&)`。它就是 `memcpy(&Data[n], &v, sizeof(v))`——把 `v` 的字节原样复制 N 次。

### `shrink`：手动缩容（不 free）

```cpp
// imgui.h:2250
inline void shrink(int new_size) {
    IM_ASSERT(new_size <= Size);
    Size = new_size;
}
```

**只改 `Size`，不 `IM_FREE`**——这是 ImGui 的"capacity 跨帧保留"哲学的体现。

典型用法：

```cpp
// 一帧开始
draw_list->VtxBuffer.shrink(0);   // size = 0，但内存保留
                                  // 或更常见的 resize(0)，效果一样

// 累计写入数千个顶点 ...
// （不会触发分配，因为上帧的 capacity 还在）

// 一帧结束 - 数据被 backend 消费
```

**关键**：`shrink(0)` 等价于 `resize(0)`（前者多一个断言）——都是"逻辑清空但不释放内存"。

---

## 2.1.4 元素访问：`operator[]` / `front` / `back`

```cpp
// imgui.h:2234~2244
inline T&       operator[](int i)         { IM_ASSERT(i >= 0 && i < Size); return Data[i]; }
inline const T& operator[](int i) const   { IM_ASSERT(i >= 0 && i < Size); return Data[i]; }
inline T*       begin()                   { return Data; }
inline T*       end()                     { return Data + Size; }
inline T&       front()                   { IM_ASSERT(Size > 0); return Data[0]; }
inline T&       back()                    { IM_ASSERT(Size > 0); return Data[Size - 1]; }
```

**与 `std::vector` 几乎一致**——但有一个关键差异：

### Debug 模式下的 IM_ASSERT 总是开启

`std::vector::operator[]` 在 Release 模式下不做边界检查（C++ 标准也不要求）。MSVC Debug 默认 `_ITERATOR_DEBUG_LEVEL=2` 会查，但慢。

`ImVector::operator[]` 总是有 `IM_ASSERT`——**Debug 与 Release 行为一致**：默认开 assert（除非用户用 `IMGUI_DISABLE_DEBUG_TOOLS` 或自定义 `IM_ASSERT` 禁用）。

**性能含义**：

- Release 构建里如果你保留了默认 IM_ASSERT，每次 `[i]` 都有一次比较 + 分支。在热路径里这可能是性能问题。
- 解决方案：`Data[i]`——直接访问 `Data` 字段绕过 assert。`imgui.cpp` 内部的 hot path 也常这么用。

```cpp
// 慢但安全
for (int i = 0; i < v.Size; i++) Total += v[i];

// 快但需要程序员保证不越界
for (int i = 0; i < v.Size; i++) Total += v.Data[i];

// 最快（指针推进）
const T* end = v.Data + v.Size;
for (const T* p = v.Data; p != end; p++) Total += *p;
```

### `swap` 方法

```cpp
// imgui.h:2245
inline void swap(ImVector<T>& rhs) {
    int   rhs_size = rhs.Size;     rhs.Size     = Size;     Size     = rhs_size;
    int   rhs_cap  = rhs.Capacity; rhs.Capacity = Capacity; Capacity = rhs_cap;
    T*    rhs_data = rhs.Data;     rhs.Data     = Data;     Data     = rhs_data;
}
```

经典的"交换三个字段"。**O(1)，无分配**——比 `std::swap` 还透明。

用途：

- 双缓冲：两个 buffer 互换"消费者"和"生产者"角色。
- 把 vector 的所有权转移给另一个 vector。

例如 `imgui_internal.h:1590` 的 RoutingTable 双缓冲：

```cpp
struct ImGuiKeyRoutingTable {
    ImVector<ImGuiKeyRoutingData> Entries;
    ImVector<ImGuiKeyRoutingData> EntriesNext;   // Double-buffer
};

// 帧切换时：
Entries.swap(EntriesNext);   // O(1)，零分配
EntriesNext.resize(0);
```

---

## 2.1.5 增删元素：`push_back` / `pop_back` / `insert` / `erase`

### `push_back`

```cpp
// imgui.h:2255
inline void push_back(const T& v) {
    if (Size == Capacity) reserve(_grow_capacity(Size + 1));
    memcpy(&Data[Size], &v, sizeof(v));
    Size++;
}
```

**3 步**：

1. 容量不足 → 扩容（1.5x）。
2. `memcpy` 把 `v` 的字节拷到末尾。
3. `Size++`。

**注意 `memcpy(&Data[Size], &v, sizeof(v))`**：

- 不调用 `T(const T&)` 拷贝构造。
- `v` 必须 trivially copyable。
- 这与 `std::vector::push_back(const T&)` 行为不同——后者会调拷贝构造。

**`imgui.h:2254` 的注释**：

```cpp
// NB: It is illegal to call push_back/push_front/insert with a reference pointing inside the ImVector data itself! e.g. v.push_back(v[10]) is forbidden.
```

——你不能 `v.push_back(v[10])`！原因：`reserve` 可能 `IM_FREE(Data)`，参数 `v` 是引用 `Data[10]`——free 之后再 `memcpy` 来源，**use-after-free**。

`std::vector` 处理这个边界情况（先取值再扩容）。`ImVector` 选择**不处理**——你自己保证。

### `pop_back`

```cpp
// imgui.h:2256
inline void pop_back() {
    IM_ASSERT(Size > 0);
    Size--;
}
```

**就这一行有效代码**：`Size--`。**不调用析构，不释放内存**——下帧 push_back 又能复用这个槽位。

### `erase`

```cpp
// imgui.h:2258~2259
inline T* erase(const T* it) {
    IM_ASSERT(it >= Data && it < Data + Size);
    const ptrdiff_t off = it - Data;
    memmove(Data + off, Data + off + 1,
            ((size_t)Size - (size_t)off - 1) * sizeof(T));
    Size--;
    return Data + off;
}

inline T* erase(const T* it, const T* it_last) {
    IM_ASSERT(it >= Data && it < Data + Size && it_last >= it && it_last <= Data + Size);
    const ptrdiff_t count = it_last - it;
    const ptrdiff_t off   = it - Data;
    memmove(Data + off, Data + off + count,
            ((size_t)Size - (size_t)off - (size_t)count) * sizeof(T));
    Size -= (int)count;
    return Data + off;
}
```

**关键操作 `memmove`**——而不是 `memcpy`。

| | `memcpy` | `memmove` |
|---|---|---|
| 重叠区域 | UB | 正确处理 |
| 性能 | 略快 | 稍慢（要判定方向） |

`erase` 删除中间元素后，后面所有元素要**前移**。这是源/目标**重叠**的拷贝，必须用 `memmove`。

### `erase_unsorted`：O(1) 删除不保序

```cpp
// imgui.h:2260
inline T* erase_unsorted(const T* it) {
    IM_ASSERT(it >= Data && it < Data + Size);
    const ptrdiff_t off = it - Data;
    if (it < Data + Size - 1)
        memcpy(Data + off, Data + Size - 1, sizeof(T));
    Size--;
    return Data + off;
}
```

**算法**：把最后一个元素**搬到**被删位置，然后 `Size--`。**O(1) 删除**，但破坏元素顺序。

适用场景：
- 元素顺序无关紧要（例如 ImGui 内部某些"待销毁列表"）。
- 大量删除时性能关键。

### `insert`

```cpp
// imgui.h:2261
inline T* insert(const T* it, const T& v) {
    IM_ASSERT(it >= Data && it <= Data + Size);
    const ptrdiff_t off = it - Data;
    if (Size == Capacity) reserve(_grow_capacity(Size + 1));
    if (off < (int)Size)
        memmove(Data + off + 1, Data + off, ((size_t)Size - (size_t)off) * sizeof(T));
    memcpy(&Data[off], &v, sizeof(v));
    Size++;
    return Data + off;
}
```

**4 步**：

1. 容量不足 → 扩容。
2. 用 `memmove` 把 [off, Size) 区段**后移一格**。
3. 把 `v` 写到 [off]。
4. `Size++`。

**性能**：O(N) 移动元素。**只有在中间插入很罕见的场景才用**——`ImGuiStorage` 的 KV 插入用它（每帧最多插一次）。

### `push_front` 是个"懒人" wrapper

```cpp
// imgui.h:2257
inline void push_front(const T& v) {
    if (Size == 0)
        push_back(v);
    else
        insert(Data, v);
}
```

**通过 `insert(Data, v)` 实现**——也是 O(N)。

`ImVector` **没有 `pop_front`**——因为 O(N) 删除头部不值得，要的话用 `erase(Data)`。

---

## 2.1.6 查找辅助函数

```cpp
// imgui.h:2262~2268
inline bool  contains(const T& v) const             { /* 线性扫描 */ }
inline T*    find(const T& v)                       { /* 返回指针，没找到返回 end() */ }
inline T*    find(const T& v) const                 { /* const 版本 */ }
inline int   find_index(const T& v) const           { /* 返回索引，没找到返回 -1 */ }
inline bool  find_erase(const T& v)                 { /* find + erase，保序 */ }
inline bool  find_erase_unsorted(const T& v)        { /* find + erase_unsorted */ }
inline int   index_from_ptr(const T* it) const      { /* 指针转索引 */ }
```

**全部 O(N) 线性扫描**——用 `==` 操作符。

**`T` 必须支持 `==`**：原始类型、指针、`ImGuiID`（typedef of `unsigned int`）都支持。复杂结构体如果想被 find，需要重载 `operator==`。

**为什么不提供 `find_if` / `lambda` 版本**：ImGui 不依赖 `std::function`/lambda（参见 0.2 章）。你需要复杂查找时，写普通 `for` 循环。

---

## 2.1.7 与 `std::vector` 的 5 个根本差异

回顾一下，把所有差异整合：

### 差异 1：不调用构造/析构

| 操作 | `std::vector` | `ImVector` |
|---|---|---|
| 默认构造容器 | 立即 lazy（不分配） | 不分配 |
| `push_back(v)` | 调 `T::T(const T&)` | `memcpy(&dst, &v, sizeof)` |
| `resize(n)` | 调 `T::T()` 初始化新元素 | **不初始化**新元素 |
| `clear()` | 调每个 `T::~T()` | **不调**析构 |
| `erase(it)` | 调被删元素的 `T::~T()` + 后续元素移动赋值 | `memmove` 后续元素 |
| `~vector()` | 调每个 `T::~T()` + 释放 buffer | **只释放 buffer** |

### 差异 2：`memcpy` 而非赋值

`ImVector` 假设 `T` **trivially relocatable**——所有移动用 `memcpy/memmove` 字节级操作。

### 差异 3：`grow_capacity` 1.5x

`ImVector` 用整数算 1.5x。MSVC 的 `std::vector` 也是 1.5x，但用更复杂的策略（兼顾对齐等）。GCC/Clang 是 2x。

### 差异 4：ABI 跨编译器一致

```
sizeof(ImVector<T>) = 16 字节（64 位）
布局：{ int Size; int Capacity; T* Data; }
```

——任何编译器、任何 STL 版本编译，布局完全一致。所以 ImGui 公开 API 可以用 `ImVector<ImDrawCmd>` 而无 ABI 顾虑。

`std::vector` 的内部布局因 STL 实现而异，**不能跨 STL 安全**。

### 差异 5：Debug 模式不慢

`std::vector` 在 MSVC Debug 默认开启 `_ITERATOR_DEBUG_LEVEL=2`——`operator[]` 检查、迭代器有效性检查、安全 SCL 检查。在每帧扫几万顶点的场景下能让 Debug 帧率从 60 跌到 10。

`ImVector` 在 Debug 和 Release 都只有一个 `IM_ASSERT`。Debug 速度可接受。

---

## 2.1.8 `ImSpan<T>`：只读视图（非所有者）

打开 `imgui_internal.h:670~697`：

```cpp
template<typename T>
struct ImSpan
{
    T*  Data;
    T*  DataEnd;

    inline ImSpan()                                 { Data = DataEnd = NULL; }
    inline ImSpan(T* data, int size)                { Data = data; DataEnd = data + size; }
    inline ImSpan(T* data, T* data_end)             { Data = data; DataEnd = data_end; }

    inline void  set(T* data, int size)             { Data = data; DataEnd = data + size; }
    inline int   size() const                       { return (int)(ptrdiff_t)(DataEnd - Data); }
    inline int   size_in_bytes() const              { return size() * (int)sizeof(T); }
    inline T&    operator[](int i)                  { /* assert + return Data[i] */ }
    inline T*    begin()                            { return Data; }
    inline T*    end()                              { return DataEnd; }
    inline int   index_from_ptr(const T* it) const  { /* it - Data */ }
};
```

### `ImSpan` 是什么

**`ImSpan` = 仅 ptr+size 视图，不持有所有权**。

- `sizeof(ImSpan<T>) = 16` 字节（两个指针）。
- 用 `Data` 和 `DataEnd` 而不是 `Data + Size`——计算 size 是 O(1)（指针相减），但访问 `end()` 是字段读取（avoid 加法）。
- 没有 `push_back / resize / reserve`——它**不**管理内存。

### `ImSpan` vs `ImVector`

| | `ImVector<T>` | `ImSpan<T>` |
|---|---|---|
| 所有权 | 持有 / 析构释放 | 不持有 |
| sizeof | 16 字节 | 16 字节 |
| 字段 | Size, Capacity, Data | Data, DataEnd |
| 修改 | 可 | 不可 |
| 跨函数传递 | 拷贝整个 vector（含数据）or 引用 | 仅传 16 字节 |

### `ImSpan` 与 C++20 `std::span`

如果你熟悉 C++20，`ImSpan` 几乎就是 `std::span<T>` 的复刻。ImGui 不依赖 C++20，所以自己造了一个。

### `ImSpan` 的典型用法

```cpp
// 一个函数想"遍历但不修改"某段数据：
void DrawSomething(ImSpan<const ImDrawVert> vertices) {
    for (const ImDrawVert& v : vertices) { /* ... */ }
}

// 调用方传 ImVector
ImVector<ImDrawVert> verts;
DrawSomething(ImSpan<const ImDrawVert>(verts.Data, verts.Size));
// 或者用 ImSpanAllocator 切出来的子区域
DrawSomething(arena_span);
```

---

## 2.1.9 `IM_MSVC_RUNTIME_CHECKS_OFF` / `_RESTORE`

```cpp
// imgui.h:2206
IM_MSVC_RUNTIME_CHECKS_OFF
template<typename T>
struct ImVector { /* ... */ };
IM_MSVC_RUNTIME_CHECKS_RESTORE
```

这两个宏在 `imgui.h` 顶部的辅助段定义：

```cpp
// 简化伪代码
#if defined(_MSC_VER)
#define IM_MSVC_RUNTIME_CHECKS_OFF      \
    __pragma(runtime_checks("", off))    \
    __pragma(check_stack(off))            \
    __pragma(component(inliner, on))
#define IM_MSVC_RUNTIME_CHECKS_RESTORE   \
    __pragma(runtime_checks("", restore))\
    __pragma(check_stack())               \
    __pragma(component(inliner, on))
#else
#define IM_MSVC_RUNTIME_CHECKS_OFF
#define IM_MSVC_RUNTIME_CHECKS_RESTORE
#endif
```

**作用**：关闭 MSVC 在 Debug 模式下注入的"运行时检查"代码：

- `runtime_checks` 在每个变量赋值前/后插入"未初始化变量检查"。
- `check_stack` 在每个函数入口/出口插入"栈溢出检查"。
- `component(inliner, on)` 强制开启内联（即使 Debug 模式）。

对 `ImVector<T>` 这种**每个成员函数都极小且会被频繁调用**的类，关闭这些检查能让 Debug 性能接近 Release。

**注意只在 ImVector 周围关掉**：因为关闭这些检查也意味着**失去某些 bug 检测**。对应用代码，让 MSVC 默认开启更安全。

---

## 2.1.10 性能特性与基准

### `push_back` 均摊复杂度

100,000 次 `push_back`（以 1.5x 增长）：

```
扩容点：8 → 12 → 18 → 27 → 40 → 60 → 90 → 135 → 202 → ...
扩容 N 次后 capacity 大约 8 * 1.5^N
要达到 100,000 → N ≈ log_1.5(100000/8) ≈ 23 次扩容
```

每次扩容拷贝当前 N 个元素。总拷贝量 ≈ 8 + 12 + 18 + ... ≈ `8 * 1.5^N / (1.5-1) ≈ 200,000`。

**100,000 次 push 总计 200,000 次"拷贝操作"——均摊每次 push 2 次拷贝**。

均摊 O(1)。但常数比理论上"提前 reserve"大 2 倍。

### 热路径优化建议

如果你**事先知道**最终 size：

```cpp
// 慢
ImVector<int> v;
for (int i = 0; i < n; i++) v.push_back(i);   // ~ 23 次扩容

// 快（提前 reserve）
ImVector<int> v;
v.reserve(n);   // 一次分配
for (int i = 0; i < n; i++) v.push_back(i);   // 0 次扩容
```

### Cache 友好性

`ImVector` 数据**连续存储**——CPU prefetcher 喜欢这种线性访问模式。

但 `ImVector<T*>`（指针数组）的元素**指向各处**——访问 `*Data[i]` 时仍可能 cache miss。

ImGui 内部偏好"值数组"而非"指针数组"——例如 `ImDrawList::VtxBuffer` 是 `ImVector<ImDrawVert>` 而不是 `ImVector<ImDrawVert*>`。

---

## 2.1.11 实战：能否放进 `ImVector` 的判定（升级版）

第 0.3 章给过 7 条 checklist，这里**配合 `ImVector` 源码**给出更精确的判断：

| 操作 | 对 `T` 的最低要求 |
|---|---|
| `ImVector<T> v;` 默认构造 | 无 |
| `v.push_back(x)` | `T` trivially copyable |
| `v.push_back(x)` 触发扩容 | `T` 还要 trivially relocatable |
| `v.resize(n)` 不带初值 | `T` 任意（新元素未初始化） |
| `v.resize(n, init_v)` | `T` trivially copyable |
| `v.erase(it)` | `T` trivially relocatable |
| `v.erase_unsorted(it)` | `T` trivially copyable |
| `v.clear()` | 无（不调析构）—— 但 `T` 不能持有 RAII 资源 |
| `v.clear_destruct()` | 必须支持 `~T()` |
| `~v()` 析构 | 同 `clear()` |

### 实战例子

**OK 的类型**：

```cpp
struct OK1 {
    int x, y;
};   // POD，全部操作 OK

struct OK2 {
    int x;
    OK2() { x = 0; }   // 自定义默认构造
    OK2(const OK2&) = default;   // 拷贝构造 trivial
};   // trivially copyable，全部操作 OK

struct OK3 {
    ImGuiWindow* parent;   // 裸指针
    ImGuiID id;
    int frame;
};   // OK，没有所有权语义
```

**不 OK 的类型**：

```cpp
struct Bad1 {
    int x;
    Bad1(const Bad1& other) { x = other.x * 2; }   // 自定义拷贝构造
};
// 不 OK：push_back 会 memcpy，绕过你的拷贝构造，行为不一致

struct Bad2 {
    std::vector<int> data;
};
// 不 OK：std::vector 含指针，memcpy 后两个 ImVector 元素共享同一个 std::vector::_M_start
//        析构时双 free → 崩溃

struct Bad3 {
    int x;
    Bad3* self = this;   // 自指针
};
// 不 OK：trivially relocatable 失败，memcpy 后 self 还指向旧地址

struct Bad4 {
    std::shared_ptr<int> p;
};
// 不 OK：shared_ptr 内部有控制块指针 + 引用计数，memcpy 后引用计数错乱
```

---

## 2.1.12 一个有意思的"边角"：`max_size`

```cpp
// imgui.h:2232
inline int max_size() const { return 0x7FFFFFFF / (int)sizeof(T); }
```

**返回 `INT32_MAX / sizeof(T)`**——`int` 类型 `Size / Capacity` 的上限。

`std::vector` 的 `max_size` 通常是 `SIZE_MAX / sizeof(T)`（64 位 ≈ 2^60）。

`ImVector` 用 `int` 存 size 是有意为之：

- 节省 4 字节（与 `size_t` 比）。
- 跨 32/64 位平台 ABI 一致。
- 索引时不需要 cast。
- 99.999% 的 ImGui 容器永远不会超过 2^31 元素。

代价：**理论上不能存超过 21 亿个 `int`**（约 8 GB 数据）。在 ImGui 的语境下完全够用。

---

## 2.1.13 实战：监控 `ImVector` 行为

ImGui 提供调试工具看运行时容器使用：

### Metrics 窗口的 "DrawLists"

```
Demo → Metrics → DrawLists → 选某个 DrawList → 看 Vtx/Idx/Cmd buffer
```

每个 DrawList 显示三个 `ImVector` 的 Size。如果你怀疑某个 ImGui 窗口顶点失控，这里能看到。

### 自己加 telemetry

```cpp
// 在你的 OnImGuiRender 末尾：
ImGuiContext& g = *ImGui::GetCurrentContext();
HZ_CORE_TRACE("ImGui Window count: {}, Total events queued: {}",
    g.Windows.Size,
    g.InputEventsQueue.Size);
```

不需要 ImGui 公开 API——`g.XXX` 直接访问 internal 字段。

---

## 2.1.14 本章小结

`ImVector<T>` 的核心设计决策：

1. **3 字段紧凑布局**：`int Size; int Capacity; T* Data;`。
2. **不调用构造/析构**：所有元素操作走 `memcpy/memmove`。
3. **1.5x 增长**：`Capacity ? Capacity * 1.5 : 8`。
4. **`clear() / resize(0)` 不释放**：内存跨帧保留，给"逐帧重建 UI"零分配。
5. **支持的 T 类型**：必须 trivially copyable + relocatable（不持有 RAII 资源、无自指针）。
6. **`ImSpan<T>` 是 ptr+size 只读视图**：用于跨函数传递不持有所有权的数据。
7. **MSVC Debug 性能优化**：用 pragma 关闭运行时检查。

这套设计的**根本目的**：

- 让 ImGui 能在每帧重建 UI 但运行时近乎零分配。
- 让 ImGui 公开 API 能跨编译器跨 STL 安全使用。
- 让 ImGui 在 Debug 构建里仍然快。

**代价**：

- `T` 类型有严格限制（不能放任何带 RAII / std:: 容器的类型）。
- 程序员必须自己保证类型符合假设——编译器不会强制（注意官方 disable 了 `-Wclass-memaccess` 警告）。

---

## 2.1.15 下一章预告

第 2.2 章 [[09_第二部分_02_ImPool与ImChunkStream]] 会带你看在 `ImVector` 之上构建的**更复杂容器**：

- `ImPool<T>`：基于 ID 的对象池，用 `ImVector<T>` + `ImGuiStorage`（ID→index 哈希表）+ 空闲槽位链表，实现 O(1) ID 查找 + 槽位复用。
- `ImChunkStream<T>`：变长记录顺序流，给 `.ini` 序列化用。
- `ImBitArray<N>` / `ImBitVector`：位图。
- `ImSpanAllocator<CHUNKS>`：编译期 N 段的 arena，让一次大分配能切成多个对齐段。
- `ImStableVector<T, BLOCKSIZE>`：块状分配，对象指针不会因扩容失效。
- `ImGuiStorage`：sorted vector + 二分查找 KV 映射，用作 `ImPool` 的索引。

每个容器都基于 `ImVector` 构建，但解决不同的"专门化场景"。理解它们等于理解 ImGui 内部所有"复杂状态"的存储方式。
