Helper scripts (double-click or run from a terminal). All use build\vs2022\bin and .\saves.

build.bat            build everything (Release); close server/client first
server.bat           local server (localhost only)
server_dev.bat       local server + dev chat commands + guest login
server_editor.bat    dev server + browser content editor (open the printed address with the token)
client.bat [name]    game client; new account: client.bat Bob --register --password secret
smoke.bat            full automated test run (several minutes)
validate_data.bat    check data\*.json
check_locale.bat     translation coverage
export_data.bat      regenerate exported data from the original game files
db_dump.bat [table]  print the SQLite database
db_backup.bat        backup saves\f3.db to saves\backup_<time>.db

Typical session: build.bat -> server_dev.bat -> client.bat Alice (and client.bat Bob in a second window).
