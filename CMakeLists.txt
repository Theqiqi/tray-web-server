cmake_minimum_required(VERSION 3.16)
project(tray_web_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tray_web_server
    src/main.cpp
    src/json.cpp
)

target_include_directories(tray_web_server PRIVATE include)

find_package(Threads REQUIRED)
target_link_libraries(tray_web_server PRIVATE Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tray_web_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
