@echo off
echo Registering Custom Mic Virtual Camera...
regsvr32 /s "%~dp0..\build\Release\virtual_cam.dll"
if %ERRORLEVEL% EQU 0 (
    echo Success! "Custom Mic Virtual Camera" is now available as a camera device.
) else (
    echo Failed to register. Make sure to run this as Administrator.
)
pause
