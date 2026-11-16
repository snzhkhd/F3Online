@echo off
rem Local game server (localhost only), saves in .\saves.
rem Extra arguments are passed through, e.g.  server.bat --allow-guest
call "%~dp0_common.bat"
cd /d "%ROOT%"
"%BIN%\f3_server.exe" --local --saves "%SAVES%" %*
pause
