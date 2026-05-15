// ==================================================================
// MiniLuaBind — C++/Lua 绑定的最小可运行示范
// 约 350 行。演示以下核心机制：
//   1. Lua 虚拟机初始化
//   2. 模板元编程自动绑定 C++ 函数为 Lua CFunction
//   3. C++ 类通过 metatable + userdata 暴露给 Lua
//   4. C++ 调 Lua 函数 (FunctionRef + 参数包展开)
//   5. C++ 驱动主循环 (每帧调 Lua 的 tick)
//   6. 热更新 (运行时替换 Lua 函数)
//
// 编译: 需要 Lua 5.1 或 LuaJIT, C++17
// ==================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// ==================================================================
// Part 1: FunctionInfo —— 编译期函数签名萃取
//         从 TFunc 拆出返回类型 + 参数类型列表 + 是否是成员函数
// ==================================================================
template<typename TFunc>
struct FunctionInfo;

// 普通函数 Ret(Args...)
template<typename Ret, typename... Args>
struct FunctionInfo<Ret(*)(Args...)> {
    using TReturn = Ret;
    using TClass = void;
    static constexpr bool is_member = false;
    static constexpr size_t param_num = sizeof...(Args);
    using TParamList = std::tuple<std::decay_t<Args>...>;
};

// 成员函数 Ret(T::*)(Args...)
template<typename Ret, typename T, typename... Args>
struct FunctionInfo<Ret(T::*)(Args...)> {
    using TReturn = Ret;
    using TClass = T;
    static constexpr bool is_member = true;
    static constexpr size_t param_num = sizeof...(Args);
    using TParamList = std::tuple<std::decay_t<Args>...>;
};

// const 成员函数
template<typename Ret, typename T, typename... Args>
struct FunctionInfo<Ret(T::*)(Args...) const> {
    using TReturn = Ret;
    using TClass = T;
    static constexpr bool is_member = true;
    static constexpr size_t param_num = sizeof...(Args);
    using TParamList = std::tuple<std::decay_t<Args>...>;
};

// ==================================================================
// Part 2: TypeConverter —— 基本类型 C++ <-> Lua 转换
//         特化哪些类型就能自动绑定哪些类型的参数/返回值
// ==================================================================
template<typename T, typename = void>
struct TypeConverter;

template<>
struct TypeConverter<int> {
    static void pushToLua(lua_State* L, int v) { lua_pushinteger(L, v); }
    static int  fromLua(lua_State* L, int idx) { return (int)lua_tointeger(L, idx); }
};

template<>
struct TypeConverter<float> {
    static void pushToLua(lua_State* L, float v) { lua_pushnumber(L, v); }
    static float fromLua(lua_State* L, int idx) { return (float)lua_tonumber(L, idx); }
};

template<>
struct TypeConverter<double> {
    static void pushToLua(lua_State* L, double v) { lua_pushnumber(L, v); }
    static double fromLua(lua_State* L, int idx) { return (double)lua_tonumber(L, idx); }
};

template<>
struct TypeConverter<bool> {
    static void pushToLua(lua_State* L, bool v) { lua_pushboolean(L, v ? 1 : 0); }
    static bool fromLua(lua_State* L, int idx) { return lua_toboolean(L, idx) != 0; }
};

template<>
struct TypeConverter<const char*> {
    static void pushToLua(lua_State* L, const char* v) { lua_pushstring(L, v ? v : ""); }
    static const char* fromLua(lua_State* L, int idx) { return lua_tostring(L, idx); }
};

template<>
struct TypeConverter<std::string> {
    static void pushToLua(lua_State* L, const std::string& v) { lua_pushstring(L, v.c_str()); }
    static std::string fromLua(lua_State* L, int idx) {
        const char* s = lua_tostring(L, idx);
        return s ? std::string(s) : std::string();
    }
};

// ==================================================================
// Part 3: C++ 类 <-> Lua 绑定
//         通过 metatable 把 C++ 类暴露给 Lua
// ==================================================================

