@echo off
setlocal enabledelayedexpansion

echo Echo GPPC SAPI5 build (32-bit and 64-bit)
echo.

set BUILD_DIR_X86=build_x86
set BUILD_DIR_X64=build_x64
set OUTPUT_DIR=output

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake is not on PATH. Install CMake 3.15 or newer.
    exit /b 1
)

REM No vcvars call and no vswhere lookup: CMake's Visual Studio generator finds
REM the toolset itself, and vswhere hides Build Tools installations unless it
REM is passed "-products *" -- which is exactly how this script used to decide,
REM wrongly, that a perfectly good Build Tools install did not exist.
REM
REM Reporting which one CMake picked is still worth the two lines.
for /f "usebackq tokens=*" %%i in (`cmake --version 2^>nul`) do (
    if not defined CMAKE_BANNER set "CMAKE_BANNER=%%i"
)
echo Using %CMAKE_BANNER%
echo.

echo === Building 64-bit ===
cmake -A x64 -S . -B %BUILD_DIR_X64%
if errorlevel 1 exit /b 1
cmake --build %BUILD_DIR_X64% --config Release
if errorlevel 1 exit /b 1

echo.
echo === Building 32-bit ===
cmake -A Win32 -S . -B %BUILD_DIR_X86%
if errorlevel 1 exit /b 1
cmake --build %BUILD_DIR_X86% --config Release
if errorlevel 1 exit /b 1

echo.
echo === Staging %OUTPUT_DIR% ===

REM The layout the installer ships and the engine expects: the 64-bit engine,
REM BOTH emulator DLLs and the Textalker ROM pairs in the root, with only the
REM 32-bit engine and test harness in x86\. Every module searches its own
REM directory and then the parent, so one copy of the ROMs -- and one copy of
REM each emulator DLL -- serves both architectures and either build of the
REM configuration utility.
if exist %OUTPUT_DIR% rmdir /s /q %OUTPUT_DIR%
mkdir %OUTPUT_DIR%
mkdir %OUTPUT_DIR%\x86

copy /Y "%BUILD_DIR_X64%\bin\Release\EchoGPPCSAPI.dll"   "%OUTPUT_DIR%\" >nul
copy /Y "%BUILD_DIR_X64%\bin\Release\EchoGPPCTest.exe"   "%OUTPUT_DIR%\" >nul
copy /Y "%BUILD_DIR_X64%\bin\Release\EchoGPPCConfig.exe" "%OUTPUT_DIR%\" >nul
copy /Y "bin\echotalk64.dll"                           "%OUTPUT_DIR%\" >nul
copy /Y "bin\echotalk32.dll"                           "%OUTPUT_DIR%\" >nul
copy /Y "bin\textalker.ram.bin"                        "%OUTPUT_DIR%\" >nul
copy /Y "bin\textalker.obj.bin"                        "%OUTPUT_DIR%\" >nul
copy /Y "bin\textalker_v13.ram.bin"                    "%OUTPUT_DIR%\" >nul
copy /Y "bin\textalker_v13.obj.bin"                    "%OUTPUT_DIR%\" >nul
copy /Y "bin\THIRD_PARTY_LICENSES.txt"                 "%OUTPUT_DIR%\" >nul
copy /Y "bin\readme.html"                              "%OUTPUT_DIR%\" >nul

copy /Y "%BUILD_DIR_X86%\bin\Release\EchoGPPCSAPI.dll"   "%OUTPUT_DIR%\x86\" >nul
copy /Y "%BUILD_DIR_X86%\bin\Release\EchoGPPCTest.exe"   "%OUTPUT_DIR%\x86\" >nul
copy /Y "%BUILD_DIR_X86%\bin\Release\EchoGPPCConfig.exe" "%OUTPUT_DIR%\x86\" >nul

echo.
echo === Building the installer ===

REM Inno Setup installs per-user by default, so check the user's location
REM before the machine-wide one.
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"

if not exist "%ISCC%" (
    echo.
    echo WARNING: Inno Setup 6 was not found, so no installer was built.
    echo          The staged files in %OUTPUT_DIR%\ are complete and usable.
    echo          Install Inno Setup 6 and re-run to produce the installer.
    goto :done
)

"%ISCC%" /Q "installer\echogppc.iss"
if errorlevel 1 (
    echo ERROR: the installer failed to compile.
    exit /b 1
)
echo Installer built: %OUTPUT_DIR%\EchoGPPC_SAPI5_Setup.exe

:done
echo.
echo Build complete. Staged in %OUTPUT_DIR%\
echo.
echo To install (this needs administrator rights):
echo     %OUTPUT_DIR%\EchoGPPC_SAPI5_Setup.exe
echo.
echo To check it afterwards:
echo     "%%ProgramFiles%%\Echo GPPC SAPI5\EchoGPPCTest.exe" --list
echo     "%%ProgramFiles%%\Echo GPPC SAPI5\EchoGPPCTest.exe" --speak
echo.
echo To register the staged build directly instead, from an elevated prompt:
echo     regsvr32 "%CD%\%OUTPUT_DIR%\EchoGPPCSAPI.dll"
echo     regsvr32 "%CD%\%OUTPUT_DIR%\x86\EchoGPPCSAPI.dll"

endlocal
