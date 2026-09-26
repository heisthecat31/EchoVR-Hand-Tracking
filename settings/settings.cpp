// EchoXRSettings -- a settings window for EchoXR Hands. Every change is written to
// plugins\EchoXRHands.txt as it's made, and the plugin re-reads that file within half a
// second, so the change shows in-game while you play.
//
//   EchoXRSettings.exe                  finds EchoXRHands.txt (see FindConfig)
//   EchoXRSettings.exe --file <path>    edits that file
//
// The file is edited in place: comments, order and keys this window doesn't know are
// kept, and only the line of the setting that changed is rewritten (the last one, if a
// key is set twice -- that's the one the plugin uses). A key missing from the file is
// appended. Edits made in another editor show up here while the window is open.
//
// The window is drawn with the installer's GDI+ kit (installer/ui.h): Paint*() draw
// and register clickable rectangles in g_hots. Text settings are EDIT controls laid
// over the drawn boxes. "Calibrate" and the game-status pill talk to the plugin over
// its loopback UDP port, like EchoXRHands.exe --calibrate / --ping.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "res.h"
#include "../plugin/htv_protocol.h"

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// the settings (keys and meanings: plugin/handtracking.cpp ApplyConfigText,
// bridge/bridge.cpp LoadSettings, and the comments in EchoXRHands.txt)
// ---------------------------------------------------------------------------
enum Kind { K_BOOL, K_NUM, K_CHOICE, K_TEXT };
enum Applies { AP_LIVE, AP_GAME, AP_BRIDGE };   // when a change takes effect
enum { PG_GENERAL, PG_POSE, PG_FINGERS, PG_SMOOTH, PG_AXES, PG_SHARING, PG_BRIDGE, PG_COUNT };

struct Field {
    int page; const char* key; Kind kind; const wchar_t* title; const wchar_t* desc;
    float lo, hi, step; const wchar_t* unit;   // K_NUM
    const char* choices;                        // K_CHOICE: "value=Label|value=Label"
    const wchar_t* cue;                         // K_TEXT: shown while empty
    Applies ap;
};
static Field Bool(int pg, const char* k, const wchar_t* t, const wchar_t* d, Applies ap = AP_LIVE) {
    return { pg, k, K_BOOL, t, d, 0, 1, 1, L"", nullptr, nullptr, ap };
}
static Field Num(int pg, const char* k, const wchar_t* t, const wchar_t* d, float lo, float hi, float step, const wchar_t* unit = L"") {
    return { pg, k, K_NUM, t, d, lo, hi, step, unit, nullptr, nullptr, AP_LIVE };
}
static Field Pick(int pg, const char* k, const wchar_t* t, const wchar_t* d, const char* choices, Applies ap = AP_LIVE) {
    return { pg, k, K_CHOICE, t, d, 0, 0, 0, L"", choices, nullptr, ap };
}
static Field Str(int pg, const char* k, const wchar_t* t, const wchar_t* d, const wchar_t* cue, Applies ap = AP_LIVE) {
    return { pg, k, K_TEXT, t, d, 0, 0, 0, L"", nullptr, cue, ap };
}

