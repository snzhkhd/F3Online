add_executable(f3_server
  main.cpp Server.cpp World.cpp Scripting.cpp)
target_link_libraries(f3_server PRIVATE f3_common)
