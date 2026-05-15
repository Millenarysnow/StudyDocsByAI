# 术语表

Lua 和 C++ 绑定涉及不少专有术语，先建立共识。

## Lua 基础术语

### Lua State
一个 Lua 虚拟机实例。由 `luaL_newstate()` 创建，`lua_close()` 销毁。
类型 `lua_State*` 贯穿所有 Lua C API。

### Stack (栈)
Lua 的值栈，所有 C/Lua 交互通过这个栈进行。
- 索引从 1 开始（栈底）或从 -1 开始（栈顶）
- 压栈：`lua_pushxxx`
- 读值：`lua_toxxx`
- 弹出：`lua_pop`

### Registry (注册表)
`LUA_REGISTRYINDEX` 指向的特殊 table，只有 C 代码能访问。
用于长期保存 Lua 值的引用。

### Reference (引用)
`luaL_ref(L, LUA_REGISTRYINDEX)` 返回的 int 句柄。
用于 C++ 侧长期持有 Lua 对象。

### Metatable (元表)
附加给 table/userdata 的表，定义**行为重载**（运算符、访问、GC 等）。
关键字段：`__index`, `__newindex`, `__gc`, `__tostring`, `__eq`, `__add` ...

### Userdata (用户数据)
一块由 Lua 管理生命周期、但内容由 C 代码填的内存。
绑定 metatable 后，在 Lua 中可当作对象使用。

### LightUserdata
纯粹的 `void*`。不被 GC 管理，没有 metatable。一般只用来存裸指针。

### Closure (闭包)
带有 upvalues 的 C 函数。用于在 C 函数里"捕获"一些值。
`lua_pushcclosure(L, func, n)` 创建。

### pcall / lua_pcall
Protected call。执行一段 Lua 代码但捕获错误，不会 longjmp。
生产代码必用。

### Garbage Collection (GC)
Lua 的自动内存回收。incremental (增量) 模式可以控制 step 大小。
`lua_gc(L, LUA_GCCOLLECT, 0)` 执行一次完整回收。

---

## 绑定相关术语

### Bind / Binding
把 C++ 类/函数**暴露给 Lua**的过程。

### TypeConverter
在 C++ 和 Lua 之间转换类型的模板。特化一个类型就能绑定。
工业级引擎中通常叫 `NativeTypeDelegate` 或类似名称。

### InstanceDelegate
userdata 里存的中间对象，描述一个 C++ 对象"怎么被持有"。
工业级引擎中通常叫 `NativeInstanceDelegate` 或类似名称。

### Handle
弱引用句柄。一般是 `(slot, generation)` 的组合。
对象销毁后 Handle 查找返回 nullptr，避免野指针。

### Reflection (反射)
运行时查询类型信息。C++ 原生不支持，需要**代码生成**模拟。

### Codegen (代码生成)
外部工具扫 C++ 源码或元数据，生成绑定代码的过程。
工业级引擎中通常会有自己的 `meta_parser` 工具。
LuaJourney 第 14 步用 C# 做类似的工具。

### Hot Reload (热更新)
运行时替换已加载的 Lua 代码，不重启进程。
Lua 原生支持函数热替换（函数是一等公民）。

---

## 工程相关术语

### CRT (C Runtime)
C/C++ 标准运行时库。MSVC 有静态（/MT）和动态（/MD）两种。
跨 DLL 时必须一致，否则 new/delete 会崩。

### Symbol Visibility
DLL 需要显式标记哪些符号导出。
`__declspec(dllexport)` / `__declspec(dllimport)`。

### SFINAE
"Substitution Failure Is Not An Error"。C++ 模板机制，允许基于类型特征选择模板。
常配合 `std::enable_if` 使用。

### Template Parameter Pack
C++11 可变参数模板。支持 `template<typename... Args>` 和参数包展开。

### index_sequence
C++14 编译期整数序列。配合参数包展开可以在编译期遍历 tuple。

---

## 工业级引擎常见术语

### Schema 系统
工业级引擎常见的元数据 / 序列化系统，元数据通常用 XML 格式定义。
对应 LuaJourney 的"XML 前端"。

### USE_MEMORY_POOL
对象池宏。让某个类的分配走内存池而不是默认 new。

### SafeHandle (类型安全句柄)
类型安全的句柄。对应 LuaJourney 的 Handle 系统。

### OptionalPtr
"可能有效也可能无效"的指针包装。类似 Rust 的 `Option<&T>`。

### ValueStorage / OwnedHandle
工业级引擎内部常见的值存储 / 强引用 Handle 类型。LuaJourney 有简化对应。

---

## LuaJIT 特有术语

### LUAJIT_MODE_WRAPCFUNC
让 LuaJIT 自动给所有 C 函数调用包一层 try/catch，捕获 C++ 异常转成 Lua error。

### JIT Trace
LuaJIT 把热点 Lua 代码编译成机器码的路径。可以通过 `jit.v` 观察。

### NYI (Not Yet Implemented)
LuaJIT 不能 JIT 优化的 Lua 特性（比如 pairs 迭代器某些用法）。

---

## 每一步提到新术语时会回头引用这里，不确定的查一下。
