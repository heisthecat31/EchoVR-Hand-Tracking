@echo off
rem Builds out\EchoXRSetup.exe: EchoXR (OpenXR runtime + launcher) and EchoXR Hands in one installer.
rem   build_installer.bat          build what's missing, then the installer
rem   build_installer.bat --all    rebuild the plugin, the bridge and EchoXR too
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
rem set up MSVC here: build.bat's setlocal would drop its environment on return
if not defined VSCMD_ARG_TGT_ARCH call "%VARS_BAT%" >nul
cd /d "%~dp0"
set "ECHO_BIN=C:\Oculus\Games\Software\Software\ready-at-dawn-echo-arena\bin\win10"

rem hand tracking plugin + bridge (quick, always rebuilt)
call ..\build.bat
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cd /d "%~dp0"

rem EchoXR runtime + launcher (slow; only when missing or --all)
set "XR_BUILD="
if /i "%~1"=="--all" set XR_BUILD=1
if not exist ..\xr\out\LibOVRRT64_1.dll set XR_BUILD=1
if not exist ..\xr\out\openxr_loader.dll set XR_BUILD=1
if defined XR_BUILD (
    call ..\xr\build_xr.bat
    if errorlevel 1 exit /b 1
) else (
    call ..\xr\build_launcher.bat
    if errorlevel 1 exit /b 1
)
cd /d "%~dp0"

if not exist stage mkdir stage
if not exist obj mkdir obj

rem plugin loader: stage\dbgcore.dll, taken from the game install if not staged yet
if not exist stage\dbgcore.dll if exist "%ECHO_BIN%\dbgcore.dll" copy /Y "%ECHO_BIN%\dbgcore.dll" stage\ >nul
if not exist stage\dbgcore.dll (
    echo [ERROR] Put the plugin loader at installer\stage\dbgcore.dll ^(or set ECHO_BIN in this script^).
    exit /b 1
)

rem third-party notices shipped with EchoXR
> stage\THIRD_PARTY_NOTICES.txt (
    echo EchoXR third-party notices
    echo.
    echo ==== Revive / ReviveXR ^(LibreVR^) -- MIT License ====
    type ..\xr\Revive\LICENSE
    echo.
    echo ==== OpenXR SDK and loader ^(The Khronos Group^) -- Apache License 2.0 ====
    type ..\xr\OpenXR-SDK\LICENSES\Apache-2.0.txt
    echo.
    echo ==== OpenVR ^(openvr_api.dll, Valve Corporation^) -- BSD 3-Clause ====
    type ..\xr\Revive\Externals\openvr\LICENSE
    echo.
    echo ==== Microsoft Detours -- MIT License ====
    echo Copyright ^(c^) Microsoft Corporation. Licensed under the MIT License.
)

echo Building EchoXRSetup.exe...
rc.exe /nologo /fo obj\setup.res setup.rc
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cl.exe /nologo /MT /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Foobj\ /Fe"..\out\EchoXRSetup.exe" setup.cpp obj\setup.res ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib ole32.lib advapi32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo.
echo Built %~dp0..\out\EchoXRSetup.exe
