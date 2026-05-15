# 准备环境

LuaJourney 设计为**零外部依赖**：Lua 源码已经内嵌在 `vendor/lua-src/`，
CMake 会自动编译它。你只需要一个 C++ 编译器 + CMake。

---

## 必备工具

### 1. C++ 编译器

**Windows**:
- Visual Studio 2022 社区版（推荐）
  - 下载：https://visualstudio.microsoft.com/downloads/
  - 安装时勾选 **使用 C++ 的桌面开发**

**Linux**:
```bash
sudo apt install build-essential
# 或
sudo apt install clang
```

**Mac**:
```bash
xcode-select --install
```

### 2. CMake 3.20+

**Windows**: Visual Studio 自带，也可独立安装 https://cmake.org/download/

**Linux**:
```bash
sudo apt install cmake
```

**Mac**:
```bash
brew install cmake
```

验证：
```bash
cmake --version
# >= 3.20
```

### 3. Ninja（可选，推荐）

Ninja 比默认生成器更快。Visual Studio 自带，或单独装：

```bash
# Linux
sudo apt install ninja-build

# Mac
brew install ninja
```

用 Ninja 时 CMake 加 `-G Ninja`。

---

## 第 14 步及以后：.NET 8 SDK

从 step-14 开始需要 C# 代码生成器。

- https://dotnet.microsoft.com/download

```bash
dotnet --version
# >= 8.0
```

Linux / Mac 也有对应安装包。

---

## 第 17 步及以后：LuaJIT（可选）

LuaJIT 是 Lua 5.1 的 JIT 实现。step-17 会演示从 Lua 5.4 切到 LuaJIT。

届时文档会说明编译方法。目前不需要。

---

## 快速验证

进入 step-01，直接编：

```bash
cd steps/step-01-hello-lua
cmake -B build
cmake --build build
```

Windows 输出在 `build/Debug/hello_lua.exe`，Linux/Mac 在 `build/hello_lua`。

运行应该看到：

```
[C++] Starting Lua VM...
[C++] Running inline code:
Hello from Lua (inline)
[C++] Running script file:
Hello from Lua (file)
Lua version: Lua 5.4
2 + 3 = 5
[C++] Shutting down Lua VM.
```

---

## 推荐的 IDE

### VS Code

装两个扩展即可：
- **C/C++** (Microsoft)
- **CMake Tools** (Microsoft)

直接打开任意 `steps/step-XX/` 目录，按 F7 构建。

### Visual Studio 2022

"打开本地文件夹"，选任意 step 目录。VS 会自动识别 CMakeLists.txt。

---

## 常见问题

### Q: CMake 报错 "cl.exe not found"

Windows 上需要在 **x64 Native Tools Command Prompt for VS 2022** 里运行 CMake。
或者让 CMake 自动探测：
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
```

### Q: Linux 报错找不到 `<string>` 等

需要 C++ 编译器：`sudo apt install g++`。

### Q: 构建非常慢

Lua 源码第一次编会编 32 个 .c 文件，约 10-30 秒。之后缓存命中只编改动部分。

### Q: 我已经系统装了 Lua，能用系统的吗？

不建议。内嵌的 Lua 保证版本一致、构建稳定。不想冲突的话就按 LuaJourney 的路径来。

---

## 目录总览

成功编完 step-01 后，你的工作目录应该类似：

```
LuaJourney/
├── vendor/
│   └── lua-src/              ← Lua 源码 (已给)
└── steps/
    └── step-01-hello-lua/
        ├── src/
        ├── scripts/
        ├── CMakeLists.txt
        └── build/             ← CMake 生成
            ├── Debug/          ← Windows
            │   └── hello_lua.exe
            └── (其它)
```

接下来进入 `steps/step-01-hello-lua/README.md`，正式开始。
