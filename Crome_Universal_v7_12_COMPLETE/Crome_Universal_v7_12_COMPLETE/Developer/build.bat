@echo off
setlocal
cd /d "%~dp0"

set "USER_DIR=%~dp0..\Crome"
set "BUILD_DIR=%~dp0build"

echo ============================================================
echo                 Crome - BUILD RELEASE
echo ============================================================
echo.

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCROME_BUILD_DEV_TOOLS=OFF
if errorlevel 1 goto :error

cmake --build "%BUILD_DIR%" --target Crome CromeRuntime --config Release -j 1
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\bin\Crome.exe" (
    echo [ERRO] O linker terminou sem gerar build\bin\Crome.exe
    goto :error
)
if not exist "%BUILD_DIR%\bin\CromeRuntime.dll" (
    echo [ERRO] O linker terminou sem gerar build\bin\CromeRuntime.dll
    goto :error
)

if not exist "%USER_DIR%" mkdir "%USER_DIR%"
if not exist "%USER_DIR%\profiles" mkdir "%USER_DIR%\profiles"
if not exist "%USER_DIR%\config" mkdir "%USER_DIR%\config"

copy /Y "%BUILD_DIR%\bin\Crome.exe" "%USER_DIR%\Crome.exe" >nul
copy /Y "%BUILD_DIR%\bin\CromeRuntime.dll" "%USER_DIR%\CromeRuntime.dll" >nul
xcopy /E /I /Y "%~dp0profiles\*" "%USER_DIR%\profiles\" >nul
copy /Y "%~dp0native_runtime\profiles\builds.json" "%USER_DIR%\profiles\builds.json" >nul

if /I "%~1"=="tests" (
    echo.
    echo [TEST] Compilando calibradores x64 Look ID + HackLight...
    call "%~dp0build_x64_feature_tests.bat"
    if errorlevel 1 goto :error
)

rem Runtime configs: backup current user values, then install a clean OFF-by-default schema.
for %%N in (bot healing targeting loot fishing tools waypoints action_test tile_action_test say_test items inspect_request inspect_result look_result) do (
    if exist "%USER_DIR%\config\%%N.json" copy /Y "%USER_DIR%\config\%%N.json" "%USER_DIR%\config\%%N.backup.json" >nul
    if exist "%~dp0config\%%N.json" copy /Y "%~dp0config\%%N.json" "%USER_DIR%\config\%%N.json" >nul
)

rem O catalogo global de IDs e conhecimento do usuario: nunca sobrescrever em rebuild.
if not exist "%USER_DIR%\config\walker_ids.json" (
    copy /Y "%~dp0config\walker_ids.json" "%USER_DIR%\config\walker_ids.json" >nul
)

echo.
echo ============================================================
echo [OK] Crome gerado com sucesso.
echo [DEV]  %BUILD_DIR%
echo [USER] %USER_DIR%\Crome.exe
echo ============================================================
echo.
pause
exit /b 0

:error
echo.
echo ============================================================
echo [ERRO] A compilacao falhou. Veja o erro acima.
echo ============================================================
echo.
pause
exit /b 1
