# Reader for the original game's files (resource.rht/grp, TGA, B3D, emap/mapdata/tre). STL only.
if(NOT TARGET f3_gamedata)
  add_library(f3_gamedata STATIC GameData.cpp Tga.cpp B3d.cpp MapFiles.cpp GuiFile.cpp)
  target_include_directories(f3_gamedata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(f3_gamedata PRIVATE /W3 /utf-8 /permissive-)
    target_compile_definitions(f3_gamedata PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  endif()

  # Command line checker: parses every b3d/tga/tre/emap/mapdata of an install.
  add_executable(f3_gdtool tool/gdtool.cpp)
  target_link_libraries(f3_gdtool PRIVATE f3_gamedata)
endif()
