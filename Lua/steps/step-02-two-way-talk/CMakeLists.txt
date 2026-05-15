cmake_minimum_required(VERSION 3.20)
project(step02_two_way_talk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/lua-src
    ${CMAKE_BINARY_DIR}/lua-build
)

add_executable(two_way_talk src/main.cpp)
target_link_libraries(two_way_talk PRIVATE LuaJourney::Lua)

add_custom_command(TARGET two_way_talk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:two_way_talk>/scripts
)
