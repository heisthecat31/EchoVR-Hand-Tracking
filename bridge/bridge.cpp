// EchoXRHands.exe (the finger bridge) -- reads per-finger curl/splay from SteamVR and streams it
// to the EchoXR Hands plugin inside echovr.exe over loopback UDP.
//
//   EchoXRHands.exe                 stream to the game (default)
//   EchoXRHands.exe --print         stream AND print the live curls
//   EchoXRHands.exe --set "Key = Value" [...]
//                                          push live settings to the plugin
//   EchoXRHands.exe --calibrate     recapture the open-hand reference
//
// WHY THIS IS A SEPARATE PROCESS
// ------------------------------
// Inside echovr.exe, SteamVR input belongs to Revive: an OpenVR process gets ONE
// action manifest, Revive has already set it, and Revive's manifest has no
// skeleton actions (Revive/Input/action_manifest.json: touch/xbox/remote/system
// only). So the finger data cannot be read in-process. This bridge runs as its
// own OpenVR overlay app with a manifest that DOES declare the two hand
// skeletons, and asks for the summary FROM THE DEVICE -- the raw Index finger
// sensing, not an animation that SteamVR has smoothed.
//
// openvr_api is loaded dynamically: next to this exe first, then Revive's copy
// (C:\Program Files\Revive\openvr_api64.dll). Nothing is linked.
//
// FINGER SOURCES (FingerSource in plugins\EchoXRHands.txt; default auto)
// ---------------------------------------------------------------------
//   device  SteamVR's per-finger summary straight from the controller: the Index
//           controllers' finger sensing, curls and splay.
//   bones   curls worked out from the hand skeleton's joint rotations, against
//           SteamVR's own open-hand reference pose. This is what controller-free
//           hand tracking (Virtual Desktop, Steam Link, ALVR, ...) and other
//           controllers provide; there is no splay.
//   auto    device on Valve Index controllers, bones on everything else, and bones
//           whenever a device summary isn't available.
//
// Ctrl+Alt+C anywhere asks the plugin to recalibrate (CalibrateHotkey = 0 turns it off).
// With CalibrateOnLaunch = 1 the same happens by itself 10 seconds after Echo starts,
// with a spoken prompt and countdown (LaunchCalibrateThread).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <math.h>
#include <string.h>
#include <atomic>
#include <sapi.h>

#define OPENVR_INTERFACE_INTERNAL
#include "openvr.h"
#include "../plugin/htv_protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")

typedef uint32_t (VR_CALLTYPE* pfn_InitInternal2)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void     (VR_CALLTYPE* pfn_ShutdownInternal)();
typedef void*    (VR_CALLTYPE* pfn_GetGenericInterface)(const char*, vr::EVRInitError*);

static pfn_InitInternal2       p_Init = nullptr;
static pfn_ShutdownInternal    p_Shutdown = nullptr;
static pfn_GetGenericInterface p_GetInterface = nullptr;

static std::string ExeDir() {
    char p[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p;
    size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "" : s.substr(0, k + 1);
}

static bool LoadOpenVR() {
    const std::string candidates[] = {
        ExeDir() + "openvr_api.dll",
        ExeDir() + "openvr_api64.dll",
        "C:\\Program Files\\Revive\\openvr_api64.dll",
        "openvr_api.dll",
    };
    for (const std::string& c : candidates) {
        HMODULE h = LoadLibraryA(c.c_str());
        if (!h) continue;
        p_Init = (pfn_InitInternal2)GetProcAddress(h, "VR_InitInternal2");
        p_Shutdown = (pfn_ShutdownInternal)GetProcAddress(h, "VR_ShutdownInternal");
        p_GetInterface = (pfn_GetGenericInterface)GetProcAddress(h, "VR_GetGenericInterface");
        if (p_Init && p_Shutdown && p_GetInterface) { printf("openvr_api: %s\n", c.c_str()); return true; }
        FreeLibrary(h);
    }
    return false;
}

static SOCKET g_Sock = INVALID_SOCKET;
static sockaddr_in g_To = {};

static bool OpenSocket() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_Sock == INVALID_SOCKET) return false;
    g_To.sin_family = AF_INET;
    g_To.sin_port = htons(HTV_PORT);
    g_To.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD tmo = 500;
    setsockopt(g_Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    return true;
}