static const char* kAxes = "auto=Auto|x=X|y=Y|z=Z";
static const char* kSign = "1=Normal|-1=Flipped";
static const std::vector<Field> kFields = {
    Bool(PG_GENERAL, "Enabled", L"Hand tracking", L"Pose your in-game hands from your real fingers. Off leaves Echo's own finger animation."),
    Bool(PG_GENERAL, "AutoCalibrate", L"Calibrate automatically", L"Capture the open-hand reference the first time both hands are fully open."),
    Bool(PG_GENERAL, "CalibrateOnLaunch", L"Calibrate hands automatically 10 seconds after Echo launches", L"The finger bridge asks you out loud to hold your hands in front of your face, flat out, counts down from 3, then calibrates. Once per launch; needs the finger bridge running.", AP_GAME),
    Num (PG_GENERAL, "OpenThreshold", L"Open-hand threshold", L"A hand counts as fully open when every finger's curl is below this.", 0, 0.5f, 0.01f),
    Bool(PG_GENERAL, "UseRig", L"Pose from the chassis rig", L"Build poses from the rig's own bind pose (recommended). Off uses a pose captured in-game."),
    Bool(PG_GENERAL, "RespectContact", L"Let go on contact", L"Leave a hand to the engine while it wraps around a surface."),
    Num (PG_GENERAL, "ContactHoldMs", L"Contact hold", L"After a hand stops touching something, leave it to the engine this long. Stops flicker.", 0, 500, 10, L" ms"),
    Num (PG_GENERAL, "StaleMs", L"Stale tracking", L"Drop tracking older than this (bridge closed, SteamVR hitch).", 50, 1000, 10, L" ms"),
    Num (PG_GENERAL, "TargetInstance", L"Your hand animator", L"Which hand-animator instance is you. EchoXRHands.log lists them; -1 poses every player with your hands.", -1, 15, 1),
    Pick(PG_GENERAL, "GameHandLeft", L"Left hand slot", L"Which of the game's two hand slots is your left hand. Swap if your hands are mirrored.", "0=Slot 0|1=Slot 1"),

    Num (PG_POSE, "OpenStraighten", L"Open straighten", L"How far a fully open finger straightens past the rig's relaxed pose.", 0, 45, 1, L"°"),
    Num (PG_POSE, "ThumbStraighten", L"Thumb straighten", L"How far an open thumb stands up past its rest (thumbs up). Raise it if a thumbs up stays folded.", 0, 90, 1, L"°"),
    Num (PG_POSE, "MaxCurl1", L"Finger curl: base joint", L"Bend of the knuckle at full curl.", 0, 150, 1, L"°"),
    Num (PG_POSE, "MaxCurl2", L"Finger curl: middle joint", L"Bend of the middle joint at full curl.", 0, 150, 1, L"°"),
    Num (PG_POSE, "MaxCurl3", L"Finger curl: tip joint", L"Bend of the fingertip joint at full curl.", 0, 150, 1, L"°"),
    Num (PG_POSE, "ThumbMax1", L"Thumb curl: base joint", L"Bend of the thumb's base at full curl.", 0, 120, 1, L"°"),
    Num (PG_POSE, "ThumbMax2", L"Thumb curl: middle joint", L"Bend of the thumb's middle joint at full curl.", 0, 120, 1, L"°"),
    Num (PG_POSE, "ThumbMax3", L"Thumb curl: tip joint", L"Bend of the thumb's tip at full curl.", 0, 120, 1, L"°"),
    Num (PG_POSE, "SplayDeg", L"Finger spread", L"Spread from the Index controllers' splay sensing, relative to your relaxed open hand. 0 = off; try 10-20.", 0, 40, 1, L"°"),
    Pick(PG_POSE, "SplayAxis", L"Spread axis", L"The joint axis fingers spread around.", "x=X|y=Y|z=Z"),

    Num (PG_FINGERS, "SpreadThumb", L"Thumb lean", L"Constant sideways lean at the knuckle.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "SpreadIndex", L"Index lean", L"Constant sideways lean at the knuckle.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "SpreadMiddle", L"Middle lean", L"Constant sideways lean at the knuckle.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "SpreadRing", L"Ring lean", L"Constant sideways lean at the knuckle.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "SpreadPinky", L"Pinky lean", L"Constant sideways lean at the knuckle.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "TwistThumb", L"Thumb drift", L"Positive tilts the thumb's fold toward the palm.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "TwistIndex", L"Index drift", L"Sideways drift as the finger curls.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "TwistMiddle", L"Middle drift", L"Sideways drift as the finger curls.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "TwistRing", L"Ring drift", L"Sideways drift as the finger curls.", -20, 20, 0.5f, L"°"),
    Num (PG_FINGERS, "TwistPinky", L"Pinky drift", L"Sideways drift as the finger curls.", -20, 20, 0.5f, L"°"),

    Num (PG_SMOOTH, "FilterMinCutoff", L"Steadiness", L"Jitter filter cutoff while still. Lower = steadier (still shaky? try 0.3), higher = less lag.", 0.05f, 5, 0.05f, L" Hz"),
    Num (PG_SMOOTH, "FilterBeta", L"Responsiveness", L"How quickly the filter catches up when your fingers move. Raise it if it feels laggy.", 0, 20, 0.1f),
    Num (PG_SMOOTH, "FilterDCutoff", L"Speed cutoff", L"Smoothing of the speed estimate the filter uses. Rarely needs changing.", 0.1f, 5, 0.1f, L" Hz"),
    Num (PG_SMOOTH, "SmoothingMs", L"Curl smoothing", L"Captured-pose fallback only (rig off). Higher = steadier, more lag; 0 = raw.", 0, 200, 5, L" ms"),
    Num (PG_SMOOTH, "Deadband", L"Deadband", L"Curl changes smaller than this are treated as sensor noise.", 0, 0.05f, 0.001f),

    Pick(PG_AXES, "BendAxis", L"Finger bend axis", L"Auto works it out from the hand's shape. Pick an axis if fingers bend sideways or twist.", kAxes),
    Pick(PG_AXES, "LeftBendSign", L"Left fingers", L"Flip if the left hand's fingers bend backwards.", kSign),
    Pick(PG_AXES, "RightBendSign", L"Right fingers", L"Flip if the right hand's fingers bend backwards.", kSign),
    Pick(PG_AXES, "ThumbBendAxis", L"Thumb bend axis", L"Auto: the thumb folds across the palm. Pick an axis if the thumb moves the wrong way.", kAxes),
    Pick(PG_AXES, "LeftThumbSign", L"Left thumb", L"Flip if the left thumb bends the wrong way.", kSign),
    Pick(PG_AXES, "RightThumbSign", L"Right thumb", L"Flip if the right thumb bends the wrong way.", kSign),
    Pick(PG_AXES, "QuatConvention", L"Rotation convention", L"Auto detects it at calibration (it's logged). Changing it recalibrates.", "auto=Auto|standard=Standard|engine=Engine"),
    Str (PG_AXES, "FingerOrder", L"Finger order", L"auto, or the game's five finger slots in order, e.g. index,middle,ring,pinky,thumb. Changing it recalibrates.", L"auto"),

    Bool(PG_SHARING, "Network", L"Share fingers with other players", L"Send your finger curls through the relay, and pose other players running the plugin. Sends your display name and your match's player names; nothing is stored."),
    Num (PG_SHARING, "NetSendHz", L"Send rate", L"How often your fingers are sent.", 5, 60, 1, L" Hz"),
    Str (PG_SHARING, "MyName", L"Display name", L"Leave empty: the plugin finds your name in the match. Set it only if the log says it couldn't.", L"found in the match"),
    Str (PG_SHARING, "RelayUrl", L"Relay", L"The relay server that pairs you with other players.", L"no relay"),
    Str (PG_SHARING, "Platform", L"Platform (experimental)", L"Your user ID's platform prefix, e.g. STM. Empty = leave it. Don't use this on EchoVRCE.", L"off", AP_GAME),
    Str (PG_SHARING, "PlatformAccount", L"Platform account", L"The number after the prefix. For STM, your SteamID64 (17 digits). Empty = keep it.", L"keep", AP_GAME),

    Pick(PG_BRIDGE, "FingerSource", L"Finger source", L"Device: Index finger sensing. Bones: the hand skeleton (other controllers, hand tracking). Auto picks per controller.", "auto=Auto|device=Device|bones=Bones", AP_BRIDGE),
    Bool(PG_BRIDGE, "CalibrateHotkey", L"Calibrate hotkey", L"Ctrl+Alt+C recalibrates the open-hand pose. Off frees the hotkey.", AP_BRIDGE),
};

struct PageInfo { const wchar_t* name; wchar_t icon; const wchar_t* blurb; };
static const PageInfo kPages[PG_COUNT] = {
    { L"General",   0xE713, L"Turning hand tracking on and off, calibration, and when the game keeps control of your hands." },
    { L"Pose",      0xE8E1, L"How far each joint bends. Changes show on your hands straight away." },
    { L"Fingers",   0xE9E9, L"Per-finger tuning. Positive leans toward the pinky side of the hand, mirrored per hand." },
    { L"Smoothing", 0xE9D9, L"The jitter filter: steady while still, responsive while moving." },
    { L"Axes",      0xE7AD, L"Troubleshooting for fingers or thumbs that bend the wrong way." },
    { L"Sharing",   0xE774, L"Showing your fingers to other players, and seeing theirs." },
    { L"Bridge",    0xE703, L"The finger bridge (EchoXRHands.exe) reads these when it starts." },
};

struct Choice { std::string v; std::wstring label; };

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static const float kW = 960, kH = 760;      // logical client size
static const float kTop = 124;              // content viewport top
static const float kFoot = kH - 72;         // footer divider
static const float kCX = 236;               // content column
static const float kCW = kW - kCX - 30;
static const DWORD kInput = 0x11141A;

static HWND  g_wnd = nullptr;
static UINT  g_dpi = 96;
static int   g_page = PG_GENERAL;
static float g_scroll = 0, g_contentH = 0, g_headH = 0;
struct Row { int f; float y, h; };          // y from the top of the scrolled content
static std::vector<Row> g_rows;

static std::wstring g_path;                 // the settings file, or "" if not found
static std::wstring g_createAt;             // where a missing file would go
static std::vector<std::string> g_lines;    // the file, one entry per line (no line ends)
static bool g_crlf = true, g_finalNL = true;
static FILETIME g_mtime = {};               // the file's write time as we last read/wrote it
static std::vector<std::string> g_val, g_def;
static std::vector<bool> g_hasDef;
static std::vector<std::vector<Choice>> g_choices;
static std::vector<std::vector<float>> g_segW;
static std::vector<float> g_anim;           // toggle knobs, easing toward the value
static std::vector<std::pair<float, float>> g_track;   // slider x range per field (logical)
static std::vector<HWND> g_edit;            // K_TEXT fields
static HFONT  g_editFont = nullptr;
static HBRUSH g_inputBrush = nullptr;
static bool   g_syncing = false;            // setting edit text from code: ignore EN_CHANGE

static std::set<int> g_pending;             // fields changed since the last write
static bool  g_dirty = false, g_flushArmed = false;
static bool  g_textPending = false;         // typing in a text box, not committed yet
static int   g_writeFails = 0;
static int   g_drag = -1;                   // field whose slider is being dragged
static int   g_calLeft = 0;                 // calibrate countdown, seconds
static std::atomic<int> g_live{ -1 };       // plugin answering: -1 unknown, 0 no, 1 yes

static std::wstring g_status = L"Every change is saved as you make it.";
static DWORD g_statusTint = 0x8E95A3;
static wchar_t g_statusIcon = 0xE946;

#define WM_APP_LIVE (WM_APP + 1)
enum { T_ANIM = 1, T_FLUSH = 3, T_WATCH = 4, T_CAL = 5, T_TEXT = 6 };
enum Hot { H_NONE, H_CAL, H_FILE, H_LOG, H_BROWSE, H_CREATE, H_NAV0 = 100, H_FIELD = 1000 };
enum Part { P_ROW, P_MINUS, P_PLUS, P_TRACK, P_RESET, P_BOX, P_SEG0 = 8 };
static int Id(int f, int part) { return H_FIELD + f * 16 + part; }

#include "../installer/ui.h"   // palette, fonts, Button, Toggle, Header, Pill, ...

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring Parent(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}
static std::wstring FullPath(const std::wstring& p) {
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr);
    return n && n < MAX_PATH ? std::wstring(full) : p;
}
static std::wstring ExeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return Parent(self);
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
static bool FileTime(const std::wstring& p, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    ft = d.ftLastWriteTime;
    return true;
}
static void SetStatus(const std::wstring& s, DWORD tint, wchar_t icon) {
    g_status = s;
    g_statusTint = tint;
    g_statusIcon = icon;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

// Sends one text datagram to the plugin (the bridge's --set syntax) and waits for its reply.
static bool SendToPlugin(const char* text, std::string& reply, int timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    DWORD to = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(HTV_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (sendto(s, text, (int)strlen(text), 0, (const sockaddr*)&a, sizeof(a)) > 0) {
        char buf[128];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) { buf[n] = 0; reply = buf; ok = true; }
    }
    closesocket(s);
    return ok;
}

// Pings the plugin every 1.5 s, for the "game running" pill.
static DWORD WINAPI PingThread(void*) {
    for (;;) {
        std::string r;
        int live = SendToPlugin("Ping", r, 400) && r == "PONG" ? 1 : 0;
        if (g_live.exchange(live) != live && g_wnd) PostMessageW(g_wnd, WM_APP_LIVE, 0, 0);
        Sleep(1500);
    }
}

// ---------------------------------------------------------------------------
// the settings file
// ---------------------------------------------------------------------------
static std::string ResText(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    return p ? std::string(p, SizeofResource(nullptr, r)) : std::string();
}

static std::vector<std::string> SplitLines(const std::string& text, bool* crlf, bool* finalNL) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        std::string l = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    if (crlf) *crlf = text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos;
    if (finalNL) *finalNL = !lines.empty() && lines.back().empty() && text.size();
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// The line that sets `key`: the last one, as the plugin applies lines in order.
// Comment rules match ApplyConfigText.
static int FindLine(const std::vector<std::string>& lines, const char* key) {
    for (int i = (int)lines.size() - 1; i >= 0; --i) {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '/') continue;
        size_t eq = l.find('=');
        if (eq != std::string::npos && Trim(l.substr(0, eq)) == key) return i;
    }
    return -1;
}
static bool LineValue(const std::vector<std::string>& lines, const char* key, std::string& v) {
    int i = FindLine(lines, key);
    if (i < 0) return false;
    v = Trim(lines[i].substr(lines[i].find('=') + 1));
    return true;
}

