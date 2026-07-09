@echo off
rem Windows launcher for tools/flash_certs.py — finds a Python and forwards args.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "SCRIPT=%HERE%flash_certs.py"
set "VENVPY=%HERE%..\.venv\Scripts\python.exe"

if exist "%VENVPY%" (
  "%VENVPY%" "%SCRIPT%" %*
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
echo Python 3 not found. Install it from https://python.org and re-run.
exit /b 1