// Sends text to the plugin and prints its reply, or reports that it is not running.
static int SendText(const std::string& text) {
    if (!OpenSocket()) { printf("socket failed\n"); return 1; }
    sendto(g_Sock, text.c_str(), (int)text.size(), 0, (SOCKADDR*)&g_To, sizeof(g_To));
    char buf[256];
    int n = recv(g_Sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { printf("no reply -- is Echo VR running with EchoXRHands.dll loaded?\n"); return 1; }
    buf[n] = 0;
    printf("plugin: %s\n", buf);
    return 0;
}

// ---------------------------------------------------------------------------
// settings shared with the plugin: <game>\bin\win10\plugins\EchoXRHands.txt,
// two folders up from EchoXR\Hands\ (or next to this exe, for a manual setup)
// ---------------------------------------------------------------------------
enum Source { SRC_AUTO, SRC_DEVICE, SRC_BONES };
static Source g_Source = SRC_AUTO;
static bool   g_Hotkey = true;

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Every "Key = Value" in the first settings file found; returns that file's path, or "".
static std::string ReadSettings(std::vector<std::pair<std::string, std::string>>& out) {
    for (const std::string& path : { ExeDir() + "..\\..\\plugins\\EchoXRHands.txt", ExeDir() + "EchoXRHands.txt" }) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "r") || !f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == ';') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            out.push_back({ Trim(line), Trim(eq + 1) });
        }
        fclose(f);
        return path;
    }
    return "";
}

static void LoadSettings() {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string path = ReadSettings(kv);
    if (path.empty()) return;
    for (auto& kvp : kv) {
        const std::string &k = kvp.first, &v = kvp.second;
        if (k == "FingerSource") g_Source = v == "device" ? SRC_DEVICE : v == "bones" ? SRC_BONES : SRC_AUTO;
        else if (k == "CalibrateHotkey") g_Hotkey = v != "0";
    }
    printf("settings: %s (FingerSource = %s)\n", path.c_str(),
           g_Source == SRC_DEVICE ? "device" : g_Source == SRC_BONES ? "bones" : "auto");
}

// CalibrateOnLaunch, read fresh each time Echo starts (so it can be changed in the
// settings window without restarting the bridge).
static bool CalibrateOnLaunch() {
    std::vector<std::pair<std::string, std::string>> kv;
    ReadSettings(kv);
    bool on = false;
    for (auto& kvp : kv) if (kvp.first == "CalibrateOnLaunch") on = atoi(kvp.second.c_str()) != 0;   // the last one wins, as in the plugin
    return on;
}

// ---------------------------------------------------------------------------
// curls from bone rotations
// ---------------------------------------------------------------------------
// OpenVR hand skeleton: thumb joints 3-5, then index/middle/ring/pinky proximal,
// middle, distal at 8-10, 13-15, 18-20, 23-25 (each finger's metacarpal is one
// before, its tip one after).
static const int kJoint0[5] = { 3, 8, 13, 18, 23 };
// How far the three joints bend together from YOUR open hand to a full fist, in
// degrees (thumb: straight up to folded across the palm). curl = bend / span.
static const float kSpanDeg[5] = { 80.0f, 210.0f, 220.0f, 220.0f, 210.0f };

static float RelAngleDeg(const vr::HmdQuaternionf_t& ref, const vr::HmdQuaternionf_t& cur) {
    // angle of conj(ref) * cur; only w is needed: w = ref . cur
    float w = ref.w * cur.w + ref.x * cur.x + ref.y * cur.y + ref.z * cur.z;
    if (w < 0) w = -w;
    if (w > 1) w = 1;
    return 2.0f * acosf(w) * 57.2957795f;
}

// A tracked open hand never matches SteamVR's reference pose exactly (fingers
// read a little bent, the thumb a lot), so each finger's zero is the straightest
// it has been this session -- opening the hand flat once sets it -- and
// Ctrl+Alt+C resets it to the hand as it is at that moment.
struct BoneSide {
    bool  haveRef = false;
    vr::VRBoneTransform_t ref[31];
    bool  haveOpen = false, captureOpen = false;
    float open[5];
};
static BoneSide g_Bones[2];

