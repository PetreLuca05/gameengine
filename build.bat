@echo off
rem Configure + build Deadground (Windows, MSVC + Ninja).
rem   build.bat            -> RelWithDebInfo in build\
rem   build.bat Debug      -> Debug in build-debug\
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo
set BUILD_DIR=build
if /I "%CONFIG%"=="Debug" set BUILD_DIR=build-debug

if exist "%~dp0.toolchain\env.bat" (
  call "%~dp0.toolchain\env.bat"
) else (
  where cl >nul 2>nul || (
    echo Run from a "x64 Native Tools Command Prompt" or create .toolchain\env.bat
    exit /b 1
  )
)

cmake -S "%~dp0." -B "%~dp0%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%~dp0%BUILD_DIR%" || exit /b 1
echo Build OK: %BUILD_DIR%\bin\deadground.exe
