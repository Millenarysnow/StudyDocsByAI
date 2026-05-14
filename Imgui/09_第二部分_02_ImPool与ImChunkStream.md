# 第 2.2 章 · `ImPool<T>` / `ImChunkStream<T>` / `ImBitArray` 等工程化容器

> **本章目标**：把 ImGui 在 `ImVector` 之上构建的**全部专门化容器**逐一剖完——每个容器解决一个具体的工程问题。读完后你应该能：(1) 看到 `ImGuiTabBars` / `ImGuiTables` 是 `ImPool<>` 时立刻知道它的 ID 索引、槽位复用如何运作；(2) 看到 `.ini` 解析时立刻知道 `ImChunkStream` 在做什么；(3) 自己写引擎工具时能借鉴这些模式。
>
> **本章对应源码**：`imgui_internal.h:618~668`（`ImBitArray` / `ImBitVector` 与位运算助手）、`imgui_internal.h:670~697`（`ImSpan`）、`imgui_internal.h:699~719`（`ImSpanAllocator`）、`imgui_internal.h:721~752`（`ImStableVector`）、`imgui_internal.h:754~785`（`ImPool`）、`imgui_internal.h:787~808`（`ImChunkStream`）、`imgui.h:2776~2828`（`ImGuiStoragePair` / `ImGuiStorage`）、`imgui.cpp:2876~2980`（`ImGuiStorage` 实现 + `ImLowerBound`）。
>
> **前置阅读**：第 2.1 章（`ImVector<T>`）、第 0.3 章（内存模型 / placement new）。

---

## 2.2.0 一张工程化容器全景图

```
┌──────────────────────────────────────────────────────────┐
│                     ImVector<T>                            │
│                  （第 2.1 章的基石）                         │
└──────────────────────────────────────────────────────────┘
            ▲                ▲                ▲
            │                │                │
┌─────────────────┐  ┌──────────────┐  ┌──────────────┐
│ ImChunkStream   │  │ ImBitVector  │  │ ImSpan       │
│  变长记录流      │  │  动态位图     │  │  ptr+size    │
│  (用作 .ini)    │  │              │  │  视图（不持有） │
└─────────────────┘  └──────────────┘  └──────────────┘

┌─────────────────┐  ┌──────────────┐  ┌──────────────┐
│ ImPool<T>       │  │ ImBitArray   │  │ ImSpanAlloc  │
│  ID 索引 + 槽位   │  │  静态位图     │  │  Arena 切片器 │
│  复用对象池       │  │              │  │              │
│   ▲              │  └──────────────┘  └──────────────┘
│   │              │
│   依赖 ImGuiStorage
│   │              │
│   依赖 ImVector  │
└──────┬──────────┘

┌──────▼─────────┐
│ ImGuiStorage    │
│  ID→Value 映射  │
│  排序数组+二分   │
└─────────────────┘

┌─────────────────┐  ┌──────────────┐
│ ImStableVector  │  │ ImGuiText*   │
│  块状分配，地址  │  │  Buffer/Index │
│  稳定（不重定位） │  │  字符串助手    │
└─────────────────┘  └──────────────┘
```

每个容器都解决一个 **`ImVector` 不擅长**的特定问题。我们逐个看。

---

## 2.2.1 位运算助手：`ImBitArray` 系列

### 单比特操作的 4 个核心函数

`imgui_internal.h:621~625` 给出 4 个操作裸 `ImU32` 数组的函数：

```cpp
inline size_t   ImBitArrayGetStorageSizeInBytes(int bitcount)   { return (size_t)((bitcount + 31) >> 5) << 2; }
inline void     ImBitArrayClearAllBits(ImU32* arr, int bitcount){ memset(arr, 0, ImBitArrayGetStorageSizeInBytes(bitcount)); }
inline bool     ImBitArrayTestBit(const ImU32* arr, int n)      { ImU32 mask = (ImU32)1 << (n & 31); return (arr[n >> 5] & mask) != 0; }
inline void     ImBitArrayClearBit(ImU32* arr, int n)           { ImU32 mask = (ImU32)1 << (n & 31); arr[n >> 5] &= ~mask; }
inline void     ImBitArraySetBit(ImU32* arr, int n)             { ImU32 mask = (ImU32)1 << (n & 31); arr[n >> 5] |= mask; }
```

### 公式拆解

**`(bitcount + 31) >> 5) << 2`** 翻译：

```
(bitcount + 31) / 32 * 4
= ceil(bitcount / 32) * 4
= 装 bitcount 个 bit 至少需要多少字节
```

`ceil` 用 `+31` 后右移实现——避免浮点。乘以 4 是因为每个 `ImU32` = 4 字节。

**位访问**：

```
arr[n >> 5]   == arr[n / 32]    // 第 n 个 bit 所在的 ImU32 索引
n & 31        == n % 32         // 在 ImU32 内的 bit 位置
1 << (n & 31)                   // 对应的位掩码
```

`>> 5` 比 `/ 32` 在某些编译器/平台快——但现代编译器都会自动优化无符号除法为右移。这里写右移是**风格**（与位运算保持一致）。

### 还有一个"宏版"

```cpp
// imgui_internal.h:619~620
#define IM_BITARRAY_TESTBIT(_ARRAY, _N)   ((_ARRAY[(_N) >> 5] & ((ImU32)1 << ((_N) & 31))) != 0)
#define IM_BITARRAY_CLEARBIT(_ARRAY, _N)  ((_ARRAY[(_N) >> 5] &= ~((ImU32)1 << ((_N) & 31))))
```

注释说：
> `// Macro version of ImBitArrayTestBit(): ensure args have side-effect or are costly!`

**为什么要宏版**：宏在编译期展开，无函数调用开销。在**极热**循环里（每帧扫几万个 bit）有微小性能优势。但宏版本要求参数没有副作用——`IM_BITARRAY_TESTBIT(arr, i++)` 会让 `i` 加 1 两次（因为 `_N` 在宏里出现了两次）。

inline 函数版本避免这个陷阱，但理论上有更大的函数调用开销。**实际上现代编译器都会内联**。优先用 inline 函数版本。

