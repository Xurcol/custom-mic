@echo off
:: uninstall.bat — Remove Custom Mic Virtual Audio Cable driver
:: Must be run as Administrator.

setlocal
cd /d "%~dp0"

echo ============================================================
echo   Custom Mic Virtual Audio Cable — Uninstaller
echo ============================================================
echo.

:: Check admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and select "Run as administrator".
    pause
    exit /b 1
)

echo Removing driver...

:: Remove with pnputil
pnputil /delete-driver vaudio.inf /uninstall /force >nul 2>&1

:: Also try devcon
where devcon >nul 2>&1
if %errorLevel% equ 0 (
    devcon remove ROOT\VaudioCable >nul 2>&1
)

echo.
echo ============================================================
echo   Driver removed.
echo   The virtual audio devices should no longer appear.
echo ============================================================
echo.
pause
