@echo off
rem Validate data\*.json without starting the game (exit 0 = ok, 1 = errors, 2 = bad usage).
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --validate-data
echo exit code: %ERRORLEVEL%
pause
