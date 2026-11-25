@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ============================================================
echo    CROME ANTICA - VALIDACAO C++ ISOLADA (SEM INJECAO)
echo ============================================================
echo.
rem Prefer MSYS2 UCRT64 when present. No production Crome.exe is compiled.
if exist "C:\msys64\ucrt64\bin\g++.exe" set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake nao encontrado. Instale ou abra MSYS2 UCRT64.
    goto :error
)
where g++ >nul 2>&1
if errorlevel 1 (
    echo [ERROR] g++ nao encontrado. Instale o toolchain MSYS2 UCRT64.
    goto :error
)
echo [1/3] Executando testes Python (PE sintetico)...
py -3 -m unittest -v test_build_identity.py
if errorlevel 1 goto :error
echo.
set "DEV=%~dp0..\..\Crome\Developer"
set "BUILD=%~dp0..\..\build_antica_identity_cpp"
echo [2/3] Compilando apenas diagnosticos C++ (SEM Crome.exe / CromeRuntime.dll)...
cmake -S "%DEV%" -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCROME_BUILD_DEV_TOOLS=ON
if errorlevel 1 goto :error
cmake --build "%BUILD%" --target code_identity_probe profile_inspector -j 2
if errorlevel 1 goto :error
echo.
echo [3/3] Comparando EXE Antica com o perfil de codigo...
set /p "EXE=Digite o caminho COMPLETO do Antica Retro opengl.exe: "
set "EXE=%EXE:"=%"
if not exist "%EXE%" (
    echo [ERROR] EXE nao encontrado: "%EXE%"
    goto :error
)
"%BUILD%\bin\code_identity_probe.exe" "%EXE%" "%~dp0antica_identity.json"
if errorlevel 1 goto :error
echo.
echo [OK] Python e C++ conferem a identidade do codigo.
echo [INFO] Nao houve leitura de memoria, injecao ou atualizacao do Crome.exe.
pause
exit /b 0

:error
echo.
echo [FALHA] Envie as linhas acima, especialmente o primeiro erro.
pause
exit /b 1
