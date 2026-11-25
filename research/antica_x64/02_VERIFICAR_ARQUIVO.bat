@echo off
setlocal
cd /d "%~dp0"
set /p "EXE=Digite o caminho completo do EXE: "
set "EXE=%EXE:"=%"
py -3 "%~dp0build_identity.py" --file "%EXE%" --out "%~dp0preflight_result.json"
echo.
pause
