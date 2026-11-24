@echo off
rem SPDX-License-Identifier: MIT
rem Builds the app, runs the tests and puts the portable app in out\<its name>
rem (the name is the root CMakeLists.txt's project). Visual Studio and its
rem CMake are found by themselves.
rem   build.cmd           build, test, install into out\
rem   build.cmd check     build and test only: out\ is not touched
rem   build.cmd package   ... and the release zip in out\
rem   build.cmd vs2022    use Visual Studio 2022 (vs2026: 2026) where both are installed
setlocal
cd /d "%~dp0"

set "CONFIGURE="
set "PACKAGE="
set "INSTALL=1"
:args
if "%~1"=="" goto args_done
if /i "%~1"=="vs2022" (
    set "CONFIGURE=vs2022"
) else if /i "%~1"=="vs2026" (
    set "CONFIGURE=vs"
) else if /i "%~1"=="package" (
    set "PACKAGE=1"
) else if /i "%~1"=="check" (
    set "INSTALL="
) else (
    echo Unknown argument %~1
    exit /b 2
)
shift
goto args
:args_done
if defined PACKAGE if not defined INSTALL (
    echo "check" and "package" do not go together.
    exit /b 2
)

rem The newest Visual Studio with the C++ tools, or the one asked for.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found. Install Visual Studio 2022 or 2026 with "Desktop development with C++".
    exit /b 1
)
set "RANGE="
if "%CONFIGURE%"=="vs2022" set "RANGE=-version [17.0,18.0)"
if "%CONFIGURE%"=="vs" set "RANGE=-version [18.0,19.0)"
set "VSDIR="
set "VSLINE="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * %RANGE% -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * %RANGE% -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion`) do set "VSLINE=%%i"
if not defined VSDIR (
    echo No Visual Studio with "Desktop development with C++" was found.
    exit /b 1
)
if not defined CONFIGURE (
    if "%VSLINE%"=="2022" (set "CONFIGURE=vs2022") else (set "CONFIGURE=vs")
)
if "%CONFIGURE%"=="vs2022" (set "BUILD=release-vs2022") else (set "BUILD=release")

rem CMake: the one on PATH, else the one Visual Studio ships.
where cmake >nul 2>&1 || set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where cmake >nul 2>&1 || (
    echo CMake was not found. Add "C++ CMake tools for Windows" in the Visual Studio Installer, or install https://cmake.org.
    exit /b 1
)

set "APP="
for /f "tokens=2 delims=( " %%n in ('findstr /b /c:"project(" CMakeLists.txt') do if not defined APP set "APP=%%n"

echo Visual Studio %VSLINE%, preset %CONFIGURE%
cmake --preset %CONFIGURE% || exit /b 1
cmake --build --preset %BUILD% || exit /b 1
ctest --preset %BUILD% || exit /b 1
if not defined INSTALL (
    echo.
    echo Built and tested: %~dp0build\%CONFIGURE%  ^(out\%APP% not touched^)
    exit /b 0
)

rem The app's files are locked while it runs: say so instead of a half install.
tasklist /fi "imagename eq %APP%.exe" 2>nul | find /i "%APP%.exe" >nul && (
    echo %APP% is running: close it and run build.cmd again to put the new build in out\%APP%.
    exit /b 1
)
cmake --install build\%CONFIGURE% --config Release || exit /b 1
if defined PACKAGE (
    cpack --config build\%CONFIGURE%\CPackConfig.cmake -C Release || exit /b 1
)

echo.
echo Built: %~dp0out\%APP%
