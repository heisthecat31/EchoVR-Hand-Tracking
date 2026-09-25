@echo off
rem Builds out\EchoXR.exe, the launcher (quick). build_xr.bat and the installer build call this.
setlocal
if not defined VSCMD_ARG_TGT_ARCH call "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
if not exist obj mkdir obj
rc.exe /nologo /fo obj\launcher.res src\launcher.rc
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cl.exe /nologo /MT /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Foobj\ /Fe"out\EchoXR.exe" src\launcher.cpp obj\launcher.res advapi32.lib shell32.lib user32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
