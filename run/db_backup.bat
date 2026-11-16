@echo off
rem Consistent backup of saves\f3.db into saves\backup_YYYYMMDD_HHMMSS.db (works while the server runs).
call "%~dp0_common.bat"
cd /d "%ROOT%"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%i"
"%BIN%\f3_server.exe" --saves "%SAVES%" --db-backup "%SAVES%\backup_%STAMP%.db"
echo exit code: %ERRORLEVEL%
pause
