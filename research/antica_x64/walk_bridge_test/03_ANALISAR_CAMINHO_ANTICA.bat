@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ================================================================
echo CROME ANTICA X64 - ANALISAR ESTRUTURA DO CAMINHO DE AUTOWALK
echo ================================================================
echo.
echo A build e validada pelo SHA-256 da secao .text.
echo Leitura apenas do EXE em disco. Nenhuma injecao ou movimento real.
echo.
set "EXE=%~1"
if not defined EXE (
  echo Cole o caminho completo do Antica Retro opengl.exe:
  set /p "EXE=EXE: "
)
set "EXE=%EXE:"=%"
if not exist "%EXE%" (
  echo [ERRO] EXE nao encontrado: "%EXE%"
  pause
  exit /b 2
)
where py >nul 2>&1
if not errorlevel 1 (
  py -3 "%~dp0InspectAnticaAutoWalk.py" --exe "%EXE%" --extended --out "%~dp0walk_path_report.txt"
) else (
  python "%~dp0InspectAnticaAutoWalk.py" --exe "%EXE%" --extended --out "%~dp0walk_path_report.txt"
)
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo [OK] Relatorio salvo em walk_path_report.txt
  echo [PROXIMO] Envie o arquivo para analisar a origem do caminho em RDX.
) else (
  echo [BLOQUEADO] Verifique os dados no walk_path_report.txt.
)
pause
exit /b %RC%
