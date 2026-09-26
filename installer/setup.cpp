// EchoXRSetup -- installs EchoXR (Echo VR on SteamVR through OpenXR) and EchoXR Hands
// (per-finger Valve Index hand tracking) into an Echo VR install.
//
//   EchoXRSetup.exe                                   the setup window
//   EchoXRSetup.exe --silent [--dir <path>] [--components <mask>] [--uninstall]
//
// Everything it writes is embedded as RCDATA (setup.rc). Layout under bin\win10:
//   dbgcore.dll                  plugin loader: loads every DLL in plugins\
//   plugins\EchoXRHands.dll      the hand tracking plugin
//   plugins\EchoXRHands.txt      its settings -- never overwritten; new defaults go to
//                                EchoXRHands.default.txt instead
//   EchoXR.exe, EchoXR\          OpenXR launcher + runtime (ReviveXR, OpenXR loader);
//                                EchoXR\echoxr.ini holds AutoStartHands
//   EchoXR\Hands\                finger bridge (EchoXRHands.exe) + its SteamVR manifest,
//                                openvr_api.dll, fake_index.py; EchoXRSettings.exe (with the plugin)
//   echovr_openxr.exe            patched copy of echovr.exe (see MakeOpenXRExe)
//
// Files from before the EchoXR rename (HandTrackingValve.dll, HandTrackingBridge\, the old
// shortcuts) are removed, and handtracking_config.txt becomes EchoXRHands.txt.
//
// Plugin loader (dbgcore.dll) rules:
//   none                             -> install the loader
//   the old 45 KB one (by hash)      -> install the loader; the old one moves to
//                                       plugins\dbgcore_legacy.dll and loads as a plugin
//   anything else + plugins\ exists  -> a working loader: kept, plugin goes in plugins\
//   anything else, no plugins\       -> not a plugin loader: replaced only when ticked,
//                                       old one saved as dbgcore.dll.bak
// Uninstall removes the ticked components, keeps EchoXRHands.txt, and keeps the
// loader unless the previous dbgcore.dll can be put back.
// The setup log goes to %TEMP%\EchoXRSetup.log.
//
// The window is drawn by hand with GDI+ (no dialog controls): Paint*() draw a page and
// register clickable rectangles in g_hots; mouse handling hit-tests against those.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <string>
#include <vector>
#include "res.h"
#include "../xr/src/echoxr_common.h"

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// what gets installed
// ---------------------------------------------------------------------------
// bit values are the --components mask: 1 hands plugin, 2 bridge, 4 EchoXR, 8 loader,
// 16 desktop shortcuts, 32 start hand tracking with EchoXR, 64 open the settings window
// with EchoXR
enum { C_PLUGIN, C_BRIDGE, C_XR, C_LOADER, C_SHORTCUTS, C_AUTOSTART, C_AUTOSETTINGS, C_COUNT };
static const int kDefaultMask = (1 << C_PLUGIN) | (1 << C_BRIDGE) | (1 << C_XR) | (1 << C_LOADER) | (1 << C_AUTOSTART);
static const int kCardOrder[C_COUNT] = { C_PLUGIN, C_BRIDGE, C_XR, C_AUTOSTART, C_AUTOSETTINGS, C_LOADER, C_SHORTCUTS };

struct Item { int comp; int res; const wchar_t* rel; };
static const Item kItems[] = {
    { C_LOADER, IDR_LOADER,    L"dbgcore.dll" },
    { C_PLUGIN, IDR_PLUGIN,    L"plugins\\EchoXRHands.dll" },
    { C_PLUGIN, IDR_CONFIG,    L"plugins\\EchoXRHands.txt" },
    { C_PLUGIN, IDR_SETTINGS,  L"EchoXR\\Hands\\EchoXRSettings.exe" },
    { C_BRIDGE, IDR_BRIDGE,    L"EchoXR\\Hands\\EchoXRHands.exe" },
    { C_BRIDGE, IDR_ACTIONS,   L"EchoXR\\Hands\\htv_actions.json" },
    { C_BRIDGE, IDR_BINDINGS,  L"EchoXR\\Hands\\htv_bindings_knuckles.json" },
    { C_BRIDGE, IDR_OPENVR,    L"EchoXR\\Hands\\openvr_api.dll" },
    { C_BRIDGE, IDR_FAKEINDEX, L"EchoXR\\Hands\\fake_index.py" },
    { C_XR,     IDR_LAUNCHER,  L"EchoXR.exe" },
    { C_XR,     IDR_OVRRT,     L"EchoXR\\LibOVRRT64_1.dll" },
    { C_XR,     IDR_XRLOADER,  L"EchoXR\\openxr_loader.dll" },
    { C_XR,     IDR_NOTICES,   L"EchoXR\\THIRD_PARTY_NOTICES.txt" },
};
static const wchar_t* kConfigRel  = L"plugins\\EchoXRHands.txt";
static const wchar_t* kDefaultRel = L"plugins\\EchoXRHands.default.txt";
static const wchar_t* kBridgeRel  = L"EchoXR\\Hands\\EchoXRHands.exe";
static const wchar_t* kIniRel     = L"EchoXR\\echoxr.ini";
static const wchar_t* kLnkBridge  = L"EchoXR Hands.lnk";
static const wchar_t* kLnkXR      = L"EchoXR.lnk";
static const wchar_t* kSettingsRel = L"EchoXR\\Hands\\EchoXRSettings.exe";
static const wchar_t* kLnkSettings = L"EchoXR Hands Settings.lnk";

// files the components leave behind at runtime; removed on uninstall
static const Item kRuntimeFiles[] = {
    { C_PLUGIN, 0, L"plugins\\EchoXRHands.log" },
    { C_PLUGIN, 0, L"plugins\\EchoXRHands.default.txt" },
    { C_XR,     0, L"EchoXR\\echoxr.ini" },
    { C_XR,     0, L"EchoXR\\launcher.log" },
    { C_XR,     0, L"EchoXR\\runtime.log" },
    { C_XR,     0, L"echovr_openxr.exe" },
};

