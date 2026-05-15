// ============================================================================
// lua_function_ref.h — 封装 "C++ 调 Lua 函数" 的完整流程
//
// 核心功能:
//   1. 用 Registry Reference 保存 Lua 函数, 避免每次重新查找
//   2. 用模板参数包展开自动压参
//   3. 用 error handler 抓完整调用栈
//   4. 调用前后做栈平衡检查
// ============================================================================
#pragma once

#include <lua.hpp>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#include "type_converter.h"

namespace luajourney {

// ----------------------------------------------------------------------------
// 参数包递归展开: 把 C++ 参数逐个压 Lua 栈
// 这是 "一个函数" 压所有参数的模板递归写法
// ----------------------------------------------------------------------------
inline void PushArgsToLua(lua_State* /*L*/) {
    // 递归终止: 没有参数可压
}

template<typename Head, typename... Tail>
void PushArgsToLua(lua_State* L, Head&& head, Tail&&... tail) {
    TypeConverter<std::decay_t<Head>>::PushToLua(L, std::forward<Head>(head));
    PushArgsToLua(L, std::forward<Tail>(tail)...);
}

// ----------------------------------------------------------------------------
// Lua 错误处理函数: 当 Lua 脚本 error 时, 被 pcall 调用
// 作用: 把原始错误信息转成 "错误 + 调用栈" 的完整报告
// 用 Lua 自带的 debug.traceback 实现
// ----------------------------------------------------------------------------
inline int LuaErrorHandler(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    if (!msg) {
        if (luaL_callmeta(L, -1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        else
            msg = lua_pushfstring(L, "(non-string error: %s)", luaL_typename(L, -1));
    }

    luaL_traceback(L, L, msg, 1);   // 官方推荐用 luaL_traceback
    return 1;
}

// ----------------------------------------------------------------------------
// LuaFunctionRef — 对 Lua 函数的持久引用
// ----------------------------------------------------------------------------
class LuaFunctionRef {
public:
    LuaFunctionRef() = default;
    ~LuaFunctionRef() { Reset(); }

    // 禁止拷贝 (ref 独占, 避免 double-unref)
    LuaFunctionRef(const LuaFunctionRef&) = delete;
    LuaFunctionRef& operator=(const LuaFunctionRef&) = delete;

    // 支持移动
    LuaFunctionRef(LuaFunctionRef&& other) noexcept
        : L_(other.L_), ref_(other.ref_) {
        other.L_ = nullptr;
        other.ref_ = LUA_NOREF;
    }
    LuaFunctionRef& operator=(LuaFunctionRef&& other) noexcept {
        if (this != &other) {
            Reset();
            L_ = other.L_;
            ref_ = other.ref_;
            other.L_ = nullptr;
            other.ref_ = LUA_NOREF;
        }
        return *this;
    }

    // 绑定一个全局 Lua 函数
    bool Bind(lua_State* L, const char* func_name) {
        Reset();
        L_ = L;

        lua_getglobal(L, func_name);
        if (!lua_isfunction(L, -1)) {
            std::cerr << "[LuaFunctionRef] '" << func_name << "' is not a function\n";
            lua_pop(L, 1);
            L_ = nullptr;
            return false;
        }

        ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        return true;
    }

    // 释放引用
    void Reset() {
        if (L_ && ref_ != LUA_NOREF) {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
        }
        L_ = nullptr;
        ref_ = LUA_NOREF;
    }

    bool IsValid() const { return L_ != nullptr && ref_ != LUA_NOREF; }

    // --- 调用: 有返回值 ---
    template<typename Ret, typename... Args>
    Ret Call(Args&&... args) {
        if (!IsValid()) return Ret{};

        int stack_before = lua_gettop(L_);

        // 1. 压入 error handler
        lua_pushcfunction(L_, &LuaErrorHandler);
        int err_idx = lua_gettop(L_);

        // 2. 压入 Lua 函数
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);

        // 3. 压入参数
        PushArgsToLua(L_, std::forward<Args>(args)...);

        // 4. pcall
        int n_args = static_cast<int>(sizeof...(Args));
        int rc = lua_pcall(L_, n_args, 1, err_idx);

        Ret result{};
        if (rc != LUA_OK) {
            std::cerr << "[Lua Error]\n" << lua_tostring(L_, -1) << "\n";
            lua_pop(L_, 1);                // 弹出错误信息
        } else {
            result = TypeConverter<Ret>::FromLua(L_, -1);
            lua_pop(L_, 1);                // 弹出返回值
        }

        lua_remove(L_, err_idx);           // 弹出 error handler

        // 5. 栈平衡检查
        int stack_after = lua_gettop(L_);
        if (stack_after != stack_before) {
            std::cerr << "[LuaFunctionRef] STACK LEAK! before=" << stack_before
                      << " after=" << stack_after << "\n";
            lua_settop(L_, stack_before);  // 强制恢复
        }

        return result;
    }

    // --- 调用: 无返回值 ---
    template<typename... Args>
    void CallVoid(Args&&... args) {
        if (!IsValid()) return;

        int stack_before = lua_gettop(L_);

        lua_pushcfunction(L_, &LuaErrorHandler);
        int err_idx = lua_gettop(L_);

        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);
        PushArgsToLua(L_, std::forward<Args>(args)...);

        int n_args = static_cast<int>(sizeof...(Args));
        int rc = lua_pcall(L_, n_args, 0, err_idx);

        if (rc != LUA_OK) {
            std::cerr << "[Lua Error]\n" << lua_tostring(L_, -1) << "\n";
            lua_pop(L_, 1);
        }

        lua_remove(L_, err_idx);

        int stack_after = lua_gettop(L_);
        if (stack_after != stack_before) {
            std::cerr << "[LuaFunctionRef] STACK LEAK! before=" << stack_before
                      << " after=" << stack_after << "\n";
            lua_settop(L_, stack_before);
        }
    }

private:
    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};

} // namespace luajourney
