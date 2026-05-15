# MetaBindingGenCS — C# 版本的 C++/Lua 绑定代码生成器

**同一套架构用 C# 重写的版本**。完全等价于 Python 版本，但：
- 类型安全（编译期检查类型错误）
- 纯 .NET，无需 Python 环境
- 生成 .exe 可以直接嵌入 CMake 构建流程
- Visual Studio 里可以直接调试、断点、重构

## 架构

```
    输入 (两种格式)              IR (统一中间表示)          输出 (统一目标代码)
    ─────────────               ─────────────────         ─────────────────
    my_widget.meta (XML)      ┐                                                
                              ├──> ClassMeta ──────────> register_my_widget_to_lua.h
    my_widget.h (宏标注)      ┘                                                
    
    XmlFrontend.cs             MetaIR/*.cs              LuaBackend.cs
    CppFrontend.cs
```

三层彻底解耦：
- 前端实现 `IFrontend` 接口，把任意输入格式转成 `List<ClassMeta>`
- IR 就是 `ClassMeta` / `MethodMeta` / `MemberMeta` 三个 record
- 后端实现 `IBackend` 接口，消费 `ClassMeta` 生成目标代码

**切换/新增前端/后端都只需要改对应层，其他层一行不动**。

## 环境要求

- .NET 8.0 SDK（或 6.0+，项目用 C# 10+ 特性）
- 下载: https://dotnet.microsoft.com/download

## 构建与运行

```bash
cd MetaBindingGenCS

# 构建
dotnet build

# 方案 A: 从 XML 生成
dotnet run -- --frontend xml --input sample_xml --output generated_xml

# 方案 B: 从 C++ 宏标注生成
dotnet run -- --frontend cpp --input sample_cpp --output generated_cpp

# 对比两个输出 (关键内容应该完全一致)
fc generated_xml\register_my_widget_to_lua.h generated_cpp\register_my_widget_to_lua.h
```

## 集成到 CMake

构建后会得到 `bin/Debug/net8.0/MetaBindingGen.exe` (或 Release)。
在你的 C++ 工程的 CMakeLists.txt 里：

```cmake
# 在构建前自动扫源码生成绑定代码
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/register_my_widget_to_lua.h
    COMMAND ${META_BIND_GEN_EXE}
            --frontend cpp
            --input ${CMAKE_SOURCE_DIR}/src
            --output ${CMAKE_BINARY_DIR}/generated
    DEPENDS ${CMAKE_SOURCE_DIR}/src/my_widget.h
)
```

## 扩展方式

想加 JSON 前端？

1. 新建 `src/Frontends/JsonFrontend.cs`，实现 `IFrontend`
2. 在 `Program.cs` 里注册它
3. 后端一行不用改

想加第二个后端（比如 pybind11 绑定）？

1. 新建 `src/Backends/PyBindBackend.cs`，实现 `IBackend`
2. 在 `Program.cs` 里注册它
3. 前端一行不用改