// Names from before the EchoXR rename. Install and uninstall clear them out so the old
// plugin can't load next to the new one. (handtracking_config.txt is migrated instead.)
static const Item kLegacyFiles[] = {
    { C_PLUGIN, 0, L"plugins\\HandTrackingValve.dll" },
    { C_PLUGIN, 0, L"plugins\\HandTrackingValve.log" },
    { C_PLUGIN, 0, L"plugins\\handtracking_config.default.txt" },
    { C_BRIDGE, 0, L"HandTrackingBridge\\HandTrackingBridge.exe" },
    { C_BRIDGE, 0, L"HandTrackingBridge\\htv_actions.json" },
    { C_BRIDGE, 0, L"HandTrackingBridge\\htv_bindings_knuckles.json" },
    { C_BRIDGE, 0, L"HandTrackingBridge\\openvr_api.dll" },
    { C_BRIDGE, 0, L"HandTrackingBridge\\fake_index.py" },
};
static const wchar_t* kLegacyConfig = L"plugins\\handtracking_config.txt";
static const wchar_t* kLegacyLnks[] = { L"Echo Hand Tracking Bridge.lnk", L"Echo VR (OpenXR).lnk" };

// ---------------------------------------------------------------------------
// log: %TEMP%\EchoXRSetup.log, plus the step list on the progress page
// ---------------------------------------------------------------------------
enum StepKind { ST_OK, ST_KEPT, ST_FAIL, ST_INFO };
struct StepLine { int kind; std::wstring text; };

static FILE* g_logFile = nullptr;
static HWND g_wnd = nullptr;
static CRITICAL_SECTION g_linesLock;
static std::vector<StepLine> g_lines;
static std::atomic<int> g_done{0}, g_total{0};
#define WM_APP_STEP (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

static void Log(const wchar_t* fmt, ...) {
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputwc(L'\n', g_logFile);
    fflush(g_logFile);
}

static void Step(int kind, const std::wstring& text) {
    static const wchar_t* tag[] = { L"ok  ", L"kept", L"FAIL", L"    " };
    Log(L"  %ls %ls", tag[kind], text.c_str());
    EnterCriticalSection(&g_linesLock);
    g_lines.push_back({ kind, text });
    LeaveCriticalSection(&g_linesLock);
    if (g_wnd) PostMessageW(g_wnd, WM_APP_STEP, 0, 0);
}

static std::wstring ErrText(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"error";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + L" (" + std::to_wstring(e) + L")";
}

// ---------------------------------------------------------------------------
// file helpers
// ---------------------------------------------------------------------------
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring Parent(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// Accepts the Echo root, bin\win10, or anything above ready-at-dawn-echo-arena.
// Returns the bin\win10 folder (the one with echovr.exe), or "" if it isn't one.
static std::wstring NormalizeEchoDir(std::wstring p) {
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"')) p.pop_back();
    while (!p.empty() && (p.front() == L' ' || p.front() == L'"')) p.erase(0, 1);
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (p.empty()) return L"";
    const wchar_t* tails[] = { L"", L"\\bin\\win10", L"\\ready-at-dawn-echo-arena\\bin\\win10",
                               L"\\Software\\ready-at-dawn-echo-arena\\bin\\win10" };
    for (const wchar_t* t : tails) {
        std::wstring d = p + t;
        if (Exists(d + L"\\echovr.exe")) {
            wchar_t full[MAX_PATH];   // canonical form: backslashes, no "..", so paths compare
            DWORD n = GetFullPathNameW(d.c_str(), MAX_PATH, full, nullptr);
            return n && n < MAX_PATH ? std::wstring(full) : d;
        }
    }
    return L"";
}

static std::wstring ExeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return Parent(self);
}

static std::wstring FindEchoDir() {
    std::vector<std::wstring> cands;
    cands.push_back(ExeDir());                       // setup run from inside the game folder
    cands.push_back(Parent(ExeDir()));
    // Oculus app libraries: HKCU\Software\Oculus VR, LLC\Oculus\Libraries\{guid}\OriginalPath
    HKEY libs;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Oculus VR, LLC\\Oculus\\Libraries", 0, KEY_READ, &libs) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = 256;
            if (RegEnumKeyExW(libs, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            for (const wchar_t* val : { L"OriginalPath", L"Path" }) {
                wchar_t buf[1024];
                DWORD size = sizeof(buf);
                if (RegGetValueW(libs, sub, val, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
                    cands.push_back(std::wstring(buf) + L"\\Software\\ready-at-dawn-echo-arena");
            }
        }
        RegCloseKey(libs);
    }
    // the usual spots on every fixed drive
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* rel : { L"Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
                                    L"Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
                                    L"Oculus\\Games\\Software\\Software\\ready-at-dawn-echo-arena",
                                    L"Oculus Apps\\Software\\ready-at-dawn-echo-arena",
                                    L"ready-at-dawn-echo-arena",
                                    L"Games\\ready-at-dawn-echo-arena",
                                    L"EchoVR\\ready-at-dawn-echo-arena" })
            cands.push_back(root + rel);
    }
    for (auto& c : cands) {
        std::wstring d = NormalizeEchoDir(c);
        if (!d.empty()) return d;
    }
    return L"";
}

static bool ResData(int id, const void** data, DWORD* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    *data = g ? LockResource(g) : nullptr;
    *size = r ? SizeofResource(nullptr, r) : 0;
    return *data != nullptr;
}

// The loader rules, the echovr_openxr.exe patch and the file helpers live in
// xr/src/echoxr_common.h, shared with the EchoXR launcher.
using echoxr::ReadAll;
using echoxr::LoaderState;
using echoxr::L_MISSING;
using echoxr::L_OURS;
using echoxr::L_LEGACY;
using echoxr::L_OTHER_LOADER;
using echoxr::L_FOREIGN;
using echoxr::LoaderWanted;
using echoxr::kLegacyRel;
using echoxr::kModdedExe;

// What dbgcore.dll in the game folder is, against the loader this setup carries.
static LoaderState CheckLoader(const std::wstring& dir) {
    const void* data = nullptr; DWORD size = 0;
    ResData(IDR_LOADER, &data, &size);
    return echoxr::ClassifyLoader(dir, data, size);
}

// Writes an embedded resource: to <path>.new first, then swapped in, so a file that
// is in use fails cleanly instead of being left half-written.
static bool g_needsAdmin = false;   // last run failed on access denied
static bool WriteRes(int id, const std::wstring& path, std::wstring& err) {
    const void* data; DWORD size;
    if (!ResData(id, &data, &size)) { err = L"missing from this setup (bad build)"; return false; }
    SHCreateDirectoryExW(nullptr, Parent(path).c_str(), nullptr);
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) g_needsAdmin = true;
        err = ErrText(e);
        return false;
    }
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, size, &wrote, nullptr) && wrote == size;
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); err = ErrText(e); return false; }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        e = GetLastError();
        DeleteFileW(tmp.c_str());
        err = (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) ? L"in use -- close Echo VR and the bridge" : ErrText(e);
        return false;
    }
    return true;
}

