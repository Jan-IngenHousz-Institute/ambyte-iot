@echo off
rem Windows launcher for tools/flash_certs.py — finds a Python and forwards args.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "SCRIPT=%HERE%flash_certs.py"
set "VENVPY=%HERE%..\.venv\Scripts\python.exe"

rem A .venv copied from another machine has a python.exe that only redirects to
rem a base interpreter that isn't here — probe it before trusting it.
if exist "%VENVPY%" (
  "%VENVPY%" -c "" >nul 2>&1
  if !errorlevel! == 0 (
    "%VENVPY%" "%SCRIPT%" %*
    exit /b !errorlevel!
  )
  echo [warn] ignoring unusable .venv ^(base interpreter missing?^); falling back to system Python.
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
