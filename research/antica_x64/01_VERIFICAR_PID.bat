@echo off
setlocal
cd /d "%~dp0"
set /p "PIDANTICA=Digite o PID atual do Antica: "
py -3 "%~dp0build_identity.py" --pid %PIDANTICA% --out "%~dp0preflight_result.json"
echo.
pause