static void ApplyLine(int f) {
    const Field& F = kFields[f];
    const std::string& v = g_val[f];
    int i = FindLine(g_lines, F.key);
    if (i >= 0) g_lines[i] = g_lines[i].substr(0, g_lines[i].find('=') + 1) + (v.empty() ? "" : " " + v);
    else g_lines.push_back(std::string(F.key) + " =" + (v.empty() ? "" : " " + v));
}

static void SyncEdits() {
    g_syncing = true;
    for (size_t f = 0; f < kFields.size(); ++f)
        if (g_edit[f]) {
            wchar_t cur[1024];
            GetWindowTextW(g_edit[f], cur, 1024);
            std::wstring want = W(g_val[f]);
            if (want != cur) SetWindowTextW(g_edit[f], want.c_str());
        }
    g_syncing = false;
}

// Reads the file; values come from it, or the defaults for keys it doesn't set.
// Fields in `keep` hold their current (unsaved) value.
static bool ReadConfig(const std::set<int>& keep = {}) {
    HANDLE h = CreateFileW(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20)) {
        text.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!text.empty() && (!ReadFile(h, &text[0], (DWORD)text.size(), &got, nullptr))) got = 0;
        text.resize(got);
    }
    FILETIME ft = {};
    GetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    if (text.size() >= 3 && !memcmp(text.data(), "\xEF\xBB\xBF", 3)) text.erase(0, 3);
    g_lines = SplitLines(text, &g_crlf, &g_finalNL);
    g_mtime = ft;
    for (size_t f = 0; f < kFields.size(); ++f) {
        if (keep.count((int)f)) { ApplyLine((int)f); continue; }
        std::string v;
        g_val[f] = LineValue(g_lines, kFields[f].key, v) ? v : g_def[f];
    }
    SyncEdits();
    return true;
}

