// EchoXR runtime entry point -- replaces Revive's injection glue (ReviveXR/main.cpp).
//
// Revive normally INJECTS itself: it hooks LoadLibrary so a game asking for the
// Oculus runtime gets Revive instead. Echo doesn't need that. Echo's own LibOVR
// loader already checks LIBOVR_DLL_DIR for LibOVRRT64_1.dll, so this DLL is
// simply built under that name and loaded like the real Oculus runtime.
//
// Revive's core still calls these around instance creation (to un-hook its
// LoadLibrary detours while the OpenXR loader runs). There are no loader hooks
// here, so they are no-ops.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void AttachDetours() {}
void DetachDetours() {}

// ---------------------------------------------------------------------------
// runtime.log -- written next to this DLL. Every failed OpenXR call (CHK_XR in
// ReviveXR/Common.h) lands here with the call text and source line, so a
// runtime rejecting something names the exact call instead of a bare error code.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static char g_logPath[MAX_PATH];

void EchoXR_Log(const char* fmt, ...)
{
	if (!g_logPath[0]) return;
	EnterCriticalSection(&g_logLock);
	FILE* f = nullptr;
	if (fopen_s(&f, g_logPath, "a") == 0 && f) {
		SYSTEMTIME t; GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fclose(f);
	}
	LeaveCriticalSection(&g_logLock);
}

void EchoXR_LogFail(const char* call, int result, const char* file, int line)
{
	const char* base = strrchr(file, '\\');
	EchoXR_Log("FAILED %d at %s:%d  %s", result, base ? base + 1 : file, line, call);
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_logLock);
		GetModuleFileNameA((HMODULE)hModule, g_logPath, MAX_PATH);
		char* slash = strrchr(g_logPath, '\\');
		if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "runtime.log");
		FILE* f = nullptr;
		if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);     // fresh log per launch
		EchoXR_Log("EchoXR runtime loaded (LibOVRRT64_1 = ReviveXR over OpenXR)");
	}
	return TRUE;
}
