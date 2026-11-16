@echo off
rem Translation coverage report (missing / unused keys per language).
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --check-locale --strict
echo exit code: %ERRORLEVEL%
pause