// Writes g_lines: to <file>.tmp first, then swapped in, so the plugin never reads half
// a file. The swap fails while the plugin has the file open; the caller retries.
static bool WriteConfig(DWORD& err) {
    FILETIME now;
    if (FileTime(g_path, now) && CompareFileTime(&now, &g_mtime) != 0)
        ReadConfig(g_pending);   // changed elsewhere since we read it: start from that, keep ours
    std::string eol = g_crlf ? "\r\n" : "\n", out;
    for (size_t i = 0; i < g_lines.size(); ++i) {
        out += g_lines[i];
        if (i + 1 < g_lines.size() || g_finalNL) out += eol;
    }
    std::wstring tmp = g_path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = GetLastError(); return false; }
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wrote, nullptr) && wrote == out.size();
    err = GetLastError();
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        FileTime(g_path, g_mtime);
        return true;
    }
    if (ok) err = GetLastError();
    DeleteFileW(tmp.c_str());
    return false;
}

static void SavedStatus() {
    Applies ap = AP_LIVE;
    for (int f : g_pending) ap = max(ap, kFields[f].ap);
    if (ap == AP_BRIDGE) SetStatus(L"Saved. Restart the finger bridge (EchoXRHands.exe) to use it.", kWarn, 0xE7BA);
    else if (ap == AP_GAME) SetStatus(L"Saved. It takes effect the next time Echo starts.", kWarn, 0xE7BA);
    else if (g_live == 1) SetStatus(L"Saved. The game picks it up within half a second.", kGood, 0xE73E);
    else SetStatus(L"Saved. It takes effect when Echo starts.", kGood, 0xE73E);
}

static void Flush() {
    if (!g_dirty || g_path.empty()) return;
    DWORD err = 0;
    if (WriteConfig(err)) {
        SavedStatus();
        g_dirty = false;
        g_pending.clear();
        g_writeFails = 0;
        return;
    }
    // the plugin reading the file blocks the swap for a moment; keep trying
    if (++g_writeFails == 25) SetStatus(L"Couldn't save: " + ErrText(err), kBad, 0xE711);
}

