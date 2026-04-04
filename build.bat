@echo off
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

rem -----------------------------------------------------------------------
rem  RetroDepth build script
rem
rem  Usage:
rem    build.bat                        (uses defaults below)
rem    build.bat <MAME_SRC_PATH>        (explicit MAME source path)
rem    build.bat <MAME_SRC_PATH> <VCPKG_ROOT>
rem
rem  Requirements:
rem    - MSYS2 with MinGW-w64 toolchain  https://www.msys2.org/
rem        pacman -S mingw-w64-x86_64-gcc make
rem    - Visual Studio 2022 Build Tools (Desktop C++ workload)
rem        https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
rem    - MAME source at a compatible commit (see MAME_COMMIT below)
rem        git clone https://github.com/mamedev/mame.git
rem        git checkout 3bca6291cc76b2b1ebfe7c50f225eb6ad44c9847
rem -----------------------------------------------------------------------

rem --- Configurable paths ------------------------------------------------
set "MAME_SRC=%~1"
if "%MAME_SRC%"=="" set "MAME_SRC=C:\mame-src"

set "VCPKG_ROOT=%~2"
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\vcpkg"

set "MSYS2_ROOT=C:\msys64"

rem MAME commit this patch set was tested against
set "MAME_COMMIT=3bca6291cc76b2b1ebfe7c50f225eb6ad44c9847"

rem MAME target name (the output exe will be rdmame.exe)
set "MAME_TARGET=rdmame"

rem Systems to include in rdmame
set "MAME_SOURCES=src/mame/neogeo/neogeo.cpp,src/mame/capcom/cps1.cpp,src/mame/capcom/cps2.cpp,src/mame/konami/tmnt.cpp,src/mame/konami/simpsons.cpp,src/mame/nintendo/snes.cpp,src/mame/nintendo/snes_m.cpp,src/mame/sega/megadriv.cpp,src/mame/sega/mdconsole.cpp,src/mame/sega/megacd.cpp,src/mame/shared/mega32x.cpp,src/mame/sega/sms.cpp,src/mame/sega/sms_m.cpp,src/mame/nintendo/gb.cpp"

echo ============================================================
echo  RetroDepth Builder
echo ============================================================
echo  MAME source : %MAME_SRC%
echo  vcpkg       : %VCPKG_ROOT%
echo  MSYS2       : %MSYS2_ROOT%
echo ============================================================
echo.

rem --- Check: MAME source -----------------------------------------------
if not exist "%MAME_SRC%\makefile" (
    echo ERROR: MAME source not found at "%MAME_SRC%"
    echo.
    echo  To get MAME source at the required commit:
    echo    git clone https://github.com/mamedev/mame.git "%MAME_SRC%"
    echo    cd /d "%MAME_SRC%"
    echo    git checkout %MAME_COMMIT%
    echo.
    echo  Or pass your MAME source path as the first argument:
    echo    build.bat C:\path\to\mame-src
    exit /b 1
)

rem --- Check: MSYS2 -------------------------------------------------------
if not exist "%MSYS2_ROOT%\msys2_shell.cmd" (
    echo ERROR: MSYS2 not found at "%MSYS2_ROOT%"
    echo.
    echo  Install MSYS2 from https://www.msys2.org/
    echo  Then open the MSYS2 MinGW64 shell and run:
    echo    pacman -S mingw-w64-x86_64-gcc make
    exit /b 1
)

rem --- Check: VS Build Tools or VS Community ------------------------------
set "VS_DEVCMD="
for %%P in (
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
) do (
    if exist %%~P if "!VS_DEVCMD!"=="" set "VS_DEVCMD=%%~P"
)
if "!VS_DEVCMD!"=="" (
    echo ERROR: Visual Studio 2022 Build Tools not found.
    echo.
    echo  Install from:
    echo    https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
    echo  Select workload: "Desktop development with C++"
    exit /b 1
)
echo [ok] VS tools : !VS_DEVCMD!

rem --- Setup vcpkg (auto-clone + bootstrap if needed) -------------------
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    if not exist "%VCPKG_ROOT%\.git" (
        echo [vcpkg] Cloning vcpkg to "%VCPKG_ROOT%"...
        git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
        if errorlevel 1 (
            echo ERROR: Failed to clone vcpkg. Make sure git is in PATH.
            exit /b 1
        )
    )
    echo [vcpkg] Bootstrapping...
    call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 (
        echo ERROR: vcpkg bootstrap failed.
        exit /b 1
    )
)
echo [ok] vcpkg    : %VCPKG_ROOT%

