# Step 02: Two-way Talk

**难度**: ⭐ (入门)
**耗时**: 1 小时
**前置**: step-01

---

## 这一步的目标

> 让 C++ 和 Lua 能**互相调用对方的函数**。

具体做 3 件事：
1. C++ 注册一个函数给 Lua，Lua 调用它
2. C++ 调用一个在 Lua 里定义的函数
3. 参数和返回值正确传递（简单类型：int / string）

---

## 上一步留下的问题

step-01 里我们只能让 Lua 跑一段脚本，自己执行完自己的事。**但如果 Lua 要做的事不是它自己能做的呢？**

游戏里常见场景：
- Lua 说"帮我创建一个敌人" → 得调 C++ 函数
- C++ 每帧想让脚本驱动 AI → 得调 Lua 函数

**桥接 C 和 Lua 的机制**就是这一步要解决的事。

---

## 核心概念

### 1. C 函数的签名

Lua 能调用的 C 函数必须长这样：

```cpp
int my_c_function(lua_State* L);
```

**固定签名**。参数和返回值不直接用 C 的机制，而是通过 Lua 栈传递。

**约定**：
- 调用前：Lua 把参数压栈
- 函数内部：`lua_toxxx(L, 1)` 读第 1 个参数，以此类推
- 函数要返回值时：`lua_pushxxx(L, value)` 压栈
- 返回一个 `int`，告诉 Lua"我压了几个返回值"

### 2. 栈索引

- **1, 2, 3, ...** 从栈底开始
- **-1, -2, -3, ...** 从栈顶开始
- 对于参数：参数 1 在位置 1，参数 2 在位置 2，最后一个参数在位置 `-1`

### 3. 注册 C 函数

```cpp
lua_pushcfunction(L, my_c_function);
lua_setglobal(L, "my_func");
// 之后 Lua 里 my_func(1, 2) 会调到 my_c_function
```

或用 `luaL_Reg` 批量注册，后面会看到。

### 4. C++ 调 Lua 函数

反过来，步骤是：
1. `lua_getglobal(L, "lua_func_name")` 把 Lua 函数压栈
2. `lua_pushxxx` 把参数压栈
3. `lua_pcall(L, nargs, nresults, 0)` 调用
4. `lua_toxxx(L, -1)` 读返回值
5. `lua_pop` 清理

---

## 代码结构

```
step-02-two-way-talk/
├── README.md
├── CMakeLists.txt
├── src/
│   └── main.cpp       ← 注册 C 函数 + C++ 调 Lua 函数
└── scripts/
    └── game.lua       ← Lua 里定义的函数 + 调用 C 函数
```

---

## 构建 & 运行

```bash
cd step-02-two-way-talk
cmake -B build
cmake --build build

# Windows:
build/Debug/two_way_talk.exe
# Linux/Mac:
build/two_way_talk
```

### 期望输出

```
[C++] Registering C functions...
[C++] Loading game.lua...
[Lua]  game.lua loaded

[C++] Lua calling C function:
[Lua]  About to call C function add(10, 20)
[C]    my_add called with 10, 20
[Lua]  Result from C: 30

[Lua]  Greeting C
[C]    c_greet called with: Greetings, C++!
[Lua]  (c_greet returned nothing)

[C++] C++ calling Lua function:
[Lua]  Lua function called with: Hello from C++
[C++] Lua function returned: HELLO FROM C++

[C++] Done.
```

---

## 关键代码解析

### 注册 C 函数给 Lua

```cpp
// C 函数实现
int my_add(lua_State* L) {
    int a = (int)lua_tointeger(L, 1);   // 从栈位置 1 读第 1 个参数
    int b = (int)lua_tointeger(L, 2);   // 从栈位置 2 读第 2 个参数
    std::cout << "[C]    my_add called with " << a << ", " << b << "\n";
    lua_pushinteger(L, a + b);           // 压入返回值
    return 1;                             // 告诉 Lua "我压了 1 个返回值"
}

// 注册
lua_pushcfunction(L, my_add);
lua_setglobal(L, "add");
```

之后 Lua 里 `add(10, 20)` 就会执行 `my_add`。

### C++ 调 Lua 函数

```cpp
// 假设 Lua 里定义了: function to_upper(s) return string.upper(s) end

lua_getglobal(L, "to_upper");          // 栈 +1: 函数
lua_pushstring(L, "hello");            // 栈 +1: 参数

// pcall(参数个数=1, 返回值个数=1, errfunc=0)
if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    std::cerr << "Lua error: " << lua_tostring(L, -1) << "\n";
    lua_pop(L, 1);
    return;
}

// 栈顶现在是 Lua 的返回值
const char* result = lua_tostring(L, -1);
std::cout << "Result: " << result << "\n";
lua_pop(L, 1);                         // 清理返回值
```

### 为什么是 `lua_pcall` 而不是 `lua_call`？

- `lua_call`：Lua 出错时会 **longjmp 跳过所有 C++ 析构**，搞坏你的 RAII
- `lua_pcall`：**捕获错误**，把错误信息压栈并返回非 0

**生产代码永远用 `lua_pcall`**。这一步就开始养成习惯。

---

## 动手挑战

1. **挑战 1**：加一个 C 函数 `c_multiply(a, b)`，让 Lua 调。

2. **挑战 2**：让 Lua 里的 `to_upper` 函数接受两个参数（字符串 + 前缀），返回拼接结果。C++ 侧传两个参数给它。

3. **挑战 3**：写一个 Lua 函数返回**两个值**，C++ 侧用 `lua_pcall(L, nargs, 2, 0)` 取两个返回值。

4. **挑战 4**：故意让 Lua 函数 `error("something")`，看 C++ 侧 `pcall` 的错误信息格式。

---

## 新概念清单

- **`lua_CFunction` 签名**：`int(*)(lua_State*)`
- **Lua 栈索引**：正数从底、负数从顶
- **参数和返回值都走栈**，不用 C 参数列表
- **`lua_pcall` vs `lua_call`**
- **pcall 的返回值表示错误码**，错误信息在栈顶

---

## 现在的限制

我们手写的所有函数都用 `lua_tointeger` / `lua_tostring` / `lua_pushinteger` 等 API。**每增加一个函数就得重新写一遍这套样板**。

想象你要绑 100 个函数，每个平均 3 个参数 → 300 次 `lua_tointeger/tostring` 调用 → 样板代码爆炸。

step-03 会引入 **TypeConverter** 来抽象掉这些重复代码。

---

## 新术语

见 [glossary](../../docs/02-glossary.md)：
- lua_CFunction
- pcall
- stack index
