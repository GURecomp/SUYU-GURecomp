@echo off
if not "%~1"=="--inner" (
  rem everything shown in this window is also saved to build-console.txt in the source root
  cmd /c ""%~f0" --inner" 2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath '%~dp0..\..\build-console.txt'"
  echo.
  echo [window kept open - press a key to close]
  pause >nul
  exit /b
)
rem Builds this source tree (suyu with the A32 recompiler) with Visual Studio 2022 (MSVC).
rem Double-click it, or run it from a normal cmd window. See docs/a32recomp/BUILDING.md.
rem Output also goes to build-log.txt in the source root.
setlocal
cd /d "%~dp0..\.."
set "SRC=%CD%"
set "LOG=%CD%\build-log.txt"
echo Build started %date% %time% > "%LOG%"

rem --- find Visual Studio and load its C++ environment (what the "Native Tools" prompt does)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] Visual Studio Installer not found. Install Visual Studio 2022 with "Desktop development with C++".
  goto :fail
)
set "VSDIR="
rem vswhere writes the install path to a temp file (avoids for /f quoting problems with "(x86)")
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\suyu_vsdir.txt"
set /p VSDIR=<"%TEMP%\suyu_vsdir.txt"
if not defined VSDIR (
  echo [error] Visual Studio found, but without C++ tools. Open Visual Studio Installer ^> Modify ^> tick "Desktop development with C++".
  goto :fail
)
echo Using Visual Studio at: %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [error] Could not load the MSVC environment.
  goto :fail
)

rem --- check the tools
where cmake >nul 2>&1 || (echo [error] cmake not found. In Visual Studio Installer, add "C++ CMake tools for Windows". & goto :fail)
where ninja >nul 2>&1 || (echo [error] ninja not found. In Visual Studio Installer, add "C++ CMake tools for Windows". & goto :fail)
where git >nul 2>&1 || (echo [error] git not found. Install Git for Windows: https://gitforwindows.org & goto :fail)
where python >nul 2>&1 || (echo [warning] python not found on PATH. Some dependencies may fail to download. Install Python 3.10+ and tick "Add to PATH".)
rem --- Vulkan SDK: suyu needs its glslangValidator to build shaders
if not defined VULKAN_SDK (
  for /d %%d in ("C:\VulkanSDK\*") do if exist "%%d\Bin\glslangValidator.exe" set "VULKAN_SDK=%%d"
)
if not defined VULKAN_SDK (
  echo [error] Vulkan SDK not found. Install it one of two ways, then run this again:
  echo         - PowerShell as Administrator: "%SRC%\tools\windows\install-vulkan-sdk.ps1"
  echo         - or the installer from https://vulkan.lunarg.com/sdk/home#windows
  goto :fail
)
if not exist "%VULKAN_SDK%\Bin\glslangValidator.exe" (
  echo [error] Vulkan SDK at "%VULKAN_SDK%" has no glslangValidator.exe. Reinstall the Vulkan SDK.
  goto :fail
)
echo Using Vulkan SDK at: %VULKAN_SDK%
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
cmake --version | findstr /i version

if not exist "%SRC%\CMakeLists.txt" (
  echo [error] Source not found at "%SRC%".
  goto :fail
)

rem --- Qt: use an installed Qt 6 (MSVC 2022 64-bit). suyu's bundled Qt download lacks Qt SVG.
rem     Set QT_DIR yourself (e.g. set QT_DIR=D:\Qt\6.8.3\msvc2022_64) if Qt is not under C:\Qt.
if not defined QT_DIR (
  for /d %%d in ("C:\Qt\6.*") do if exist "%%d\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QT_DIR=%%d\msvc2022_64"
)
if not defined QT_DIR (
  echo [error] No Qt 6 for MSVC 2022 64-bit found under C:\Qt.
  echo         Install it with the Qt online installer: Qt 6.x ^> "MSVC 2022 64-bit" ^(Qt SVG is part of it^).
  goto :fail
)
if not exist "%QT_DIR%\lib\cmake\Qt6Svg\Qt6SvgConfig.cmake" (
  echo [error] Qt at "%QT_DIR%" has no Qt SVG module. In Qt Maintenance Tool, add it for MSVC 2022 64-bit.
  goto :fail
)
echo Using Qt at: %QT_DIR%
set "PATH=%QT_DIR%\bin;%PATH%"

rem --- a running suyu.exe (also one left in the background after an export) locks the file
tasklist /fi "imagename eq suyu.exe" 2>nul | find /i "suyu.exe" >nul
if not errorlevel 1 (
  echo [error] suyu.exe is still running, possibly in the background after an export.
  echo         End it in Task Manager ^(Details tab, suyu.exe^), then run this again.
  goto :fail
)

rem --- a previous run cached the bundled Qt; start the configuration over in that case
if exist "%SRC%\build\CMakeCache.txt" (
  findstr /c:"YUZU_USE_BUNDLED_QT:BOOL=ON" "%SRC%\build\CMakeCache.txt" >nul && del "%SRC%\build\CMakeCache.txt"
)

rem --- configure (first run downloads dependencies; takes a while)
echo.
echo === Configuring (first time downloads dependencies, can take 10+ minutes) ===
cmake -S "%SRC%" -B "%SRC%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_TESTS=OFF -DYUZU_USE_BUNDLED_QT=OFF -DQt6_DIR="%QT_DIR%\lib\cmake\Qt6" -DCMAKE_PREFIX_PATH="%QT_DIR%" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [error] Configure failed. Last lines of build-log.txt:
  powershell -NoProfile -Command "Get-Content -Tail 40 '%LOG%'"
  goto :fail
)

rem --- build
echo.
echo === Building suyu and suyu-cmd (30-90 minutes the first time) ===
cmake --build "%SRC%\build" --target suyu suyu-cmd >> "%LOG%" 2>&1
set "BUILD_RC=%errorlevel%"
findstr /c:"FAILED:" "%LOG%" >nul && set "BUILD_RC=1"
if not "%BUILD_RC%"=="0" (
  echo [error] Build failed. Last lines of build-log.txt:
  powershell -NoProfile -Command "Get-Content -Tail 60 '%LOG%'"
  goto :fail
)

echo.
echo === Done ===
echo Run: "%SRC%\build\bin\suyu.exe"   (run it from there, not a copy)
echo Build finished %date% %time% >> "%LOG%"
exit /b 0

:fail
echo.
echo Build stopped. Attach build-log.txt (in the source root) to your issue report.
exit /b 1
