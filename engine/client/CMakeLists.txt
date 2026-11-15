# Original-game file readers (resource.rht/grp, TGA, B3D, map files).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/gamedata ${CMAKE_BINARY_DIR}/gamedata)

# Granny 2 reader (skeletons + animations for critters).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/gr2 ${CMAKE_BINARY_DIR}/gr2)

# Network + world-state client logic; no rendering, no raylib.
add_library(f3_client_core STATIC ClientCore.cpp ProtoDb.cpp Settings.cpp)
target_link_libraries(f3_client_core PUBLIC f3_common nlohmann_json::nlohmann_json)

# Headless bot used for smoke tests.
add_executable(f3_testclient TestClient.cpp)
target_link_libraries(f3_testclient PRIVATE f3_client_core)

# Dev tool: dumps b3d bones/weights against the Granny skeleton.
add_executable(f3_skinprobe tools/skinprobe.cpp)
target_link_libraries(f3_skinprobe PRIVATE f3_gamedata f3_gr2)

if(F3_BUILD_CLIENT)
  add_executable(f3_client main.cpp RaylibRenderer.cpp GameAssets.cpp MapView.cpp CritterRig.cpp OrigUi.cpp OrigUiChar.cpp)
  target_link_libraries(f3_client PRIVATE f3_client_core f3_gamedata f3_gr2 raylib)
endif()
