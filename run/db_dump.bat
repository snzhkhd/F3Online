@echo off
rem Print the SQLite database as JSON lines (safe while the server runs).
rem   db_dump.bat            all tables
rem   db_dump.bat mail       one table
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --saves "%SAVES%" --db-dump %*
pause