### 范围置位：`ImBitArraySetBitRange`

```cpp
// imgui_internal.h:626~637
inline void ImBitArraySetBitRange(ImU32* arr, int n, int n2)
{
    n2--;
    while (n <= n2)
    {
        int a_mod = (n & 31);
        int b_mod = (n2 > (n | 31) ? 31 : (n2 & 31)) + 1;
        ImU32 mask = (ImU32)(((ImU64)1 << b_mod) - 1) & ~(ImU32)(((ImU64)1 << a_mod) - 1);
        arr[n >> 5] |= mask;
        n = (n + 32) & ~31;
    }
}
```

把 `[n, n2)` 区间内的所有 bit 置 1。

**精彩之处**：**逐 ImU32 处理**——每次循环处理一个 32-bit 段，不是逐 bit。

让我们走一遍 `SetBitRange(arr, 5, 50)`：

```
迭代 1:
  n=5, n2=49
  a_mod = 5 & 31 = 5
  n | 31 = 5 | 31 = 31，n2(49) > 31 → b_mod = 31 + 1 = 32
  mask = ((1ULL << 32) - 1) & ~((1ULL << 5) - 1)
       = 0xFFFFFFFF & ~0x0000001F
       = 0xFFFFFFE0   （bit 5..31 置 1）
  arr[0] |= 0xFFFFFFE0
  n = (5 + 32) & ~31 = 37 & ~31 = 32

迭代 2:
  n=32, n2=49
  a_mod = 32 & 31 = 0
  n | 31 = 32 | 31 = 63，n2(49) <= 63 → b_mod = (49 & 31) + 1 = 17 + 1 = 18
  mask = ((1ULL << 18) - 1) & ~((1ULL << 0) - 1)
       = 0x0003FFFF & 0xFFFFFFFF
       = 0x0003FFFF   （bit 0..17 置 1）
  arr[1] |= 0x0003FFFF
  n = (32 + 32) & ~31 = 64

n(64) > n2(49) → 退出
```

**O(N/32) 而不是 O(N)**——比逐 bit 快 32 倍。

适用场景：标记一连串项目"已访问"——例如 `ListClipper` 标记可见行范围。

---

### `ImBitArray<BITCOUNT, OFFSET>`：编译期固定大小的位图

```cpp
// imgui_internal.h:643~655
template<int BITCOUNT, int OFFSET = 0>
struct ImBitArray
{
    ImU32 Data[(BITCOUNT + 31) >> 5];

    ImBitArray()                                { ClearAllBits(); }
    void ClearAllBits()                         { memset(Data, 0, sizeof(Data)); }
    void SetAllBits()                           { memset(Data, 255, sizeof(Data)); }
    bool TestBit(int n) const                   { n += OFFSET; IM_ASSERT(n >= 0 && n < BITCOUNT); return IM_BITARRAY_TESTBIT(Data, n); }
    void SetBit(int n)                          { n += OFFSET; ... }
    void ClearBit(int n)                        { n += OFFSET; ... }
    void SetBitRange(int n, int n2)             { ... }
    bool operator[](int n) const                { ... }
};
```

### 两个模板参数

- **`BITCOUNT`**：bit 总数，编译期常量。
- **`OFFSET = 0`**：访问时给索引加的偏移。

`Data[(BITCOUNT + 31) >> 5]` —— 编译期算出需要多少个 `ImU32`。**整个对象大小在编译期确定**：

```cpp
sizeof(ImBitArray<32, 0>)   = 4   字节  (1 ImU32)
sizeof(ImBitArray<64, 0>)   = 8   字节  (2 ImU32)
sizeof(ImBitArray<150, 0>)  = 20  字节  (5 ImU32)
sizeof(ImBitArray<1000, 0>) = 128 字节  (32 ImU32)
```

### `OFFSET` 的妙用

**ImGui 的典型用法**（`imgui_internal.h:1491`）：

```cpp
typedef ImBitArray<ImGuiKey_NamedKey_COUNT, -ImGuiKey_NamedKey_BEGIN>  ImBitArrayForNamedKeys;
```

`ImGuiKey_NamedKey_BEGIN = 512`，`ImGuiKey_NamedKey_COUNT ≈ 154`。如果不用 OFFSET，直接 `ImBitArray<668>`，要从 bit 512 开始用——前面 512 个 bit 浪费。

用 `OFFSET = -512`：访问 `TestBit(ImGuiKey_Tab)`（值是 512）时，内部 `n + OFFSET = 0`——映射到 bit 0。`Data[]` 数组只需要存 `(154 + 31) >> 5 = 5` 个 `ImU32` = 20 字节。

**省了 64 字节**。在每个 ImGuiKey 都有这种位图的场景下（输入路由表 / 焦点设置 / Owner），累计节省可观。

### 何时用 `ImBitArray`

- 编译期已知 bit 数（< 几千）。
- 需要快速 set/test 大量 bit。
- 不需要动态扩容。

例如 `imgui.cpp:10438`：

```cpp
ImBitArray<ImGuiKey_NamedKey_COUNT> key_changed_mask;   // 局部变量
```

`UpdateInputEvents` 里用它跟踪"这一帧已变化的键"——20 字节栈分配，零开销。

---

### `ImBitVector`：动态位图

```cpp
// imgui_internal.h:659~667
struct IMGUI_API ImBitVector
{
    ImVector<ImU32> Storage;
    void Create(int sz)         { Storage.resize((sz + 31) >> 5); memset(Storage.Data, 0, ...); }
    void Clear()                { Storage.clear(); }
    bool TestBit(int n) const   { /* assert + IM_BITARRAY_TESTBIT */ }
    void SetBit(int n)          { /* ... */ }
    void ClearBit(int n)        { /* ... */ }
};
```

**`ImBitVector` = `ImVector<ImU32>` + 位运算助手**。

与 `ImBitArray` 相比：

| | `ImBitArray<N>` | `ImBitVector` |
|---|---|---|
| 大小 | 编译期固定 | 运行时动态 |
| 内存位置 | 栈 / 内嵌结构体 | 堆 |
| sizeof | `(N+31)/32 * 4` 字节 | 16 字节（ImVector 头） |
| 初始化 | 立即 | 调 `Create(sz)` 才分配 |

