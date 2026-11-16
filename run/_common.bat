@echo off
rem Shared settings for the run\*.bat helpers. Called by the other scripts.
rem Repo root = parent of this folder.
pushd "%~dp0.."
set "ROOT=%CD%"
popd
set "BIN=%ROOT%\build\vs2022\bin"
set "SAVES=%ROOT%\saves"
