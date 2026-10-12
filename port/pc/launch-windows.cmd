@echo off
setlocal
set "ROM=%~1"

if not defined ROM (
    echo BattleTanx: Global Assault requires your own USA Rev 0 .z64 ROM.
    echo You can also drag the ROM onto this launcher.
    set /p "ROM=ROM path: "
)

if not exist "%ROM%" (
    echo ROM not found: %ROM%
    pause
    exit /b 2
)

"%~dp0BattleTanxGA.exe" --play "%ROM%"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" pause
exit /b %RESULT%