何时用 `ImBitVector`：

- bit 数运行时才知道。
- 数量很大（上千以上）—— `ImBitArray` 内嵌会让结构体变胖。

---

## 2.2.2 `ImSpan<T>` 与 `ImSpanAllocator<CHUNKS>`

`ImSpan<T>` 已在 2.1 章讲过——这里讲它的"配套设计模式"。

### `ImSpanAllocator<CHUNKS>`：单次分配切多段

```cpp
// imgui_internal.h:702~719
template<int CHUNKS>
struct ImSpanAllocator
{
    char*   BasePtr;
    int     CurrOff;
    int     CurrIdx;
    int     Offsets[CHUNKS];
    int     Sizes[CHUNKS];

    ImSpanAllocator()                               { memset((void*)this, 0, sizeof(*this)); }
    inline void  Reserve(int n, size_t sz, int a=4) {
        IM_ASSERT(n == CurrIdx && n < CHUNKS);
        CurrOff = IM_MEMALIGN(CurrOff, a);
        Offsets[n] = CurrOff;
        Sizes[n] = (int)sz;
        CurrIdx++;
        CurrOff += (int)sz;
    }
    inline int   GetArenaSizeInBytes()              { return CurrOff; }
    inline void  SetArenaBasePtr(void* base_ptr)    { BasePtr = (char*)base_ptr; }
    inline void* GetSpanPtrBegin(int n)             { ... return (void*)(BasePtr + Offsets[n]); }
    inline void* GetSpanPtrEnd(int n)               { ... return (void*)(BasePtr + Offsets[n] + Sizes[n]); }
    template<typename T>
    inline void  GetSpan(int n, ImSpan<T>* span)    { span->set(...); }
};
```

### 设计意图

**问题**：你的复杂数据结构有 N 个相关数组，每个长度运行时才知道。一次性分配 N 块是 N 次 `malloc`——慢且碎片化。

**解决**：先用 `ImSpanAllocator` 在**栈上**记录每段需要的大小 + 对齐 + 偏移；最后一次性 `IM_ALLOC` 总大小，再切成 N 个 `ImSpan`。

### 工作流程

```cpp
// 1. 构造 allocator
ImSpanAllocator<3> alloc;

// 2. Reserve 每段
alloc.Reserve(0, sizeof(ImGuiTableColumn) * column_count);   // 段 0
alloc.Reserve(1, sizeof(int) * column_count, 4);              // 段 1，对齐 4
alloc.Reserve(2, sizeof(ImGuiTableInstanceData));             // 段 2

// 3. 一次性分配总空间
char* arena = (char*)IM_ALLOC(alloc.GetArenaSizeInBytes());
alloc.SetArenaBasePtr(arena);

// 4. 取 ImSpan
ImSpan<ImGuiTableColumn> columns;
ImSpan<int>              display_order;
ImSpan<ImGuiTableInstanceData> instances;
alloc.GetSpan(0, &columns);
alloc.GetSpan(1, &display_order);
alloc.GetSpan(2, &instances);

// 5. 用 columns / display_order / instances（它们都是 arena 内的视图）

// 6. 释放：只需要 free arena
IM_FREE(arena);
```

**这正是 `ImGuiTable` 的 RawData 用法**。`imgui.cpp` 中查找 `RawData` 关键词能看到它的实际使用。

### 优势

1. **一次分配代替 N 次**：减少 malloc 开销 + 减少内存碎片。
2. **数据连续**：N 个段在内存中相邻，cache 友好。
3. **统一释放**：只需要一次 `IM_FREE`。
4. **对齐控制**：每段可以指定自己的对齐要求（`Reserve` 第三个参数）。

### `IM_MEMALIGN` 宏

```cpp
// imgui_internal.h（节选）
#define IM_MEMALIGN(_OFF,_ALIGN)    (((_OFF) + ((_ALIGN) - 1)) & ~((_ALIGN) - 1))
```

把 `_OFF` 向上取整到 `_ALIGN` 的倍数。`Reserve` 在每段开始前先对齐：

```cpp
CurrOff = IM_MEMALIGN(CurrOff, a);   // 把当前偏移向上对齐
Offsets[n] = CurrOff;                 // 这一段从对齐后位置开始
```

---

## 2.2.3 `ImStableVector<T, BLOCKSIZE>`：地址稳定的块状向量

打开 `imgui_internal.h:725~752`：

```cpp
template<typename T, int BLOCKSIZE>
struct ImStableVector
{
    int                 Size = 0;
    int                 Capacity = 0;
    ImVector<T*>        Blocks;

    inline ~ImStableVector()                        { for (T* block : Blocks) IM_FREE(block); }

    inline void clear()                             { Size = Capacity = 0; Blocks.clear_delete(); }
    inline void resize(int new_size)                { if (new_size > Capacity) reserve(new_size); Size = new_size; }
    inline void reserve(int new_cap)
    {
        new_cap = IM_MEMALIGN(new_cap, BLOCKSIZE);
        int old_count = Capacity / BLOCKSIZE;
        int new_count = new_cap / BLOCKSIZE;
        if (new_count <= old_count) return;
        Blocks.resize(new_count);
        for (int n = old_count; n < new_count; n++)
            Blocks[n] = (T*)IM_ALLOC(sizeof(T) * BLOCKSIZE);
        Capacity = new_cap;
    }
    inline T& operator[](int i)        { IM_ASSERT(i >= 0 && i < Size); return Blocks[i / BLOCKSIZE][i % BLOCKSIZE]; }
    inline T* push_back(const T& v)    { ... void* ptr = &Blocks[i / BLOCKSIZE][i % BLOCKSIZE]; memcpy(ptr, &v, sizeof(v)); Size++; return (T*)ptr; }
};
```

### 与 `ImVector` 的根本差异

| | `ImVector<T>` | `ImStableVector<T, B>` |
|---|---|---|
| 内存布局 | 单一连续数组 | N 个固定大小的 block，每个 block `BLOCKSIZE` 个 T |
| 扩容 | `realloc + memcpy` 整个数组 | 加一个新 block，旧 block 不动 |
| 元素地址 | **会随扩容变化** | **永远不变** |
| 访问 | `Data[i]` | `Blocks[i / B][i % B]`——两次间接 |