static std::wstring DesktopDir() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetWorkingDirectory(Parent(target).c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

// Processes that hold files this setup replaces.
static std::wstring RunningBlockers(int mask) {
    std::vector<const wchar_t*> names;
    if (mask & ((1 << C_PLUGIN) | (1 << C_XR) | (1 << C_LOADER))) { names.push_back(L"echovr.exe"); names.push_back(kModdedExe); }
    if (mask & (1 << C_PLUGIN)) names.push_back(L"EchoXRSettings.exe");
    if (mask & (1 << C_BRIDGE)) { names.push_back(L"EchoXRHands.exe"); names.push_back(L"HandTrackingBridge.exe"); }
    if (mask & (1 << C_XR)) names.push_back(L"EchoXR.exe");
    std::wstring found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        for (const wchar_t* n : names)
            if (!_wcsicmp(pe.szExeFile, n) && found.find(n) == std::wstring::npos)
                found += (found.empty() ? L"" : L", ") + std::wstring(n);
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------------------
// install / uninstall
// ---------------------------------------------------------------------------
// echovr_openxr.exe (see echoxr::MakeOpenXRExe)
static bool MakeOpenXRExe(const std::wstring& dir, std::wstring& err) {
    size_t off = 0;
    DWORD e = 0;
    if (!echoxr::MakeOpenXRExe(dir, err, &off, &e)) {
        if (e == ERROR_ACCESS_DENIED) g_needsAdmin = true;
        return false;
    }
    Log(L"  patched file offset 0x%zx (rva 0x%lx)", off, echoxr::kSigCheckRva);
    return true;
}

static int CountSteps(int mask) {
    int n = (mask & (1 << C_XR)) ? 1 : 0;   // + echovr_openxr.exe
    for (const Item& it : kItems) if (mask & (1 << it.comp)) ++n;
    return max(n, 1);
}

// Where a .lnk points, or "".
static std::wstring ShortcutTarget(const std::wstring& lnk) {
    std::wstring out;
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
        SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(sl->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH))) out = buf;
    }
    if (pf) pf->Release();
    if (sl) sl->Release();
    return out;
}

// Clears out pre-rename files for the ticked components and carries the old settings over.
// Returns true when old desktop shortcuts into this install were removed, so the new
// ones get made in their place.
static bool RemoveLegacy(const std::wstring& dir, int mask) {
    if (mask & (1 << C_PLUGIN)) {
        std::wstring oldCfg = dir + L"\\" + kLegacyConfig, cfg = dir + L"\\" + kConfigRel;
        if (Exists(oldCfg) && !Exists(cfg) && MoveFileExW(oldCfg.c_str(), cfg.c_str(), 0))
            Step(ST_INFO, std::wstring(L"Kept your settings: ") + kLegacyConfig + L" is now " + kConfigRel);
    }
    int removed = 0;
    for (const Item& it : kLegacyFiles) {
        if (!(mask & (1 << it.comp))) continue;
        std::wstring p = dir + L"\\" + it.rel;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str())) { Log(L"  removed old %ls", it.rel); ++removed; }
    }
    if (mask & (1 << C_BRIDGE)) RemoveDirectoryW((dir + L"\\HandTrackingBridge").c_str());
    bool hadLnk = false;
    std::wstring desk = DesktopDir();
    if (!desk.empty() && (mask & ((1 << C_BRIDGE) | (1 << C_XR))))
        for (const wchar_t* l : kLegacyLnks) {
            std::wstring lnk = desk + L"\\" + l, target = ShortcutTarget(lnk);
            if (target.size() > dir.size() && !_wcsnicmp(target.c_str(), dir.c_str(), dir.size()) &&
                DeleteFileW(lnk.c_str())) {
                Log(L"  removed old shortcut %ls", l);
                ++removed;
                hadLnk = true;
            }
        }
    if (removed) Step(ST_INFO, L"Removed " + std::to_wstring(removed) + L" file(s) from the old Hand Tracking install");
    return hadLnk;
}

