// ============================================================================
// type_converter.h — C++ ↔ Lua 类型转换抽象
//
// 核心思想: 为每个支持的类型特化 TypeConverter<T>, 提供统一的
//          PushToLua 和 FromLua 接口。
//
// 以后每次加新类型, 只需要加一个特化, 不改现有代码。
// ============================================================================
#pragma once

#include <lua.hpp>
#include <string>
#include <type_traits>
#include <utility>

namespace luajourney {

// ----------------------------------------------------------------------------
// 主模板 — 只声明, 不实现
// 用到没特化的类型会编译期报错, 这是有意为之
// typename = void 是给以后 SFINAE 特化预留的钩子
// ----------------------------------------------------------------------------
template<typename T, typename = void>
struct TypeConverter;

// ----------------------------------------------------------------------------
// 基本数值类型
// ----------------------------------------------------------------------------

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

// ----------------------------------------------------------------------------
// 字符串类型
//
// 注意 FromLua 返回 std::string (拷贝), 不返回 const char*。
// 因为 lua_tostring 返回的指针在下次 Lua 栈操作后可能失效,
// 拷贝出来最安全。
// ----------------------------------------------------------------------------

template<>
struct TypeConverter<const char*> {
    static void PushToLua(lua_State* L, const char* v) { lua_pushstring(L, v ? v : ""); }
    // 注意: 返回的指针生命周期由 Lua 管理, 下次栈操作可能失效
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

// ----------------------------------------------------------------------------
// 引用类型的 "脱外套": const T&, T&, T&&  → T
// 为了写 TypeConverter<const std::string&> 也能工作
// ----------------------------------------------------------------------------
template<typename T>
struct TypeConverter<T&> : TypeConverter<T> {};

template<typename T>
struct TypeConverter<const T&> : TypeConverter<T> {};

template<typename T>
struct TypeConverter<T&&> : TypeConverter<T> {};

// ----------------------------------------------------------------------------
// 以后会加的:
//   - TypeConverter<UserType*>   对指向类的指针 (step-05 起)
//   - TypeConverter<Handle<T>>   句柄 (step-10)
//   - TypeConverter<std::vector<T>>  容器 (以后)
// ----------------------------------------------------------------------------

} // namespace luajourney
