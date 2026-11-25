@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ======================================================
echo   CROME - WALK BRIDGE DLL x64 - TESTE CONTROLADO
echo ======================================================
echo.
if exist "C:\msys64\ucrt64\bin\g++.exe" set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where g++ >nul 2>&1
if errorlevel 1 (
    echo [ERRO] g++ do MSYS2 UCRT64 nao encontrado.
    goto :fail
)
if not exist "build" mkdir "build"
echo [1/3] Compilando a DLL x64...
g++ -std=c++20 -O2 -Wall -Wextra -shared -o "build\CromeWalkBridge_x64.dll" "CromeWalkBridge.cpp" -static-libgcc -static-libstdc++
if errorlevel 1 goto :fail
echo [2/3] Compilando host de teste...
g++ -std=c++20 -O2 -Wall -Wextra -o "build\CromeWalkBridgeTest_x64.exe" "TestHost.cpp" -static-libgcc -static-libstdc++
if errorlevel 1 goto :fail
echo [3/3] Carregando a DLL somente no host de teste...
pushd "build"
CromeWalkBridgeTest_x64.exe
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :fail
echo.
echo [OK] Teste controlado concluido.
echo [NOTA] Nao injeta no Antica; confirma apenas o protocolo interno da bridge.
pause
exit /b 0

:fail
echo.
echo [ERRO] Teste falhou. Copie a saida para o chat.
pause
exit /b 1
