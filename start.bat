@echo off
setlocal
cd /d "%~dp0"
if not exist "build\queue_overlay.exe" (
  call build.bat
  if errorlevel 1 exit /b 1
)
title Yeguo Danmu Queue - C
"build\queue_overlay.exe"
if errorlevel 1 pause