// 存放在 userdata 里的东西: 指针 + 是否拥有该对象的标志
template<typename T>
struct UserDataBlock {
    T* ptr;
    bool owned;  // true: Lua GC 时 delete；false: C++ 侧管理生命周期
};

// 每个 C++ 类在 Lua 侧 metatable 的注册表名
template<typename T>
struct ClassMetaName {
    static const char* value;
};
template<typename T> const char* ClassMetaName<T>::value = "__CXXClass__";

// 从 Lua 栈某位置拿到 C++ 对象指针
template<typename T>
T* getInstance(lua_State* L, int idx) {
    void* ud = lua_touserdata(L, idx);
    if (!ud) {
        luaL_error(L, "Cannot get %s instance (did you use '.' instead of ':'?)",
                   ClassMetaName<T>::value);
        return nullptr;
    }
    UserDataBlock<T>* block = static_cast<UserDataBlock<T>*>(ud);
    return block->ptr;
}

// ------------------------------------------------------------
// 3.1 成员函数 -> lua_CFunction 自动适配 (核心模板魔法)
// ------------------------------------------------------------
template<typename TFunc, TFunc func>
struct MemberFunctionCaller {
    using Info = FunctionInfo<TFunc>;
    using TClass = typename Info::TClass;
    using TReturn = typename Info::TReturn;
    using TParamList = typename Info::TParamList;

    // getArg<I>: 从 Lua 栈上位置 (2 + I) 取第 I 个参数
    // 位置 1 是 self
    template<size_t I>
    static auto getArg(lua_State* L) {
        using TArg = std::tuple_element_t<I, TParamList>;
        return TypeConverter<TArg>::fromLua(L, static_cast<int>(2 + I));
    }

    // 用 index_sequence 展开 doCall
    template<size_t... Is>
    static TReturn doCallImpl(lua_State* L, TClass* self, std::index_sequence<Is...>) {
        return (self->*func)(getArg<Is>(L)...);
    }

    template<typename R = TReturn>
    static std::enable_if_t<!std::is_void<R>::value, int> entry(lua_State* L) {
        TClass* self = getInstance<TClass>(L, 1);
        R ret = doCallImpl(L, self, std::make_index_sequence<Info::param_num>{});
        TypeConverter<std::decay_t<R>>::pushToLua(L, ret);
        return 1;
    }

    template<typename R = TReturn>
    static std::enable_if_t<std::is_void<R>::value, int> entry(lua_State* L) {
        TClass* self = getInstance<TClass>(L, 1);
        doCallImpl(L, self, std::make_index_sequence<Info::param_num>{});
        return 0;
    }
};

// 统一接口: REGISTER_MEMBER(L, "setValue", &MyClass::setValue)
#define REGISTER_MEMBER(L, name, method) \
    do { \
        lua_pushstring(L, name); \
        lua_pushcfunction(L, (&MemberFunctionCaller<decltype(method), method>::entry)); \
        lua_settable(L, -3); \
    } while (0)

// ------------------------------------------------------------
// 3.2 构造函数 & 析构函数
// ------------------------------------------------------------
template<typename T>
int class_new(lua_State* L) {
    // Lua: MyClass:new(arg1, arg2, ...)   --> args 从 idx=2 开始
    //   (idx=1 是 class table 自身)
    T* obj = nullptr;
    int n = lua_gettop(L);
    if (n >= 2 && lua_isstring(L, 2)) {
        obj = new T(lua_tostring(L, 2));   // 假设 T 有 T(const char*) ctor
    } else {
        obj = new T();
    }
    // 创建 userdata
    UserDataBlock<T>* ud =
        (UserDataBlock<T>*)lua_newuserdata(L, sizeof(UserDataBlock<T>));
    ud->ptr = obj;
    ud->owned = true;
    // 绑定 metatable
    luaL_getmetatable(L, ClassMetaName<T>::value);
    lua_setmetatable(L, -2);
    return 1;
}

