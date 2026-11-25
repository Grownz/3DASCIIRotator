@echo off
REM ---------------------------------------------------------------------------
REM Builds one libFuzzer+ASan executable per model format into build\fuzz\.
REM Needs the MSVC "C++ AddressSanitizer" component of Visual Studio 2022.
REM Usage:  tests\fuzz\build_fuzz.bat            (all formats)
REM         build\fuzz\fuzz_obj.exe corpus\obj -max_total_time=30
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0\..\.."

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

if not exist build\fuzz md build\fuzz

REM Compile the loader sources once, then link one harness per format.
set "CF=/nologo /Zi /W3 /fsanitize=address /fsanitize=fuzzer /Isrc"
cl %CF% /c src\loader.c src\gltf.c src\zip3mf.c src\fbx.c src\inflate.c src\meshbuf.c /Fo:build\fuzz\ /Fd:build\fuzz\vc.pdb
if errorlevel 1 (
  echo [error] Compiling the loaders failed.
  exit /b 1
)
for %%E in (stl obj ply off gltf glb 3mf fbx) do (
  cl %CF% /DFUZZ_EXT=L\".%%E\" tests\fuzz\fuzz_loader.c build\fuzz\loader.obj build\fuzz\gltf.obj build\fuzz\zip3mf.obj build\fuzz\fbx.obj build\fuzz\inflate.obj build\fuzz\meshbuf.obj /Fo:build\fuzz\h_%%E.obj /Fd:build\fuzz\vc.pdb /Fe:build\fuzz\fuzz_%%E.exe /link /DEBUG
  if errorlevel 1 (
    echo [error] Linking fuzz_%%E failed.
    exit /b 1
  )
)
echo [ok] Built build\fuzz\fuzz_*.exe
endlocal
