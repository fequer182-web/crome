@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ================================================================
echo CROME ANTICA X64 - INSPECAO OFFLINE Game::autoWalk
echo ================================================================
echo.
echo Este teste NAO movimenta o jogador nem injeta DLL.
echo.
set "EXE=%~1"
if not defined EXE (
  echo Cole o caminho completo ou ARRASTE o Antica Retro opengl.exe:
  set /p "EXE=EXE: "
)
set "EXE=%EXE:"=%"
if not exist "%EXE%" (
  echo [ERRO] Arquivo nao encontrado.
  pause
  exit /b 2
)
where py >nul 2>&1
if not errorlevel 1 (
  py -3 "%~dp0InspectAnticaAutoWalk.py" --exe "%EXE%" --out "%~dp0walk_abi_report.txt"
) else (
  python "%~dp0InspectAnticaAutoWalk.py" --exe "%EXE%" --out "%~dp0walk_abi_report.txt"
)
set "RC=%ERRORLEVEL%"
echo.
echo Envie o arquivo walk_abi_report.txt do diretorio atual.
pause
exit /b %RC%
