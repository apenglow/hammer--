cmake_minimum_required(VERSION 3.24)
project(HammerPlusPlusTesting VERSION 0.15.26 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(HAMMER_BUILD_QT "Build the Qt 6 desktop application" ON)
#option(HAMMER_BUILD_TESTS "Build portable original-source tests" ON)
option(HAMMER_ENABLE_WAYLAND_CAPTURE "Build native Wayland Z-camera pointer locking" ON)
option(HAMMER_ENABLE_VULKAN_RAY_TRACING "Build Vulkan KHR ray-traced preview when dependencies are available" ON)

if(HAMMER_BUILD_TESTS)
    include(CTest)
endif()

add_subdirectory(src/linux_qt)