static bool Install(const std::wstring& dir, int mask) {
    g_needsAdmin = false;
    g_done = 0;
    g_total = CountSteps(mask);
    Log(L"Installing into %ls (components %d)", dir.c_str(), mask);
    int failed = 0;
    if (RemoveLegacy(dir, mask)) mask |= 1 << C_SHORTCUTS;   // replace the old shortcuts
    for (const Item& it : kItems) {
        if (!(mask & (1 << it.comp))) continue;
        std::wstring dst = dir + L"\\" + it.rel;
        bool keepConfig = it.res == IDR_CONFIG && Exists(dst);
        if (keepConfig) dst = dir + L"\\" + kDefaultRel;   // keep the player's tuned settings
        LoaderState ls = it.res == IDR_LOADER ? CheckLoader(dir) : L_OURS;
        if (ls == L_LEGACY) {
            // keep it running as a plugin under the new loader
            std::wstring legacy = dir + L"\\" + kLegacyRel;
            SHCreateDirectoryExW(nullptr, Parent(legacy).c_str(), nullptr);
            if (!MoveFileExW(dst.c_str(), legacy.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DWORD e = GetLastError();
                if (e == ERROR_ACCESS_DENIED) g_needsAdmin = true;
                Step(ST_FAIL, L"dbgcore.dll  -- couldn't move the old one into plugins\\: " + ErrText(e));
                ++failed;
                ++g_done;
                continue;
            }
            Step(ST_INFO, std::wstring(L"Moved the old dbgcore.dll to ") + kLegacyRel);
        } else if (ls == L_OTHER_LOADER || ls == L_FOREIGN) {
            std::wstring bak = dst + L".bak";
            for (int i = 2; Exists(bak); ++i) bak = dst + L".bak" + std::to_wstring(i);
            if (MoveFileExW(dst.c_str(), bak.c_str(), 0)) Step(ST_INFO, L"Backed up the old dbgcore.dll to " + bak.substr(dir.size() + 1));
        }
        std::wstring err;
        if (WriteRes(it.res, dst, err)) Step(keepConfig ? ST_KEPT : ST_OK, keepConfig ? std::wstring(it.rel) + L"  (your settings kept)" : it.rel);
        else { Step(ST_FAIL, std::wstring(it.rel) + L"  -- " + err); ++failed; }
        ++g_done;
        if (g_wnd) Sleep(60);   // lets the progress page show each step
    }
    if (mask & (1 << C_XR)) {
        std::wstring err;
        if (MakeOpenXRExe(dir, err)) Step(ST_OK, std::wstring(kModdedExe) + L"  (patched copy of echovr.exe)");
        else { Step(ST_FAIL, std::wstring(kModdedExe) + L"  -- " + err); ++failed; }
        ++g_done;
    }
    // EchoXR\echoxr.ini: whether EchoXR.exe starts the finger bridge and the settings window
    if ((mask & (1 << C_XR)) || Exists(dir + L"\\EchoXR.exe")) {
        bool on = (mask & (1 << C_AUTOSTART)) && Exists(dir + L"\\" + kBridgeRel);
        bool settings = (mask & (1 << C_AUTOSETTINGS)) && Exists(dir + L"\\" + kSettingsRel);
        std::string ini = std::string("# EchoXR launcher settings (written by EchoXRSetup)\r\n") +
                          "# 1 = EchoXR.exe also runs EchoXR\\Hands\\EchoXRHands.exe while Echo runs\r\n" +
                          "AutoStartHands = " + (on ? "1" : "0") + "\r\n" +
                          "# 1 = EchoXR.exe also opens the hand tracking settings window (EchoXRSettings.exe)\r\n" +
                          "AutoStartSettings = " + (settings ? "1" : "0") + "\r\n";
        FILE* f = nullptr;
        SHCreateDirectoryExW(nullptr, (dir + L"\\EchoXR").c_str(), nullptr);
        if (!_wfopen_s(&f, (dir + L"\\" + kIniRel).c_str(), L"wb") && f) {
            fwrite(ini.data(), 1, ini.size(), f);
            fclose(f);
            Step(on ? ST_OK : ST_INFO, on ? L"Hand tracking starts with EchoXR" : L"Hand tracking won't start with EchoXR (turned off)");
            Step(settings ? ST_OK : ST_INFO, settings ? L"The settings window opens with EchoXR" : L"The settings window won't open with EchoXR (turned off)");
        } else {
            Step(ST_FAIL, std::wstring(kIniRel) + L"  -- " + ErrText(GetLastError()));
            ++failed;
        }
    }
    if (mask & (1 << C_SHORTCUTS)) {
        std::wstring desk = DesktopDir();
        if (!desk.empty() && Exists(dir + L"\\" + kBridgeRel)) {
            bool ok = MakeShortcut(desk + L"\\" + kLnkBridge, dir + L"\\" + kBridgeRel,
                                   L"--print", L"EchoXR Hands: streams your Valve Index finger tracking to Echo VR");
            Step(ok ? ST_OK : ST_FAIL, std::wstring(L"Desktop shortcut: ") + kLnkBridge);
        }
        if (!desk.empty() && Exists(dir + L"\\EchoXR.exe")) {
            std::wstring args = Exists(dir + L"\\" + kModdedExe) ? std::wstring(L"--exe ") + kModdedExe : L"";
            bool ok = MakeShortcut(desk + L"\\" + kLnkXR, dir + L"\\EchoXR.exe", args, L"EchoXR: Echo VR on SteamVR through OpenXR");
            Step(ok ? ST_OK : ST_FAIL, std::wstring(L"Desktop shortcut: ") + kLnkXR);
        }
        if (!desk.empty() && Exists(dir + L"\\" + kSettingsRel)) {
            bool ok = MakeShortcut(desk + L"\\" + kLnkSettings, dir + L"\\" + kSettingsRel, L"",
                                   L"EchoXR Hands settings: tune your hands, applied in-game as you change them");
            Step(ok ? ST_OK : ST_FAIL, std::wstring(L"Desktop shortcut: ") + kLnkSettings);
        }
    }
    Log(failed ? L"%d file(s) failed" : L"Done", failed);
    return failed == 0;
}

static bool Uninstall(const std::wstring& dir, int mask) {
    g_needsAdmin = false;
    g_done = 0;
    g_total = CountSteps(mask);
    Log(L"Removing from %ls (components %d)", dir.c_str(), mask);
    int failed = 0;
    auto remove = [&](const wchar_t* rel, bool quiet) {
        std::wstring p = dir + L"\\" + rel;
        if (!Exists(p)) return;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str())) { if (!quiet) Step(ST_OK, std::wstring(L"Removed ") + rel); }
        else {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) g_needsAdmin = true;
            Step(ST_FAIL, std::wstring(rel) + L"  -- " + ErrText(e));
            ++failed;
        }
    };
    for (const Item& it : kItems) {
        if (!(mask & (1 << it.comp))) continue;
        if (it.comp == C_LOADER) {
            std::wstring cur = dir + L"\\dbgcore.dll", bak = cur + L".bak", legacy = dir + L"\\" + kLegacyRel;
            std::wstring prev = Exists(legacy) ? legacy : bak;
            if (Exists(prev) && CheckLoader(dir) == L_OURS && DeleteFileW(cur.c_str()) && MoveFileExW(prev.c_str(), cur.c_str(), 0))
                Step(ST_OK, L"Put the previous dbgcore.dll back");
            else
                Step(ST_KEPT, L"dbgcore.dll  (plugin loader; other plugins use it)");
        } else if (it.res == IDR_CONFIG) {
            if (Exists(dir + L"\\" + it.rel)) Step(ST_KEPT, std::wstring(it.rel) + L"  (your settings)");
        } else {
            remove(it.rel, false);
        }
        ++g_done;
        if (g_wnd) Sleep(60);
    }
    for (const Item& it : kRuntimeFiles)
        if (mask & (1 << it.comp)) remove(it.rel, true);
    for (const Item& it : kLegacyFiles)
        if (mask & (1 << it.comp)) remove(it.rel, true);
    if (mask & ((1 << C_BRIDGE) | (1 << C_PLUGIN))) RemoveDirectoryW((dir + L"\\EchoXR\\Hands").c_str());   // only if nothing's left in it
    if (mask & (1 << C_BRIDGE)) RemoveDirectoryW((dir + L"\\HandTrackingBridge").c_str());
    if (mask & (1 << C_XR)) RemoveDirectoryW((dir + L"\\EchoXR").c_str());
    std::wstring desk = DesktopDir();
    if (!desk.empty()) {
        if ((mask & (1 << C_BRIDGE)) && DeleteFileW((desk + L"\\" + kLnkBridge).c_str())) Step(ST_OK, std::wstring(L"Removed shortcut ") + kLnkBridge);
        if ((mask & (1 << C_XR)) && DeleteFileW((desk + L"\\" + kLnkXR).c_str())) Step(ST_OK, std::wstring(L"Removed shortcut ") + kLnkXR);
        if ((mask & (1 << C_PLUGIN)) && DeleteFileW((desk + L"\\" + kLnkSettings).c_str())) Step(ST_OK, std::wstring(L"Removed shortcut ") + kLnkSettings);
    }
    Log(failed ? L"%d file(s) could not be removed" : L"Done", failed);
    return failed == 0;
}

