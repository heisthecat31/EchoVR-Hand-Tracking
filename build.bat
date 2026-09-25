@echo off
setlocal
set "VARS_BAT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VARS_BAT=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VARS_BAT if exist "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" set "VARS_BAT=J:\vs2026\VC\Auxiliary\Build\vcvars64.bat"
if not defined VARS_BAT (
    echo [ERROR] MSVC not found.
    exit /b 1
)
if not defined VSCMD_ARG_TGT_ARCH call "%VARS_BAT%" >nul

cd /d "%~dp0"
if not exist out mkdir out
if not exist obj mkdir obj

echo Building EchoXRHands.dll (plugin)...
cl.exe /nologo /LD /MD /O2 /EHa /W3 /Ithird_party /Foobj\ /Fe"out\EchoXRHands.dll" plugin\handtracking.cpp plugin\htv_net.cpp third_party\detours.lib ws2_32.lib winhttp.lib user32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo Building EchoXRHands.exe (finger bridge)...
rc.exe /nologo /fo obj\bridge.res bridge\bridge.rc
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cl.exe /nologo /MD /O2 /EHsc /W3 /Ithird_party /Foobj\ /Fe"out\EchoXRHands.exe" bridge\bridge.cpp obj\bridge.res ws2_32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

copy /Y EchoXRHands.txt out\ >nul
copy /Y bridge\htv_actions.json out\ >nul
copy /Y bridge\htv_bindings_knuckles.json out\ >nul
del /q out\*.exp out\*.lib 2>nul
echo.
echo Built into %~dp0out
