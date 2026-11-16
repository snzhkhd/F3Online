@echo off
rem Full automated smoke test (takes several minutes; starts its own test servers on high ports).
call "%~dp0_common.bat"
cd /d "%ROOT%"
powershell -NoProfile -ExecutionPolicy Bypass -File engine\tests\smoke_test.ps1 -BinDir build\vs2022\bin %*
echo.
echo exit code: %ERRORLEVEL%
pause