// Re-runs this setup elevated with the same choices and waits for it.
static bool RunElevated(const std::wstring& dir, int mask, bool uninstall) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args = L"--silent --dir \"" + dir + L"\" --components " + std::to_wstring(mask) + (uninstall ? L" --uninstall" : L"");
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }   // the elevated run appends to it
    bool started = ShellExecuteExW(&sei) && sei.hProcess;
    DWORD code = 1;
    if (started) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _wfopen_s(&g_logFile, (std::wstring(tmp) + L"EchoXRSetup.log").c_str(), L"a");
    if (!started) Step(ST_FAIL, L"Administrator run was cancelled");
    else Step(code == 0 ? ST_OK : ST_FAIL, code == 0 ? L"Finished as administrator" : L"Failed as administrator -- see %TEMP%\\EchoXRSetup.log");
    return started && code == 0;
}

// ---------------------------------------------------------------------------
// window state
// ---------------------------------------------------------------------------
enum Page { P_MAIN, P_WORK, P_DONE };
enum Hot { H_NONE, H_CHANGE, H_COMP0, H_INSTALL = H_COMP0 + C_COUNT, H_UNINSTALL, H_OPEN, H_CLOSE,
           H_BRIDGE, H_ADMIN, H_BACK, H_LOGFILE };

static const float kW = 720, kH = 800;   // logical client size
static const float kFoot = kH - 76;      // footer divider
static UINT  g_dpi = 96;
static Page  g_page = P_MAIN;
static std::wstring g_dir;               // validated bin\win10, or ""
static std::wstring g_dirArg;            // --dir from the command line
static LoaderState g_loader = L_MISSING;
static bool  g_on[C_COUNT];
static float g_anim[C_COUNT];            // toggle knob position, eases toward g_on
static std::wstring g_banner;            // warning above the footer on the main page
static DWORD g_confirmUntil = 0;         // uninstall asks for a second click until then
static bool  g_uninstalling = false, g_ok = false;
static int   g_runMask = 0;
static float g_prog = 0;                 // shown progress, eases toward g_done/g_total
static HANDLE g_worker = nullptr;
#include "ui.h"   // palette, fonts, Button, Toggle, Header, ... (shared with settings.cpp)

// ---------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------
struct CompInfo { wchar_t icon; const wchar_t* title; const wchar_t* desc; };
static const CompInfo kComp[C_COUNT] = {
    { 0xE8E1, L"Hand tracking",       L"Per-finger poses on your chassis hands, and other players'" },
    { 0xE703, L"Finger bridge",       L"Sends your own Valve Index fingers from SteamVR" },
    { 0xE7FC, L"EchoXR runtime",      L"Runs Echo on SteamVR through OpenXR, with no Oculus app" },
    { 0xE943, L"Plugin loader",       L"dbgcore.dll: loads everything in plugins\\" },
    { 0xE7F4, L"Desktop shortcuts",   L"EchoXR, EchoXR Hands and its settings on your desktop" },
    { 0xE768, L"Start hand tracking with EchoXR", L"Launching EchoXR also runs the finger bridge, and closes it after" },
    { 0xE713, L"Open settings with EchoXR", L"Launching EchoXR also opens the hand tracking settings window" },
};

// Auto-start needs the bridge and EchoXR, either ticked now or already installed.
static bool AutoStartAvailable() {
    return (g_on[C_BRIDGE] || Exists(g_dir + L"\\" + kBridgeRel)) && (g_on[C_XR] || Exists(g_dir + L"\\EchoXR.exe"));
}

// Opening the settings window with EchoXR needs it (it comes with hand tracking) and EchoXR.
static bool AutoSettingsAvailable() {
    return (g_on[C_PLUGIN] || Exists(g_dir + L"\\" + kSettingsRel)) && (g_on[C_XR] || Exists(g_dir + L"\\EchoXR.exe"));
}
static bool Available(int c) {
    return c == C_AUTOSTART ? AutoStartAvailable() : c == C_AUTOSETTINGS ? AutoSettingsAvailable() : true;
}

static int Mask() {
    int m = 0;
    for (int c = 0; c < C_COUNT; ++c) if (g_on[c]) m |= 1 << c;
    if (!AutoStartAvailable()) m &= ~(1 << C_AUTOSTART);
    if (!AutoSettingsAvailable()) m &= ~(1 << C_AUTOSETTINGS);
    return m;
}

