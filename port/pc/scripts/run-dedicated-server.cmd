@echo off
setlocal
if "%~1"=="" (
    echo Drag your verified BattleTanx: Global Assault USA .z64 ROM onto this file.
    echo.
    pause
    exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dedicated-server.ps1" -Rom "%~f1"
set "BTGA_EXIT=%ERRORLEVEL%"
if not "%BTGA_EXIT%"=="0" pause
exit /b %BTGA_EXIT%
