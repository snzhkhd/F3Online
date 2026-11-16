add_executable(f3_server
  main.cpp Server.cpp World.cpp Scripting.cpp Game.cpp Doors.cpp Move.cpp Timing.cpp DataStore.cpp Account.cpp Persist.cpp Character.cpp Pip.cpp Amx.cpp AmxHost.cpp AmxActions.cpp AmxSpatial.cpp)
target_link_libraries(f3_server PRIVATE f3_common nlohmann_json::nlohmann_json)
