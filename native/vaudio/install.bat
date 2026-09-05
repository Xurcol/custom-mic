@echo off
:: install.bat — Install Custom Mic Virtual Audio Cable driver
:: Must be run as Administrator.
::
:: IMPORTANT: On Windows 10/11 with Secure Boot, unsigned drivers
:: require test-signing mode. Run this once (then reboot):
::   bcdedit /set testsigning on
::
:: To disable test signing later:
::   bcdedit /set testsigning off

setlocal
cd /d "%~dp0"

echo ============================================================
echo   Custom Mic Virtual Audio Cable — Installer
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

:: Check driver file exists
if not exist "vaudio.sys" (
    echo ERROR: vaudio.sys not found.
    echo Build the driver with WDK first. See BUILD.md for instructions.
    pause
    exit /b 1
)

:: Check if test signing is enabled (needed for unsigned drivers)
for /f "tokens=*" %%a in ('bcdedit /enum {current} ^| findstr /i "testsigning"') do set TSLINE=%%a
echo %TSLINE% | findstr /i "Yes" >nul 2>&1
if %errorLevel% neq 0 (
    echo.
    echo WARNING: Test signing does not appear to be enabled.
    echo If the driver is not WHQL-signed, you need to enable test signing:
    echo.
    echo   bcdedit /set testsigning on
    echo.
    echo Then reboot before installing the driver.
    echo.
    choice /M "Continue anyway?"
    if errorlevel 2 exit /b 1
)

echo.
echo Installing driver...

:: Create a devnode for our virtual device
pnputil /add-driver vaudio.inf /install
if %errorLevel% neq 0 (
    echo.
    echo pnputil failed. Trying devcon...

    :: Try devcon as fallback (comes with WDK)
    where devcon >nul 2>&1
    if %errorLevel% equ 0 (
        devcon install vaudio.inf ROOT\VaudioCable
    ) else (
        echo.
        echo ERROR: Installation failed.
        echo Make sure you are running as Administrator.
        echo If the driver is unsigned, enable test signing mode.
        pause
        exit /b 1
    )
)

echo.
echo ============================================================
echo   Installation complete!
echo.
echo   You should now see these audio devices:
echo     - "Custom Mic Output" (playback device)
echo     - "Custom Mic Input"  (recording device)
echo.
echo   Open Sound Settings to verify they appear.
echo ============================================================
echo.
pause
