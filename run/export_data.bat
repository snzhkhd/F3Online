@echo off
rem Re-generate data\items, data\npcs\original.json etc. from the original game data.
rem Files that have lost their "generator" key (hand-edited) are skipped unless you add --force.
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --export-data %*
echo exit code: %ERRORLEVEL%
pause