### "地址稳定"的意义

```cpp
ImVector<MyObj> v;
v.push_back(MyObj{1});
MyObj* p = &v[0];

v.reserve(10000);   // 触发扩容 → memcpy 到新地址
// 此时 p 是悬挂指针！
```

`ImStableVector` 解决这个问题：

```cpp
ImStableVector<MyObj, 256> sv;
sv.push_back(MyObj{1});
MyObj* p = &sv[0];

sv.resize(10000);   // 加 39 个新 block，原来的 block 不动
// p 仍然有效！
```

### 代价：访问慢一点

`Blocks[i / B][i % B]`——两次内存访问（一次取 block 指针，一次取元素）。**对 cache 不太友好**。

适用场景：**地址稳定性比访问速度更重要**——例如某些 ImGui 内部把对象指针存在外部数据结构里跨帧持有。

ImGui 实际使用 `ImStableVector` 的地方很少——绝大多数容器允许地址变化。但为某些特殊数据结构（例如 1.92 引入的某些字体相关存储）保留这个工具。

---

## 2.2.4 `ImPool<T>`：基于 ID 的对象池

打开 `imgui_internal.h:758~785`，这是**整个 ImGui 内部状态管理的核心容器**。

### 数据结构

```cpp
typedef int ImPoolIdx;

template<typename T>
struct ImPool
{
    ImVector<T>     Buf;        // 实际对象数组
    ImGuiStorage    Map;        // ID → index 哈希表
    ImPoolIdx       FreeIdx;    // 空闲槽位链表的头
    ImPoolIdx       AliveCount; // 活对象数

    ImPool()    { FreeIdx = AliveCount = 0; }
    ~ImPool()   { Clear(); }
    // ... 方法
};
```

**4 个字段**：

- `Buf`：对象的连续存储——这是 `ImVector<T>`，所以 `T` 仍然要 trivially relocatable。
- `Map`：ID → index 的映射，`ImGuiStorage` 是 sorted vector + 二分查找。
- `FreeIdx`：空闲槽位链表头部。
- `AliveCount`：用于显示统计。

### `Add()`：分配新对象

```cpp
// imgui_internal.h:774
T* Add() {
    int idx = FreeIdx;
    if (idx == Buf.Size) {
        Buf.resize(Buf.Size + 1);
        FreeIdx++;
    } else {
        FreeIdx = *(int*)&Buf[idx];      // ★ 从槽位的字节里读"下一个空闲索引"
    }
    IM_PLACEMENT_NEW(&Buf[idx]) T();      // 在槽位上构造对象
    AliveCount++;
    return &Buf[idx];
}
```

**关键 trick**：当槽位"空闲"时，它的字节**被复用作为链表节点的 `next` 指针**（实际上是 next index）。这是经典的**侵入式空闲链表**——空槽位的内存就是链表节点本身，零额外存储。

### `Remove()`：释放对象

```cpp
// imgui_internal.h:776
void Remove(ImGuiID key, ImPoolIdx idx) {
    Buf[idx].~T();                       // 显式析构
    *(int*)&Buf[idx] = FreeIdx;          // 槽位写入"链表上一个头"
    FreeIdx = idx;                       // 链表头改为这个槽位
    Map.SetInt(key, -1);                 // Map 中标记为"无效"
    AliveCount--;
}
```

### 流程演示

```
状态 0：Buf=[], Map={}, FreeIdx=0

Add(): idx=FreeIdx=0
  Buf.Size(0)==idx(0)，所以 resize(1) → Buf=[uninit], FreeIdx=1
  PLACEMENT_NEW(&Buf[0]) T()
  → Buf=[T0], FreeIdx=1, AliveCount=1
  返回 &T0

Add(): idx=FreeIdx=1
  Buf.Size(1)==idx(1)，所以 resize(2) → Buf=[T0, uninit], FreeIdx=2
  PLACEMENT_NEW(&Buf[1]) T()
  → Buf=[T0, T1], FreeIdx=2, AliveCount=2

Remove(key0, 0):
  Buf[0].~T()                       ← 析构
  *(int*)&Buf[0] = 2 (= FreeIdx)    ← 把"上一个 FreeIdx" 写入 Buf[0] 的字节
  FreeIdx = 0                        ← FreeIdx 指向 0
  Map.SetInt(key0, -1)
  → Buf=[Free→2, T1], FreeIdx=0, AliveCount=1

Add(): idx=FreeIdx=0
  Buf.Size(2)!=idx(0)，所以走 else 分支：
    FreeIdx = *(int*)&Buf[0] = 2     ← 沿着链表前进
  PLACEMENT_NEW(&Buf[0]) T()
  → Buf=[T2, T1], FreeIdx=2, AliveCount=2
```

**完美复用**：第二次 `Add` 把已释放的 `Buf[0]` 重新利用，没有触发 `Buf.resize`——零分配。

### `GetByKey`：通过 ID 查对象

```cpp
// imgui_internal.h:768
T* GetByKey(ImGuiID key) {
    int idx = Map.GetInt(key, -1);
    return (idx != -1) ? &Buf[idx] : NULL;
}
```

`Map.GetInt(key, -1)` 是 `ImGuiStorage` 的 O(log N) 二分查找。

### `GetOrAddByKey`：经典的"懒创建"模式

```cpp
// imgui_internal.h:771
T* GetOrAddByKey(ImGuiID key) {
    int* p_idx = Map.GetIntRef(key, -1);
    if (*p_idx != -1) return &Buf[*p_idx];
    *p_idx = FreeIdx;
    return Add();
}
```

**用法**：你想"如果窗口 ID 对应的 ImGuiTable 存在就用它，否则新建一个"——一行搞定。

ImGui 内部到处都是这个模式：

```cpp
ImGuiTable* table = g.Tables.GetOrAddByKey(table_id);
ImGuiTabBar* tab_bar = g.TabBars.GetOrAddByKey(bar_id);
ImGuiDockNode* node = g.DockContext.Nodes.GetOrAddByKey(node_id);
```

