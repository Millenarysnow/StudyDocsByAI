# 架构演进图

这份图表展示同一个问题在不同阶段的**演化过程**。读完全部阶段后，你会发现"为什么工业级引擎要搞那么复杂的类"变得非常清楚。

## "C++ 对象如何传给 Lua"

```
step 05: userdata 里直接存 T*
  ┌──────────────┐
  │ userdata     │
  │   T* ptr ────┼──→ [T 对象]
  │   bool owned │
  └──────────────┘
  问题: 对象销毁后 ptr 变野指针


step 09: userdata 里存 InstanceDelegate
  ┌──────────────┐
  │ userdata     │
  │   Delegate* ─┼──→ [Delegate]
  └──────────────┘        ├─ type (Owned/Weak/Global/...)
                          ├─ type_id
                          └─ T* / buffer / ...
  问题: 对象销毁后还是野指针


step 10: Delegate 持有 Handle 而不是裸指针
  ┌──────────────┐
  │ userdata     │
  │   Delegate* ─┼──→ [Delegate]
  └──────────────┘        └─ Handle{slot, gen}
                               ↓ 查表
                              [ObjectPool]
                               ↓ gen 对得上才返回
                              [T 对象] / nullptr
  现在: 对象销毁后 Lua 访问返回 nil


step 11: Delegate 从对象池分配
  [DelegatePool_4]     [DelegatePool_8]     [DelegatePool_16]
      ↓                    ↓                    ↓
   <4 字节对象>         <8 字节对象>         <16 字节对象>
  减少堆分配开销


step 12: 弱表缓存避免重复创建 userdata
  C++ 侧: pushToLua(same_object) 两次
  Lua 侧: 应该拿到同一个 userdata
  
  做法: 维护一张弱表 cache[obj_key] = userdata
        每次 push 前先查表, 有就直接用
```

---

## "Lua 调 C++ 函数"

```
step 02: 手写 lua_CFunction
  int my_add(lua_State* L) {
      int a = lua_tointeger(L, 1);
      int b = lua_tointeger(L, 2);
      lua_pushinteger(L, a + b);
      return 1;
  }
  问题: 每个函数都要手写, 样板代码爆炸


step 03: TypeConverter 抽象类型转换
  int my_add(lua_State* L) {
      int a = TypeConverter<int>::fromLua(L, 1);
      int b = TypeConverter<int>::fromLua(L, 2);
      TypeConverter<int>::pushToLua(L, a + b);
      return 1;
  }
  好处: 类型转换和业务逻辑解耦
  问题: 调用还是手写


step 06: 模板自动生成 lua_CFunction
  REGISTER_FUNC(L, "add", &my_add);
  
  编译期展开:
  ┌─────────────────────────────────┐
  │ FunctionInfo<F>::param_num == 2 │
  │ 参数类型: <int, int>            │
  │ 返回类型: int                   │
  │ → 生成专用 entry<F, &my_add>()   │
  │   int entry(lua_State* L) {     │
  │     int a = ...::fromLua(L, 1); │
  │     int b = ...::fromLua(L, 2); │
  │     ...::pushToLua(L, (*f)(a,b));│
  │     return 1;                   │
  │   }                             │
  └─────────────────────────────────┘
  好处: 一行注册, 编译器生成代码


step 14: codegen 批量生成
  开发者只写:
    [[bind_to_lua]] int add(int, int);
  
  工具生成:
    REGISTER_FUNC(L, "add", &add);
  
  加类只要改标注, 不用改绑定代码
```

---

## "大规模类的绑定"

```
step 05: 手动注册每个成员
  beginRegister<MyClass>(L, "MyClass");
  REGISTER_MEMBER(L, "foo", &MyClass::foo);
  REGISTER_MEMBER(L, "bar", &MyClass::bar);
  REGISTER_MEMBER(L, "baz", &MyClass::baz);
  ...
  endRegister<MyClass>(L, "MyClass");

step 14: XML 元数据 + 代码生成
  my_class.meta:
    <class name="MyClass">
      <method name="foo" />
      <method name="bar" />
      <method name="baz" />
    </class>
  
  工具生成: register_my_class_to_lua.h
  C++ 只 include 即可

step 15: CMake 集成
  .meta 改动 → 自动跑生成器 → 自动重编
  开发者完全不用手工跑命令

step 16: 大规模
  50+ 类, 500+ 方法都按同一模式生成
  增量构建, 只重跑改动的部分
```

---

## "Lua 侧对象生命周期管理"

```
step 01-08: 没管
  Lua 里创建什么就是什么, 生命周期粗放


step 09: 基础 Delegate + type_id 检查
  错误类型转换会报 Lua 错误, 不 crash


step 10: Handle 系统
  C++ 对象销毁 → Lua 侧 isValid() 返回 false
  Lua 里可以安全检测


step 11+12: 池 + 缓存
  性能优化: 重复对象复用 userdata, 减少分配


step 13: 自定义 Lua 分配器
  Lua 的所有内存都走你的分配器
  可追踪、可统计、可限制上限
```

---

## "热更新"

```
step 07: 重新 dofile
  if (key_R_pressed) luaL_dofile(L, "script.lua");
  简单粗暴, 状态保留


step 18: 远程热更新
  开发机 ──TCP──→ 游戏进程
                    ↓
                热更新线程 enqueue → 主线程消费
                    ↓
                执行注入的 Lua 代码
                支持断点、调用栈 dump
  工业级: 线上 hotfix 不重启
```

---

## "代码生成工具"

```
step 05-13: 没有
  手写所有绑定, 量小的时候够用


step 14: C# 写的 codegen
  支持 XML 前端 (元数据风格)
  支持 C++ 宏标注前端 (Unreal 风格)
  三层架构: Frontend / IR / Backend


step 15: CMake 集成
  .cs 改 → rebuild 工具
  .meta 改 → 跑工具 → 生成代码 → 重编 C++


step 16: 大规模应用
  几十个类走同一套
  增量构建正确工作
```

---

## 整体视角

```
阶段 1-8:   学会"能跑"
阶段 9-13:  学会"别 crash"
阶段 14-16: 学会"大规模"
阶段 17-19: 学会"生产级"
```

每个阶段新增的是一个**解决特定问题的能力**。你写自己引擎时可以按需停在任意阶段。
