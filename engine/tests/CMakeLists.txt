# End-to-end test client for the data-driven server (needs gamedata/server, see docs/server-data.md).
add_executable(f3_gametest GameTest.cpp)
target_link_libraries(f3_gametest PRIVATE f3_common)

# Unit-style tests of the data layer (loader, override / merge, validation): no server, no original game files.
add_executable(f3_datatest DataTest.cpp ../server/Content.cpp ../server/DataStore.cpp)
target_include_directories(f3_datatest PRIVATE ../server)
target_link_libraries(f3_datatest PRIVATE f3_common nlohmann_json::nlohmann_json)
add_test(NAME f3_datatest COMMAND f3_datatest)

# Persistence layer: SQLite store, schema migrations, JSON import / export, corrupt files. No server, no game data.
add_executable(f3_dbtest DbTest.cpp ../server/Db.cpp ../server/Store.cpp ../server/Migrate.cpp ../server/Account.cpp)
target_include_directories(f3_dbtest PRIVATE ../server)
target_link_libraries(f3_dbtest PRIVATE f3_common nlohmann_json::nlohmann_json sqlite3_static)
add_test(NAME f3_dbtest COMMAND f3_dbtest)
