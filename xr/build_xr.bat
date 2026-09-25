@echo off
rem Builds LibOVRRT64_1.dll: Revive's OpenXR backend (MIT) as a drop-in Oculus runtime for Echo.
setlocal
if not defined VSCMD_ARG_TGT_ARCH call "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

set REV=Revive\ReviveXR
set EXT=Revive\Externals
set OVR=ovr_sdk_pc\LibOVR
set XR=OpenXR-SDK\include
set XRB=openxr-build

if not exist out mkdir out
if not exist obj mkdir obj
if not exist include\detours mkdir include\detours
copy /Y ..\third_party\detours.h include\detours\detours.h >nul

set INC=/I%EXT%\microprofile /I%XR% /I%XRB%\include /I%OVR%\Include /I%EXT%\glad\include /I%EXT%\Vulkan\include /IRevive\ReviveOverlay /Iinclude
set DEF=/DXR_USE_PLATFORM_WIN32 /DVK_NO_PROTOTYPES /DVK_USE_PLATFORM_WIN32_KHR /DNOMINMAX /DMICROPROFILE_ENABLED=0 /DMICROPROFILE_GPU_TIMERS=0 /DOVR_DLL_BUILD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE

set SRC=%REV%\Common.cpp %REV%\HapticsBuffer.cpp %REV%\InputManager.cpp %REV%\REV_CAPI.cpp %REV%\REV_CAPI_Audio.cpp %REV%\REV_CAPI_D3D.cpp %REV%\REV_CAPI_GL.cpp %REV%\REV_CAPI_Vk.cpp %REV%\Session.cpp %REV%\Runtime.cpp %REV%\Swapchain.cpp %REV%\SwapchainD3D11.cpp %REV%\SwapchainD3D12.cpp %REV%\SwapchainGL.cpp %REV%\SwapchainVk.cpp %REV%\microprofile.cpp %OVR%\Shim\OVR_CAPI_Util.cpp %OVR%\Shim\OVR_StereoProjection.cpp src\xr_main.cpp

cl.exe /nologo /c /MD /O2 /W1 %INC% %DEF% /Foobj\glad.obj %EXT%\glad\src\glad.c
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cl.exe /nologo /LD /MD /O2 /EHsc /std:c++17 /W1 /MP /FIchrono %INC% %DEF% /Foobj\ /Fe"out\LibOVRRT64_1.dll" %SRC% obj\glad.obj ^
  /link %XRB%\src\loader\openxr_loader.lib ..\third_party\detours.lib Ws2_32.lib opengl32.lib d3d11.lib d3d12.lib dxgi.lib dxguid.lib dsound.lib Winmm.lib Shlwapi.lib Pathcch.lib user32.lib advapi32.lib ole32.lib
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
copy /Y %XRB%\src\loader\openxr_loader.dll out\ >nul
call build_launcher.bat
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cd /d "%~dp0"
del /q out\*.exp out\*.lib 2>nul
echo Built out\LibOVRRT64_1.dll and out\EchoXR.exe
