cmake_minimum_required(VERSION 3.16)
project(gr2lib CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library: Oodle0 decoder + GR2 parser + scene extraction (no dependencies)
add_library(gr2lib STATIC oodle0.cpp gr2_file.cpp gr2_types.cpp gr2_scene.cpp)
target_include_directories(gr2lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# CLI: skeleton/animation summary + JSON export
add_executable(gr2dump gr2dump.cpp)
target_link_libraries(gr2dump gr2lib)

# Verifier: native decoder vs original granny2.dll (build with -A Win32 for the DLL comparison)
add_executable(gr2verify gr2verify.cpp)
target_link_libraries(gr2verify gr2lib)
