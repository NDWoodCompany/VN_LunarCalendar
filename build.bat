@echo off
setlocal EnableExtensions
cd /d "%~dp0"
taskkill /im "VN_LunarCalendar.exe" /f >nul 2>nul
set "G="
if defined GXX if exist "%GXX%" set "G=%GXX%"
if not defined G if exist "C:\msys64\ucrt64\bin\g++.exe" set "G=C:\msys64\ucrt64\bin\g++.exe"
if not defined G if exist "C:\msys64\mingw64\bin\g++.exe" set "G=C:\msys64\mingw64\bin\g++.exe"
if not defined G if exist "C:\msys64\clang64\bin\g++.exe" set "G=C:\msys64\clang64\bin\g++.exe"
if not defined G for /f "delims=" %%A in ('where g++ 2^>nul') do if not defined G set "G=%%A"
if not defined G (
  echo ERROR: g++ not found. Install MSYS2 and run:  pacman -S mingw-w64-ucrt-x86_64-gcc
  exit /b 1
)
for %%I in ("%G%") do set "PATH=%%~dpI;%PATH%"
echo Using %G%
for %%I in ("%G%") do set "WR=%%~dpIwindres.exe"
if not exist "%WR%" (
  echo ERROR: windres.exe not found next to g++ ^(needed for the icon and version info^).
  exit /b 1
)
if not exist build mkdir build
"%WR%" -c 65001 -O coff -i resource.rc -o build\resource.o > build_error.log 2>&1
if errorlevel 1 (
  echo RESOURCE BUILD FAILED. See build_error.log:
  type build_error.log
  exit /b 1
)

"%G%" -std=c++17 -Os -s -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-unwind-tables ^
  -ffunction-sections -fdata-sections -Wl,--gc-sections -mwindows -municode ^
  -static-libgcc -static-libstdc++ -finput-charset=UTF-8 ^
  -o VN_LunarCalendar.exe main.cpp build\resource.o -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luiautomationcore > build_error.log 2>&1
if errorlevel 1 (
  echo BUILD FAILED. See build_error.log:
  type build_error.log
  exit /b 1
)
del build_error.log >nul 2>&1
for %%F in (VN_LunarCalendar.exe) do echo BUILD OK: %%F  ^(%%~zF bytes^)
exit /b 0
