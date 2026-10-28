@echo off
REM ---------------------------------------------------------------------------
REM  Build mxlight.exe with MSVC. No third-party dependencies.
REM  Finds the Visual Studio C++ toolchain automatically via vswhere, then
REM  compiles a single statically-linked, dependency-free executable.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

REM --- Locate and enter the VS developer environment if cl.exe isn't present ---
where cl.exe >nul 2>&1
if %errorlevel%==0 goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found. Install "Desktop development with C++" via the Visual Studio Installer.
    exit /b 1
)
REM Capture vswhere's output via a temp file -- robust against the spaces and
REM parentheses in "Program Files (x86)" that trip up a for /f command.
set "VSPATH="
set "VSTMP=%TEMP%\mxlight_vspath.txt"
"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "!VSTMP!" 2>nul
if exist "!VSTMP!" set /p VSPATH=<"!VSTMP!"
del "!VSTMP!" 2>nul
if not defined VSPATH (
    echo [ERROR] No Visual Studio C++ toolchain found. Install "Desktop development with C++".
    exit /b 1
)
REM 2>nul hides a benign "vswhere not recognized" probe message emitted by
REM Microsoft's own vcvars scripts on some installs; we verify cl below anyway.
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Failed to initialise the MSVC environment ^(cl.exe not found^).
    exit /b 1
)

:compile
echo Compiling mxlight.exe ...
REM /link /SUBSYSTEM:WINDOWS makes it a GUI (tray) app so no console window opens.
cl /nologo /W4 /O2 /MT /Fe:"%OUT%\mxlight.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\mxlight.c" ^
   setupapi.lib hid.lib shell32.lib user32.lib gdi32.lib ^
   /link /SUBSYSTEM:WINDOWS
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo [OK] Built %OUT%\mxlight.exe
endlocal