template<typename T>
int class_gc(lua_State* L) {
    UserDataBlock<T>* ud = (UserDataBlock<T>*)lua_touserdata(L, 1);
    if (ud && ud->owned && ud->ptr) {
        delete ud->ptr;
        ud->ptr = nullptr;
    }
    return 0;
}

template<typename T>
int class_tostring(lua_State* L) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s@%p",
                  ClassMetaName<T>::value, lua_touserdata(L, 1));
    lua_pushstring(L, buf);
    return 1;
}

// 开始注册一个 C++ 类
template<typename T>
void beginRegisterClass(lua_State* L, const char* class_name) {
    ClassMetaName<T>::value = class_name;

    // 1. 创建 metatable 并放入 registry
    luaL_newmetatable(L, class_name);

    // 2. __index = metatable 自身 (让 obj:method() 查到方法)
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_settable(L, -3);

    // 3. __gc
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &class_gc<T>);
    lua_settable(L, -3);

    // 4. __tostring
    lua_pushstring(L, "__tostring");
    lua_pushcfunction(L, &class_tostring<T>);
    lua_settable(L, -3);

    // 5. new
    lua_pushstring(L, "new");
    lua_pushcfunction(L, &class_new<T>);
    lua_settable(L, -3);
}

// 完成注册: 把 metatable 同时设为全局变量 (这样 Lua 可以 MyClass:new())
template<typename T>
void endRegisterClass(lua_State* L, const char* class_name) {
    (void)class_name;
    lua_setglobal(L, class_name);
}

// ==================================================================
// Part 4: LuaFunctionRef —— C++ 持有 Lua 函数引用并调用
// ==================================================================

// 递归展开参数包压栈
template<typename Arg>
void pushArgsToLua(lua_State* L, Arg&& arg) {
    TypeConverter<std::decay_t<Arg>>::pushToLua(L, std::forward<Arg>(arg));
}
template<typename Head, typename... Tail>
void pushArgsToLua(lua_State* L, Head&& h, Tail&&... t) {
    pushArgsToLua(L, std::forward<Head>(h));
    pushArgsToLua(L, std::forward<Tail>(t)...);
}

class LuaFunctionRef {
public:
    LuaFunctionRef() = default;
    ~LuaFunctionRef() { clear(); }

    // 初始化: 在 class_name 表里找 func_name (class_name 为空则找全局)
    void init(lua_State* L, const char* class_name, const char* func_name) {
        L_ = L;
        if (!class_name || class_name[0] == '\0') {
            lua_getglobal(L, func_name);
        } else {
            lua_getglobal(L, class_name);
            if (lua_isnil(L, -1)) {
                std::fprintf(stderr, "[LuaFunctionRef] class '%s' not found\n", class_name);
                lua_pop(L, 1);
                return;
            }
            lua_getfield(L, -1, func_name);
            lua_remove(L, -2);
        }
        if (lua_isnil(L, -1)) {
            std::fprintf(stderr, "[LuaFunctionRef] function '%s' not found\n", func_name);
            lua_pop(L, 1);
            return;
        }
        ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    void clear() {
        if (L_ && ref_ != LUA_NOREF) {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
            ref_ = LUA_NOREF;
        }
    }

    bool isValid() const { return ref_ != LUA_NOREF; }

    // 调用: invokeVoid(self_ref, arg1, arg2, ...)
    // self_ref 是 Lua 实例在 registry 里的引用 ID
    template<typename... Args>
    void invokeVoid(int self_ref, Args&&... args) {
        if (!isValid()) return;

        int stack_before = lua_gettop(L_);

        // 1. 压入错误处理函数
        lua_pushcfunction(L_, &LuaFunctionRef::errHandler);
        int err_idx = lua_gettop(L_);

        // 2. 压入函数
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);

        // 3. 压入 self (通过 ref)
        lua_rawgeti(L_, LUA_REGISTRYINDEX, self_ref);

        // 4. 压入参数
        pushArgsToLua(L_, std::forward<Args>(args)...);

        // 5. lua_pcall
        int arg_count = 1 + static_cast<int>(sizeof...(Args));  // +1 for self
        if (lua_pcall(L_, arg_count, 0, err_idx) != 0) {
            std::fprintf(stderr, "[LuaFunctionRef] pcall error: %s\n",
                         lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }

        // 6. 弹出错误处理函数
        lua_remove(L_, err_idx);

        // 7. 栈平衡检查
        int stack_after = lua_gettop(L_);
        if (stack_after != stack_before) {
            std::fprintf(stderr, "[LuaFunctionRef] stack leak! before=%d after=%d\n",
                         stack_before, stack_after);
            lua_settop(L_, stack_before);
        }
    }

private:
    static int errHandler(lua_State* L) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[Lua Error] %s\n", err ? err : "(null)");
        return 1;
    }

    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};

