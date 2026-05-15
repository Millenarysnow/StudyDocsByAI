// ============================================================================
// step-01: Hello Lua
//
// 目标: 让 C++ 启动 Lua 虚拟机, 执行一段 Lua 代码, 再关闭。
//
// 这是 Lua 嵌入最基础的形态。读完这份代码你应该理解:
//   1. 什么是 lua_State*
//   2. 怎么执行 Lua 代码 (字符串形式 和 文件形式)
//   3. 错误怎么从 Lua 传回 C++
// ============================================================================

#include <lua.hpp>        // Lua 5.4 的 C++ 头 (内部 include 了 lua.h/lauxlib.h/lualib.h)
#include <iostream>

// ----------------------------------------------------------------------------
// 辅助函数: 如果上一步 Lua 执行失败, 打印错误并清理栈
// ----------------------------------------------------------------------------
static void CheckLuaError(lua_State* L, int result, const char* context) {
    if (result != LUA_OK) {
        std::cerr << "[Lua Error in " << context << "]\n    "
                  << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);  // 弹出错误信息
    }
}

int main() {
    // ------------------------------------------------------------------------
    // 1. 创建 Lua 虚拟机
    //
    // luaL_newstate() 内部用默认的 malloc/free 分配内存。
    // 后面我们会换成自定义分配器 (step-13), 但现在不用管。
    // ------------------------------------------------------------------------
    std::cout << "[C++] Starting Lua VM...\n";
    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "[C++] Failed to create Lua state!\n";
        return 1;
    }

    // ------------------------------------------------------------------------
    // 2. 打开标准库
    //
    // 这一步后, Lua 代码里才能用 print / math / string / table / io / os 等。
    // 不打开的话连 print("hi") 都会报 "attempt to call a nil value".
    // ------------------------------------------------------------------------
    luaL_openlibs(L);

    // ------------------------------------------------------------------------
    // 3. 执行一段"内联"的 Lua 代码字符串
    //
    // luaL_dostring = luaL_loadstring + lua_pcall
    // 即 "编译 + 执行" 合二为一。
    // ------------------------------------------------------------------------
    std::cout << "[C++] Running inline code:\n";
    const char* inline_code = "print('Hello from Lua (inline)')";
    CheckLuaError(L, luaL_dostring(L, inline_code), "inline code");

    // ------------------------------------------------------------------------
    // 4. 执行一个 Lua 文件
    //
    // luaL_dofile 就是 luaL_loadfile + lua_pcall。
    // 注意: 路径是相对于进程工作目录的。CMake 已经把 scripts/ 拷到 exe 旁边。
    // ------------------------------------------------------------------------
    std::cout << "[C++] Running script file:\n";
    CheckLuaError(L, luaL_dofile(L, "scripts/hello.lua"), "hello.lua");

    // ------------------------------------------------------------------------
    // 5. 销毁虚拟机
    //
    // lua_close 会触发所有 __gc metamethod, 释放所有 Lua 对象占用的内存。
    // ------------------------------------------------------------------------
    std::cout << "[C++] Shutting down Lua VM.\n";
    lua_close(L);
    return 0;
}
