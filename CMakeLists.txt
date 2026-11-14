cmake_minimum_required(VERSION 3.20)
project(F3Online LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

option(F3_BUILD_CLIENT "Build the raylib client (f3_client)" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/engine/cmake)
include(Dependencies)

add_subdirectory(engine)
add_subdirectory(tools/dataimport)  # f3_dataimport: original data -> gamedata/server

enable_testing()
add_test(NAME smoke
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/engine/tests/smoke_test.ps1
          -BinDir ${CMAKE_BINARY_DIR}/bin -ScriptsDir ${CMAKE_SOURCE_DIR}/scripts)
