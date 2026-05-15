cmake_minimum_required(VERSION 3.15)
project(MiniLuaBind CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 找 Lua ---
# 优先方案 1: 通过 find_package (vcpkg / package manager 安装的 Lua)
# 优先方案 2: 手动指定 LUA_INCLUDE_DIR 和 LUA_LIBRARIES
find_package(Lua 5.1 QUIET)

if(NOT LUA_FOUND)
    message(STATUS "Lua not found via find_package, please specify manually:")
    message(STATUS "  cmake -DLUA_INCLUDE_DIR=... -DLUA_LIBRARIES=...")
endif()

add_executable(demo demo.cpp)
target_include_directories(demo PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(demo PRIVATE ${LUA_LIBRARIES})

# 复制 script.lua 到输出目录
add_custom_command(TARGET demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/script.lua
        $<TARGET_FILE_DIR:demo>/script.lua
)

message(STATUS "Lua include: ${LUA_INCLUDE_DIR}")
message(STATUS "Lua libs:    ${LUA_LIBRARIES}")
