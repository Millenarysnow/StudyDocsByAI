// ============================================================================
// step-03: Type Converter
//
// 这一步用 TypeConverter<T> 重写 step-02 里的所有类型转换调用。
// 功能完全等价, 但代码更统一了。
// ============================================================================

#include <lua.hpp>
#include <iostream>
#include <string>

#include "type_converter.h"

using namespace luajourney;

// ============================================================================
// Part 1: 给 Lua 提供的 C 函数 (用 TypeConverter 改写过)
// ============================================================================

static int my_add(lua_State* L) {
    // 对比 step-02: 不再有 lua_tointeger / lua_pushinteger
    int a = TypeConverter<int>::FromLua(L, 1);
    int b = TypeConverter<int>::FromLua(L, 2);

    std::cout << "[C]    my_add called with " << a << ", " << b << "\n";

    TypeConverter<int>::PushToLua(L, a + b);
    return 1;
}

static int c_greet(lua_State* L) {
    std::string msg = TypeConverter<std::string>::FromLua(L, 1);
    std::cout << "[C]    c_greet called with: " << msg << "\n";
    return 0;
}

// ============================================================================
// Part 2: C++ 调 Lua 函数 (也用 TypeConverter)
// ============================================================================

static std::string CallLuaToUpper(lua_State* L, const std::string& input) {
    lua_getglobal(L, "to_upper");
    if (!lua_isfunction(L, -1)) {
        std::cerr << "[C++] to_upper is not a function in Lua!\n";
        lua_pop(L, 1);
        return "";
    }

    // 用 TypeConverter 压参数
    TypeConverter<std::string>::PushToLua(L, input);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::cerr << "[C++] Lua pcall error: " << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
        return "";
    }

    // 用 TypeConverter 读返回值
    std::string result = TypeConverter<std::string>::FromLua(L, -1);
    lua_pop(L, 1);

    return result;
}

// ============================================================================
// Part 3: 辅助
// ============================================================================

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
// Main
// ============================================================================

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    std::cout << "[C++] Registering C functions (via TypeConverter)...\n";
    lua_pushcfunction(L, my_add);
    lua_setglobal(L, "add");
    lua_pushcfunction(L, c_greet);
    lua_setglobal(L, "c_greet");

    std::cout << "[C++] Loading game.lua...\n";
    if (!CheckLua(L, luaL_dofile(L, "scripts/game.lua"), "game.lua")) {
        lua_close(L);
        return 1;
    }
    std::cout << "\n";

    std::cout << "[C++] Lua calling C function:\n";
    lua_getglobal(L, "test_c_calls");
    CheckLua(L, lua_pcall(L, 0, 0, 0), "test_c_calls");
    std::cout << "\n";

    std::cout << "[C++] C++ calling Lua function:\n";
    std::string result = CallLuaToUpper(L, "Hello from C++");
    std::cout << "[C++] Lua function returned: " << result << "\n";

    std::cout << "\n[C++] Done.\n";
    lua_close(L);
    return 0;
}
