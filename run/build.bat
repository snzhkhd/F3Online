@echo off
rem Build everything (Release). Close f3_server.exe / f3_client.exe first.
call "%~dp0_common.bat"
cd /d "%ROOT%"
cmake --build build/vs2022 --config Release
if errorlevel 1 (echo. & echo BUILD FAILED) else (echo. & echo BUILD OK)
pause