### `Clear()`：批量释放

```cpp
// imgui_internal.h:773
void Clear() {
    for (int n = 0; n < Map.Data.Size; n++) {
        int idx = Map.Data[n].val_i;
        if (idx != -1)
            Buf[idx].~T();           // 显式析构所有活对象
    }
    Map.Clear();
    Buf.clear();                     // 释放底层 ImVector
    FreeIdx = AliveCount = 0;
}
```

**注意**：必须遍历 Map 找到所有"活"对象（`idx != -1`），逐个 `~T()`。`Buf.clear()` 会释放内存但不调析构（参见 2.1 章），`ImPool::Clear` 必须自己处理。

### 迭代 ImPool

`imgui_internal.h:779~784` 给出了官方的迭代姿势：

```cpp
// To iterate a ImPool: for (int n = 0; n < pool.GetMapSize(); n++) if (T* t = pool.TryGetMapData(n)) { ... }
// Can be avoided if you know .Remove() has never been called on the pool, or AliveCount == GetMapSize()
int GetAliveCount() const               { return AliveCount; }
int GetBufSize() const                  { return Buf.Size; }
int GetMapSize() const                  { return Map.Data.Size; }
T*  TryGetMapData(ImPoolIdx n)          { int idx = Map.Data[n].val_i; if (idx == -1) return NULL; return GetByIndex(idx); }
```

迭代要走 Map，因为 `Buf` 里有"已 Remove 但尚未复用"的洞——直接 `for (i; i < Buf.Size; i++)` 会遍历到这些洞。

### `ImPool` 的内存特性

```
活对象数 = 5000，从未 Remove → Buf.Size = 5000
活对象数 = 5000，曾 Remove 1000 个，新 Add 1000 个 → Buf.Size = 5000（完美复用）
活对象数 = 1000，曾 Add 5000 个再 Remove 4000 → Buf.Size 仍然是 5000
```

**`Buf.Size` 永远是"历史最大同时存活数"——不会缩小**。这是与"零分配"妥协的代价：内存只增不减。

---

## 2.2.5 `ImChunkStream<T>`：变长记录顺序流

打开 `imgui_internal.h:792~808`：

```cpp
template<typename T>
struct ImChunkStream
{
    ImVector<char>  Buf;

    void  clear()                       { Buf.clear(); }
    bool  empty() const                 { return Buf.Size == 0; }
    int   size() const                  { return Buf.Size; }
    T*    alloc_chunk(size_t sz)        { size_t HDR_SZ = 4; sz = IM_MEMALIGN(HDR_SZ + sz, 4u); int off = Buf.Size; Buf.resize(off + (int)sz); ((int*)(void*)(Buf.Data + off))[0] = (int)sz; return (T*)(void*)(Buf.Data + off + (int)HDR_SZ); }
    T*    begin()                       { size_t HDR_SZ = 4; if (!Buf.Data) return NULL; return (T*)(void*)(Buf.Data + HDR_SZ); }
    T*    next_chunk(T* p)              { size_t HDR_SZ = 4; IM_ASSERT(p >= begin() && p < end()); p = (T*)(void*)((char*)(void*)p + chunk_size(p)); if (p == (T*)(void*)((char*)end() + HDR_SZ)) return (T*)0; return p; }
    int   chunk_size(const T* p)        { return ((const int*)p)[-1]; }
    T*    end()                         { return (T*)(void*)(Buf.Data + Buf.Size); }
    int   offset_from_ptr(const T* p)   { ... return (int)off; }
    T*    ptr_from_offset(int off)      { ... return (T*)(void*)(Buf.Data + off); }
    void  swap(ImChunkStream<T>& rhs)   { rhs.Buf.swap(Buf); }
};
```

### 数据布局

`ImChunkStream` 在一个 `ImVector<char>` 里**串行排列变长记录**。每条记录前面有一个 4 字节"头"存储该记录的大小。

```
┌────────┬────────┬─────────┬────────┬────────┬─────────┬──────┐
│ size_0 │ data_0 │ pad_0   │ size_1 │ data_1 │ pad_1   │ ...  │
│ 4 字节  │ T 数据  │ 对齐填充 │ 4 字节  │ T 数据  │ 对齐填充 │      │
└────────┴────────┴─────────┴────────┴────────┴─────────┴──────┘
        ↑                            ↑
    begin()                      next_chunk(begin())
    返回这里                       返回这里
    (跳过 4 字节头)               (跳过 4 字节头)
```

### `alloc_chunk` 详解

```cpp
T* alloc_chunk(size_t sz) {
    size_t HDR_SZ = 4;
    sz = IM_MEMALIGN(HDR_SZ + sz, 4u);          // ① 总大小（含头）对齐到 4
    int off = Buf.Size;
    Buf.resize(off + (int)sz);                   // ② 扩展 Buf
    ((int*)(void*)(Buf.Data + off))[0] = (int)sz;// ③ 把总大小写入头部
    return (T*)(void*)(Buf.Data + off + (int)HDR_SZ);  // ④ 返回数据起点（跳过头）
}
```

**`IM_MEMALIGN(HDR_SZ + sz, 4u)`** —— 把"头 + 数据"总大小对齐到 4 字节倍数。这样下一条记录的头**也是 4 字节对齐**，可以直接 cast 成 `int*` 读取。

### `begin / next_chunk` 详解

```cpp
T* begin() {
    size_t HDR_SZ = 4;
    if (!Buf.Data) return NULL;
    return (T*)(void*)(Buf.Data + HDR_SZ);    // 跳过第一条记录的头
}

T* next_chunk(T* p) {
    size_t HDR_SZ = 4;
    IM_ASSERT(p >= begin() && p < end());
    p = (T*)(void*)((char*)(void*)p + chunk_size(p));  // p += 当前 chunk 大小
    if (p == (T*)(void*)((char*)end() + HDR_SZ)) return (T*)0;  // 到尾了
    return p;
}

int chunk_size(const T* p) {
    return ((const int*)p)[-1];        // p 指向数据，p[-1] = 头部 = 大小
}
```

