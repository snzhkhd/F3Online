@echo off
rem Dev server + browser content editor. Open the printed  http://127.0.0.1:4041/?token=...  address.
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --local --dev-kit --allow-guest --editor --editor-port 4041 --saves "%SAVES%" %*
pause
