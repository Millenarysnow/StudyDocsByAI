cmake_minimum_required(VERSION 3.20)
project(step04_function_ref CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/lua-src
    ${CMAKE_BINARY_DIR}/lua-build
)

add_executable(function_ref src/main.cpp)
target_include_directories(function_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(function_ref PRIVATE LuaJourney::Lua)

add_custom_command(TARGET function_ref POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:function_ref>/scripts
)
