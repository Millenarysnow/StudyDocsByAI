# Step 04: Function Ref

**难度**: ⭐⭐
**耗时**: 2 小时
**前置**: step-03

---

## 这一步的目标

> 封装一个 **LuaFunctionRef** 类，让 C++ 调 Lua 函数变得安全、简洁、复用。

---

## 上一步留下的问题

step-03 里 `CallLuaToUpper` 每次都要手写 6 步：

```cpp
lua_getglobal(L, "to_upper");         // 1. 找函数
lua_pushstring(L, input.c_str());     // 2. 压参数
lua_pcall(L, 1, 1, 0);                 // 3. 调用
result = lua_tostring(L, -1);          // 4. 读返回值
lua_pop(L, 1);                         // 5. 清栈
// 还少了错误处理 / 栈平衡检查 (6, 7)
```

**3 个问题**：
1. 每次调 Lua 都得写这 6 步
2. `lua_getglobal` 每次都查表，慢
3. 错误处理粗糙，没调用栈

---

## 这一步解决的问题

### 1. 用 Registry Reference 避免重复查找

Lua 提供一个特殊的表叫 **Registry**（`LUA_REGISTRYINDEX`），只有 C 能访问。你可以把一个 Lua 函数"塞进去"，拿一个 int 引用，以后用这个 int 取回，而不用再查全局表。

```cpp
// 一次性设置 (启动时):
lua_getglobal(L, "to_upper");
int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // 返回一个 int 句柄

// 之后每次调用:
lua_rawgeti(L, LUA_REGISTRYINDEX, ref);   // 把函数取回栈顶
// ... pcall ...

// 不用时释放:
luaL_unref(L, LUA_REGISTRYINDEX, ref);
```

**性能关键**：`lua_getglobal` 要 hash 字符串查全局表；`lua_rawgeti` 是数组索引，O(1) 极快。

### 2. 包装错误处理

**正确的 pcall 姿势**：
1. 压一个 **error handler** 函数
2. 压函数
3. 压参数
4. `pcall(nargs, nresults, err_idx)`

Error handler 会在 Lua 抛错时被调用，它可以用 `debug.traceback` 抓到完整调用栈，而不是只有一行错误信息。

### 3. 栈平衡检查

每次调用前记录 `lua_gettop`，调用后对比。不平衡说明有 bug，立即暴露。

---

## 代码结构

```
step-04-function-ref/
├── README.md
├── CMakeLists.txt
├── src/
│   ├── type_converter.h   (从 step-03 拷过来)
│   ├── lua_function_ref.h ← 新文件
│   └── main.cpp           ← 使用 LuaFunctionRef
└── scripts/
    └── game.lua
```

---

## 核心 API 设计

```cpp
class LuaFunctionRef {
public:
    LuaFunctionRef();
    ~LuaFunctionRef();

    // 在 Lua 全局表里找 func_name, 存一个 ref
    bool Bind(lua_State* L, const char* func_name);

    // 调用并读取一个返回值
    template<typename Ret, typename... Args>
    Ret Call(Args&&... args);

    // 调用无返回值
    template<typename... Args>
    void CallVoid(Args&&... args);

    bool IsValid() const;
    void Reset();
};
```

---

## 新增：`pushArgsToLua` 参数包展开

调用时想把多个参数一次全压栈：

```cpp
template<typename Arg>
void pushArgsToLua(lua_State* L, Arg&& arg) {
    TypeConverter<std::decay_t<Arg>>::PushToLua(L, std::forward<Arg>(arg));
}

template<typename Head, typename... Tail>
void pushArgsToLua(lua_State* L, Head&& h, Tail&&... t) {
    pushArgsToLua(L, std::forward<Head>(h));
    pushArgsToLua(L, std::forward<Tail>(t)...);
}
```

**递归展开**。调用 `pushArgsToLua(L, 1, 3.14f, "hi")` 等价于：

```cpp
TypeConverter<int>::PushToLua(L, 1);
TypeConverter<float>::PushToLua(L, 3.14f);
TypeConverter<const char*>::PushToLua(L, "hi");
```

**全编译期展开**，运行时零额外开销。

---

## 错误处理函数长什么样？

```cpp
static int LuaErrorHandler(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    if (!msg) msg = "(non-string error)";

    // 用 Lua 的 debug.traceback 抓调用栈
    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);                 // 弹掉 debug 表, 只留 traceback 函数
    lua_pushstring(L, msg);            // 原错误信息
    lua_pushinteger(L, 2);             // stack level
    lua_pcall(L, 2, 1, 0);             // debug.traceback(msg, 2)

    return 1;  // 返回增强后的错误信息
}
```

**为什么是 `debug.traceback`** 而不是我们自己 `lua_getstack` 遍历？因为 Lua 标准库已经写好了，直接复用。

---

## 构建 & 运行

```bash
cd step-04-function-ref
cmake -B build
cmake --build build

# Windows:
build/Debug/function_ref.exe
# Linux/Mac:
build/function_ref
```

### 期望输出

```
[C++] Binding Lua functions (to_upper / greet / fail)...
[C++] Calling to_upper("hello world"):
[Lua]  to_upper called with: hello world
  → HELLO WORLD

[C++] Calling greet("Alice", 30):
[Lua]  greet: Hello Alice, you are 30 years old

[C++] Calling fail() (will error):
[Lua Error] scripts/game.lua:16: intentional error!
stack traceback:
        [C]: in function 'error'
        scripts/game.lua:16: in function <scripts/game.lua:15>
        [C]: in ?

[C++] Calling to_upper("again"):
  (still works after error)
  → AGAIN

[C++] Calling Lua 100000 times (benchmark)...
[C++] Total: 12 ms
```

---

## 动手挑战

1. **挑战 1**：加一个 `Call<std::tuple<int, std::string>>(...)`，支持多返回值（Lua 函数 `return 42, "hello"`）。提示：压栈 nresults=2，读两个值，包成 tuple。

2. **挑战 2**：让 `LuaFunctionRef` 支持**在某个 table 下**查函数（`Bind(L, "Game", "tick")` 等价于 `Game.tick`）。这是为 step-08 预热。

3. **挑战 3**：benchmark 对比"每次 `lua_getglobal`"和"用 ref"的性能差距。100 万次调用差多少？

---

## 新概念清单

- **Lua Registry**: C 专属的全局存储表
- **`luaL_ref` / `lua_rawgeti`**: 存取 Lua 值的 int 句柄
- **error handler**: pcall 的第 4 个参数，自动加调用栈
- **`debug.traceback`**: Lua 的调用栈抓取库
- **栈平衡检查**: 调用前后 `lua_gettop` 对比
- **参数包递归展开**: `pushArgsToLua<Head, Tail...>`

---

## 架构演进视角

```
step-02/03: 手写 "找函数 + 压参 + pcall + 读返回 + 清栈"
            ↓
step-04: LuaFunctionRef 封装上述流程
         Registry ref 避免重复查找
         Error handler 抓调用栈
         栈平衡检查防泄漏

   → 这一步对应原理 6 "C++ 调 Lua" 的完整形态
```

---

## 下一步

至此 C++ ↔ Lua 的**值类型**交互已经成熟。
**但 C++ 对象类型（类的实例）**还没法传。

step-05 开始绑定**真正的 C++ 类**（`class`），引入 metatable 和 userdata。这是工业级引擎设计最核心的部分开始的地方。
