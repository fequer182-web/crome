@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ===================================================
echo CROME - INSTALAR TESTE DE CAMINHADA REAL NO OTC
echo ===================================================
echo.
echo [INFO] Use somente no seu OTC/TFS controlado.
echo [INFO] Feche o OTC antes de instalar.
echo.
set /p "MODS=Informe o caminho completo da pasta mods do OTC: "
set "MODS=%MODS:"=%"
if not exist "%MODS%\" (
  echo [ERRO] Pasta mods nao encontrada: "%MODS%"
  goto :fail
)
set "DEST=%MODS%\crome_walk_2sqm_test"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%~dp0crome_walk_2sqm_test.otmod" "%DEST%\crome_walk_2sqm_test.otmod" >nul
if errorlevel 1 goto :fail
copy /Y "%~dp0walk.lua" "%DEST%\walk.lua" >nul
if errorlevel 1 goto :fail
echo.
echo [OK] Modulo instalado em: "%DEST%"
echo [PROXIMO] Inicie OTC e entre no seu servidor de testes.
echo [PROXIMO] Em dois SQMs livres a Leste, pressione Ctrl+F11 UMA vez.
echo [LOG] Veja a mensagem na janela do jogo e/ou console de logs.
echo [AVISO] O BAT so instala o modulo. O movimento acontece pelo OTC, nao pela DLL.
pause
exit /b 0
:fail
echo.
echo [FALHA] Nao foi possivel instalar o modulo.
pause
exit /b 1
