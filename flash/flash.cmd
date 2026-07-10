@echo off
rem =====================================================================
rem  ambyte firmware flasher  (NO COMPILE NEEDED) - launcher for flash.py
rem  Reads the board MAC, checks allowed_macs.txt, then writes the images
rem  in bin\ with esptool. Nothing is compiled.
rem
rem  Usage:   flash.cmd [options]        (full help:  flash.cmd --help)
rem     flash.cmd                 auto-detect port, gate, confirm, flash
rem     flash.cmd --port COM7     explicit serial port
rem     flash.cmd --any           bypass the allow-list
rem     flash.cmd --list          print the allow-list and exit
rem =====================================================================
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "SCRIPT=%HERE%flash.py"

rem Prefer PlatformIO's penv python (carries pyserial); else the py launcher /
rem python on PATH. flash.py resolves esptool itself.
set "CORE=%PLATFORMIO_CORE_DIR%"
if not defined CORE set "CORE=%USERPROFILE%\.platformio"
set "PENVPY=%CORE%\penv\Scripts\python.exe"

if exist "%PENVPY%" (
  "%PENVPY%" "%SCRIPT%" %*
  exit /b !errorlevel!
)
where py >nul 2>&1
if !errorlevel! == 0 (
  py -3 "%SCRIPT%" %*
  exit /b !errorlevel!
)
where python >nul 2>&1
if !errorlevel! == 0 (
  python "%SCRIPT%" %*
  exit /b !errorlevel!
)
echo [error] Python 3 not found. Install PlatformIO or Python 3, then re-run.
exit /b 1
