// ============================================================================
// step-04: Function Ref
//
// 用 LuaFunctionRef 替代 step-03 里手写的 "找函数 + 压参 + pcall + 清理"
// 流程。代码大幅简洁, 且多了错误处理和栈平衡检查。
// ============================================================================

#include <lua.hpp>
#include <iostream>
#include <chrono>

#include "type_converter.h"
#include "lua_function_ref.h"

using namespace luajourney;

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    if (luaL_dofile(L, "scripts/game.lua") != LUA_OK) {
        std::cerr << "Failed to load game.lua: " << lua_tostring(L, -1) << "\n";
        lua_close(L);
        return 1;
    }

    // ------------------------------------------------------------
    // 1. 一次性绑定 Lua 函数
    // ------------------------------------------------------------
    std::cout << "[C++] Binding Lua functions (to_upper / greet / fail)...\n";

    LuaFunctionRef fn_to_upper, fn_greet, fn_fail;
    fn_to_upper.Bind(L, "to_upper");
    fn_greet.Bind(L, "greet");
    fn_fail.Bind(L, "fail");

    // ------------------------------------------------------------
    // 2. 调用它们
    // ------------------------------------------------------------

    std::cout << "[C++] Calling to_upper(\"hello world\"):\n";
    std::string up = fn_to_upper.Call<std::string>(std::string("hello world"));
    std::cout << "  \xE2\x86\x92 " << up << "\n\n";

    std::cout << "[C++] Calling greet(\"Alice\", 30):\n";
    fn_greet.CallVoid(std::string("Alice"), 30);
    std::cout << "\n";

    std::cout << "[C++] Calling fail() (will error):\n";
    fn_fail.CallVoid();
    std::cout << "\n";

    std::cout << "[C++] Calling to_upper(\"again\"):\n";
    std::cout << "  (still works after error)\n";
    std::string up2 = fn_to_upper.Call<std::string>(std::string("again"));
    std::cout << "  \xE2\x86\x92 " << up2 << "\n\n";

    // ------------------------------------------------------------
    // 3. benchmark: 10 万次调用
    // ------------------------------------------------------------
    std::cout << "[C++] Calling Lua 100000 times (benchmark)...\n";
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        fn_to_upper.Call<std::string>(std::string("x"));
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "[C++] Total: " << elapsed.count() << " ms\n";

    lua_close(L);
    return 0;
}