static bool CurlsFromBones(vr::IVRInput* input, vr::VRActionHandle_t action, int side, float curl[5]) {
    BoneSide& b = g_Bones[side];
    if (!b.haveRef)
        b.haveRef = input->GetSkeletalReferenceTransforms(action, vr::VRSkeletalTransformSpace_Parent,
                                                           vr::VRSkeletalReferencePose_OpenHand, b.ref, 31) == vr::VRInputError_None;
    if (!b.haveRef) return false;
    vr::VRBoneTransform_t cur[31];
    if (input->GetSkeletalBoneData(action, vr::VRSkeletalTransformSpace_Parent,
                                   vr::VRSkeletalMotionRange_WithoutController, cur, 31) != vr::VRInputError_None) return false;
    float raw[5];
    for (int f = 0; f < 5; ++f) {
        raw[f] = 0;
        for (int j = 0; j < 3; ++j) raw[f] += RelAngleDeg(b.ref[kJoint0[f] + j].orientation, cur[kJoint0[f] + j].orientation);
    }
    if (!b.haveOpen || b.captureOpen) {
        memcpy(b.open, raw, sizeof(b.open));
        if (b.captureOpen) printf("\n%s hand: open-hand zero set\n", side ? "right" : "left");
        b.haveOpen = true;
        b.captureOpen = false;
    }
    for (int f = 0; f < 5; ++f) {
        if (raw[f] < b.open[f]) b.open[f] = raw[f];
        float c = (raw[f] - b.open[f]) / kSpanDeg[f];
        curl[f] = c < 0 ? 0 : c > 1 ? 1 : c;
    }
    return true;
}

// Which controller drives this hand ("knuckles" = Valve Index), refreshed every second.
static vr::IVRSystem* g_System = nullptr;
static std::string ControllerType(vr::IVRInput* input, const vr::InputSkeletalActionData_t& ad) {
    if (!g_System) return "";
    vr::InputOriginInfo_t oi = {};
    if (input->GetOriginTrackedDeviceInfo(ad.activeOrigin, &oi, sizeof(oi)) != vr::VRInputError_None) return "";
    char buf[128] = {};
    g_System->GetStringTrackedDeviceProperty(oi.trackedDeviceIndex, vr::Prop_ControllerType_String, buf, sizeof(buf), nullptr);
    return buf;
}

// ---------------------------------------------------------------------------
// calibrate on launch (CalibrateOnLaunch = 1): once per Echo launch, 10 seconds after
// the plugin loaded, say out loud to hold both hands open, count down, and calibrate
// the same way Ctrl+Alt+C does. The plugin's "Uptime" reply tells a fresh launch
// apart from this bridge (re)starting while a match is already going.
// ---------------------------------------------------------------------------
static const ULONGLONG kLaunchCalibrateMs = 10000;
static const ULONGLONG kFreshLaunchMs = 60000;   // plugin older than this: not a fresh launch
static std::atomic<bool> g_CalibrateNow(false);   // the streaming loop does the calibrating

// ms since the plugin loaded, -1 if Echo isn't running, -2 if it answered without an uptime
static long long PluginUptime(SOCKET s) {
    static const char q[] = "Uptime";
    if (sendto(s, q, sizeof(q) - 1, 0, (SOCKADDR*)&g_To, sizeof(g_To)) <= 0) return -1;
    char buf[64];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strncmp(buf, "UP ", 3) ? -2 : _atoi64(buf + 3);
}

static void Say(ISpVoice* voice, const wchar_t* text) {
    if (voice) voice->Speak(text, SPF_DEFAULT, nullptr);   // waits until it has been said
}

static void LaunchCalibrateThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISpVoice* voice = nullptr;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD tmo = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    bool up = false;
    ULONGLONG fireAt = 0;   // GetTickCount64 to start at, 0 = nothing pending
    for (;;) {
        long long uptime = PluginUptime(s);
        bool nowUp = uptime != -1;
        if (nowUp && !up) {   // Echo (the plugin) just appeared
            if (uptime >= 0 && (ULONGLONG)uptime < kFreshLaunchMs && CalibrateOnLaunch()) {
                ULONGLONG wait = (ULONGLONG)uptime < kLaunchCalibrateMs ? kLaunchCalibrateMs - uptime : 0;
                fireAt = GetTickCount64() + wait;
                printf("\nEcho started -- calibrating in %llu s (CalibrateOnLaunch)\n", (wait + 999) / 1000);
            }
        } else if (!nowUp && up) {
            fireAt = 0;   // Echo closed before it was time
        }
        up = nowUp;
        if (fireAt && GetTickCount64() >= fireAt) {
            fireAt = 0;
            if (!voice && FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void**)&voice)))
                printf("\ntext-to-speech unavailable -- calibrating silently\n");
            printf("\nhold both hands in front of your face, flat out...\n");
            Say(voice, L"Please hold your hands in front of your face, flat out.");
            for (const wchar_t* n : { L"3", L"2", L"1" }) {
                ULONGLONG t = GetTickCount64();
                printf("%ls... ", n);
                Say(voice, n);
                ULONGLONG spent = GetTickCount64() - t;
                if (spent < 1000) Sleep((DWORD)(1000 - spent));
            }
            g_CalibrateNow = true;
            Sleep(200);
            Say(voice, L"Calibrated.");
        }
        Sleep(fireAt ? 250 : 1000);
    }
}

