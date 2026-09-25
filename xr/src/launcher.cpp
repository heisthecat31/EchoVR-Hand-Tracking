// EchoXR launcher -- starts Echo VR on the OpenXR runtime (SteamVR), no Oculus software.
//
//   EchoXR.exe [--exe <name>] [--runtime steamvr|active] [--setup-only] [echo arguments...]
//
// It lives in bin\win10 next to echovr.exe and starts echovr_openxr.exe by default
// (--exe picks another). First it sets up what's missing: echovr_openxr.exe, a patched
// copy of echovr.exe (echoxr_common.h), and -- from a release zip's
// EchoXR\Hands\install\ -- the hand tracking plugin and plugin loader (SetupHands).
// Then it does three things before launching:
//   1. holds the "OculusHMDConnected" event. Echo's LibOVR shim calls ovr_Detect(),
//      which opens this event to decide whether a headset is present; the Oculus
//      service normally owns it. A plain named event -- no hooks, no injection.
//   2. sets LIBOVR_DLL_DIR to bin\win10\EchoXR\ -- the directory Echo's own loader
//      checks FIRST for LibOVRRT64_1.dll -- and puts that folder on PATH so the
//      runtime's openxr_loader.dll resolves. Only this launch sees these; a normal
//      launch of Echo is untouched.
//   3. logs which OpenXR runtime is active (SteamVR, VDXR, ...).
// Then it starts Echo with the remaining arguments and waits for it to exit. With
// "AutoStartHands = 1" in EchoXR\echoxr.ini it also runs the finger bridge
// (EchoXR\Hands\EchoXRHands.exe) for as long as Echo runs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include "echoxr_common.h"

static FILE* g_log = nullptr;
static void Log(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stdout, fmt, ap);
    va_end(ap);
    fputwc(L'\n', stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfwprintf(g_log, fmt, ap);
        va_end(ap);
        fputwc(L'\n', g_log);
        fflush(g_log);
    }
}

static std::wstring ActiveOpenXRRuntime() {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

// SteamVR's OpenXR manifest, from Steam's own runtime registry
// (%LOCALAPPDATA%\openvr\openvrpaths.vrpath -> "runtime": [ "<SteamVR dir>", ... ]).
static std::wstring SteamVROpenXRJson() {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(local) + L"\\openvr\\openvrpaths.vrpath").c_str(), L"rb") || !f) return L"";
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    size_t k = text.find("\"runtime\"");
    if (k == std::string::npos) return L"";
    size_t q1 = text.find('"', text.find('[', k));
    size_t q2 = text.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) return L"";
    std::string dir;
    for (size_t i = q1 + 1; i < q2; ++i) {             // unescape JSON "\\"
        if (text[i] == '\\' && i + 1 < q2) ++i;
        dir += text[i];
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    std::wstring wdir(wn ? wn - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, &wdir[0], wn);
    std::wstring json = wdir + L"\\steamxr_win64.json";
    return GetFileAttributesW(json.c_str()) != INVALID_FILE_ATTRIBUTES ? json : L"";
}

// "Key = 1" in a small INI-style file (whitespace and case around the key ignored).
static bool ReadIniFlag(const std::wstring& path, const char* key) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    char line[512];
    bool on = false;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, key, klen)) continue;
        p += klen;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        on = *p == '1' || !_strnicmp(p, "true", 4) || !_strnicmp(p, "yes", 3);
    }
    fclose(f);
    return on;
}

static bool IsRunning(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, name);
    CloseHandle(snap);
    return found;
}

