cmake_minimum_required(VERSION 3.20)
project(step03_type_converter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/lua-src
    ${CMAKE_BINARY_DIR}/lua-build
)

add_executable(type_converter src/main.cpp)
target_include_directories(type_converter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(type_converter PRIVATE LuaJourney::Lua)

add_custom_command(TARGET type_converter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:type_converter>/scripts
)
