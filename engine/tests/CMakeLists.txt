# End-to-end test client for the data-driven server (needs gamedata/server, see docs/server-data.md).
add_executable(f3_gametest GameTest.cpp)
target_link_libraries(f3_gametest PRIVATE f3_common)