static void PaintMain(Graphics& g) {
    Header(g, L"EchoXR", L"Echo VR on SteamVR, with full Valve Index hand tracking");

    // game folder card
    RectF fc(32, 124, kW - 64, 84);
    FillRound(g, fc, 14, C(kCard));
    StrokeRound(g, fc, 14, C(kBorder));
    {
        DWORD tint = g_dir.empty() ? kBad : kAccent;
        SolidBrush bub(C(tint, 38));
        g.FillEllipse(&bub, fc.X + 18, fc.Y + 22, 40.f, 40.f);
        float fx = fc.X + 28, fy = fc.Y + 34;               // 20x15 folder
        FillRound(g, RectF(fx, fy, 9, 5), 1.5f, C(tint));
        FillRound(g, RectF(fx, fy + 2.5f, 20, 13.5f), 2.5f, C(tint));
    }
    Text(g, L"GAME FOLDER", g_fLabel, C(kFaint), RectF(fc.X + 74, fc.Y + 16, 90, 18));
    if (g_dir.empty()) Pill(g, fc.X + 162, fc.Y + 25, L"Echo VR not found", kBad);
    else Pill(g, fc.X + 162, fc.Y + 25, L"Echo VR found", kGood);
    Text(g, g_dir.empty() ? L"Choose the ready-at-dawn-echo-arena folder" : g_dir, g_fBody,
         C(g_dir.empty() ? kMuted : kText), RectF(fc.X + 74, fc.Y + 42, fc.Width - 74 - 130, 26));
    Button(g, RectF(fc.X + fc.Width - 118, fc.Y + 24, 100, 36), H_CHANGE, g_dir.empty() ? L"Browse" : L"Change", B_GHOST);

    // components
    SectionLabel(g, 222, L"COMPONENTS");
    float y = 244;
    for (int i = 0; i < C_COUNT; ++i) {
        int c = kCardOrder[i];
        RectF r(32, y, kW - 64, 54);
        bool avail = Available(c);
        bool on = g_on[c] && avail;
        bool hot = avail && g_hot == H_COMP0 + c;
        FillRound(g, r, 14, C(hot ? kCardHi : kCard));
        StrokeRound(g, r, 14, on ? C(kAccent, 90) : C(kBorder));
        DWORD tint = kAccent;
        std::wstring desc = kComp[c].desc, badge;
        DWORD badgeTint = kGood;
        if (!avail) desc = c == C_AUTOSETTINGS ? L"Needs hand tracking and the EchoXR runtime" : L"Needs the finger bridge and the EchoXR runtime";
        if (c == C_LOADER && !g_dir.empty()) {
            switch (g_loader) {
            case L_MISSING:      badge = L"Missing"; badgeTint = kWarn; desc = L"Needed: nothing in plugins\\ loads without it"; break;
            case L_OURS:         badge = L"Installed"; desc = L"Already in place. Turn on to reinstall"; break;
            case L_LEGACY:       badge = L"Upgrade"; badgeTint = kAccent;
                                 desc = L"Your current dbgcore.dll moves into plugins\\ and keeps working"; break;
            case L_OTHER_LOADER: badge = L"Already set up"; desc = L"Your loader is kept; the plugin goes into its plugins\\ folder"; break;
            case L_FOREIGN:      badge = L"Not a plugin loader"; badgeTint = kWarn; tint = kWarn;
                                 desc = L"Turn on to replace this dbgcore.dll (old one saved as .bak)"; break;
            }
        }
        IconBubble(g, RectF(r.X + 16, r.Y + 7, 40, 40), kComp[c].icon, tint, on);
        Text(g, kComp[c].title, g_fBodyB, C(avail ? kText : kFaint), RectF(r.X + 72, r.Y + 6, 400, 22));
        if (!badge.empty()) {
            RectF m;
            g.MeasureString(kComp[c].title, -1, g_fBodyB, PointF(0, 0), &m);
            Pill(g, r.X + 72 + m.Width + 8, r.Y + 17, badge, badgeTint);
        }
        Text(g, desc, g_fSmall, C(c == C_LOADER && g_loader == L_FOREIGN ? kWarn : avail ? kMuted : kFaint), RectF(r.X + 72, r.Y + 28, r.Width - 150, 20));
        Toggle(g, r.X + r.Width - 64, r.Y + 15, avail ? g_anim[c] : 0.f);
        if (avail) AddHot(r, H_COMP0 + c);
        y += 60;
    }

    // warning banner
    if (!g_banner.empty()) {
        RectF b(32, kFoot - 56, kW - 64, 40);
        FillRound(g, b, 10, C(kWarn, 28));
        Glyph(g, 0xE7BA, g_fIcon, C(kWarn), RectF(b.X + 10, b.Y, 26, b.Height));
        Text(g, g_banner, g_fSmall, C(kWarn), RectF(b.X + 42, b.Y, b.Width - 52, b.Height));
    }

    // footer
    SolidBrush line(C(kBorder));
    g.FillRectangle(&line, 0.f, kFoot, kW, 1.f);
    bool can = !g_dir.empty();
    bool confirm = g_confirmUntil && GetTickCount() < g_confirmUntil;
    Button(g, RectF(32, kFoot + 20, 140, 40), H_UNINSTALL, confirm ? L"Confirm remove" : L"Uninstall", confirm ? B_DANGER : B_GHOST, can);
    Button(g, RectF(182, kFoot + 20, 132, 40), H_OPEN, L"Open folder", B_GHOST, can);
    Button(g, RectF(kW - 32 - 190, kFoot + 16, 190, 48), H_INSTALL, L"Install", B_PRIMARY, can && (Mask() & ~(1 << C_SHORTCUTS)), 0xE896);
}

static std::vector<StepLine> Lines() {
    EnterCriticalSection(&g_linesLock);
    std::vector<StepLine> v = g_lines;
    LeaveCriticalSection(&g_linesLock);
    return v;
}

static void PaintSteps(Graphics& g, float top, float bottom) {
    std::vector<StepLine> lines = Lines();
    RectF box(32, top, kW - 64, bottom - top);
    FillRound(g, box, 14, C(kCard));
    StrokeRound(g, box, 14, C(kBorder));
    int fit = (int)((box.Height - 20) / 28);
    int first = max(0, (int)lines.size() - fit);
    float y = box.Y + 10;
    for (int i = first; i < (int)lines.size(); ++i) {
        const StepLine& l = lines[i];
        wchar_t ic = l.kind == ST_FAIL ? 0xE711 : l.kind == ST_KEPT ? 0xE72E : l.kind == ST_INFO ? 0xE946 : 0xE73E;
        DWORD tint = l.kind == ST_FAIL ? kBad : l.kind == ST_KEPT ? kMuted : l.kind == ST_INFO ? kAccent : kGood;
        Glyph(g, ic, g_fIconSm, C(tint), RectF(box.X + 14, y, 20, 28));
        Text(g, l.text, g_fSmall, C(l.kind == ST_FAIL ? kBad : kText), RectF(box.X + 42, y, box.Width - 56, 28));
        y += 28;
    }
}

static void PaintWork(Graphics& g) {
    Header(g, g_uninstalling ? L"Removing..." : L"Installing...", g_dir.c_str());
    float t = g_prog;
    RectF bar(32, 140, kW - 64, 8);
    FillRound(g, bar, 4, C(0x262B36));
    if (t > 0.001f) {
        RectF fill(bar.X, bar.Y, max(8.f, bar.Width * t), bar.Height);
        LinearGradientBrush lg(PointF(bar.X, 0), PointF(bar.X + bar.Width, 0), C(kAccent), C(kAccent2));
        FillRound(g, fill, 4, lg);
    }
    Text(g, std::to_wstring((int)(t * 100 + 0.5f)) + L"%", g_fSmall, C(kMuted), RectF(kW - 132, 154, 100, 20), A_RIGHT);
    PaintSteps(g, 184, kH - 20);
}

