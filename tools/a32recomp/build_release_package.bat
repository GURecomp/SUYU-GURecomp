@echo off
rem Builds the prebuilt release: suyu.exe plus the link kit (suyu-cmd-static's objects and
rem libraries, so players can export without a source tree), packaged into publish\ by
rem make_package.py with its personal-data scan. See docs/a32recomp/BUILDING.md.
rem
rem Run it from a source tree at a neutral path such as C:\suyu-src: the compiler embeds source
rem paths into the binaries, and the scan rejects anything under C:\Users\.
rem Usage: build_release_package.bat [version] [forbidden word ...]
setlocal
cd /d "%~dp0..\.."
set "SRC=%CD%"
set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=dev"
echo %SRC% | findstr /i /c:"\Users\" >nul && (
  echo [error] This tree is under C:\Users\. Clone it to a neutral path such as C:\suyu-src first.
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\suyu_vsdir_rel.txt"
set /p VSDIR=<"%TEMP%\suyu_vsdir_rel.txt"
if not defined VSDIR (echo [error] Visual Studio 2022 with C++ not found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VULKAN_SDK for /d %%d in ("C:\VulkanSDK\*") do if exist "%%d\Bin\glslangValidator.exe" set "VULKAN_SDK=%%d"
if not defined QT_DIR for /d %%d in ("C:\Qt\6.*") do if exist "%%d\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QT_DIR=%%d\msvc2022_64"
if not defined VULKAN_SDK (echo [error] Vulkan SDK not found. & exit /b 1)
if not defined QT_DIR (echo [error] Qt 6 for MSVC 2022 64-bit not found; set QT_DIR. & exit /b 1)
set "PATH=%VULKAN_SDK%\Bin;%QT_DIR%\bin;%PATH%"

rem The static launcher is configured against an empty stub module: only its objects and
rem libraries go into the kit; the export links them with the player's recompiled game.
set "STUB=%SRC%\tools\a32recomp\kit_stub"
cmake -S "%SRC%" -B "%SRC%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON ^
  -DYUZU_TESTS=OFF -DYUZU_USE_BUNDLED_QT=OFF -DQt6_DIR="%QT_DIR%\lib\cmake\Qt6" ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%" -DSUYU_CMD_RECOMP_DIR="%STUB:\=/%" || (echo [error] configure failed & exit /b 1)
cmake --build "%SRC%\build" --target suyu || (echo [error] suyu build failed & exit /b 1)
rem The static launcher's own link fails on the stub (it has no registration table); that one
rem error is expected, anything else is not.
cmake --build "%SRC%\build" --target suyu-cmd-static > "%SRC%\build\static_objs.log" 2>&1
findstr /c:"LNK2019: unresolved external symbol suyu_recomp_static_modules" "%SRC%\build\static_objs.log" >nul || (
  echo [error] building the kit objects failed; see build\static_objs.log
  exit /b 1
)

set "OUT=%SRC%\publish\suyu-mhgu-%VERSION%-windows-x64"
python "%SRC%\tools\a32recomp\make_link_kit.py" "%SRC%\build" "%SRC%\publish\link_kit" || exit /b 1
python "%SRC%\tools\a32recomp\make_package.py" "%SRC%\build\bin" "%SRC%\publish\link_kit" "%OUT%" %2 %3 %4 %5 %6 || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%\*' -DestinationPath '%OUT%.zip'" || exit /b 1
echo.
echo Release package: %OUT%.zip
