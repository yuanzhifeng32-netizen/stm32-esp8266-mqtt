@echo off
rem stm32103 - compile helper (double-clickable)
rem Usage: build.cmd  [Debug|Release]
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
echo.
pause
exit /b %ERRORLEVEL%
