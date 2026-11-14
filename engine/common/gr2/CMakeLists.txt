# Granny 2 (.gr2) reader: Oodle0 decoder, container, type walker, skeleton/animation extraction. STL only.
# Vendored copy of tools/gr2lib (keep the two in sync; docs/formats/granny.md describes the format).
if(NOT TARGET f3_gr2)
  add_library(f3_gr2 STATIC oodle0.cpp gr2_file.cpp gr2_types.cpp gr2_scene.cpp)
  target_include_directories(f3_gr2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(f3_gr2 PRIVATE /W3 /utf-8 /permissive-)
    target_compile_definitions(f3_gr2 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  endif()
endif()
