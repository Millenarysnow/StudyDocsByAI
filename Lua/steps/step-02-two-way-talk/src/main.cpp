// ============================================================================
// step-02: Two-way Talk
//
// 目标: C++ 和 Lua 互相调用对方的函数, 参数/返回值正确传递。
//
// 读完这份代码你应该理解:
//   1. C 函数的签名 int(*)(lua_State*)
//   2. Lua 栈怎么传参数和返回值
//   3. 怎么注册 C 函数给 Lua
//   4. 怎么从 C++ 调 Lua 函数 (含错误处理)
// ============================================================================

#include <lua.hpp>
#include <iostream>
#include <string>

// ============================================================================
// Part 1: 给 Lua 提供的 C 函数们
//
// 所有给 Lua 用的 C 函数必须是这个签名:
//     int function(lua_State* L)
// 返回值是 "我压了几个返回值到栈顶"。
// ============================================================================

// 加法: Lua 里 add(a, b) 调这个
static int my_add(lua_State* L) {
    // 从栈位置 1 和 2 读两个参数
    int a = static_cast<int>(lua_tointeger(L, 1));
    int b = static_cast<int>(lua_tointeger(L, 2));

    std::cout << "[C]    my_add called with " << a << ", " << b << "\n";

    // 压入返回值
    lua_pushinteger(L, a + b);

    // 返回 1 表示压了 1 个返回值
    return 1;
}

// 打印一个字符串参数: Lua 里 c_greet("msg") 调这个。不返回值。
static int c_greet(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = "(null)";

    std::cout << "[C]    c_greet called with: " << msg << "\n";

    return 0;   // 没返回值
}

// ============================================================================
// Part 2: 辅助函数
// ============================================================================

// 检查 Lua 调用结果, 失败时打印错误
static bool CheckLua(lua_State* L, int result, const char* context) {
    if (result != LUA_OK) {
        std::cerr << "[Lua Error in " << context << "]\n    "
                  << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// ============================================================================
// Part 3: C++ 调 Lua 函数的封装
// ============================================================================

// 调用 Lua 里定义的 function upper(s): string
// 参数: input
// 返回值: 大写后的字符串 (调用失败返回空字符串)
static std::string CallLuaToUpper(lua_State* L, const std::string& input) {
    // 1. 把 Lua 函数压栈
    lua_getglobal(L, "to_upper");
    if (!lua_isfunction(L, -1)) {
        std::cerr << "[C++] to_upper is not a function in Lua!\n";
        lua_pop(L, 1);
        return "";
    }

    // 2. 把参数压栈
    lua_pushstring(L, input.c_str());

    // 3. pcall(参数个数=1, 返回值个数=1, 错误处理函数=0)
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::cerr << "[C++] Lua pcall error: " << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
        return "";
    }

    // 4. 读返回值 (栈顶 -1 就是返回值)
    const char* result_cstr = lua_tostring(L, -1);
    std::string result = result_cstr ? result_cstr : "";

    // 5. 把返回值弹掉
    lua_pop(L, 1);

    return result;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // -- 1. 把我们的 C 函数注册给 Lua --
    std::cout << "[C++] Registering C functions...\n";

    lua_pushcfunction(L, my_add);
    lua_setglobal(L, "add");

    lua_pushcfunction(L, c_greet);
    lua_setglobal(L, "c_greet");

    // -- 2. 加载 Lua 脚本 --
    std::cout << "[C++] Loading game.lua...\n";
    if (!CheckLua(L, luaL_dofile(L, "scripts/game.lua"), "game.lua")) {
        lua_close(L);
        return 1;
    }
    std::cout << "\n";

    // -- 3. 让 Lua 主动调 C 函数 (在 game.lua 里已经定义了 test_c_calls) --
    std::cout << "[C++] Lua calling C function:\n";
    lua_getglobal(L, "test_c_calls");
    CheckLua(L, lua_pcall(L, 0, 0, 0), "test_c_calls");
    std::cout << "\n";

    // -- 4. C++ 主动调 Lua 函数 --
    std::cout << "[C++] C++ calling Lua function:\n";
    std::string result = CallLuaToUpper(L, "Hello from C++");
    std::cout << "[C++] Lua function returned: " << result << "\n";

    std::cout << "\n[C++] Done.\n";
    lua_close(L);
    return 0;
}
