@echo off
:: build.cmd — Build the Custom Mic Virtual Audio Cable driver
::
:: Prerequisites:
::   1. Install Windows Driver Kit (WDK) 10 or later
::      https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
::   2. Install the matching Windows SDK
::   3. Open a "Developer Command Prompt for VS" or
::      "x64 Native Tools Command Prompt for VS"
::
:: This script invokes MSBuild on the .vcxproj if available,
:: or falls back to a direct cl.exe invocation.

setlocal

cd /d "%~dp0"

echo ============================================================
echo   Building Custom Mic Virtual Audio Cable (vaudio.sys)
echo ============================================================
echo.

:: Try MSBuild first (if .vcxproj exists)
if exist vaudio.vcxproj (
    echo Using MSBuild...
    msbuild vaudio.vcxproj /p:Configuration=Release /p:Platform=x64
    if %errorLevel% equ 0 (
        echo Build succeeded.
        goto :done
    )
    echo MSBuild failed, trying direct compilation...
)

:: Direct WDK compilation
:: Adjust WDK_ROOT if your WDK is installed elsewhere
if not defined WDK_ROOT (
    if exist "C:\Program Files (x86)\Windows Kits\10" (
        set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
    ) else (
        echo ERROR: WDK not found. Set WDK_ROOT environment variable.
        exit /b 1
    )
)

:: Find the latest WDK version
for /d %%d in ("%WDK_ROOT%\Include\10.*") do set "WDK_VER=%%~nxd"

echo WDK: %WDK_ROOT%
echo WDK Version: %WDK_VER%

set "INC=/I"%WDK_ROOT%\Include\%WDK_VER%\km" /I"%WDK_ROOT%\Include\%WDK_VER%\shared" /I"%WDK_ROOT%\Include\%WDK_VER%\um""
set "LIB_PATH=/LIBPATH:"%WDK_ROOT%\Lib\%WDK_VER%\km\x64""
set "LIBS=portcls.lib ks.lib ntoskrnl.lib hal.lib wmilib.lib BufferOverflowK.lib"

:: Rename .c to .cpp for C++ compilation (PortCls requires C++)
if not exist vaudio.cpp copy vaudio.c vaudio.cpp >nul

cl.exe /nologo /kernel /Zi /W4 /WX- /Ox /D_AMD64_ /DAMD64 /D_WIN64 ^
    /DNTDDI_VERSION=0x0A000000 /D_KERNEL_MODE=1 ^
    %INC% ^
    /c vaudio.cpp /Fo:vaudio.obj

if %errorLevel% neq 0 (
    echo Compilation failed.
    exit /b 1
)

link.exe /nologo /DRIVER /KERNEL /SUBSYSTEM:NATIVE /ENTRY:DriverEntry ^
    %LIB_PATH% %LIBS% ^
    vaudio.obj /OUT:vaudio.sys /PDB:vaudio.pdb

if %errorLevel% neq 0 (
    echo Linking failed.
    exit /b 1
)

echo.
echo Build succeeded: vaudio.sys

:done
echo.
echo Next steps:
echo   1. Sign the driver (or enable test signing: bcdedit /set testsigning on)
echo   2. Run install.bat as Administrator
echo.