// ---------------------------------------------------------------------------
// SteamVR action manifest + one default binding per controller type. Written
// next to this exe at startup (so an old install's Index-only manifest is
// upgraded too); if that folder isn't writable, to %LOCALAPPDATA%\EchoXR\Hands.
// Every binding is the same: both hand skeletons. Controller-free hand tracking
// drivers present themselves as one of these types.
// ---------------------------------------------------------------------------
static const char* kControllerTypes[] = { "knuckles", "oculus_touch", "vive_controller", "vive_cosmos_controller",
                                          "holographic_controller", "hpmotioncontroller" };

static bool WriteIfChanged(const std::string& path, const std::string& text) {
    FILE* f = nullptr;
    if (!fopen_s(&f, path.c_str(), "rb") && f) {
        std::string cur;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) cur.append(buf, n);
        fclose(f);
        if (cur == text) return true;
    }
    if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

static bool WriteManifestTo(const std::string& dir) {
    std::string actions =
        "{\n  \"action_manifest_version\": 0,\n  \"default_bindings\": [\n";
    for (size_t i = 0; i < sizeof(kControllerTypes) / sizeof(*kControllerTypes); ++i) {
        const char* t = kControllerTypes[i];
        actions += std::string("    { \"controller_type\": \"") + t + "\", \"binding_url\": \"htv_bindings_" + t + ".json\" }" +
                   (i + 1 < sizeof(kControllerTypes) / sizeof(*kControllerTypes) ? ",\n" : "\n");
        std::string binding = std::string("{\n  \"action_manifest_version\": 0,\n  \"controller_type\": \"") + t +
            "\",\n  \"name\": \"EchoXR Hands - " + t + "\",\n  \"bindings\": {\n    \"/actions/htv\": {\n      \"skeleton\": [\n"
            "        { \"output\": \"/actions/htv/in/skeletonleft\",  \"path\": \"/user/hand/left/input/skeleton/left\" },\n"
            "        { \"output\": \"/actions/htv/in/skeletonright\", \"path\": \"/user/hand/right/input/skeleton/right\" }\n"
            "      ]\n    }\n  }\n}\n";
        if (!WriteIfChanged(dir + "htv_bindings_" + t + ".json", binding)) return false;
    }
    actions +=
        "  ],\n  \"actions\": [\n"
        "    { \"name\": \"/actions/htv/in/SkeletonLeft\",  \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/left\" },\n"
        "    { \"name\": \"/actions/htv/in/SkeletonRight\", \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/right\" }\n"
        "  ],\n  \"action_sets\": [\n    { \"name\": \"/actions/htv\", \"usage\": \"leftright\" }\n  ],\n"
        "  \"localization\": [\n    {\n      \"language_tag\": \"en_US\",\n      \"/actions/htv\": \"EchoXR Hands\",\n"
        "      \"/actions/htv/in/SkeletonLeft\": \"Left hand skeleton\",\n"
        "      \"/actions/htv/in/SkeletonRight\": \"Right hand skeleton\"\n    }\n  ]\n}\n";
    return WriteIfChanged(dir + "htv_actions.json", actions);
}

static std::string WriteManifest() {
    if (WriteManifestTo(ExeDir())) return ExeDir() + "htv_actions.json";
    char local[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) {
        std::string dir = std::string(local) + "\\EchoXR\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        dir += "Hands\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        if (WriteManifestTo(dir)) { printf("manifest: %s (this folder isn't writable)\n", dir.c_str()); return dir + "htv_actions.json"; }
    }
    return ExeDir() + "htv_actions.json";   // whatever was shipped
}

