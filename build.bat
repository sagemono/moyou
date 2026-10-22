@echo off
rem Build moyou on Windows. Needs Visual Studio 2022 and vcpkg (VCPKG_ROOT set).
rem   build.bat            vcpkg manifest build (first run builds Qt, which takes a while)
rem   build.bat classic    use packages already installed in %VCPKG_ROOT% (qtbase, zlib, catch2)
setlocal
if "%VCPKG_ROOT%"=="" (
  echo VCPKG_ROOT is not set. Point it at your vcpkg checkout:
  echo   cmd:        set VCPKG_ROOT=C:\vcpkg
  echo   PowerShell: $env:VCPKG_ROOT = "C:\vcpkg"
  exit /b 1
)
set PRESET=windows
if /i "%1"=="classic" set PRESET=windows-classic
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
echo.
echo Built: build\%PRESET%\Release\moyou.exe
