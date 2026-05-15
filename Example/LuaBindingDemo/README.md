# MiniLuaBind — C++/Lua 绑定的最小可运行示范

一份**极简但完整**的 C++/Lua 绑定系统教学示例，约 350 行代码演示以下核心机制：

## 覆盖的核心概念

1. **Lua 虚拟机初始化**：`luaL_newstate` + 打开基础库
2. **C++ 类 → Lua metatable**：每个 C++ 类一张 metatable，注册 `__index` / `__gc`
3. **C++ 对象 → Lua userdata**：userdata 里放一个 block 指针
4. **TypeConverter**：类型擦除的 C++ ↔ Lua 双向转换（int/float/string/对象）
5. **模板元编程绑定**：`FunctionInfo` 拆解函数签名，`std::index_sequence`
   展开参数包，编译期生成 `lua_CFunction`
6. **C++ 调 Lua**：`LuaFunctionRef` 拿函数引用，`pushArgsToLua` 压参，`lua_pcall` 调用
7. **主循环驱动**：Lua 端 `App:tick(dt)` 被 C++ 每帧调
8. **热更新**：运行时 `doString` 重写函数

## 构建

需要 Lua 5.1 或 LuaJIT。建议用 vcpkg / package manager 安装。

```bash
# 以 vcpkg 为例
vcpkg install lua:x64-windows

cmake -B build -G "Visual Studio 17 2022" ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

cd build\Release
demo.exe
```

## 运行效果

```
[Lua] App:initialize() called
[Lua] Initial value = 100
[Lua] Object name = Sample
[Lua] frame=60  value=100  dt=0.0167
[Lua] frame=120 value=90   dt=0.0167
...
========= [C++] Hot-reloading App:tick =========
[Lua-HOTFIXED] tick 1
[Lua-HOTFIXED] tick 31
...
```

热更新那一段你会看到**同一个 App 实例**（`self.tick_count` 延续），
但 `tick` 方法被整个替换。这就是 Lua 热更新的最小形态。

## 架构精简度

| 特性 | 工业级方案 | 本 Demo |
|------|-----------|---------|
| 自定义内存分配器 | ✅ | ❌ 默认 realloc |
| 对象句柄（Handle）系统 | ✅ | ❌ 裸指针 + owned 标志 |
| userdata 对象池 | ✅ | ❌ 直接 `lua_newuserdata` |
| 弱表缓存（对象去重） | ✅ | ❌ |
| 运行时类型 ID 检查 | ✅ | ❌ |
| 模板参数包展开 | ✅ | ✅ |
| 栈深度检查 | ✅ | ✅ |
| 热更新 | ✅ 独立线程 + 网络 | ✅ `doString` 演示 |
| 外部代码生成器 | ✅ | ❌ 手写宏 |

## 继续扩展到生产级的方向

1. **对象池**：按 userdata 大小分级（4/8/16/128 字节），减少分配开销。

2. **代码生成**：写一个 parser（Python / C# / Clang libtooling）扫 C++ 头文件或
   自定义 XML/JSON 元数据，自动生成 `register_XXX_to_lua.h`。这样程序员
   只要标注类名和方法名，不用手写绑定代码。

3. **Handle 系统**：C++ 对象被销毁时，Lua 侧的 userdata 应当失效
   （调用时返回 nil 或报 Lua 错误，而不是野指针 crash）。
   典型做法：slot + generation counter 的弱引用句柄。

4. **异常安全**：LuaJIT 提供 `LUAJIT_MODE_WRAPCFUNC`，让所有 C 闭包被
   自动包一层 try/catch。C++ 异常转成 Lua 错误，不会破坏 VM 状态。

5. **热更新 Lua 端**：实现一个 `Debugger` 类，里面一个
   `runDebugScriptFromContext(ctx_name, code)` 函数，内部 `loadstring` + `pcall`
   并捕获 print/error 输出返回给 C++。
