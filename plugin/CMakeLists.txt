cmake_minimum_required(VERSION 3.20)
project(RoutePlanner VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT, like the SPF template
add_compile_options(/utf-8)

add_library(RoutePlanner SHARED RoutePlanner.cpp)
target_include_directories(RoutePlanner PRIVATE SPF_API)
target_compile_definitions(RoutePlanner PRIVATE PLUGIN_NAME="RoutePlanner" PLUGIN_VERSION="${PROJECT_VERSION}" _AMD64_)

add_executable(routes_test routes_test.cpp)