// A change: into g_lines now, onto disk at most every 120 ms (a slider drag writes
// as it goes, not on every mouse move).
static void SetValue(int f, const std::string& v) {
    if (g_val[f] == v) return;
    g_val[f] = v;
    ApplyLine(f);
    g_pending.insert(f);
    g_dirty = true;
    if (!g_flushArmed) { g_flushArmed = true; SetTimer(g_wnd, T_FLUSH, 120, nullptr); }
    if (kFields[f].kind == K_BOOL) SetTimer(g_wnd, T_ANIM, 16, nullptr);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// Typed text is saved 700 ms after the last key (or when the box loses focus), so a
// half-typed relay address never reaches the plugin.
static void CommitText() {
    KillTimer(g_wnd, T_TEXT);
    g_textPending = false;
    for (size_t f = 0; f < kFields.size(); ++f)
        if (g_edit[f]) {
            int n = GetWindowTextLengthW(g_edit[f]);
            std::wstring w(n + 1, L'\0');
            GetWindowTextW(g_edit[f], &w[0], n + 1);
            w.resize(n);
            SetValue((int)f, Trim(U8(w)));
        }
}

// Looks next to this exe: EchoXR\Hands\ (the install layout), bin\win10, or the
// plugins folder itself. createAt is a plugins folder with no settings file yet.
static std::wstring FindConfig(std::wstring& createAt) {
    std::wstring exe = ExeDir();
    const std::wstring dirs[] = { exe + L"\\..\\..\\plugins", exe + L"\\plugins", exe };
    for (auto& d : dirs)
        if (Exists(d + L"\\EchoXRHands.txt")) return FullPath(d + L"\\EchoXRHands.txt");
    for (auto& d : dirs)
        if (Exists(d + L"\\EchoXRHands.dll")) { createAt = FullPath(d + L"\\EchoXRHands.txt"); break; }
    return L"";
}

// ---------------------------------------------------------------------------
// numbers
// ---------------------------------------------------------------------------
static int Decimals(float step) {
    for (int d = 0; d < 4; ++d) {
        double s = step * pow(10.0, d);
        if (fabs(s - floor(s + 0.5)) < 1e-4) return d;
    }
    return 4;
}
static std::string FmtNum(int f, double v) {
    const Field& F = kFields[f];
    v = F.lo + floor((v - F.lo) / F.step + 0.5) * F.step;
    v = min((double)F.hi, max((double)F.lo, v));
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", Decimals(F.step), v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
static double NumVal(int f) {
    const std::string& s = g_val[f];
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    return end == s.c_str() ? kFields[f].lo : v;
}
static bool ChoiceIs(const std::string& a, const std::string& b) {
    if (!_stricmp(a.c_str(), b.c_str())) return true;
    char *ea, *eb;
    double x = strtod(a.c_str(), &ea), y = strtod(b.c_str(), &eb);
    return ea != a.c_str() && !*ea && eb != b.c_str() && !*eb && x == y;
}
static bool IsDefault(int f) {
    if (!g_hasDef[f]) return true;
    if (kFields[f].kind == K_NUM) return fabs(NumVal(f) - strtod(g_def[f].c_str(), nullptr)) < 1e-6;
    return ChoiceIs(g_val[f], g_def[f]) || g_val[f] == g_def[f];
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
static float CtrlWidth(int f) {
    switch (kFields[f].kind) {
    case K_BOOL: return 44;
    case K_NUM: return 286;
    case K_TEXT: return 280;
    default: { float w = 0; for (float s : g_segW[f]) w += s; return w + 6; }
    }
}
static float TextWidth(int f) { return kCW - 40 - CtrlWidth(f) - 44; }

static void Layout() {
    Bitmap bm(1, 1);
    Graphics g(&bm);
    g_rows.clear();
    g_headH = 36 + TextHeight(g, kPages[g_page].blurb, g_fSmall, kCW - 8) + 18;
    float y = g_headH;
    for (size_t f = 0; f < kFields.size(); ++f) {
        if (kFields[f].page != g_page) continue;
        float h = max(72.f, 40 + TextHeight(g, kFields[f].desc, g_fSmall, TextWidth((int)f)) + 16);
        g_rows.push_back({ (int)f, y, h });
        y += h + 8;
    }
    g_contentH = y + 8;
    g_scroll = max(0.f, min(g_scroll, g_contentH - (kFoot - kTop)));
}

static void PlaceEdits() {
    for (size_t f = 0; f < kFields.size(); ++f) {
        if (!g_edit[f]) continue;
        bool show = false;
        if (!g_path.empty())
            for (const Row& r : g_rows)
                if (r.f == (int)f) {
                    float cy = kTop + r.y - g_scroll + r.h / 2;
                    RectF box(kCX + kCW - 20 - 280, cy - 17, 280, 34);
                    if (box.Y >= kTop && box.Y + box.Height <= kFoot - 2) {
                        float s = g_dpi / 96.f;
                        SetWindowPos(g_edit[f], nullptr, (int)((box.X + 12) * s), (int)((cy - 10) * s),
                                     (int)((box.Width - 24) * s), (int)(20 * s), SWP_NOZORDER | SWP_NOACTIVATE);
                        show = true;
                    }
                }
        ShowWindow(g_edit[f], show ? SW_SHOWNA : SW_HIDE);
    }
}

static void Scroll(float dy) {
    float before = g_scroll;
    g_scroll = max(0.f, min(g_scroll + dy, g_contentH - (kFoot - kTop)));
    if (g_scroll != before) { PlaceEdits(); InvalidateRect(g_wnd, nullptr, FALSE); }
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------
// hit areas inside the scrolled viewport only count where they're visible
static void HotIn(RectF r, int id) {
    float top = max(r.Y, kTop), bottom = min(r.Y + r.Height, kFoot);
    if (bottom > top) AddHot(RectF(r.X, top, r.Width, bottom - top), id);
}

static void RoundButton(Graphics& g, RectF r, int id, wchar_t glyph, bool enabled = true) {
    bool hot = enabled && g_hot == id;
    SolidBrush b(hot ? C(0xFFFFFF, g_press == id ? 26 : 16) : C(0xFFFFFF, 7));
    g.FillEllipse(&b, r);
    Pen p(C(hot ? 0x3A4150 : kBorder), 1);
    g.DrawEllipse(&p, r);
    Glyph(g, glyph, g_fIconSm, C(enabled ? kText : kFaint), RectF(r.X, r.Y + 0.5f, r.Width, r.Height));
    if (enabled) HotIn(r, id);
}

static void PaintRow(Graphics& g, int f, RectF r) {
    const Field& F = kFields[f];
    float R = r.X + r.Width - 20, cy = r.Y + r.Height / 2, cw = CtrlWidth(f);
    bool rowHot = F.kind == K_BOOL && g_hot == Id(f, P_ROW);
    FillRound(g, r, 14, C(rowHot ? kCardHi : kCard));
    StrokeRound(g, r, 14, C(kBorder));

    float tw = TextWidth(f);
    Text(g, F.title, g_fBodyB, C(kText), RectF(r.X + 20, r.Y + 14, tw, 22));
    if (F.ap != AP_LIVE) {
        RectF m;
        g.MeasureString(F.title, -1, g_fBodyB, PointF(0, 0), &m);
        Pill(g, r.X + 20 + m.Width + 8, r.Y + 25, F.ap == AP_BRIDGE ? L"Restart bridge" : L"Next launch", kWarn);
    }
    Text(g, F.desc, g_fSmall, C(kMuted), RectF(r.X + 20, r.Y + 40, tw, r.Height - 48), A_LEFT, true);

    if (!IsDefault(f)) {
        RectF rr(R - cw - 36, cy - 13, 26, 26);
        bool hot = g_hot == Id(f, P_RESET);
        if (hot) { SolidBrush b(C(0xFFFFFF, 14)); g.FillEllipse(&b, rr); }
        Glyph(g, 0xE7A7, g_fIconSm, C(hot ? kAccentHi : kFaint), RectF(rr.X, rr.Y + 0.5f, rr.Width, rr.Height));
        HotIn(rr, Id(f, P_RESET));
    }

    switch (F.kind) {
    case K_BOOL:
        Toggle(g, R - 44, cy - 12, g_anim[f]);
        HotIn(r, Id(f, P_ROW));
        break;
    case K_NUM: {
        double v = NumVal(f);
        float t = (float)max(0.0, min(1.0, (v - F.lo) / (F.hi - F.lo)));
        RoundButton(g, RectF(R - 286, cy - 13, 26, 26), Id(f, P_MINUS), 0xE738, v > F.lo + 1e-6);
        RoundButton(g, RectF(R - 90, cy - 13, 26, 26), Id(f, P_PLUS), 0xE710, v < F.hi - 1e-6);
        float x0 = R - 250, x1 = R - 100, kx = x0 + t * (x1 - x0);
        g_track[f] = { x0, x1 };
        FillRound(g, RectF(x0, cy - 2, x1 - x0, 4), 2, C(0x303644));
        if (kx > x0 + 1) {
            LinearGradientBrush lg(PointF(x0, 0), PointF(x1, 0), C(kAccent), C(kAccent2));
            FillRound(g, RectF(x0, cy - 2, kx - x0, 4), 2, lg);
        }
        bool active = g_drag == f || g_hot == Id(f, P_TRACK);
        if (active) { SolidBrush ring(C(kAccent, 60)); g.FillEllipse(&ring, kx - 12, cy - 12, 24.f, 24.f); }
        SolidBrush knob(C(0xFFFFFF));
        g.FillEllipse(&knob, kx - 8, cy - 8, 16.f, 16.f);
        HotIn(RectF(x0 - 10, cy - 15, x1 - x0 + 20, 30), Id(f, P_TRACK));
        Text(g, W(FmtNum(f, v)) + F.unit, g_fBodyB, C(kText), RectF(R - 60, cy - 12, 60, 24), A_RIGHT);
        break;
    }
    case K_CHOICE: {
        RectF box(R - cw, cy - 17, cw, 34);
        FillRound(g, box, 17, C(kInput));
        StrokeRound(g, box, 17, C(kBorder));
        float x = box.X + 3;
        for (size_t k = 0; k < g_choices[f].size(); ++k) {
            RectF s(x, cy - 14, g_segW[f][k], 28);
            bool sel = ChoiceIs(g_val[f], g_choices[f][k].v), hot = g_hot == Id(f, P_SEG0 + (int)k);
            if (sel) {
                LinearGradientBrush lg(PointF(s.X, 0), PointF(s.X + s.Width, 0), C(kAccent), C(kAccent2));
                FillRound(g, s, 14, lg);
            } else if (hot) FillRound(g, s, 14, C(0xFFFFFF, 14));
            Text(g, g_choices[f][k].label, g_fSmall, C(sel ? 0xFFFFFF : hot ? kText : kMuted), s, A_CENTER);
            if (!sel) HotIn(s, Id(f, P_SEG0 + (int)k));
            x += s.Width;
        }
        break;
    }
    case K_TEXT: {
        RectF box(R - 280, cy - 17, 280, 34);
        bool focus = g_edit[f] && GetFocus() == g_edit[f], hot = g_hot == Id(f, P_BOX);
        FillRound(g, box, 10, C(kInput));
        StrokeRound(g, box, 10, focus ? C(kAccent, 170) : C(hot ? 0x3A4150 : kBorder));
        HotIn(box, Id(f, P_BOX));
        break;
    }
    }
}

static void PaintNav(Graphics& g) {
    for (int p = 0; p < PG_COUNT; ++p) {
        RectF r(20, kTop + p * 46, 196, 40);
        bool sel = p == g_page, hot = g_hot == H_NAV0 + p;
        if (sel) FillRound(g, r, 12, C(kAccent, 34));
        else if (hot) FillRound(g, r, 12, C(0xFFFFFF, 10));
        Glyph(g, kPages[p].icon, g_fIcon, C(sel ? kAccentHi : hot ? kText : kMuted), RectF(r.X + 10, r.Y, 26, r.Height));
        Text(g, kPages[p].name, sel ? g_fBodyB : g_fBody, C(sel ? kText : hot ? kText : kMuted), RectF(r.X + 46, r.Y, r.Width - 50, r.Height));
        int changed = 0;
        for (size_t f = 0; f < kFields.size(); ++f) if (kFields[f].page == p && !IsDefault((int)f)) ++changed;
        if (changed) {
            std::wstring n = std::to_wstring(changed);
            RectF b(r.X + r.Width - 34, r.Y + 11, 24, 18);
            FillRound(g, b, 9, C(0xFFFFFF, 12));
            Text(g, n, g_fLabel, C(kMuted), b, A_CENTER);
        }
        AddHot(r, H_NAV0 + p);
    }
    const wchar_t* note = L"Numbers show how many settings on a page differ from the defaults. The undo arrow on a setting puts its default back.";
    Text(g, note, g_fSmall, C(kFaint), RectF(26, kTop + PG_COUNT * 46 + 18, 186, 120), A_LEFT, true);
}

static void PaintContent(Graphics& g) {
    g.SetClip(RectF(kCX - 8, kTop, kW - kCX + 8, kFoot - kTop));
    float top = kTop - g_scroll;
    Text(g, kPages[g_page].name, g_fH2, C(kText), RectF(kCX + 4, top, kCW, 30));
    Text(g, kPages[g_page].blurb, g_fSmall, C(kMuted), RectF(kCX + 4, top + 34, kCW - 8, g_headH - 34), A_LEFT, true);
    for (const Row& row : g_rows) {
        RectF r(kCX, top + row.y, kCW, row.h);
        if (r.Y > kFoot || r.Y + r.Height < kTop) continue;
        PaintRow(g, row.f, r);
    }
    g.ResetClip();
    float view = kFoot - kTop;
    if (g_contentH > view) {   // scroll position
        float th = max(40.f, view * view / g_contentH), ty = kTop + 4 + (view - 8 - th) * g_scroll / (g_contentH - view);
        FillRound(g, RectF(kW - 16, ty, 4, th), 2, C(0xFFFFFF, 30));
    }
}

static void PaintMissing(Graphics& g) {
    RectF c(kCX, kTop + 10, kCW, 200);
    FillRound(g, c, 14, C(kCard));
    StrokeRound(g, c, 14, C(kBorder));
    IconBubble(g, RectF(c.X + 20, c.Y + 22, 40, 40), 0xE7BA, kWarn, true);
    Text(g, L"EchoXRHands.txt wasn't found", g_fBodyB, C(kText), RectF(c.X + 76, c.Y + 22, c.Width - 96, 22));
    std::wstring msg = g_createAt.empty()
        ? L"It lives in Echo VR's bin\\win10\\plugins folder, next to EchoXRHands.dll. Run this from EchoXR\\Hands\\ in the game folder, or find the file yourself."
        : L"The plugin is installed in " + Parent(g_createAt) + L", but its settings file is missing. Create it with the default settings?";
    Text(g, msg, g_fBody, C(kMuted), RectF(c.X + 76, c.Y + 48, c.Width - 96, 80), A_LEFT, true);
    float x = c.X + 76;
    if (!g_createAt.empty()) { Button(g, RectF(x, c.Y + 140, 170, 40), H_CREATE, L"Create it", B_PRIMARY); x += 180; }
    Button(g, RectF(x, c.Y + 140, 150, 40), H_BROWSE, L"Find the file", B_GHOST);
}

static void Paint(Graphics& g) {
    g_hots.clear();
    SolidBrush bg(C(kBg));
    g.FillRectangle(&bg, 0.f, 0.f, kW, kH);
    Header(g, L"EchoXR Hands settings", g_path.empty() ? L"Settings file not found" : g_path.c_str());
    {
        const wchar_t* s = g_live == 1 ? L"Echo running: changes apply live" : g_live == 0 ? L"Echo not running" : L"Looking for Echo...";
        RectF m;
        g.MeasureString(s, -1, g_fSmall, PointF(0, 0), &m);
        Pill(g, kW - 32 - (m.Width + 26), 52, s, g_live == 1 ? kGood : kMuted);
    }
    if (g_path.empty()) PaintMissing(g);
    else { PaintNav(g); PaintContent(g); }

    SolidBrush line(C(kBorder));
    g.FillRectangle(&line, 0.f, kFoot, kW, 1.f);
    Glyph(g, g_statusIcon, g_fIconSm, C(g_statusTint), RectF(30, kFoot + 20, 20, 32));
    Text(g, g_status, g_fSmall, C(g_statusTint), RectF(56, kFoot + 20, 440, 32));
    bool have = !g_path.empty();
    std::wstring cal = g_calLeft ? L"Hands open... " + std::to_wstring(g_calLeft) : L"Calibrate";
    Button(g, RectF(kW - 32 - 170, kFoot + 14, 170, 44), H_CAL, cal, B_PRIMARY, have && g_live == 1 && !g_calLeft, 0xE8E1);
    Button(g, RectF(kW - 32 - 170 - 118, kFoot + 16, 108, 40), H_LOG, L"Open log", B_GHOST, have);
    Button(g, RectF(kW - 32 - 170 - 236, kFoot + 16, 108, 40), H_FILE, L"Open file", B_GHOST, have);
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------
static void Browse() {
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return;
    COMDLG_FILTERSPEC spec[] = { { L"EchoXR Hands settings", L"EchoXRHands.txt;*.txt" } };
    fd->SetFileTypes(1, spec);
    fd->SetTitle(L"Find EchoXRHands.txt (Echo VR's bin\\win10\\plugins folder)");
    IShellItem* item = nullptr;
    if (SUCCEEDED(fd->Show(g_wnd)) && SUCCEEDED(fd->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            g_path = p;
            CoTaskMemFree(p);
            if (!ReadConfig()) { SetStatus(L"Couldn't read " + g_path, kBad, 0xE711); g_path.clear(); }
            Layout();
            PlaceEdits();
        }
        item->Release();
    }
    fd->Release();
}

static void Create() {
    std::string text = ResText(IDR_DEFAULTS);
    FILE* f = nullptr;
    if (text.empty() || _wfopen_s(&f, g_createAt.c_str(), L"wb") || !f) {
        SetStatus(L"Couldn't create " + g_createAt + L": " + ErrText(GetLastError()), kBad, 0xE711);
        return;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    g_path = g_createAt;
    ReadConfig();
    Layout();
    PlaceEdits();
    SetStatus(L"Created with the default settings.", kGood, 0xE73E);
}

static void Calibrate() {
    std::string r;
    if (SendToPlugin("Calibrate = 1\n", r, 500)) SetStatus(L"Calibrated: your open hands are the new reference.", kGood, 0xE73E);
    else SetStatus(L"The game didn't answer. Is Echo running with EchoXRHands.dll?", kBad, 0xE711);
}

static void DragTo(int f, float x) {
    const Field& F = kFields[f];
    float t = max(0.f, min(1.f, (x - g_track[f].first) / (g_track[f].second - g_track[f].first)));
    SetValue(f, FmtNum(f, F.lo + t * (F.hi - F.lo)));
}

static void Click(int id) {
    if (id >= H_NAV0 && id < H_NAV0 + PG_COUNT) {
        CommitText();
        g_page = id - H_NAV0;
        g_scroll = 0;
        Layout();
        PlaceEdits();
    } else if (id >= H_FIELD) {
        int f = (id - H_FIELD) / 16, part = (id - H_FIELD) % 16;
        const Field& F = kFields[f];
        if (part == P_ROW && F.kind == K_BOOL) SetValue(f, atoi(g_val[f].c_str()) ? "0" : "1");
        else if (part == P_MINUS) SetValue(f, FmtNum(f, NumVal(f) - F.step));
        else if (part == P_PLUS) SetValue(f, FmtNum(f, NumVal(f) + F.step));
        else if (part == P_RESET) { SetValue(f, g_def[f]); SyncEdits(); }
        else if (part == P_BOX && g_edit[f]) { SetFocus(g_edit[f]); SendMessageW(g_edit[f], EM_SETSEL, 0, -1); }
        else if (part >= P_SEG0) SetValue(f, g_choices[f][part - P_SEG0].v);
    }
    switch (id) {
    case H_CAL: g_calLeft = 3; SetTimer(g_wnd, T_CAL, 1000, nullptr); SetStatus(L"Hold both hands fully open...", kAccent, 0xE8E1); break;
    case H_FILE: ShellExecuteW(g_wnd, L"open", g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case H_LOG: {
        std::wstring log = Parent(g_path) + L"\\EchoXRHands.log";
        if (Exists(log)) ShellExecuteW(g_wnd, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else SetStatus(L"No EchoXRHands.log yet: the plugin writes it when Echo starts.", kMuted, 0xE946);
        break;
    }
    case H_BROWSE: Browse(); break;
    case H_CREATE: Create(); break;
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
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-MulDiv(14, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (HWND e : g_edit) if (e) SendMessageW(e, WM_SETFONT, (WPARAM)g_editFont, TRUE);
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
        for (size_t f = 0; f < kFields.size(); ++f) {
            if (kFields[f].kind != K_TEXT) continue;
            g_edit[f] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                                        (HMENU)(INT_PTR)(5000 + f), nullptr, nullptr);
            SendMessageW(g_edit[f], EM_SETCUEBANNER, FALSE, (LPARAM)kFields[f].cue);
            SendMessageW(g_edit[f], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        }
        ResizeForDpi(h, nullptr);
        if (!g_path.empty() && !ReadConfig()) {
            SetStatus(L"Couldn't read " + g_path, kBad, 0xE711);
            g_path.clear();
        }
        for (size_t f = 0; f < kFields.size(); ++f) g_anim[f] = atoi(g_val[f].c_str()) ? 1.f : 0.f;
        Layout();
        PlaceEdits();
        SetTimer(h, T_WATCH, 500, nullptr);
        return 0;
    }
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        ResizeForDpi(h, (RECT*)lp);
        PlaceEdits();
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
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB((kText >> 16) & 255, (kText >> 8) & 255, kText & 255));
        SetBkColor(dc, RGB((kInput >> 16) & 255, (kInput >> 8) & 255, kInput & 255));
        return (LRESULT)g_inputBrush;
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= 5000) {
            if (HIWORD(wp) == EN_CHANGE && !g_syncing) { g_textPending = true; SetTimer(h, T_TEXT, 700, nullptr); }
            else if (HIWORD(wp) == EN_KILLFOCUS) CommitText();
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        switch (wp) {
        case T_ANIM: {
            bool moving = false;
            for (size_t f = 0; f < kFields.size(); ++f) {
                float target = atoi(g_val[f].c_str()) ? 1.f : 0.f;
                g_anim[f] += (target - g_anim[f]) * 0.3f;
                if (fabsf(target - g_anim[f]) < 0.01f) g_anim[f] = target; else moving = true;
            }
            if (!moving) KillTimer(h, T_ANIM);
            break;
        }
        case T_FLUSH:
            Flush();
            if (!g_dirty) { KillTimer(h, T_FLUSH); g_flushArmed = false; }
            break;
        case T_WATCH: {   // edited in another program: show it
            FILETIME ft;
            if (!g_path.empty() && !g_dirty && !g_textPending && FileTime(g_path, ft) && CompareFileTime(&ft, &g_mtime) != 0) {
                ReadConfig();
                for (size_t f = 0; f < kFields.size(); ++f) if (kFields[f].kind == K_BOOL) SetTimer(h, T_ANIM, 16, nullptr);
                SetStatus(L"Reloaded: EchoXRHands.txt was changed outside this window.", kAccent, 0xE72C);
            }
            break;
        }
        case T_CAL:
            if (--g_calLeft <= 0) { g_calLeft = 0; KillTimer(h, T_CAL); Calibrate(); }
            break;
        case T_TEXT: CommitText(); break;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_LIVE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL:
        Scroll(-GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 72);
        return 0;
    case WM_MOUSEMOVE: {
        float s = 96.f / g_dpi;
        if (g_drag >= 0) { DragTo(g_drag, GET_X_LPARAM(lp) * s); return 0; }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hot = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot != g_hot) { g_hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_drag < 0) { g_hot = H_NONE; g_press = H_NONE; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, g_hot != H_NONE ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        g_press = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (g_press < H_FIELD || (g_press - H_FIELD) % 16 != P_BOX) SetFocus(h);   // leave any text box
        SetCapture(h);
        if (g_press >= H_FIELD && (g_press - H_FIELD) % 16 == P_TRACK) {
            g_drag = (g_press - H_FIELD) / 16;
            DragTo(g_drag, GET_X_LPARAM(lp) * 96.f / g_dpi);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = g_press;
        g_press = H_NONE;
        if (g_drag >= 0) g_drag = -1;
        else if (id != H_NONE && id == pressed) Click(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (wp == VK_NEXT) Scroll(kFoot - kTop - 60);
        else if (wp == VK_PRIOR) Scroll(-(kFoot - kTop - 60));
        return 0;
    case WM_CLOSE:
        CommitText();
        for (int i = 0; i < 20 && g_dirty; ++i) { Flush(); if (g_dirty) Sleep(50); }
        if (g_dirty && MessageBoxW(h, L"The last change couldn't be saved to EchoXRHands.txt. Close anyway?",
                                   L"EchoXR Hands settings", MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--file") && i + 1 < argc) g_path = FullPath(argv[++i]);
    LocalFree(argv);
    if (g_path.empty()) g_path = FindConfig(g_createAt);
    else if (!Exists(g_path)) { g_createAt = g_path; g_path.clear(); }

    // defaults: the EchoXRHands.txt this was built with
    size_t n = kFields.size();
    g_val.resize(n); g_def.resize(n); g_hasDef.resize(n); g_anim.resize(n); g_track.resize(n);
    g_edit.resize(n); g_choices.resize(n); g_segW.resize(n);
    std::vector<std::string> defLines = SplitLines(ResText(IDR_DEFAULTS), nullptr, nullptr);
    for (size_t f = 0; f < n; ++f) {
        g_hasDef[f] = LineValue(defLines, kFields[f].key, g_def[f]);
        g_val[f] = g_def[f];
        if (kFields[f].kind != K_CHOICE) continue;
        std::string list = kFields[f].choices;
        for (size_t i = 0; i <= list.size();) {
            size_t bar = list.find('|', i);
            std::string item = list.substr(i, bar == std::string::npos ? std::string::npos : bar - i);
            size_t eq = item.find('=');
            g_choices[f].push_back({ item.substr(0, eq), W(item.substr(eq + 1)) });
            if (bar == std::string::npos) break;
            i = bar + 1;
        }
    }

    ULONG_PTR gdipToken;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdipToken, &gsi, nullptr);
    MakeFonts();
    g_uiW = kW;
    {
        Bitmap bm(1, 1);
        Graphics g(&bm);
        for (size_t f = 0; f < n; ++f)
            for (auto& c : g_choices[f]) {
                RectF m;
                g.MeasureString(c.label.c_str(), -1, g_fSmall, PointF(0, 0), &m);
                g_segW[f].push_back(max(52.f, m.Width + 26));
            }
    }
    g_inputBrush = CreateSolidBrush(RGB((kInput >> 16) & 255, (kInput >> 8) & 255, kInput & 255));
    CloseHandle(CreateThread(nullptr, 0, PingThread, nullptr, 0, nullptr));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x10, 0x15));
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"EchoXRSettings";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"EchoXR Hands settings", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, 960, 760, nullptr, nullptr, inst, nullptr);
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
    GdiplusShutdown(gdipToken);
    WSACleanup();
    CoUninitialize();
    return 0;
}
