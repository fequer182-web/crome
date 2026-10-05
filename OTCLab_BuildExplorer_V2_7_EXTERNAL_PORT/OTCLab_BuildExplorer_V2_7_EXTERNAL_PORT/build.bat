@echo off
setlocal
cd /d "%~dp0"

set "CXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%CXX%" (
  echo [FAIL] Compilador nao encontrado: %CXX%
  echo Ajuste CXX no build.bat se seu MSYS2 estiver em outro caminho.
  pause
  exit /b 1
)

if not exist Build mkdir Build

echo ============================================================
echo   OTCLab BuildExplorer V2.7 EXTERNAL PORT - x64
echo ============================================================
echo [1/1] Compilando EXE externo x64...

"%CXX%" -std=c++20 -O2 -Wall -Wextra -static-libgcc -static-libstdc++ ^
  "Source\OTCLab_BuildExplorer_V2_7_EXTERNAL_PORT.cpp" ^
  -o "Build\OTCLab_BuildExplorer_V2_7_EXTERNAL_PORT.exe"

if errorlevel 1 (
  echo.
  echo [FAIL] Compilacao falhou.
  pause
  exit /b 1
)

echo.
echo [OK] EXE gerado:
echo %CD%\Build\OTCLab_BuildExplorer_V2_7_EXTERNAL_PORT.exe
echo.
echo IMPORTANTE:
echo - NAO injete este EXE.
echo - Abra o cliente normalmente e rode o EXE por fora.
echo - Usa OpenProcess + ReadProcessMemory; sem DLL, sem hook e sem write.
echo - Se leitura falhar, tente Executar como administrador. Nao ha bypass.
echo - Os profiles em profiles\reference alimentam o scanner estatico de funcoes.
pause
