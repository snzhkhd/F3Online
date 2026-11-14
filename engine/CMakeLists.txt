# --- shared: protocol, serialization, math/types, Lua binding helper ---
add_library(f3_common INTERFACE)
target_include_directories(f3_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f3_common INTERFACE sol2_headers enet)
if(MSVC)
  target_compile_options(f3_common INTERFACE /W3 /utf-8 /permissive-)
  target_compile_definitions(f3_common INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

add_subdirectory(server)
add_subdirectory(client)
