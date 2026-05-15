# LuaJourney

> 一份从零开始、逐步深入的 C++/Lua 集成学习项目。
> 目标是让你能够在自己的 C++ 引擎中搭建出一套工业级的 Lua 脚本系统。

## 它是什么

这是一个**分阶段解锁**的学习工程，风格类似 Codecrafters：
- 每个阶段是一个独立的小项目，可单独编译运行
- 每一步只解决一个新问题，代码量增幅可控
- 每个阶段都有 README 解释"这一步为什么存在"
- 后续阶段基于前一阶段做**重构 / 扩展**，最终达到工业级完成度

## 最终能达成什么

走完全部阶段后，你将拥有：

- 一套完整的 C++/Lua 双向绑定系统
- 支持模板元编程自动生成 lua_CFunction
- 对象生命周期管理（Handle、对象池、弱表缓存）
- C# 代码生成器（从 XML 或 C++ 标注自动生成绑定）
- 热更新系统（可远程调试注入 Lua）
- LuaJIT 集成 + 异常安全
- 工业级内存分配策略 + 内存追踪

这套系统足够支撑 AAA 级游戏引擎的 Lua 脚本层。

## 零配置启动

为了让你能**立刻开始**，LuaJourney 项目已经把 Lua 源码内嵌在 `vendor/lua/` 目录。
你**不需要**装 vcpkg、也不需要系统里装 Lua。

只要有 **CMake + MSVC**（或 GCC/Clang），就可以编。

```bash
cd steps/step-01-hello-lua
cmake -B build
cmake --build build
./build/hello_lua     # Linux/Mac
build\Debug\hello_lua.exe   # Windows (取决于 config)
```

---

## 学习路径总览

| 阶段 | 标题 | 难度 | 核心概念 |
|-----|------|-----|---------|
| 01  | Hello Lua                 | ⭐       | Lua VM, 嵌入解释器 |
| 02  | Two-way Talk              | ⭐       | C++↔Lua 双向调用 |
| 03  | Type Converter            | ⭐⭐     | 类型转换模板 |
| 04  | Function Ref              | ⭐⭐     | lua_pcall, 错误处理, 栈平衡 |
| 05  | Bind a Class              | ⭐⭐⭐   | metatable, userdata |
| 06  | Template Magic            | ⭐⭐⭐⭐ | 参数包展开, 编译期绑定 |
| 07  | Hot Reload                | ⭐⭐     | 运行时重载脚本 |
| 08  | Main Loop Driven          | ⭐⭐     | Lua 驱动游戏逻辑 |
| 09  | Instance Delegate         | ⭐⭐⭐⭐ | 对象生命周期抽象 |
| 10  | Handle System             | ⭐⭐⭐⭐ | 弱引用句柄 |
| 11  | Userdata Pool             | ⭐⭐⭐⭐ | 按大小分池 |
| 12  | Weak Table Cache          | ⭐⭐⭐   | Lua 侧对象去重 |
| 13  | Custom Allocator          | ⭐⭐     | 内存追踪 |
| 14  | Codegen - C# Tool         | ⭐⭐⭐   | 反射式代码生成 |
| 15  | CMake Integration         | ⭐⭐⭐   | 工具接入构建链 |
| 16  | Big Scale                 | ⭐⭐⭐⭐ | 大规模自动绑定 |
| 17  | LuaJIT Upgrade            | ⭐⭐⭐   | 切换 LuaJIT |
| 18  | Live Debug                | ⭐⭐⭐⭐⭐ | 远程热更新 |
| 19  | Production Polish         | ⭐⭐⭐   | GC/线程/CRT |

## 目录结构

```
LuaJourney/
├── README.md                  ← 本文件 (路径总览)
├── docs/
│   ├── 00-prerequisites.md    ← 准备环境
│   ├── 01-philosophy.md       ← 整体设计哲学
│   ├── 02-glossary.md         ← 术语表
│   └── architecture-evolution.md ← 架构演进图
├── vendor/
│   └── lua/                   ← Lua 5.4 源码 (CMake 自动编)
├── steps/
│   ├── step-01-hello-lua/
│   │   ├── README.md          ← 这一步的目标/原理
│   │   ├── CMakeLists.txt
│   │   ├── src/
│   │   └── scripts/
│   ├── ...
│   └── step-19-production-polish/
└── tools/
    └── shared-scripts/        ← 所有阶段共用的辅助脚本
```

每一步都可以独立 `cmake -S step-XX -B build` 构建运行。

## 如何使用

### 方式 1：从头走一遍（推荐新手）

```bash
cd steps/step-01-hello-lua
cat README.md                    # 读目标
cmake -B build
cmake --build build
./build/hello_lua
```

理解后进入下一步。

### 方式 2：直接跳到感兴趣的阶段

每个阶段都是独立的，可以跳过前面直接看。但建议至少读那一步的 README 了解前置知识。

### 方式 3：对比学习

步骤的文件名大多相似，可以 `diff` 两个相邻步骤，看新增了什么。

## 环境要求

- **CMake 3.20+**
- **C++17** 支持的编译器（MSVC 2019+ / GCC 8+ / Clang 9+）
- （第 14 步起）**.NET 8 SDK**

Lua 5.4 源码已内嵌，无需单独安装。

## 原则

1. **每一步都可运行**。
2. **每一步都解释 why**。
3. **代码量可控**。单步新增通常 < 300 行。
4. **先跑起来再优化**。
5. **最终对齐工业级引擎完成度**。

## 下一步

- 第一次接触：从 `docs/00-prerequisites.md` 开始
- 直接上手：进 `steps/step-01-hello-lua/` 看 README