**精彩之处**：`((const int*)p)[-1]` —— 用户拿到的指针 `p` 指向**数据**（不含头），读 `p[-1]` 就是头部值（chunk 的总大小）。

### 迭代

```cpp
ImChunkStream<MySettings> stream;
// ... 已经填充了若干 chunk ...

for (MySettings* s = stream.begin(); s != NULL; s = stream.next_chunk(s)) {
    // 处理 s
}
```

### 用途：`.ini` 设置序列化

`ImChunkStream` 的**唯一**实用场景是 ImGui 的 `.ini` 设置系统：

```cpp
// imgui_internal.h 中（伪代码节选）
struct ImGuiContext {
    ImChunkStream<ImGuiWindowSettings>  SettingsWindows;
    ImChunkStream<ImGuiTableSettings>   SettingsTables;
};
```

每个 `ImGuiWindowSettings` 的大小因为窗口名长度（变长字符串）而**不固定**。`ImChunkStream` 让所有 settings **连续存储**，加载时按 chunk 顺序反序列化。

### 与 `ImVector<MySettings*>` 的对比

| | `ImChunkStream<MySettings>` | `ImVector<MySettings*>` |
|---|---|---|
| 内存布局 | 单一连续 buffer | N 个分散的堆块 |
| 分配次数 | 几次（buffer 扩容时） | N 次（每个对象一次） |
| Cache 局部性 | 优秀 | 差 |
| 元素大小 | 可变 | 必须固定（指针指向不同实例） |
| 元素构造 | 不支持复杂构造（仅原始字节） | 完整构造 |

`ImChunkStream` 牺牲"复杂对象构造"换取"零分配 + cache 友好"。适用于**写一次、读多次、不需要修改**的场景。

---

## 2.2.6 `ImGuiStorage`：sorted vector + 二分查找

`imgui.h:2776~2828` 定义结构，`imgui.cpp:2876~2980` 是实现。

### 数据结构

```cpp
// imgui.h:2776
struct ImGuiStoragePair
{
    ImGuiID key;
    union { int val_i; float val_f; void* val_p; };

    ImGuiStoragePair(ImGuiID _key, int _val)    { key = _key; val_i = _val; }
    ImGuiStoragePair(ImGuiID _key, float _val)  { key = _key; val_f = _val; }
    ImGuiStoragePair(ImGuiID _key, void* _val)  { key = _key; val_p = _val; }
};

// imgui.h:2793
struct ImGuiStorage
{
    ImVector<ImGuiStoragePair> Data;

    void Clear() { Data.clear(); }
    int   GetInt(ImGuiID key, int default_val = 0) const;
    void  SetInt(ImGuiID key, int val);
    bool  GetBool(ImGuiID key, bool default_val = false) const;
    void  SetBool(ImGuiID key, bool val);
    float GetFloat(ImGuiID key, float default_val = 0.0f) const;
    void  SetFloat(ImGuiID key, float val);
    void* GetVoidPtr(ImGuiID key) const;
    void  SetVoidPtr(ImGuiID key, void* val);

    int*   GetIntRef(ImGuiID key, int default_val = 0);
    bool*  GetBoolRef(ImGuiID key, bool default_val = false);
    float* GetFloatRef(ImGuiID key, float default_val = 0.0f);
    void** GetVoidPtrRef(ImGuiID key, void* default_val = NULL);

    void BuildSortByKey();
    void SetAllInt(int val);
};
```

### 关键设计：union 共享存储

```cpp
union { int val_i; float val_f; void* val_p; };
```

**`val_i / val_f / val_p` 共用同一段内存**——`sizeof(union) = max(sizeof(int), sizeof(float), sizeof(void*)) = 8`（64 位）。

**没有类型标签**——`ImGuiStorage` **不**记录值的类型！调用者必须自己确保 Get 和 Set 用同一类型：

```cpp
storage.SetInt(key, 42);
float f = storage.GetFloat(key);  // ❌ 读到的是 0x29.??（用 int 42 当 float 解读）
```

`imgui.h:2792` 的注释明确说：
> `Types are NOT stored, so it is up to you to make sure your Key don't collide with different types.`

**这是 ImGui 哲学的另一面**：信任程序员，省去类型标签 4 字节 + 类型检查开销。

### 二分查找：`ImLowerBound`

```cpp
// imgui.cpp:2881
ImGuiStoragePair* ImLowerBound(ImGuiStoragePair* in_begin, ImGuiStoragePair* in_end, ImGuiID key)
{
    ImGuiStoragePair* in_p = in_begin;
    for (size_t count = (size_t)(in_end - in_p); count > 0; )
    {
        size_t count2 = count >> 1;
        ImGuiStoragePair* mid = in_p + count2;
        if (mid->key < key) {
            in_p = ++mid;
            count -= count2 + 1;
        } else {
            count = count2;
        }
    }
    return in_p;
}
```

注释 `// std::lower_bound but without the bullshit`——直译"std::lower_bound 但没那些扯淡"。

**做的事**：在已排序的 `[in_begin, in_end)` 中找到第一个 `key >= 给定 key` 的位置。如果所有 key 都更小，返回 `in_end`。

为什么不用 `std::lower_bound`：

- 不依赖 STL（参见 0.2 章）。
- 自己手写更可控、debug 模式更快。

### `GetInt` 实现

```cpp
// imgui.cpp:2916
int ImGuiStorage::GetInt(ImGuiID key, int default_val) const {
    ImGuiStoragePair* it = ImLowerBound(...);
    if (it == Data.Data + Data.Size || it->key != key)
        return default_val;
    return it->val_i;
}
```

**O(log N) 查找**，找不到返回 `default_val`。

### `GetIntRef`：找不到则插入

```cpp
// imgui.cpp:2946
int* ImGuiStorage::GetIntRef(ImGuiID key, int default_val) {
    ImGuiStoragePair* it = ImLowerBound(...);
    if (it == Data.Data + Data.Size || it->key != key)
        it = Data.insert(it, ImGuiStoragePair(key, default_val));
    return &it->val_i;
}
```

**找不到则插入**——保持有序的位置插入，**O(N)** 因为需要 `memmove` 后续元素。