rem --- Find cmake and ninja bundled with VS ------------------------------
set "CMAKE_EXE=cmake"
set "NINJA_EXE=ninja"
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do (
    if exist %%~P if "!CMAKE_EXE!"=="cmake" set "CMAKE_EXE=%%~P"
)
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
) do (
    if exist %%~P if "!NINJA_EXE!"=="ninja" set "NINJA_EXE=%%~P"
)
echo [ok] cmake    : !CMAKE_EXE!
echo [ok] ninja    : !NINJA_EXE!

rem -----------------------------------------------------------------------
echo.
echo === [1/4] Applying MAME patches ===
xcopy /s /y "%ROOT%\src\*"      "%MAME_SRC%\src\"
xcopy /s /y "%ROOT%\scripts\*"  "%MAME_SRC%\scripts\"
echo Patches applied.

rem -----------------------------------------------------------------------
echo.
echo === [2/4] Building rdmame (this takes 30-60 minutes) ===

rem Skip if already built and patches have not changed since last build
set "MAME_EXE=%MAME_SRC%\%MAME_TARGET%.exe"
if exist "%MAME_EXE%" (
    echo [skip] %MAME_EXE% already exists. Delete it to force a rebuild.
) else (
    cmd /c call "%MSYS2_ROOT%\msys2_shell.cmd" -defterm -no-start -mingw64 -here -c ^
        "cd '%MAME_SRC:\=/%' && make -j%NUMBER_OF_PROCESSORS% IGNORE_GIT=1 REGENIE=1 SUBTARGET=%MAME_TARGET% SOURCES=%MAME_SOURCES%"
    if errorlevel 1 (
        echo ERROR: MAME build failed.
        exit /b 1
    )
    echo [ok] %MAME_EXE% built.
)

rem -----------------------------------------------------------------------
echo.
echo === [3/4] Building retrodepth ===

set "RD_SRC=%ROOT%\retrodepth"
set "RD_OUT=%ROOT%\out\build-release"
set "TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

call "!VS_DEVCMD!" -arch=x64 -host_arch=x64 >nul 2>&1

mkdir "%RD_OUT%" 2>nul
"!CMAKE_EXE!" -S "%RD_SRC%" -B "%RD_OUT%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%"
if errorlevel 1 (
    echo ERROR: cmake configure failed.
    exit /b 1
)

"!NINJA_EXE!" -C "%RD_OUT%" retrodepth
if errorlevel 1 (
    echo ERROR: retrodepth build failed.
    exit /b 1
)
echo [ok] retrodepth.exe built.

rem -----------------------------------------------------------------------
echo.
echo === [4/4] Packaging to output\ ===

set "PKG=%ROOT%\output"
mkdir "%PKG%" 2>nul
mkdir "%PKG%\configs" 2>nul
mkdir "%PKG%\bios" 2>nul
mkdir "%PKG%\roms" 2>nul
mkdir "%PKG%\ini" 2>nul

copy /y "%RD_OUT%\retrodepth.exe"    "%PKG%\" >nul
copy /y "%RD_OUT%\jsoncpp.dll"       "%PKG%\" >nul
copy /y "%RD_OUT%\openxr_loader.dll" "%PKG%\" >nul
copy /y "%MAME_EXE%"                 "%PKG%\" >nul

for %%F in ("%RD_OUT%\configs\*.json") do (
    if /i not "%%~nxF"=="settings.json" (
        copy /y "%%F" "%PKG%\configs\" >nul
    )
)

rem Write clean settings.json with relative paths
(
    echo {
    echo   "mame_exe": "%MAME_TARGET%.exe",
    echo   "mame_args": "-video gdi -window -nomaximize -keyboardprovider win32 -skip_gameinfo",
    echo   "bios_path": "bios",
    echo   "roms_path": "roms"
    echo }
) > "%PKG%\configs\settings.json"

echo.
echo ============================================================
echo  Build complete!
echo  Distribution folder: %PKG%
echo.
echo  Next steps:
echo    1. Copy your ROMs to %PKG%\roms\
echo    2. Copy SNES BIOS (if needed) to %PKG%\bios\
echo    3. Run %PKG%\retrodepth.exe
echo ============================================================
exit /b 0