static void PaintDone(Graphics& g) {
    bool bridge = !g_uninstalling && (g_runMask & (1 << C_BRIDGE)) && Exists(g_dir + L"\\" + kBridgeRel);
    const wchar_t* title = g_ok ? (g_uninstalling ? L"Removed" : L"You're all set") : L"Something didn't work";
    std::wstring sub = g_ok ? (g_uninstalling ? L"The selected components were removed." : L"Installed into " + g_dir)
                            : (g_needsAdmin ? L"Windows blocked access to the game folder." : L"Some files couldn't be written. Details below.");
    Header(g, title, sub.c_str());
    // status mark over the header badge
    RectF mark(32, 36, 56, 56);   // result mark over the logo
    FillRound(g, mark, 16, C(g_ok ? kGood : kBad));
    Glyph(g, g_ok ? 0xE73E : 0xE711, g_fIconBig, C(0x0E1015), RectF(mark.X, mark.Y + 1, 56, 56));

    float y = 124;
    if (g_ok && !g_uninstalling) {
        std::vector<std::pair<DWORD, std::wstring>> notes;
        if (g_runMask & (1 << C_PLUGIN))
            notes.push_back({ kAccent, L"Start Echo VR. Other players' tracked fingers show on their avatars." });
        if ((g_runMask & (1 << C_PLUGIN)) && Exists(g_dir + L"\\" + kSettingsRel))
            notes.push_back({ kAccent, L"Tune your hands with EchoXR\\Hands\\EchoXRSettings.exe. Changes show in-game while you play." });
        if ((g_runMask & (1 << C_AUTOSTART)) && Exists(g_dir + L"\\" + kBridgeRel))
            notes.push_back({ kAccent, L"Hand tracking starts by itself when you launch EchoXR. In-game, hold both hands fully open once to calibrate." });
        else if (g_runMask & (1 << C_BRIDGE))
            notes.push_back({ kAccent, L"To send your own fingers, start SteamVR, then Start hands. In-game, hold both hands fully open once to calibrate." });
        if ((g_runMask & (1 << C_AUTOSETTINGS)) && Exists(g_dir + L"\\" + kSettingsRel))
            notes.push_back({ kAccent, L"The settings window opens with EchoXR too, so you can tune your hands while you play." });
        if (g_runMask & (1 << C_XR))
            notes.push_back(Exists(g_dir + L"\\" + kModdedExe)
                ? std::make_pair(kAccent, std::wstring(L"Echo on SteamVR: open SteamVR, then run EchoXR.exe --exe echovr_openxr.exe (or use the desktop shortcut)."))
                : std::make_pair(kWarn, std::wstring(L"EchoXR is installed, but echovr_openxr.exe couldn't be made. See the details below.")));
        LoaderState ls = CheckLoader(g_dir);
        if (ls == L_MISSING || ls == L_FOREIGN || ls == L_LEGACY)
            notes.push_back({ kWarn, L"There's no plugin loader in the game folder, so plugins won't load. Run setup again with Plugin loader on." });
        SectionLabel(g, y, L"NEXT STEPS");
        y += 22;
        int n = 1;
        for (auto& note : notes) {
            float h = max(52.f, TextHeight(g, note.second, g_fBody, kW - 64 - 80) + 28);
            RectF r(32, y, kW - 64, h);
            FillRound(g, r, 14, C(kCard));
            StrokeRound(g, r, 14, C(kBorder));
            RectF num(r.X + 16, r.Y + h / 2 - 14, 28, 28);
            SolidBrush nb(C(note.first, 36));
            g.FillEllipse(&nb, num);
            if (note.first == kWarn) Glyph(g, 0xE7BA, g_fIconSm, C(kWarn), num);
            else Text(g, std::to_wstring(n), g_fBodyB, C(note.first), num, A_CENTER);
            Text(g, note.second, g_fBody, C(kText), RectF(r.X + 60, r.Y + 14, r.Width - 76, h - 20), A_LEFT, true);
            y += h + 8;
            ++n;
        }
        y += 8;
    }
    SectionLabel(g, y, L"DETAILS");
    PaintSteps(g, y + 22, kFoot - 34);
    Text(g, L"Full log: %TEMP%\\EchoXRSetup.log", g_fSmall, C(g_hot == H_LOGFILE ? kAccentHi : kFaint), RectF(34, kFoot - 28, 400, 18));
    AddHot(RectF(34, kFoot - 28, 250, 18), H_LOGFILE);

    SolidBrush line(C(kBorder));
    g.FillRectangle(&line, 0.f, kFoot, kW, 1.f);
    Button(g, RectF(32, kFoot + 20, 110, 40), H_BACK, L"Back", B_GHOST);
    float x = 152;
    if (bridge) { Button(g, RectF(x, kFoot + 20, 150, 40), H_BRIDGE, L"Start hands", B_GHOST, true, 0xE768); x += 160; }
    Button(g, RectF(x, kFoot + 20, 132, 40), H_OPEN, L"Open folder", B_GHOST);
    if (!g_ok && g_needsAdmin) Button(g, RectF(kW - 32 - 230, kFoot + 16, 230, 48), H_ADMIN, L"Retry as administrator", B_PRIMARY, true, 0xE7EF);
    else Button(g, RectF(kW - 32 - 150, kFoot + 16, 150, 48), H_CLOSE, L"Done", B_PRIMARY);
}

