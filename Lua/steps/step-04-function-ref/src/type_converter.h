// ============================================================================
// type_converter.h — 从 step-03 拷贝, 无改动
// ============================================================================
#pragma once

#include <lua.hpp>
#include <string>
#include <type_traits>

namespace luajourney {

template<typename T, typename = void>
struct TypeConverter;

template<>
struct TypeConverter<int> {
    static void PushToLua(lua_State* L, int v) { lua_pushinteger(L, v); }
    static int  FromLua(lua_State* L, int idx) { return static_cast<int>(lua_tointeger(L, idx)); }
};

template<>
struct TypeConverter<long long> {
    static void PushToLua(lua_State* L, long long v) { lua_pushinteger(L, v); }
    static long long FromLua(lua_State* L, int idx) { return lua_tointeger(L, idx); }
};

template<>
struct TypeConverter<float> {
    static void PushToLua(lua_State* L, float v) { lua_pushnumber(L, v); }
    static float FromLua(lua_State* L, int idx) { return static_cast<float>(lua_tonumber(L, idx)); }
};

template<>
struct TypeConverter<double> {
    static void PushToLua(lua_State* L, double v) { lua_pushnumber(L, v); }
    static double FromLua(lua_State* L, int idx) { return lua_tonumber(L, idx); }
};

template<>
struct TypeConverter<bool> {
    static void PushToLua(lua_State* L, bool v) { lua_pushboolean(L, v ? 1 : 0); }
    static bool FromLua(lua_State* L, int idx) { return lua_toboolean(L, idx) != 0; }
};

template<>
struct TypeConverter<const char*> {
    static void PushToLua(lua_State* L, const char* v) { lua_pushstring(L, v ? v : ""); }
    static const char* FromLua(lua_State* L, int idx) { return lua_tostring(L, idx); }
};

template<>
struct TypeConverter<std::string> {
    static void PushToLua(lua_State* L, const std::string& v) {
        lua_pushlstring(L, v.c_str(), v.size());
    }
    static std::string FromLua(lua_State* L, int idx) {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        return s ? std::string(s, len) : std::string();
    }
};

template<typename T> struct TypeConverter<T&> : TypeConverter<T> {};
template<typename T> struct TypeConverter<const T&> : TypeConverter<T> {};
template<typename T> struct TypeConverter<T&&> : TypeConverter<T> {};

} // namespace luajourney