但插入是**罕见操作**——通常一帧最多发生一次（用户首次访问某个 ID）。所以**摊销下来**仍很便宜。

注释里有 `FIXME-OPT`：

```cpp
// imgui.cpp:2975
// FIXME-OPT: Need a way to reuse the result of lower_bound when doing GetInt()/SetInt() - not too bad because it only happens on explicit interaction (maximum one a frame)
```

——`GetInt` 和 `SetInt` 都做一次 `ImLowerBound`，理论上可以缓存结果。但因为 set 操作不频繁，没有优化的紧迫性。

### 为什么不用 `std::unordered_map`

`std::unordered_map<ImGuiID, int>`：
- 哈希表 + 拉链 → cache 不友好。
- ABI 不稳定（不能跨 STL）。
- 哈希表元数据 + 桶指针开销大。
- 小数据量（通常每个窗口存几个到几十个键值对）下哈希表反而慢。

`ImGuiStorage` 的 sorted vector：
- 数据连续 → cache 友好。
- O(log N) 二分查找在 N < 1000 时几乎和 hash 一样快。
- ABI 跨编译器一致（基于 `ImVector`）。
- 32 字节起步（几乎全是数据，没有元数据）。

### `BuildSortByKey`：批量重建

```cpp
// imgui.cpp:2911
void ImGuiStorage::BuildSortByKey() {
    ImQsort(Data.Data, (size_t)Data.Size, sizeof(ImGuiStoragePair), PairComparerByID);
}
```

**`ImQsort` 是 `qsort` 的包装**。如果你要批量加大量 key（例如 ini 加载），可以：

1. 直接 `Data.push_back(...)` 加进去（不保序）。
2. 全部加完后调 `BuildSortByKey()` 一次性排序。

这比逐个 `SetInt`（每次 O(N)）快得多——O(N log N) 总成本。

### 用户场景：自定义状态

`ImGuiStorage` 也对用户开放，给应用代码当"自由 KV 存储"：

```cpp
ImGuiWindow* w = ImGui::GetCurrentWindow();
int* pvar = w->StateStorage.GetIntRef(ImGui::GetID("MyVar"), 0);
ImGui::SliderInt("Var", pvar, 0, 100);
```

注释 `imgui.h:2811~2814`:

