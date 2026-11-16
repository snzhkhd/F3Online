# Third-party dependencies, all fetched at configure time with pinned versions.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(enet
  URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Lua and sol2 have no (usable) CMake build: fetch only, build ourselves below.
FetchContent_Declare(lua
  URL https://www.lua.org/ftp/lua-5.4.6.tar.gz
  URL_HASH SHA256=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88
  SOURCE_SUBDIR no_cmake_here
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(sol2
  URL https://github.com/ThePhD/sol2/archive/refs/tags/v3.3.0.tar.gz
  SOURCE_SUBDIR no_cmake_here
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

# SQLite amalgamation (public domain): the server's persistence layer (docs/persistence.md). Only a zip with sqlite3.c / sqlite3.h,
# built as a static C library below. Offline: point FETCHCONTENT_SOURCE_DIR_SQLITE3 at an unpacked amalgamation directory.
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
  URL_HASH SHA256=ea170e73e447703e8359308ca2e4366a3ae0c4304a8665896f068c736781c651
  SOURCE_SUBDIR no_cmake_here
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# cpp-httplib (MIT, single header httplib.h): the loopback-only HTTP server behind the content editor (docs/editor.md). Header only: fetched,
# not built; only Editor.cpp includes it. Offline: point FETCHCONTENT_SOURCE_DIR_HTTPLIB at an unpacked cpp-httplib-0.18.3 directory.
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  URL_HASH SHA256=a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e
  SOURCE_SUBDIR no_cmake_here
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(enet lua sol2 nlohmann_json sqlite3 httplib)

add_library(httplib_headers INTERFACE)
target_include_directories(httplib_headers INTERFACE ${httplib_SOURCE_DIR})
if(WIN32)
  target_link_libraries(httplib_headers INTERFACE ws2_32)
endif()

target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*/(lua|luac)\.c$")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
set_target_properties(lua_static PROPERTIES C_STANDARD 99)
if(MSVC)
  target_compile_options(lua_static PRIVATE /W0)
endif()

add_library(sqlite3_static STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_static SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3_static PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_OMIT_DEPRECATED=1 SQLITE_OMIT_SHARED_CACHE=1 SQLITE_USE_URI=0)
set_target_properties(sqlite3_static PROPERTIES C_STANDARD 99)
if(MSVC)
  target_compile_options(sqlite3_static PRIVATE /W0)
endif()

add_library(sol2_headers INTERFACE)
target_include_directories(sol2_headers SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
target_compile_definitions(sol2_headers INTERFACE SOL_ALL_SAFETIES_ON=1)
target_link_libraries(sol2_headers INTERFACE lua_static)

if(F3_BUILD_CLIENT)
  # Prefer the local prebuilt raylib 5.5 (include/ + lib/raylib.lib, static MSVC build);
  # fall back to fetching raylib 5.5 sources if the local path is absent.
  set(F3_RAYLIB_DIR "F:/RayLib/raylib-5.5" CACHE PATH "Local raylib 5.5 (include/, lib/raylib.lib)")
  if(MSVC AND EXISTS "${F3_RAYLIB_DIR}/lib/raylib.lib" AND EXISTS "${F3_RAYLIB_DIR}/include/raylib.h")
    message(STATUS "raylib: using local prebuilt ${F3_RAYLIB_DIR}")
    add_library(raylib STATIC IMPORTED GLOBAL)
    set_target_properties(raylib PROPERTIES
      IMPORTED_LOCATION "${F3_RAYLIB_DIR}/lib/raylib.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${F3_RAYLIB_DIR}/include"
      INTERFACE_LINK_LIBRARIES "winmm;gdi32;user32;shell32")
  else()
    message(STATUS "raylib: local copy not found, fetching 5.5 from the network")
    FetchContent_Declare(raylib
      URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
  endif()
endif()
