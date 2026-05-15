# Step 03: Type Converter

**难度**: ⭐⭐
**耗时**: 1-2 小时
**前置**: step-02

---

## 这一步的目标

> 抽象出 **TypeConverter<T>**，让 C++↔Lua 的类型转换自动化。

这是 LuaJourney 里的第一次**重构**。我们会**重写 step-02 的一部分代码**，但不引入任何新功能。

---

## 上一步留下的问题

step-02 里每个 C 函数都长这样：

```cpp
int my_add(lua_State* L) {
    int a = (int)lua_tointeger(L, 1);
    int b = (int)lua_tointeger(L, 2);
    lua_pushinteger(L, a + b);
    return 1;
}

int my_concat(lua_State* L) {
    const char* a = lua_tostring(L, 1);
    const char* b = lua_tostring(L, 2);
    std::string result = std::string(a) + b;
    lua_pushstring(L, result.c_str());
    return 1;
}

int my_and(lua_State* L) {
    bool a = lua_toboolean(L, 1);
    bool b = lua_toboolean(L, 2);
    lua_pushboolean(L, a && b);
    return 1;
}
```

**每个类型的 push/read 都要记一遍 API 名字**：
- int → `lua_tointeger` / `lua_pushinteger`
- string → `lua_tostring` / `lua_pushstring`
- bool → `lua_toboolean` / `lua_pushboolean`
- float → `lua_tonumber` / `lua_pushnumber`
- ...

这是样板代码。样板代码的特征是**格式固定、内容仅类型不同**。这种东西在 C++ 里有一个完美的工具叫**模板特化**。

---

## 这一步做的事

定义一个模板类：

```cpp
template<typename T, typename = void>
struct TypeConverter;
```

为每个支持的类型**特化**它：

```cpp
template<>
struct TypeConverter<int> {
    static void PushToLua(lua_State* L, int v) { lua_pushinteger(L, v); }
    static int  FromLua(lua_State* L, int idx) { return (int)lua_tointeger(L, idx); }
};

template<>
struct TypeConverter<std::string> {
    static void PushToLua(lua_State* L, const std::string& v) { lua_pushstring(L, v.c_str()); }
    static std::string FromLua(lua_State* L, int idx) {
        const char* s = lua_tostring(L, idx);
        return s ? std::string(s) : std::string();
    }
};

// ... 等等
```

以后 C 函数都写成：

```cpp
int my_add(lua_State* L) {
    int a = TypeConverter<int>::FromLua(L, 1);
    int b = TypeConverter<int>::FromLua(L, 2);
    TypeConverter<int>::PushToLua(L, a + b);
    return 1;
}
```

看起来**没变短**啊？别急，这只是第一步。真正的威力要到 step-06 才能看到：那时我们会用 TypeConverter 配合参数包展开，让整个 C 函数**自动生成**。

---

## 为什么模板特化？

你可能会问：为什么不用 if-else 分支判断类型？

```cpp
// 反例: 运行时分派
void PushToLua(lua_State* L, void* value, const std::type_info& type) {
    if (type == typeid(int))    { lua_pushinteger(L, *(int*)value); }
    else if (type == typeid(float)) { lua_pushnumber(L, *(float*)value); }
    // ...
}
```

这个设计的问题：
1. **慢**：运行时 typeid 比较
2. **类型不安全**：`void*` + `typeid`，编译期检查不出错
3. **无法静态绑定**：自动绑定那一步（step-06）要求编译期已知类型

**模板特化在编译期就选好了对应版本**。零运行时开销，完全类型安全。

---

## 新概念：SFINAE

你会在代码里看到这个声明：

```cpp
template<typename T, typename = void>
struct TypeConverter;
```

那个 `typename = void` 是为了以后（step-09 起）可以给"所有类类型"一次性特化：

```cpp
template<typename T>
struct TypeConverter<T*, std::enable_if_t<std::is_class<T>::value>> {
    // 对所有指向类的指针都适用
};
```

`std::enable_if_t` 就是所谓的 **SFINAE**（Substitution Failure Is Not An Error）。现在先不用懂全部细节，留着这个钩子以后加。

---

## 代码结构

```
step-03-type-converter/
├── README.md
├── CMakeLists.txt
├── src/
│   ├── type_converter.h   ← 新文件! TypeConverter 定义
│   └── main.cpp           ← 重构: 使用 TypeConverter
└── scripts/
    └── game.lua           ← 和 step-02 基本一样
```

---

## 构建 & 运行

```bash
cd step-03-type-converter
cmake -B build
cmake --build build

# Windows:
build/Debug/type_converter.exe
# Linux/Mac:
build/type_converter
```

期望输出和 step-02 一致。这一步只重构，不改行为。

---

## 关键观察点

### 1. 对比 step-02 和 step-03 的 `my_add`

**step-02**:
```cpp
int a = (int)lua_tointeger(L, 1);
int b = (int)lua_tointeger(L, 2);
lua_pushinteger(L, a + b);
```

**step-03**:
```cpp
int a = TypeConverter<int>::FromLua(L, 1);
int b = TypeConverter<int>::FromLua(L, 2);
TypeConverter<int>::PushToLua(L, a + b);
```

**读代码时你不用再记"int 对应哪个 Lua API"了**。所有类型走同一个调用形式。

### 2. 换类型几乎零成本

试着把 `my_add` 改成处理 `float`：

```cpp
float a = TypeConverter<float>::FromLua(L, 1);
float b = TypeConverter<float>::FromLua(L, 2);
TypeConverter<float>::PushToLua(L, a + b);
```

**改 3 个类型参数就完了**。对比 step-02 要改 3 个 API 名字加若干强转。

### 3. 添加新类型是统一的

支持 `double`？只要加一个特化：

```cpp
template<>
struct TypeConverter<double> {
    static void PushToLua(lua_State* L, double v) { lua_pushnumber(L, v); }
    static double FromLua(lua_State* L, int idx) { return lua_tonumber(L, idx); }
};
```

加完所有地方都能用。**不需要改任何现有代码**。

---

## 动手挑战

1. **挑战 1**：加 `TypeConverter<double>` 特化，让 `game.lua` 调一个返回浮点数的 C 函数。

2. **挑战 2**：给 `TypeConverter<const char*>` 做一个特化（和 `std::string` 类似但不分配）。

3. **挑战 3**：试着用 TypeConverter 重写 `CallLuaToUpper`。封装一个泛型版本：
   ```cpp
   template<typename Ret, typename Arg>
   Ret CallLua(lua_State* L, const char* func_name, Arg arg);
   ```
   思考：多参数怎么办？这个问题 step-06 会解决。

---

## 架构演进视角

```
step-02: 每个 C 函数手写 lua_toxxx / lua_pushxxx
            ↓
step-03: TypeConverter 统一类型转换接口
            ↓ (step-06 会来)
step-06: 用 TypeConverter 配合模板自动生成 C 函数
```

step-03 本身看起来改进不大，但它是 step-06 的**前置重构**。你会在 step-06 看到它的真正威力。

---

## 新概念清单

- **模板特化**：为特定类型定制模板行为
- **类型擦除**：调用者不用关心类型细节，统一接口
- **SFINAE 钩子**：`typename = void` 占位以后特化用
- **重构的价值**：代码量没减，可读性和可扩展性大幅提升

---

## 下一步

step-04 要解决另一个痛点：**C++ 调 Lua 函数时，错误处理和栈管理依然是手写的**。我们会把这一块也封装成一个 `LuaFunctionRef` 类。
