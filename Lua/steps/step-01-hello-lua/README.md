# Step 01: Hello Lua

**难度**: ⭐ (入门)
**耗时**: 30 分钟
**前置**: 配好 C++ 工具链 + Lua

---

## 这一步的目标

> 让一个 C++ 程序启动一个 Lua 虚拟机，执行一段 Lua 代码，然后关掉。

就这么简单。听起来像 "Hello World"，但它要让你建立一些**核心认知**。

---

## 背景：Lua 是个"嵌入式语言"

Lua 和 JavaScript/Python 不一样。它不是一门独立跑的语言（严格说 `lua.exe` 只是一个小壳子）。它的**本质是一个 C 库**，任何 C/C++ 程序都可以把它"嵌入"进来。

把 Lua 集成到你的引擎就是：
1. `#include <lua.h>` 几个头
2. 连接 Lua 的 lib
3. 用它提供的 C API

这一步做的就是上面三件事。

---

## 第一个认知：`lua_State*`

`lua_State*` 是 Lua 虚拟机实例的句柄。你需要把它传给所有 Lua API。

可以把它想象成：
- Node.js 里的一个进程
- Python 里的一个解释器实例
- Java 里的一个 JVM

**一个 C++ 程序可以同时开多个 `lua_State`**。它们完全独立，内存、全局变量、GC 都不共享。

---

## 关键 API

| API | 作用 |
|-----|------|
| `luaL_newstate()` | 创建一个 Lua 虚拟机 |
| `luaL_openlibs(L)` | 打开标准库（print, string, math, table 等） |
| `luaL_dostring(L, "code")` | 执行一段字符串形式的 Lua 代码 |
| `luaL_dofile(L, "file.lua")` | 执行一个 Lua 文件 |
| `lua_close(L)` | 销毁虚拟机 |

这一步只用这五个。

---

## 代码结构

```
step-01-hello-lua/
├── README.md         ← 本文件
├── CMakeLists.txt    ← 构建脚本
├── src/
│   └── main.cpp      ← 主程序
└── scripts/
    └── hello.lua     ← Lua 脚本
```

---

## 构建 & 运行

```bash
cd step-01-hello-lua
cmake -B build
cmake --build build

# 运行 (路径取决于生成器)
build/Debug/hello_lua.exe          # Windows (Visual Studio)
build/hello_lua                    # Linux/Mac (Makefile/Ninja)
```

Lua 会从 `vendor/lua-src/` 自动编译, 无需额外安装。

### 期望输出

```
[C++] Starting Lua VM...
[C++] Running inline code:
Hello from Lua (inline)
[C++] Running script file:
Hello from Lua (file)
Lua version: Lua 5.4
2 + 3 = 5
[C++] Shutting down Lua VM.
```

---

## 关键代码讲解

### main.cpp

```cpp
#include <lua.hpp>     // 或 lua.h / lauxlib.h / lualib.h
#include <iostream>

int main() {
    // 1. 创建 Lua 虚拟机
    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "Failed to create Lua state!\n";
        return 1;
    }

    // 2. 打开标准库 (否则 Lua 里连 print 都没有)
    luaL_openlibs(L);

    // 3. 执行一段 Lua 代码
    const char* code = "print('Hello from Lua (inline)')";
    if (luaL_dostring(L, code) != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
    }

    // 4. 执行一个 Lua 文件
    if (luaL_dofile(L, "scripts/hello.lua") != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
    }

    // 5. 销毁虚拟机
    lua_close(L);
    return 0;
}
```

### 错误处理

`luaL_dostring` 和 `luaL_dofile` 都返回 int：
- `LUA_OK` (0)：成功
- 其它值：失败，错误信息在栈顶（`lua_tostring(L, -1)` 读出来）

记得读完要 `lua_pop(L, 1)` 清理栈顶的错误字符串，否则栈会泄漏。

### Lua 版本

这一步使用 Lua 5.4。和 5.1 的主要区别：
- 5.4 有 integer 类型（5.1 所有数字都是 double）
- 错误处理宏略不同

LuaJourney 前 16 步都用 Lua 5.4。第 17 步会切 LuaJIT（基于 Lua 5.1 语法）。

---

## 动手挑战

跑通基础版之后，试试：

1. **挑战 1**：在 `hello.lua` 里写一个会报错的代码（比如 `error("oh no")`），看 C++ 侧拿到的错误信息长什么样。

2. **挑战 2**：尝试 **创建两个 lua_State**，给它们各执行一段代码，验证它们的全局变量不互通。

3. **挑战 3**：读一下 `luaL_loadstring` 和 `luaL_dostring` 的区别。`dostring = load + call`，分开用有什么好处？

---

## 新概念清单

- **`lua_State*`**：虚拟机实例
- **标准库**：需要 `luaL_openlibs` 才能用
- **字符串形式执行 vs 文件形式执行**
- **错误信息通过栈传递**
- **栈上读完的值要 pop 掉**

---

## 下一步会做什么？

`step-02-two-way-talk` 要做的是：

- C++ 注册一个函数给 Lua，让 Lua 能调
- C++ 定义一个函数，让 Lua 脚本调用它
- C++ 反过来调 Lua 里定义的函数

这是第一次真正的"双向对话"。

---

## 参考

- [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/manual.html#4)
- [术语表](../../docs/02-glossary.md)
- [架构演进图](../../docs/architecture-evolution.md)
