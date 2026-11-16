@echo off
rem Game client.  client.bat [name] [extra args]   (default name: Alice)
rem First time with a new name add --register and --password, e.g.
rem   client.bat Bob --register --password secret
call "%~dp0_common.bat"
cd /d "%ROOT%"
set "NAME=%~1"
if "%NAME%"=="" set "NAME=Alice"
shift
"%BIN%\f3_client.exe" --name %NAME% %1 %2 %3 %4 %5 %6 %7 %8 %9
pause
