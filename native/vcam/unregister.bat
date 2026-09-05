@echo off
echo Unregistering Custom Mic Virtual Camera...
regsvr32 /u /s "%~dp0..\build\Release\virtual_cam.dll"
if %ERRORLEVEL% EQU 0 (
    echo Success! Virtual camera has been removed.
) else (
    echo Failed to unregister. Make sure to run this as Administrator.
)
pause
