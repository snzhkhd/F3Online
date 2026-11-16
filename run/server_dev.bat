@echo off
rem Local server with dev chat commands (/goto /spawn /heal /reload ...) and guest login.
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --local --dev-kit --allow-guest --saves "%SAVES%" %*
pause