> A typical use case where this is convenient for quick hacking (e.g. add storage during a live Edit&Continue session if you can't modify existing struct)

——给"想加状态又不想改结构体"的快速 hack 用。

---

## 2.2.7 `ImGuiTextBuffer` 与 `ImGuiTextIndex`

### `ImGuiTextBuffer`：动态字符串 buffer

```cpp
// imgui.h（节选）
struct ImGuiTextBuffer {
    ImVector<char> Buf;
    static char EmptyString[1];

    ImGuiTextBuffer() {}
    inline char operator[](int i) const { ... }
    const char* begin() const  { return Buf.Data ? &Buf.front() : EmptyString; }
    const char* end() const    { return Buf.Data ? &Buf.back() : EmptyString; }
    int   size() const         { return Buf.Size ? Buf.Size - 1 : 0; }
    bool  empty() const        { return Buf.Size <= 1; }
    void  clear()              { Buf.clear(); }
    void  reserve(int capacity){ Buf.reserve(capacity); }
    const char* c_str() const  { return Buf.Data ? Buf.Data : EmptyString; }
    void  append(const char* str, const char* str_end = NULL);
    void  appendf(const char* fmt, ...) IM_FMTARGS(2);
    void  appendfv(const char* fmt, va_list args) IM_FMTLIST(2);
};
```

### 关键设计：null 终止符在 Buf.back()

`Buf` 存的是 `[char...] + '\0'`。所以：

- `size()` 返回 `Buf.Size - 1`（不算 `\0`）。
- `c_str()` 直接返回 `Buf.Data`（含 `\0`）。

`empty()` 检查 `Buf.Size <= 1`：空 buffer 时 `Buf.Size` 是 0 或 1（仅含 `\0`）。

### `append` 与 `appendf`

```cpp
buf.append("hello");
buf.appendf("count=%d", 42);
```

`appendf` 内部用 `vsnprintf`（或 `stb_sprintf`，看 `imconfig.h` 的 `IMGUI_USE_STB_SPRINTF`）。

### `IM_FMTARGS(2)`

```cpp
void appendf(const char* fmt, ...) IM_FMTARGS(2);
```

`IM_FMTARGS(N)` 是 GCC/Clang 的 `__attribute__((format(printf, N-1, N)))` 包装：让编译器**像检查 printf 一样**检查 fmt 字符串和参数：

```cpp
buf.appendf("%d", "hello");  // ❌ 编译警告：%d 不匹配 const char*
```

**这是 ImGui 不用 C++ 可变参模板的好处之一**——保留 printf 风格的静态检查。

### `ImGuiTextIndex`：行索引

```cpp
// imgui_internal.h:812
struct ImGuiTextIndex
{
    ImVector<int> Offsets;
    int           EndOffset = 0;

    void  clear()                                 { Offsets.clear(); EndOffset = 0; }
    int   size()                                  { return Offsets.Size; }
    const char* get_line_begin(const char* base, int n) { return base + (Offsets.Size != 0 ? Offsets[n] : 0); }
    // ...
};
```

**用途**：给 `ImGuiTextBuffer` 建立"每行起始偏移"索引。

`Demo → Tools → Debug Log` 等场景下，buffer 里有大量文本——直接 search 太慢。`ImGuiTextIndex.Offsets[n]` 直接给出第 n 行的起始字节偏移。

注释 `imgui_internal.h:811`：

> `// This is a strong candidate to be moved into the public API.`

——可能未来移到公开 API，但目前还在 internal。

---

## 2.2.8 容器选择决策树

给一份决策图，帮你在自己代码里选对容器：

```
                需要存储多个 T？
                        │
        ┌───────────────┴───────────────┐
        ▼                               ▼
    元素地址要求稳定？               元素地址不需要稳定
        │                               │
        ▼                               ▼
   ImStableVector              T 通过 ID 索引？
    <T, BLOCKSIZE>                      │
                          ┌─────────────┴─────────────┐
                          ▼                           ▼
                        是                          否
                          │                           │
                          ▼                           ▼
                    ImPool<T>                  T 大小固定？
                                                      │
                                          ┌───────────┴───────────┐
                                          ▼                       ▼
                                        是                       否
                                          │                       │
                                          ▼                       ▼
                                    ImVector<T>          ImChunkStream<T>
                                                          (变长记录)


      只需要 N 个 bit？
        │
        ▼
      N 编译期已知？
        │
   ┌────┴────┐
   ▼         ▼
   是        否
   │         │
   ▼         ▼
ImBitArray  ImBitVector
<N, OFF>


      ID 到 (int/float/ptr) 映射？
        │
        ▼
      ImGuiStorage
```

### 真实 ImGui 内部使用对照表

| 数据结构 | 内部场景 |
|---|---|
| `ImPool<ImGuiTabBar>` | TabBar 持久化（按 ID） |
| `ImPool<ImGuiTable>` | Table 持久化 |
| `ImPool<ImGuiTableTempData>` | Table 临时数据池 |
| `ImChunkStream<ImGuiWindowSettings>` | .ini 中的窗口配置 |
| `ImChunkStream<ImGuiTableSettings>` | .ini 中的表格配置 |
| `ImBitArray<ImGuiKey_NamedKey_COUNT, -ImGuiKey_NamedKey_BEGIN>` | 各种"哪些键已变"位图 |
| `ImBitVector` | TablesLastTimeActive 等动态位图 |
| `ImVector<ImGuiWindow*>` | g.Windows / g.WindowsFocusOrder |
| `ImVector<ImDrawVert>` | DrawList 顶点 |
| `ImVector<ImDrawIdx>` | DrawList 索引 |
| `ImVector<ImDrawCmd>` | DrawList 命令 |
| `ImGuiStorage` | 每个 ImGuiWindow 的 StateStorage（TreeNode 展开等） |
| `ImSpan<char>` | Table RawData 切片 |
| `ImSpanAllocator<3>` | Table 一次分配三段 |
| `ImGuiTextBuffer` | DebugLog / Demo Console 输出 |
| `ImStableVector` | 某些字体相关存储 |

---

## 2.2.9 实战：为引擎写一个高性能 Job 队列

把这些容器组合起来能造什么？看一个例子——一个简单的 Job 队列：

```cpp
// 用 ImChunkStream 存变长的 Job 命令
struct JobHeader {
    enum Type { ProcessImage, RunScript, ... } type;
    int payload_size;   // 后面紧跟的 payload 字节数
};

class JobQueue {
    ImChunkStream<JobHeader> m_Stream;
    
public:
    template<typename TPayload>
    void Submit(typename JobHeader::Type type, const TPayload& payload) {
        size_t total = sizeof(JobHeader) + sizeof(TPayload);
        JobHeader* hdr = m_Stream.alloc_chunk(total);
        hdr->type = type;
        hdr->payload_size = sizeof(TPayload);
        memcpy(hdr + 1, &payload, sizeof(TPayload));
    }
    
    void Process() {
        for (JobHeader* j = m_Stream.begin(); j != NULL; j = m_Stream.next_chunk(j)) {
            switch (j->type) {
                case JobHeader::ProcessImage: {
                    auto* p = (ImagePayload*)(j + 1);
                    DoProcessImage(*p);
                    break;
                }
                // ...
            }
        }
        m_Stream.clear();   // 全部处理完，下帧重用 buffer
    }
};
```

**优势**：

- 所有 Job 在内存连续——cache 友好。
- 没有 N 次 `new` 分配——只有 buffer 扩容时偶发 IM_ALLOC。
- 不依赖 std::variant 等重型机制。

这就是从 ImGui 学到的"C 风格但工程化"思路。

---

## 2.2.10 本章小结

ImGui 在 `ImVector` 之上构建的 7 类专门化容器：

| 容器 | 解决的问题 | 关键技巧 |
|---|---|---|
| `ImBitArray<N>` / `ImBitVector` | 紧凑存储 N 个 bool | `ImU32` 位运算、批量 SetBitRange |
| `ImSpan<T>` | 跨函数传递不持有所有权的视图 | ptr+ptr_end 双指针 |
| `ImSpanAllocator<CHUNKS>` | 一次分配切多段，省 N 次 malloc | 编译期 chunk 数量、IM_MEMALIGN 对齐 |
| `ImStableVector<T, B>` | 元素地址稳定，扩容不移动 | 块状分配，`Blocks[i/B][i%B]` 双重间接 |
| `ImPool<T>` | ID 索引 + 槽位复用 | 侵入式空闲链表（空槽位字节当链表节点） |
| `ImChunkStream<T>` | 变长记录顺序流 | 每条记录前 4 字节头存大小 |
| `ImGuiStorage` | ID → 标量值映射 | sorted vector + 二分查找 + 类型擦除 union |

**共同设计哲学**：

- 全部基于 `ImVector` 构建——一种基础数据结构服务全部场景。
- 全部支持 trivially relocatable 假设——`memcpy` 第一原则。
- 全部"capacity 跨帧保留"——零分配运行时。
- 全部不依赖 STL——ABI 跨编译器稳定。

读完本章你应该知道：当 ImGui 内部要"存什么"时，它会怎么选容器、为什么这么选、以及怎么优化。

---

## 2.2.11 下一章预告

第 2.3 章 [[10_第二部分_03_内存分配器与Context生命周期]] 会回到内存系统的"顶层"——

- `ImGui::SetAllocatorFunctions` 的实现细节与时序约束。
- `ImGuiContext` 字段全景图（>4KB 的"巨型上下文"按职能分组）。
- `CreateContext` / `DestroyContext` 的内部步骤。
- DLL 边界陷阱：每个 DLL 自己的 GImGui / GImAllocator——必须手工同步。
- Hot Reload：怎样让 Context 跨 DLL 重载存活。
- 多 Context 模式（`SetCurrentContext`）。

读完后你应该能在 Hazel 这种支持 DLL 模块 / Hot Reload 的引擎里**正确**集成 ImGui，避免"ImGui 跨模块崩溃"的经典坑。