// ==================================================================
// Part 5: 示例 C++ 类 MyObject
// ==================================================================
class MyObject {
public:
    MyObject() : name_("Unknown"), value_(0) {}
    explicit MyObject(const char* name) : name_(name ? name : ""), value_(100) {}

    void setValue(int v) { value_ = v; }
    int  getValue() const { return value_; }
    void add(int delta) {
        value_ += delta;
        if (value_ < 0) value_ = 0;
    }
    const char* getName() const { return name_.c_str(); }

private:
    std::string name_;
    int value_;
};

// ==================================================================
// Part 6: 主程序
// ==================================================================
int main() {
    // 1. 创建 Lua 虚拟机
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // 2. 把 MyObject 类注册给 Lua
    beginRegisterClass<MyObject>(L, "MyObject");
    REGISTER_MEMBER(L, "setValue", &MyObject::setValue);
    REGISTER_MEMBER(L, "getValue", &MyObject::getValue);
    REGISTER_MEMBER(L, "add",      &MyObject::add);
    REGISTER_MEMBER(L, "getName",  &MyObject::getName);
    endRegisterClass<MyObject>(L, "MyObject");

    // 3. 加载 Lua 脚本 script.lua
    if (luaL_dofile(L, "script.lua") != 0) {
        std::fprintf(stderr, "Error loading script.lua: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    // 4. 创建 Lua 侧的 App 实例 (App:new())
    lua_getglobal(L, "App");
    lua_getfield(L, -1, "new");
    lua_pushvalue(L, -2);          // App 作为 self
    if (lua_pcall(L, 1, 1, 0) != 0) {
        std::fprintf(stderr, "App:new() error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    int app_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);  // 弹出 App class table

    // 5. 预解析 initialize/tick 函数
    LuaFunctionRef fn_init, fn_tick;
    fn_init.init(L, "App", "initialize");
    fn_tick.init(L, "App", "tick");

    // 6. 调 initialize (传入 app_ref 作为 self)
    fn_init.invokeVoid(app_ref);

    // 7. 主循环: 模拟 300 帧
    const float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 300; ++frame) {
        fn_tick.invokeVoid(app_ref, dt);

        // 第 100 帧演示热更新: 把 App:tick 换成一个新版本
        if (frame == 100) {
            std::printf("\n========= [C++] Hot-reloading App:tick =========\n");
            const char* patch =
                "App.tick = function(self, dt)\n"
                "    self.tick_count = (self.tick_count or 0) + 1\n"
                "    if self.tick_count % 30 == 0 then\n"
                "        print('[Lua-HOTFIXED] tick ' .. self.tick_count)\n"
                "    end\n"
                "end\n";
            if (luaL_dostring(L, patch) != 0) {
                std::fprintf(stderr, "Hotfix error: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            // 重新拿新的 tick 引用
            fn_tick.clear();
            fn_tick.init(L, "App", "tick");
        }
    }

    // 8. 清理
    fn_init.clear();
    fn_tick.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, app_ref);
    lua_close(L);
    std::printf("\n[C++] Done.\n");
    return 0;
}
