cmake_minimum_required(VERSION 3.10)
project(modelinfo CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_executable(modelinfo main.cpp)
if(MSVC)
  target_compile_options(modelinfo PRIVATE /W3)
  target_compile_definitions(modelinfo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# NOTE: the optional granny2.dll helpers (Oodle0-compressed GR2 object counts) only work in a 32-bit build:
#   cmake -S tools/modelinfo -B build/modelinfo32 -A Win32
