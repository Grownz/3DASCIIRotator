@echo off
REM ---------------------------------------------------------------------------
REM Build script for 3D ASCII Rotator (Windows x64, MSVC).
REM Produces build\ascii3D.exe.  Requires Visual Studio / Build Tools with the
REM MSVC C compiler and the Windows SDK.
REM ---------------------------------------------------------------------------
setlocal

set "APP=ascii3D"

REM Locate a Visual Studio installation that has the C++ build tools.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found. Install Visual Studio Build Tools.
  exit /b 1
)

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (
  echo [error] No Visual Studio installation with C++ build tools found.
  exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [error] Failed to initialize the MSVC environment.
  exit /b 1
)

if not exist build mkdir build

cl /nologo /O2 /W4 /TC ^
   src\main.c ^
   /Fe:build\%APP%.exe /Fo:build\
if errorlevel 1 (
  echo [error] Build failed.
  exit /b 1
)

echo.
echo [ok] Built build\%APP%.exe
echo      Run:  build\%APP%.exe -s cube
endlocal
