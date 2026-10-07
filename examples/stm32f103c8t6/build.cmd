@echo off
rem stm32103 - build helper (double-clickable)
rem Requires GNU Make on PATH (mingw32-make). See README.md for install hints.
rem
rem Usage: build.cmd                  -> Debug   (-O0 -g3 -DDEBUG, default)
rem        build.cmd CONFIG=Release   -> Release (-Os -g0)
rem        build.cmd -j8              -> parallel build
rem        build.cmd clean            -> remove build\ (both Debug and Release)
setlocal

where mingw32-make >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] mingw32-make not found on PATH.
    echo         Install MinGW or add its bin directory to PATH, then retry.
    echo.
    pause
    exit /b 1
)

mingw32-make %*
set ERR=%ERRORLEVEL%
echo.
pause
exit /b %ERR%