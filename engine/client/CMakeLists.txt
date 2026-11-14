# Network + world-state client logic; no rendering, no raylib.
add_library(f3_client_core STATIC ClientCore.cpp)
target_link_libraries(f3_client_core PUBLIC f3_common)

# Headless bot used for smoke tests.
add_executable(f3_testclient TestClient.cpp)
target_link_libraries(f3_testclient PRIVATE f3_client_core)

if(F3_BUILD_CLIENT)
  add_executable(f3_client main.cpp RaylibRenderer.cpp)
  target_link_libraries(f3_client PRIVATE f3_client_core raylib)
endif()
