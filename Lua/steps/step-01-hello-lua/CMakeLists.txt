cmake_minimum_required(VERSION 3.20)
project(step01_hello_lua CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 引入 vendored Lua (从项目根目录 vendor/lua-src)
# 注意: 用 CMAKE_CURRENT_SOURCE_DIR 从这个 step 目录往上找到 vendor/
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/lua-src
    ${CMAKE_BINARY_DIR}/lua-build
)

add_executable(hello_lua src/main.cpp)
target_link_libraries(hello_lua PRIVATE LuaJourney::Lua)

# 复制 scripts/ 到输出目录, 使 luaL_dofile("scripts/hello.lua") 能找到
add_custom_command(TARGET hello_lua POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:hello_lua>/scripts
)
