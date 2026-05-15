# ============================================================================
# vendor/lua-src/CMakeLists.txt
#
# 用 CMake 编译 Lua 5.4.7 源码为一个静态库 target "lua_static"。
# 使用者链接这个 target 即可获得 Lua 的所有 C API。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(lua_vendor C)

# Lua 5.4 官方源码列表 (排除 lua.c 和 luac.c, 它们是独立可执行文件)
set(LUA_CORE_SOURCES
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

add_library(lua_static STATIC ${LUA_CORE_SOURCES})
target_include_directories(lua_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Linux 需要 dl 和 m (dynamic loader + math)
if(UNIX AND NOT APPLE)
    target_link_libraries(lua_static PUBLIC dl m)
elseif(APPLE)
    target_link_libraries(lua_static PUBLIC m)
endif()

# 在 Windows 上 Lua 的 luaconf.h 已经自动定义 LUA_USE_WINDOWS, 不需要额外加。
# Linux / Mac 需要显式定义以启用 dlopen 等。
if(APPLE)
    target_compile_definitions(lua_static PUBLIC LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(lua_static PUBLIC LUA_USE_LINUX)
endif()

# MSVC 下关闭 "与 POSIX 名冲突" 的警告 (Lua 源码用 fopen 等被 MSVC 标记)
if(MSVC)
    target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 给用户方便用的 alias target
add_library(LuaJourney::Lua ALIAS lua_static)
