# ============================================================================
# CMakeLists.txt
#
# 演示: 如何把 C# 代码生成器集成到 C++ 工程的构建流程里。
#
# 整体构建顺序 (CMake 根据依赖关系自动推导):
#
#   1. 编译 C# 工具 MetaBindingGen.exe  (dotnet build)
#   2. 运行 MetaBindingGen.exe 扫 src/ 生成 register_*.h
#   3. 编译 C++ 工程 my_engine.exe (包含 #include "register_*.h")
#
# 关键点: CMake 不是靠"你写的顺序"工作, 而是靠"你声明的依赖关系"。
# 只要依赖关系正确, 即使你在 CMakeLists.txt 里把第 3 步写在前面也没关系。
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(MyEngineWithCodegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------------
# 路径约定
# ----------------------------------------------------------------------------
set(CODEGEN_TOOL_DIR   "${CMAKE_SOURCE_DIR}/tools/MetaBindingGenCS")
set(CODEGEN_CSPROJ     "${CODEGEN_TOOL_DIR}/MetaBindingGen.csproj")
set(CODEGEN_OUTPUT_EXE "${CODEGEN_TOOL_DIR}/bin/Release/net8.0/MetaBindingGen.exe")
set(CPP_SRC_DIR        "${CMAKE_SOURCE_DIR}/src")
set(GENERATED_DIR      "${CMAKE_BINARY_DIR}/generated")

# 确保输出目录存在
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# ----------------------------------------------------------------------------
# 步骤 1: 编译 C# 代码生成器
# ----------------------------------------------------------------------------
# 这条规则说: "MetaBindingGen.exe 这个文件, 通过运行 dotnet build 产生,
#              如果任何 .cs 文件或 .csproj 变了, 就重新产生"。
#
# CMake 会自动在下次构建时检查 DEPENDS 列表里文件的修改时间, 决定是否重跑。

# 收集所有 C# 源文件作为依赖 (只要任何一个改了, 就要重建工具)
file(GLOB_RECURSE CODEGEN_CS_SOURCES
    "${CODEGEN_TOOL_DIR}/src/*.cs"
    "${CODEGEN_TOOL_DIR}/MetaBindingGen.csproj"
)

add_custom_command(
    OUTPUT  "${CODEGEN_OUTPUT_EXE}"
    COMMAND dotnet build "${CODEGEN_CSPROJ}" -c Release --nologo -v minimal
    DEPENDS ${CODEGEN_CS_SOURCES}
    COMMENT "[1/3] Building C# code generator..."
    VERBATIM
)

# 包装成一个命名的 target, 方便别的地方引用
add_custom_target(build_codegen
    DEPENDS "${CODEGEN_OUTPUT_EXE}"
)

# ----------------------------------------------------------------------------
# 步骤 2: 运行 C# 生成器, 产出 register_*.h
# ----------------------------------------------------------------------------
# 这条规则说: "register_my_widget_to_lua.h 这个文件, 通过运行 MetaBindingGen.exe
#              产生, 如果 my_widget.h 或 MetaBindingGen.exe 变了, 就重新产生"。

set(GENERATED_BINDING_HEADER
    "${GENERATED_DIR}/register_my_widget_to_lua.h"
)

add_custom_command(
    OUTPUT  "${GENERATED_BINDING_HEADER}"
    COMMAND "${CODEGEN_OUTPUT_EXE}"
            --frontend cpp
            --input   "${CPP_SRC_DIR}"
            --output  "${GENERATED_DIR}"
    DEPENDS "${CODEGEN_OUTPUT_EXE}"         # 依赖步骤 1 的产出
            "${CPP_SRC_DIR}/my_widget.h"    # 依赖 C++ 头文件 (改了要重新扫)
    COMMENT "[2/3] Generating Lua bindings..."
    VERBATIM
)

# 包装成 target
add_custom_target(generate_bindings
    DEPENDS "${GENERATED_BINDING_HEADER}"
)
add_dependencies(generate_bindings build_codegen)

# ----------------------------------------------------------------------------
# 步骤 3: 编译 C++ 引擎
# ----------------------------------------------------------------------------
# C++ 可执行文件依赖于生成的头文件。CMake 看到 add_executable 的源列表里有
# ${GENERATED_BINDING_HEADER}, 就知道这个文件必须先存在才能编译 main.cpp。
# (严格讲, 是 main.cpp #include 了它, 所以实际是 main.cpp 依赖它)

add_executable(my_engine
    "${CPP_SRC_DIR}/main.cpp"
    "${CPP_SRC_DIR}/my_widget.h"
    "${CPP_SRC_DIR}/binding_runtime.h"
    "${GENERATED_BINDING_HEADER}"           # 把生成的头加进来, 让 CMake 追踪依赖
)

# 关键: 让 C++ 能 #include "register_my_widget_to_lua.h"
target_include_directories(my_engine PRIVATE
    "${CPP_SRC_DIR}"
    "${GENERATED_DIR}"
)

# 确保编译 C++ 之前, generate_bindings 先完成
# (对大部分情况, 上面的 add_executable 里列了生成文件已经够了,
#  但显式加 add_dependencies 更稳妥)
add_dependencies(my_engine generate_bindings)

# ----------------------------------------------------------------------------
# 打印一些构建配置信息, 方便调试
# ----------------------------------------------------------------------------
message(STATUS "=======================================================")
message(STATUS "  MyEngineWithCodegen - build configuration")
message(STATUS "  -----------------------------------------")
message(STATUS "  Codegen tool: ${CODEGEN_OUTPUT_EXE}")
message(STATUS "  C++ source:   ${CPP_SRC_DIR}")
message(STATUS "  Generated:    ${GENERATED_DIR}")
message(STATUS "=======================================================")
