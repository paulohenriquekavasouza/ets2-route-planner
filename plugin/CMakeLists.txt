cmake_minimum_required(VERSION 3.20)
project(RoutePlanner VERSION 4.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT, like the SPF template
add_compile_options(/utf-8)

# Host: the DLL SPF loads (manifest, keys, windows). Changing it needs SPF -> "Recarregar Framework".
add_library(RoutePlanner SHARED Host.cpp)
target_include_directories(RoutePlanner PRIVATE SPF_API)
target_compile_definitions(RoutePlanner PRIVATE PLUGIN_NAME="RoutePlanner" PLUGIN_VERSION="${PROJECT_VERSION}" _AMD64_)

# Core: all the logic; the host hot-reloads it when the file changes (see Host.cpp).
add_library(RoutePlannerCore SHARED Core.cpp)
target_include_directories(RoutePlannerCore PRIVATE SPF_API)
target_compile_definitions(RoutePlannerCore PRIVATE PLUGIN_NAME="RoutePlanner" _AMD64_)

add_executable(routes_test routes_test.cpp)
