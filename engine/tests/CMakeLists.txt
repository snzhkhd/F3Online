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

# Localization layer: locale loader, fallback chain, placeholders, --check-locale report, UTF-8 helpers and UTF-8 safe wrapping. No game data.
add_executable(f3_localetest LocaleTest.cpp)
target_include_directories(f3_localetest PRIVATE ../client)
target_link_libraries(f3_localetest PRIVATE f3_locale f3_common nlohmann_json::nlohmann_json)
add_test(NAME f3_localetest COMMAND f3_localetest)

# Content editor core (docs/editor.md): request handler, path confinement, token / host checks, atomic saves, schema vs. validator drift. No server, no sockets, no game data.
add_executable(f3_editortest EditorTest.cpp ../server/EditorCore.cpp ../server/EditorSchema.cpp ../server/DataFormat.cpp ../server/LocaleCheck.cpp ../server/Content.cpp ../server/DataStore.cpp)
target_include_directories(f3_editortest PRIVATE ../server)
target_link_libraries(f3_editortest PRIVATE f3_locale f3_common nlohmann_json::nlohmann_json)
add_test(NAME f3_editortest COMMAND f3_editortest)
