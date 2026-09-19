@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
gcc -std=c17 -O2 -Wall -Wextra -Wpedantic -D_WIN32_WINNT=0x0602 ^
  src\main.c src\util.c src\queue.c src\bilibili.c src\web_server.c ^
  -o build\queue_overlay.exe -lwinhttp -lws2_32 -lbcrypt -lshell32
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed.
  pause
  exit /b 1
)
if exist "C:\mingw64\bin\zlib1.dll" copy /Y "C:\mingw64\bin\zlib1.dll" "build\zlib1.dll" >nul
if not exist "build\zlib1.dll" (
  echo [ERROR] zlib1.dll was not found in C:\mingw64\bin.
  exit /b 1
)
echo.
echo Build complete: build\queue_overlay.exe
exit /b 0