// The bridge is a console app: give it its own console, minimised and unfocused.
static HANDLE StartBridge(const std::wstring& exe) {
    std::wstring cmd = L"\"" + exe + L"\" --print";
    std::wstring wd = exe.substr(0, exe.find_last_of(L'\\'));
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, wd.c_str(), &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Fatal setup problem: log it, and show it too (a double-clicked console closes at once).
static int Fail(const std::wstring& msg, int code) {
    Log(L"ERROR: %ls", msg.c_str());
    MessageBoxW(nullptr, msg.c_str(), L"EchoXR", MB_OK | MB_ICONERROR);
    return code;
}

static bool SameFile(const std::wstring& a, const std::wstring& b) {
    std::string x = echoxr::ReadAll(a);
    return !x.empty() && x == echoxr::ReadAll(b);
}

// Release-zip layout: EchoXR\Hands\install\ carries the hand tracking plugin, its
// default settings and the plugin loader. Each launch puts them into the game folder
// (same loader rules as the installer), so unzipping a newer release updates them.
// An installer-made install has no install\ folder, and this does nothing.
static void SetupHands(const std::wstring& gameDir, const std::wstring& xrDir) {
    std::wstring src = xrDir + L"Hands\\install\\", plugins = gameDir + L"\\plugins\\";
    if (!echoxr::Exists(src + L"EchoXRHands.dll")) return;
    std::string ours = echoxr::ReadAll(src + L"dbgcore.dll");
    echoxr::LoaderState st = echoxr::ClassifyLoader(gameDir, ours.data(), ours.size());
    if (st == echoxr::L_FOREIGN) {
        Log(L"hand tracking: a dbgcore.dll that isn't a plugin loader is in the game folder -- "
            L"left alone, so the hand tracking plugin isn't installed. Run EchoXRSetup.exe to replace it.");
        return;
    }
    CreateDirectoryW(plugins.c_str(), nullptr);
    if (st == echoxr::L_LEGACY) {
        std::wstring legacy = gameDir + L"\\" + echoxr::kLegacyRel;
        if (!MoveFileExW((gameDir + L"\\dbgcore.dll").c_str(), legacy.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log(L"hand tracking: couldn't move the old dbgcore.dll into plugins\\ (error %lu)", GetLastError());
            return;
        }
        Log(L"hand tracking: moved the old dbgcore.dll to %ls", echoxr::kLegacyRel);
    }
    if (st == echoxr::L_MISSING || st == echoxr::L_LEGACY) {
        DWORD e = echoxr::WriteAll(gameDir + L"\\dbgcore.dll", ours.data(), ours.size());
        Log(e ? L"hand tracking: couldn't install the plugin loader (error %lu)" : L"hand tracking: installed the plugin loader (dbgcore.dll)", e);
        if (e) return;
    }
    // pre-rename plugin: it would load next to the new one
    if (DeleteFileW((plugins + L"HandTrackingValve.dll").c_str())) Log(L"hand tracking: removed the old HandTrackingValve.dll");
    if (!echoxr::Exists(plugins + L"EchoXRHands.txt") && echoxr::Exists(plugins + L"handtracking_config.txt") &&
        MoveFileExW((plugins + L"handtracking_config.txt").c_str(), (plugins + L"EchoXRHands.txt").c_str(), 0))
        Log(L"hand tracking: kept your settings (handtracking_config.txt is now EchoXRHands.txt)");
    if (!SameFile(src + L"EchoXRHands.dll", plugins + L"EchoXRHands.dll")) {
        BOOL ok = CopyFileW((src + L"EchoXRHands.dll").c_str(), (plugins + L"EchoXRHands.dll").c_str(), FALSE);
        Log(ok ? L"hand tracking: installed plugins\\EchoXRHands.dll" : L"hand tracking: couldn't copy EchoXRHands.dll (error %lu) -- is Echo already running?", GetLastError());
    }
    if (!echoxr::Exists(plugins + L"EchoXRHands.txt") &&
        CopyFileW((src + L"EchoXRHands.txt").c_str(), (plugins + L"EchoXRHands.txt").c_str(), TRUE))
        Log(L"hand tracking: installed default settings (plugins\\EchoXRHands.txt)");
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);          // the bin\win10 folder
    std::wstring xrDir = dir + L"EchoXR\\";
    _wfopen_s(&g_log, (xrDir + L"launcher.log").c_str(), L"w");

    std::wstring exe = echoxr::kModdedExe;      // --exe <name> picks another executable in bin\win10
    std::wstring runtimeMode = L"steamvr";      // steamvr | active
    std::wstring passArgs;
    bool setupOnly = false;                     // --setup-only: do the first-run setup, don't launch
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--exe" && i + 1 < argc) { exe = argv[++i]; continue; }
        if (a == L"--runtime" && i + 1 < argc) { runtimeMode = argv[++i]; continue; }
        if (a == L"--setup-only") { setupOnly = true; continue; }
        passArgs += L" \"" + a + L"\"";
    }

    Log(L"EchoXR launcher");
    std::wstring gameDir = dir.substr(0, dir.size() - 1);
    if (!echoxr::Exists(dir + L"echovr.exe"))
        return Fail(L"EchoXR.exe has to sit in Echo VR's bin\\win10 folder, next to echovr.exe.\n\n"
                    L"Copy EchoXR.exe and the EchoXR folder into ...\\ready-at-dawn-echo-arena\\bin\\win10\\.", 2);
    if (!echoxr::Exists(xrDir + L"LibOVRRT64_1.dll"))
        return Fail(L"EchoXR\\LibOVRRT64_1.dll is missing. Copy the whole EchoXR folder next to EchoXR.exe.", 2);

    // first run: the patched game executable Echo needs to accept this runtime
    if (!_wcsicmp(exe.c_str(), echoxr::kModdedExe) && !echoxr::Exists(dir + exe)) {
        std::wstring err;
        size_t off = 0;
        if (!echoxr::MakeOpenXRExe(gameDir, err, &off))
            return Fail(L"Couldn't create echovr_openxr.exe: " + err + L".", 4);
        Log(L"created %ls (patched copy of echovr.exe, file offset 0x%zx)", echoxr::kModdedExe, off);
    }
    SetupHands(gameDir, xrDir);
    if (setupOnly) {
        Log(L"--setup-only: done, not launching");
        if (g_log) fclose(g_log);
        return 0;
    }
    std::wstring rt = ActiveOpenXRRuntime();
    Log(L"system OpenXR runtime: %ls", rt.empty() ? L"(none registered!)" : rt.c_str());
    if (runtimeMode == L"steamvr") {
        // Pin THIS launch to SteamVR. Other apps (e.g. Virtual Desktop's streamer) keep
        // re-registering themselves as the system runtime; the loader's XR_RUNTIME_JSON
        // override wins over that without changing any system setting.
        std::wstring svr = SteamVROpenXRJson();
        if (!svr.empty()) {
            SetEnvironmentVariableW(L"XR_RUNTIME_JSON", svr.c_str());
            Log(L"using SteamVR for this launch: %ls", svr.c_str());
        } else {
            Log(L"SteamVR not found -- falling back to the system runtime above");
        }
    }

    // 1. headset-present signal for ovr_Detect()
    HANDLE hmd = CreateEventW(nullptr, TRUE, TRUE, L"OculusHMDConnected");
    DWORD evErr = GetLastError();
    if (hmd)
        Log(L"OculusHMDConnected event: %ls", evErr == ERROR_ALREADY_EXISTS ? L"already present (Oculus service running)" : L"created");
    else if (evErr == ERROR_ACCESS_DENIED)
        Log(L"OculusHMDConnected event: owned by the Oculus service -- it reports the headset itself");
    else
        Log(L"OculusHMDConnected event: could not create (error %lu) -- Echo may start without VR", evErr);

    // 2. point Echo's LibOVR loader at our runtime, and let it find openxr_loader.dll
    SetEnvironmentVariableW(L"LIBOVR_DLL_DIR", xrDir.c_str());
    wchar_t path[32767];
    DWORD n = GetEnvironmentVariableW(L"PATH", path, 32767);
    std::wstring newPath = xrDir + L";" + (n ? std::wstring(path, n) : L"");
    SetEnvironmentVariableW(L"PATH", newPath.c_str());

    std::wstring cmd = L"\"" + dir + exe + L"\"" + passArgs;
    Log(L"launching: %ls", cmd.c_str());
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        Log(L"ERROR: could not start %ls (error %lu)", exe.c_str(), GetLastError());
        return 3;
    }

    // 3. hand tracking: EchoXR\echoxr.ini "AutoStartHands = 1" (set by the installer)
    //    starts the finger bridge next to Echo, restarts it if it drops out (e.g. SteamVR
    //    wasn't up yet), and closes it when Echo exits.
    std::wstring bridge = xrDir + L"Hands\\EchoXRHands.exe";
    if (!echoxr::Exists(xrDir + L"echoxr.ini") && echoxr::Exists(bridge)) {
        // release zip: no ini shipped, so an unzip never overwrites the player's choice
        const char ini[] = "# EchoXR launcher settings\r\n"
                           "# 1 = EchoXR.exe also runs EchoXR\\Hands\\EchoXRHands.exe while Echo runs\r\n"
                           "AutoStartHands = 1\r\n";
        echoxr::WriteAll(xrDir + L"echoxr.ini", ini, sizeof(ini) - 1);
        Log(L"hand tracking: created EchoXR\\echoxr.ini (AutoStartHands = 1)");
    }
    bool hands = ReadIniFlag(xrDir + L"echoxr.ini", "AutoStartHands");
    HANDLE hb = nullptr;
    DWORD lastStart = 0;
    int starts = 0;
    if (hands && IsRunning(L"EchoXRHands.exe")) {
        Log(L"hand tracking: EchoXRHands.exe is already running");
        hands = false;
    } else if (hands && GetFileAttributesW(bridge.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"hand tracking: %ls is missing -- reinstall EchoXR Hands", bridge.c_str());
        hands = false;
    }
    for (;;) {
        if (hands && (!hb || WaitForSingleObject(hb, 0) == WAIT_OBJECT_0) && starts < 20 &&
            (!starts || GetTickCount() - lastStart > 5000)) {
            if (hb) { CloseHandle(hb); hb = nullptr; }
            hb = StartBridge(bridge);
            lastStart = GetTickCount();
            ++starts;
            Log(hb ? L"hand tracking: started EchoXRHands.exe (%d)" : L"hand tracking: could not start EchoXRHands.exe (%d)", starts);
        }
        if (WaitForSingleObject(pi.hProcess, hands ? 1000 : INFINITE) == WAIT_OBJECT_0) break;
    }
    if (hb) {
        if (WaitForSingleObject(hb, 0) == WAIT_TIMEOUT) { TerminateProcess(hb, 0); Log(L"hand tracking: stopped EchoXRHands.exe"); }
        CloseHandle(hb);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Log(L"Echo exited with code %lu", code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hmd) CloseHandle(hmd);
    if (g_log) fclose(g_log);
    return (int)code;
}