int main(int argc, char** argv) {
    bool print = false;
    std::string text;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--print") print = true;
        else if (a == "--calibrate") text += "Calibrate = 1\n";
        else if (a == "--ping") text += "Ping";
        else if (a == "--set" && i + 1 < argc) { text += argv[++i]; text += "\n"; }
        else { printf("unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (!text.empty()) return SendText(text);

    if (!OpenSocket()) { printf("socket failed\n"); return 1; }
    LoadSettings();
    std::string manifest = WriteManifest();
    if (!LoadOpenVR()) {
        printf("Could not load openvr_api.dll. Put one next to this exe, or install Revive.\n");
        return 1;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    p_Init(&err, vr::VRApplication_Overlay, nullptr);
    if (err != vr::VRInitError_None) {
        printf("VR init failed (%d). Is SteamVR running?\n", (int)err);
        return 1;
    }
    vr::IVRInput* input = (vr::IVRInput*)p_GetInterface(vr::IVRInput_Version, &err);
    if (!input) { printf("IVRInput %s unavailable (%d)\n", vr::IVRInput_Version, (int)err); p_Shutdown(); return 1; }

    vr::EVRInputError ie = input->SetActionManifestPath(manifest.c_str());
    if (ie != vr::VRInputError_None) {
        printf("SetActionManifestPath(%s) failed: %d\n", manifest.c_str(), (int)ie);
        p_Shutdown();
        return 1;
    }

    vr::VRActionSetHandle_t set = 0;
    vr::VRActionHandle_t skel[2] = { 0, 0 };
    input->GetActionSetHandle("/actions/htv", &set);
    input->GetActionHandle("/actions/htv/in/SkeletonLeft", &skel[0]);
    input->GetActionHandle("/actions/htv/in/SkeletonRight", &skel[1]);

    g_System = (vr::IVRSystem*)p_GetInterface(vr::IVRSystem_Version, &err);
    if (g_Hotkey) {
        if (RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C')) printf("Ctrl+Alt+C recalibrates\n");
        else printf("Ctrl+Alt+C is taken by another program -- no calibrate hotkey\n");
    }

    std::thread(LaunchCalibrateThread).detach();
    printf("streaming to 127.0.0.1:%d -- Ctrl+C to stop\n", HTV_PORT);
    HtvFrame fr = {};
    fr.magic = HTV_MAGIC;
    auto next = std::chrono::steady_clock::now();
    int printTick = 0, typeTick = 0;
    std::string type[2];
    const char* used[2] = { "-", "-" };
    for (;;) {
        MSG m;
        bool calibrate = g_CalibrateNow.exchange(false);
        while (PeekMessageA(&m, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) calibrate = true;
        if (calibrate) {
            static const char cmd[] = "Calibrate = 1\n";
            sendto(g_Sock, cmd, sizeof(cmd) - 1, 0, (SOCKADDR*)&g_To, sizeof(g_To));
            printf("\ncalibrate requested -- hold both hands fully open\n");
            g_Bones[0].captureOpen = g_Bones[1].captureOpen = true;   // hand-skeleton zero too
        }

        vr::VRActiveActionSet_t active = {};
        active.ulActionSet = set;
        input->UpdateActionState(&active, sizeof(active), 1);
        bool refreshType = (typeTick++ % 120) == 0;

        for (int side = 0; side < 2; ++side) {
            vr::InputSkeletalActionData_t ad = {};
            fr.valid[side] = 0;
            used[side] = "-";
            if (input->GetSkeletalActionData(skel[side], &ad, sizeof(ad)) != vr::VRInputError_None || !ad.bActive)
                continue;
            if (refreshType) {
                std::string t = ControllerType(input, ad);
                if (t != type[side]) {
                    type[side] = t;
                    printf("\n%s hand: controller '%s'\n", side ? "right" : "left", t.empty() ? "?" : t.c_str());
                }
            }
            bool wantDevice = g_Source == SRC_DEVICE || (g_Source == SRC_AUTO && type[side] == "knuckles");
            vr::VRSkeletalSummaryData_t sum = {};
            if (wantDevice && input->GetSkeletalSummaryData(skel[side], vr::VRSummaryType_FromDevice, &sum) == vr::VRInputError_None) {
                for (int f = 0; f < 5; ++f) fr.curl[side][f] = sum.flFingerCurl[f];
                for (int f = 0; f < 4; ++f) fr.splay[side][f] = sum.flFingerSplay[f];
                fr.valid[side] = 1;
                used[side] = "device";
            } else if (g_Source != SRC_DEVICE && CurlsFromBones(input, skel[side], side, fr.curl[side])) {
                for (int f = 0; f < 4; ++f) fr.splay[side][f] = 0.0f;
                fr.valid[side] = 1;
                used[side] = "bones";
            }
        }
        ++fr.seq;
        sendto(g_Sock, (const char*)&fr, sizeof(fr), 0, (SOCKADDR*)&g_To, sizeof(g_To));

        if (print && ++printTick >= 12) {
            printTick = 0;
            printf("\rL %-6s T%.2f I%.2f M%.2f R%.2f P%.2f   R %-6s T%.2f I%.2f M%.2f R%.2f P%.2f   ",
                   used[0], fr.curl[0][0], fr.curl[0][1], fr.curl[0][2], fr.curl[0][3], fr.curl[0][4],
                   used[1], fr.curl[1][0], fr.curl[1][1], fr.curl[1][2], fr.curl[1][3], fr.curl[1][4]);
            fflush(stdout);
        }
        next += std::chrono::microseconds(8333);   // ~120 Hz
        std::this_thread::sleep_until(next);
    }
}
