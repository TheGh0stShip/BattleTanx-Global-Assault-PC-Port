@echo off
setlocal DisableDelayedExpansion

if "%~1"=="" goto picker
set "ROM=%~1"
if not exist "%ROM%" goto missing
"%~dp0BattleTanxGA.exe" --play "%ROM%"
goto finished

:picker
"%~dp0BattleTanxGA.exe"
goto finished

:missing
echo ROM not found: "%ROM%"
pause
exit /b 2

:finished
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" pause
exit /b %RESULT%
