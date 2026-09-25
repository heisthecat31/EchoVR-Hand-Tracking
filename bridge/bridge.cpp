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

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#define OPENVR_INTERFACE_INTERNAL
#include "openvr.h"
#include "../plugin/htv_protocol.h"

#pragma comment(lib, "ws2_32.lib")

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

    std::string manifest = ExeDir() + "htv_actions.json";
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

    printf("streaming to 127.0.0.1:%d -- Ctrl+C to stop\n", HTV_PORT);
    HtvFrame fr = {};
    fr.magic = HTV_MAGIC;
    auto next = std::chrono::steady_clock::now();
    int printTick = 0;
    for (;;) {
        vr::VRActiveActionSet_t active = {};
        active.ulActionSet = set;
        input->UpdateActionState(&active, sizeof(active), 1);

        for (int side = 0; side < 2; ++side) {
            vr::InputSkeletalActionData_t ad = {};
            fr.valid[side] = 0;
            if (input->GetSkeletalActionData(skel[side], &ad, sizeof(ad)) != vr::VRInputError_None || !ad.bActive)
                continue;
            vr::VRSkeletalSummaryData_t sum = {};
            if (input->GetSkeletalSummaryData(skel[side], vr::VRSummaryType_FromDevice, &sum) != vr::VRInputError_None)
                continue;
            for (int f = 0; f < 5; ++f) fr.curl[side][f] = sum.flFingerCurl[f];
            for (int f = 0; f < 4; ++f) fr.splay[side][f] = sum.flFingerSplay[f];
            fr.valid[side] = 1;
        }
        ++fr.seq;
        sendto(g_Sock, (const char*)&fr, sizeof(fr), 0, (SOCKADDR*)&g_To, sizeof(g_To));

        if (print && ++printTick >= 12) {
            printTick = 0;
            printf("\rL %s T%.2f I%.2f M%.2f R%.2f P%.2f   R %s T%.2f I%.2f M%.2f R%.2f P%.2f   ",
                   fr.valid[0] ? "on " : "off", fr.curl[0][0], fr.curl[0][1], fr.curl[0][2], fr.curl[0][3], fr.curl[0][4],
                   fr.valid[1] ? "on " : "off", fr.curl[1][0], fr.curl[1][1], fr.curl[1][2], fr.curl[1][3], fr.curl[1][4]);
            fflush(stdout);
        }
        next += std::chrono::microseconds(8333);   // ~120 Hz
        std::this_thread::sleep_until(next);
    }
}
