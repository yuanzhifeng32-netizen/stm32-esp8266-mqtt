@echo off
rem stm32103 - flash helper via ST-Link + OpenOCD (double-clickable)
rem Usage: flash.cmd  [-Config Debug|Release] [-Probe] [-Erase]
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" %*
echo.
pause
exit /b %ERRORLEVEL%
