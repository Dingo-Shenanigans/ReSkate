@echo off
setlocal
if exist "%~dp0bmx_demo.exe" (
  start "" /D "%~dp0" "%~dp0bmx_demo.exe" --streets-observed-v2 --camera-behind
) else (
  start "" /D "%~dp0.." "%~dp0..\build\bmx_demo.exe" --streets-observed-v2 --camera-behind
)
endlocal