static void Paint(Graphics& g) {
    g_hots.clear();
    SolidBrush bg(C(kBg));
    g.FillRectangle(&bg, 0.f, 0.f, kW, kH);
    if (g_page == P_MAIN) PaintMain(g);
    else if (g_page == P_WORK) PaintWork(g);
    else PaintDone(g);
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------
static void SetDir(const std::wstring& d) {
    g_dir = d;
    g_loader = g_dir.empty() ? L_MISSING : CheckLoader(g_dir);
    g_on[C_LOADER] = !g_dir.empty() && LoaderWanted(g_loader);
    g_banner.clear();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void Browse() {
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return;
    DWORD opt = 0;
    fd->GetOptions(&opt);
    fd->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    fd->SetTitle(L"Select the Echo VR folder (ready-at-dawn-echo-arena)");
    IShellItem* item = nullptr;
    if (SUCCEEDED(fd->Show(g_wnd)) && SUCCEEDED(fd->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            std::wstring d = NormalizeEchoDir(p);
            SetDir(d);
            if (d.empty()) g_banner = L"There's no echovr.exe in that folder. Pick ready-at-dawn-echo-arena or its bin\\win10.";
            CoTaskMemFree(p);
        }
        item->Release();
    }
    fd->Release();
}

static bool g_elevate = false;
static DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = g_elevate ? RunElevated(g_dir, g_runMask, g_uninstalling)
                        : (g_uninstalling ? Uninstall(g_dir, g_runMask) : Install(g_dir, g_runMask));
    if (g_elevate) { g_done = g_total.load(); g_needsAdmin = false; }
    CoUninitialize();
    PostMessageW(g_wnd, WM_APP_DONE, ok, 0);
    return 0;
}

static void Start(bool uninstall, bool elevate = false) {
    if (g_dir.empty() || g_worker) return;
    if (!elevate) {
        g_runMask = Mask();
        std::wstring busy = RunningBlockers(g_runMask);
        if (!busy.empty()) { g_banner = L"Close " + busy + L" first, then try again."; InvalidateRect(g_wnd, nullptr, FALSE); return; }
    }
    g_banner.clear();
    g_uninstalling = uninstall;
    g_elevate = elevate;
    EnterCriticalSection(&g_linesLock);
    g_lines.clear();
    LeaveCriticalSection(&g_linesLock);
    g_prog = 0;
    g_done = 0;
    g_total = CountSteps(g_runMask);
    g_page = P_WORK;
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    SetTimer(g_wnd, 1, 16, nullptr);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void Click(int id) {
    if (id >= H_COMP0 && id < H_COMP0 + C_COUNT) {
        g_on[id - H_COMP0] = !g_on[id - H_COMP0];
        if (g_on[C_AUTOSTART] && !AutoStartAvailable()) g_anim[C_AUTOSTART] = 0;
        if (g_on[C_AUTOSETTINGS] && !AutoSettingsAvailable()) g_anim[C_AUTOSETTINGS] = 0;
        SetTimer(g_wnd, 1, 16, nullptr);
    }
    switch (id) {
    case H_CHANGE: Browse(); break;
    case H_INSTALL: Start(false); break;
    case H_UNINSTALL:
        if (g_confirmUntil && GetTickCount() < g_confirmUntil) { g_confirmUntil = 0; Start(true); }
        else { g_confirmUntil = GetTickCount() + 3000; SetTimer(g_wnd, 2, 3050, nullptr); }
        break;
    case H_OPEN: ShellExecuteW(g_wnd, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case H_BRIDGE: {
        std::wstring exe = g_dir + L"\\" + kBridgeRel;
        ShellExecuteW(g_wnd, L"open", exe.c_str(), L"--print", Parent(exe).c_str(), SW_SHOWNORMAL);
        break;
    }
    case H_ADMIN: Start(g_uninstalling, true); break;
    case H_LOGFILE: {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        ShellExecuteW(g_wnd, L"open", (std::wstring(tmp) + L"EchoXRSetup.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case H_BACK: g_page = P_MAIN; SetDir(g_dir); break;
    case H_CLOSE: DestroyWindow(g_wnd); break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static int HitTest(int px, int py) {
    float x = px * 96.f / g_dpi, y = py * 96.f / g_dpi;
    for (auto it = g_hots.rbegin(); it != g_hots.rend(); ++it)
        if (it->r.Contains(x, y)) return it->id;
    return H_NONE;
}

static void ResizeForDpi(HWND h, const RECT* suggested) {
    RECT r = { 0, 0, MulDiv((int)kW, g_dpi, 96), MulDiv((int)kH, g_dpi, 96) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, 0, g_dpi);
    if (suggested) SetWindowPos(h, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        g_dpi = GetDpiForWindow(h);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        COLORREF cap = RGB(0x18, 0x21, 0x4A);
        DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR, Win11*/, &cap, sizeof(cap));
        ResizeForDpi(h, nullptr);
        for (int c = 0; c < C_COUNT; ++c) { g_on[c] = (kDefaultMask >> c) & 1; }
        SetDir(g_dirArg.empty() ? FindEchoDir() : NormalizeEchoDir(g_dirArg));
        for (int c = 0; c < C_COUNT; ++c) g_anim[c] = g_on[c] ? 1.f : 0.f;
        return 0;
    }
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        ResizeForDpi(h, (RECT*)lp);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.ScaleTransform(g_dpi / 96.f, g_dpi / 96.f);
            Paint(g);
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            bool moving = false;
            for (int c = 0; c < C_COUNT; ++c) {
                float target = g_on[c] ? 1.f : 0.f;
                g_anim[c] += (target - g_anim[c]) * 0.3f;
                if (fabsf(target - g_anim[c]) < 0.01f) g_anim[c] = target; else moving = true;
            }
            float target = g_total ? (float)g_done / g_total : 0.f;
            g_prog += (target - g_prog) * 0.2f;
            if (fabsf(target - g_prog) < 0.002f) g_prog = target; else moving = true;
            if (!moving && !g_worker) KillTimer(h, 1);
        } else if (wp == 2) {
            KillTimer(h, 2);
            g_confirmUntil = 0;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_STEP: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_APP_DONE:
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
        g_ok = wp != 0;
        g_prog = 1;
        g_page = P_DONE;
        g_loader = CheckLoader(g_dir);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot != g_hot) { g_hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = H_NONE; g_press = H_NONE; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, g_hot != H_NONE ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
        g_press = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = g_press;
        g_press = H_NONE;
        if (id != H_NONE && id == pressed) Click(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !g_worker) DestroyWindow(h);
        else if (wp == VK_RETURN) {
            if (g_page == P_MAIN && !g_dir.empty()) Start(false);
            else if (g_page == P_DONE) DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        if (g_worker) return 0;   // don't leave a half-written install
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_linesLock);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    bool silent = false, uninstall = false;
    std::wstring dirArg;
    int mask = -1;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--silent") silent = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--dir" && i + 1 < argc) dirArg = argv[++i];
        else if (a == L"--components" && i + 1 < argc) mask = _wtoi(argv[++i]);
    }
    LocalFree(argv);
    _wfopen_s(&g_logFile, (std::wstring(tmp) + L"EchoXRSetup.log").c_str(), silent ? L"a" : L"w");

    if (silent) {
        std::wstring dir = NormalizeEchoDir(dirArg.empty() ? FindEchoDir() : dirArg);
        if (dir.empty()) { Log(L"Echo VR not found%ls%ls", dirArg.empty() ? L"" : L" at ", dirArg.c_str()); return 2; }
        if (mask < 0) {
            mask = kDefaultMask;
            if (!LoaderWanted(CheckLoader(dir))) mask &= ~(1 << C_LOADER);   // don't replace a working dbgcore.dll unasked
        }
        bool ok = uninstall ? Uninstall(dir, mask) : Install(dir, mask);
        if (g_logFile) fclose(g_logFile);
        return ok ? 0 : 1;
    }

    g_dirArg = dirArg;
    ULONG_PTR gdipToken;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdipToken, &gsi, nullptr);
    MakeFonts();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x10, 0x15));
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"EchoXRSetup";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"EchoXR Setup", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, 800, 800, nullptr, nullptr, inst, nullptr);
    // centre on the monitor it opened on
    RECT wr;
    GetWindowRect(wnd, &wr);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    SetWindowPos(wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                 mi.rcWork.top + max(0L, (mi.rcWork.bottom - mi.rcWork.top - wh) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_logFile) fclose(g_logFile);
    GdiplusShutdown(gdipToken);
    CoUninitialize();
    return 0;
}
