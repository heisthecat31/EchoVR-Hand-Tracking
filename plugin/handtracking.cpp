// HandTrackingValve -- full per-finger hand tracking for Echo VR's chassis.
//
// WHERE THIS HOOKS, AND WHY THERE
// -------------------------------
// Echo runs its per-frame work as a table of gamespace tasks. Two of them belong
// to CR15HandAnimatorCS, back to back:
//
//   0x6008  UpdateFingerAnimPoses        echovr+0x99f440
//   0x6009  UpdateThumbPistonAnimPoses   echovr+0x9a16e0   <- detoured here
//
// UpdateFingerAnimPoses does NOT pose fingers itself. For every hand whose
// CONTACT weight (inst+0xbb4 + hand*12) is non-zero, it queues one job per
// finger (echovr+0x99f830) that bends that finger onto whatever surface it is
// touching. A hand with zero contact weight gets no jobs at all.
//
// So by 0x6009 the animation system has already written the hand's pose, and a
// hand with no contact has nothing in flight that could race us. This hook
// poses exactly those hands, then calls the original. A hand that IS touching
// something is left to the engine (RespectContact=1) -- its contact posing is
// what makes fingers wrap a handhold, and it is already running on jobs.
//
// THE ENGINE CALLS USED
// ---------------------
//   get  echovr+0x331bd0  Xform* (void* pose, Xform* out, u16 skel, i32 joint)
//   set  echovr+0x37c8c0  void   (void* pose, u16 skel, u32 joint, const Xform*)
//
// Xform is {quat xyzw, pos xyz, scale}. `set` carries the joint's CHILDREN along
// with it (it snapshots them relative to the old transform and re-applies them),
// which is why the engine's own finger job only sets the first and last joint.
// Here each finger is set proximal -> middle -> distal, so every set lands on a
// parent that has already moved.
//
// Per hand-animator instance (CS+0xf8 array, stride 0xd80, count u16 CS+0xf4):
//   +0x008  pose object          +0x010  skeleton key (u16)
//   +0x118  finger joints: u32[3] per (hand*5 + slot), 12 bytes each
//   +0x7e8  wrist joint, u32 per hand
//   +0xbb4  contact weight, float per hand, stride 12
//
// All of this was read from the one live build:
//   C:\Oculus\Games\Software\Software\ready-at-dawn-echo-arena\bin\win10\echovr.exe
// The prologues are checked before hooking; a mismatch disables the plugin
// instead of patching the wrong code.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include "detours.h"
#include "htv_protocol.h"
#include "rig_table.h"
#include "htv_net.h"
#include <map>

#pragma comment(lib, "ws2_32.lib")

// =============================================================================
// Game offsets (RVAs into echovr.exe) and their expected prologues
// =============================================================================
static const uintptr_t RVA_THUMB_PISTON = 0x9a16e0;
static const uintptr_t RVA_GET_JOINT    = 0x331bd0;
static const uintptr_t RVA_SET_JOINT    = 0x37c8c0;

static const uint8_t SIG_THUMB_PISTON[] = { 0x4c,0x8b,0xdc,0x49,0x89,0x4b,0x08,0x48,0x83,0xec,0x68,0x48,0x83,0x3d };
static const uint8_t SIG_GET_JOINT[]    = { 0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x8b,0x81,0xd0,0x00,0x00,0x00 };
static const uint8_t SIG_SET_JOINT[]    = { 0x48,0x83,0xec,0x28,0x48,0x8b,0x81,0xd0,0x00,0x00,0x00,0x45,0x8b,0xd8 };

// CR15HandAnimatorCS layout
static const size_t CS_COUNT      = 0xf4;
static const size_t CS_ARRAY      = 0xf8;
static const size_t INST_STRIDE   = 0xd80;
static const size_t INST_POSE     = 0x008;
static const size_t INST_SKEL     = 0x010;
static const size_t INST_FINGERS  = 0x118;   // + (hand*5 + slot) * 12
static const size_t INST_WRIST    = 0x7e8;   // + hand * 4
static const size_t INST_CONTACT  = 0xbb4;   // + hand * 12
static const float  CONTACT_EPS   = 1.1920929e-07f;  // the engine's own test

struct Xform { float q[4]; float p[3]; float s; };

typedef void  (__fastcall* pf_ThumbPiston)(uint8_t* cs);
typedef Xform*(__fastcall* pf_GetJoint)(void* pose, Xform* out, uint16_t skel, int32_t joint);
typedef void  (__fastcall* pf_SetJoint)(void* pose, uint16_t skel, uint32_t joint, const Xform* x);

static pf_ThumbPiston Real_ThumbPiston = nullptr;
static pf_GetJoint    GetJoint = nullptr;
static pf_SetJoint    SetJoint = nullptr;

// =============================================================================
// Logging
// =============================================================================
static std::mutex g_LogMutex;
static std::string g_Dir;

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_LogMutex);
    FILE* f = nullptr;
    if (fopen_s(&f, (g_Dir + "HandTrackingValve.log").c_str(), "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fclose(f);
    }
}

// =============================================================================
// Settings -- plain scalars, written by the config/UDP threads, read by the
// game thread. 4-byte aligned, so a read is never torn.
// =============================================================================
// Bend axis setting: 0/1/2 = the joint's local x/y/z, AXIS_AUTO = derived from the hand shape
static const int AXIS_AUTO = 3;

struct Settings {
    int   enabled        = 1;
    int   targetInstance = 0;     // -1 = every instance (all players!)
    int   gameHandLeft   = 0;     // which game hand slot is the player's LEFT
    int   respectContact = 1;     // leave hands the engine is contact-posing
    int   staleMs        = 250;   // ignore tracking older than this
    int   autoCalibrate  = 1;
    float openThreshold  = 0.12f; // all curls below this = "hand open"
    float smoothingMs    = 45.0f; // curl smoothing time constant (0 = raw)
    float deadband       = 0.006f;// ignore curl changes smaller than this (sensor noise)
    int   contactHoldMs  = 120;   // keep hands off for this long after contact ends
    int   convention     = -1;    // -1 auto, 0 standard, 1 engine (conjugate)
    int   snapFingerAxis = 1;     // snap finger bend axes to the rig's own hinge axis
    int   useRig         = 1;     // pose from the embedded rig table (bind pose), not a captured pose
    // Networking: share tracking with other players through the relay
    int   network        = 1;
    char  relayUrl[256]  = "wss://sparkapi-production-e6df.up.railway.app/htv/ws";
    int   netSendHz      = 30;
    char  myName[64]     = "";    // override the auto-detected local display name
    int   mirrorToRemotes = 0;    // TEST: pose every other player's avatar with YOUR local tracking
    float openStraighten = 12.0f; // degrees a fully OPEN finger straightens past the rig's relaxed bind pose
    // One Euro filter on the curls: heavy smoothing when a finger is still, light
    // when it moves fast -- kills sensor jitter without adding lag to real motion
    float filterMinCutoff = 0.5f; // Hz, smoothing at rest (lower = steadier)
    float filterBeta      = 4.0f; // how fast the cutoff opens up with finger speed
    float filterDCutoff   = 1.0f; // Hz, smoothing of the speed estimate
    // Finger bend: local axis (0=x,1=y,2=z) and sign, per side
    int   bendAxis       = AXIS_AUTO;
    float leftBendSign   = 1.0f;
    float rightBendSign  = 1.0f;
    int   thumbBendAxis  = AXIS_AUTO;
    float leftThumbSign  = 1.0f;
    float rightThumbSign = 1.0f;
    float maxCurl[3]     = { 70.0f, 95.0f, 65.0f };  // degrees per joint
    float thumbMax[3]    = { 22.0f, 34.0f, 34.0f };
    // Per-finger tuning, indexed by HtvFinger, in degrees. Positive = toward the
    // PINKY side of the hand (mirrored per hand, so one value suits both).
    //   spread: constant sideways offset at the knuckle
    //   twist:  tilts the curl so the finger drifts sideways as it bends
    //           (thumb: positive tilts its fold toward the palm instead)
    float spread[5]      = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float twist[5]       = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    int   splayAxis      = 1;
    float splayDeg       = 0.0f;  // 0 = splay off
    // "auto", or five names in game-slot order, e.g. "index,middle,ring,pinky,thumb"
    char  fingerOrder[96] = "auto";
};
static Settings g_S;
static std::atomic<int> g_CalibrateRequest(0);
static std::atomic<int> g_AxisVersion(1);
static std::atomic<int> g_Faulted(0);        // a fault while posing turns the plugin off   // bumped when a twist changes
static std::atomic<int> g_LogInstancesRequest(1);

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static int AxisFromString(const std::string& v, int fallback) {
    if (v == "x" || v == "X") return 0;
    if (v == "y" || v == "Y") return 1;
    if (v == "z" || v == "Z") return 2;
    if (v == "auto" || v == "Auto" || v == "AUTO") return AXIS_AUTO;
    return fallback;
}

static int FingerFromName(const std::string& n);
static int ApplyConfigText(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    int applied = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '/') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        try {
            if      (k == "Enabled")         g_S.enabled = std::stoi(v);
            else if (k == "TargetInstance")  { g_S.targetInstance = std::stoi(v); g_LogInstancesRequest = 1; }
            else if (k == "GameHandLeft")    g_S.gameHandLeft = std::stoi(v) ? 1 : 0;
            else if (k == "RespectContact")  g_S.respectContact = std::stoi(v);
            else if (k == "StaleMs")         g_S.staleMs = std::stoi(v);
            else if (k == "AutoCalibrate")   g_S.autoCalibrate = std::stoi(v);
            else if (k == "OpenThreshold")   g_S.openThreshold = std::stof(v);
            else if (k == "SmoothingMs")     g_S.smoothingMs = std::stof(v);
            else if (k == "Deadband")        g_S.deadband = std::stof(v);
            else if (k == "SnapFingerAxis")  { g_S.snapFingerAxis = std::stoi(v); ++g_AxisVersion; }
            else if (k == "UseRig")          g_S.useRig = std::stoi(v);
            else if (k == "Network")         g_S.network = std::stoi(v);
            else if (k == "RelayUrl")        strncpy_s(g_S.relayUrl, v.c_str(), _TRUNCATE);
            else if (k == "NetSendHz")       g_S.netSendHz = std::stoi(v);
            else if (k == "MyName")          strncpy_s(g_S.myName, v.c_str(), _TRUNCATE);
            else if (k == "MirrorToRemotes") g_S.mirrorToRemotes = std::stoi(v);
            else if (k == "DebugRoster") {   // "me|other1,other2" -- test the relay without a match
                size_t bar = v.find('|');
                if (bar != std::string::npos) {
                    std::vector<std::string> others;
                    std::stringstream ss(v.substr(bar + 1));
                    std::string tok;
                    while (std::getline(ss, tok, ',')) if (!Trim(tok).empty()) others.push_back(Trim(tok));
                    Net_SetRoster(Trim(v.substr(0, bar)), others);
                }
            }
            else if (k == "OpenStraighten")  g_S.openStraighten = std::stof(v);
            else if (k == "FilterMinCutoff") g_S.filterMinCutoff = std::stof(v);
            else if (k == "FilterBeta")      g_S.filterBeta = std::stof(v);
            else if (k == "FilterDCutoff")   g_S.filterDCutoff = std::stof(v);
            else if (k == "ContactHoldMs")   g_S.contactHoldMs = std::stoi(v);
            else if (k == "QuatConvention")  {
                int c = (v == "standard") ? 0 : (v == "engine") ? 1 : -1;
                if (c != g_S.convention) { g_S.convention = c; g_CalibrateRequest = 1; }
            }
            else if (k == "BendAxis")        g_S.bendAxis = AxisFromString(v, g_S.bendAxis);
            else if (k == "LeftBendSign")    g_S.leftBendSign = std::stof(v);
            else if (k == "RightBendSign")   g_S.rightBendSign = std::stof(v);
            else if (k == "ThumbBendAxis")   g_S.thumbBendAxis = AxisFromString(v, g_S.thumbBendAxis);
            else if (k == "LeftThumbSign")   g_S.leftThumbSign = std::stof(v);
            else if (k == "RightThumbSign")  g_S.rightThumbSign = std::stof(v);
            else if (k == "MaxCurl1")        g_S.maxCurl[0] = std::stof(v);
            else if (k == "MaxCurl2")        g_S.maxCurl[1] = std::stof(v);
            else if (k == "MaxCurl3")        g_S.maxCurl[2] = std::stof(v);
            else if (k == "ThumbMax1")       g_S.thumbMax[0] = std::stof(v);
            else if (k == "ThumbMax2")       g_S.thumbMax[1] = std::stof(v);
            else if (k == "ThumbMax3")       g_S.thumbMax[2] = std::stof(v);
            else if (k == "SplayAxis")       g_S.splayAxis = AxisFromString(v, g_S.splayAxis);
            else if (k == "SplayDeg")        g_S.splayDeg = std::stof(v);
            else if (k.compare(0, 6, "Spread") == 0 || k.compare(0, 5, "Twist") == 0) {
                bool isTwist = (k[0] == 'T');
                std::string name = k.substr(isTwist ? 5 : 6);
                for (char& ch : name) ch = (char)tolower((unsigned char)ch);
                int f = FingerFromName(name);
                if (f < 0) continue;
                (isTwist ? g_S.twist : g_S.spread)[f] = std::stof(v);
                if (isTwist) ++g_AxisVersion;
            }
            else if (k == "FingerOrder")     {
                if (v != g_S.fingerOrder) { strncpy_s(g_S.fingerOrder, v.c_str(), _TRUNCATE); g_CalibrateRequest = 1; }
            }
            else if (k == "Calibrate")       { if (std::stoi(v)) g_CalibrateRequest = 1; continue; }
            else if (k == "LogInstances")    { if (std::stoi(v)) g_LogInstancesRequest = 1; continue; }
            else continue;
            ++applied;
        } catch (...) { /* malformed value: skip the key, never take the game down */ }
    }
    Net_Configure(g_S.network, g_S.relayUrl, g_S.netSendHz);
    return applied;
}

// =============================================================================
// Latest tracking frame (written by the UDP thread, read by the game thread)
// =============================================================================
static std::mutex g_FrameMutex;
static HtvFrame   g_Frame;
static ULONGLONG  g_FrameTick = 0;

static bool LatestFrame(HtvFrame& out, ULONGLONG& ageMs) {
    std::lock_guard<std::mutex> lock(g_FrameMutex);
    if (!g_FrameTick) return false;
    out = g_Frame;
    ageMs = GetTickCount64() - g_FrameTick;
    return true;
}

// =============================================================================
// Quaternion helpers (xyzw, Hamilton product)
// =============================================================================
static void QMul(const float a[4], const float b[4], float o[4]) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void QConj(const float a[4], float o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void QNorm(float q[4]) {
    float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n < 1e-8f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; ++i) q[i] /= n;
}
static void QAxisAngle(int axis, float deg, float o[4]) {
    float h = deg * 0.00872664626f;  // deg -> rad, halved
    o[0] = o[1] = o[2] = 0.0f;
    o[axis] = sinf(h);
    o[3] = cosf(h);
}
static void QAxisAngleVec(const float v[3], float deg, float o[4]) {
    float h = deg * 0.00872664626f, s = sinf(h);
    o[0] = v[0] * s; o[1] = v[1] * s; o[2] = v[2] * s; o[3] = cosf(h);
}
// v' = q v q*
static void QRotate(const float q[4], const float v[3], float o[3]) {
    float p[4] = { v[0], v[1], v[2], 0.0f }, c[4], t[4], r[4];
    QConj(q, c); QMul(q, p, t); QMul(t, c, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
static void VSub(const float a[3], const float b[3], float o[3]) { o[0] = a[0]-b[0]; o[1] = a[1]-b[1]; o[2] = a[2]-b[2]; }
static float VDot(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void VCross(const float a[3], const float b[3], float o[3]) {
    float x = a[1]*b[2] - a[2]*b[1], y = a[2]*b[0] - a[0]*b[2], z = a[0]*b[1] - a[1]*b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static bool VNorm(float v[3]) {
    float n = sqrtf(VDot(v, v));
    if (n < 1e-6f) return false;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return true;
}
// Remove b's component from v (b unit), then normalise.
static bool VOrtho(float v[3], const float b[3]) {
    float d = VDot(v, b);
    v[0] -= d * b[0]; v[1] -= d * b[1]; v[2] -= d * b[2];
    return VNorm(v);
}

// =============================================================================
// Per-hand calibration: the reference LOCAL rotation of each finger joint,
// captured from the game's own pose while the tracked hand is open. Tracking
// then bends each joint away from that reference, so the game's grip/trigger
// animation is REPLACED rather than stacked on top of.
// =============================================================================
static const int MAX_INST = 16;

// Shake diagnosis, logged every 5 s: frames posed vs skipped, how much the
// incoming curl jumps per frame, and how much the GAME moves each knuckle per
// frame relative to the wrist (its own animation jitter underneath ours).
struct Stats {
    int applied = 0, skipContact = 0, skipHold = 0, skipStale = 0, frames = 0;
    double curlJump[5] = { 0 };       // summed |delta curl| per tracked finger
    double knuckleMove[5] = { 0 };    // summed wrist-local knuckle motion, metres
    int knuckleN = 0;
    float lastCurl[5] = { 0 };
    float lastKnuckle[5][3] = { { 0 } };
    bool  haveLast = false;
    ULONGLONG since = 0;
};
static Stats g_Stats[2];

struct HandCal {
    uint8_t* inst = nullptr;     // instance this was captured on
    bool     have = false;
    int      slotFinger[5];      // game slot -> HtvFinger
    float    ref[5][3][4];       // [slot][joint] local quaternion
    float    axis[5][3][3];      // [slot][joint] auto bend axis, in the joint's LOCAL frame
    bool     axisOk[5][3];
    float    spreadAxis[5][3];   // [slot] knuckle axis that moves the tip toward the pinky side (local)
    bool     spreadOk[5];
    Xform    capW;               // the captured pose, kept so axes can be
    Xform    capX[5][3];         // recomputed when a twist setting changes
    bool     capLeft = false;
    int      axisVersion = 0;
    int      conj = 0;           // 1 = engine quats are conjugates of standard ones
    LONGLONG lastQpc = 0;        // for time-based smoothing
    ULONGLONG contactUntil = 0;  // hands-off until this tick after contact
    // rig mode
    bool     rigChecked = false;
    const RigHand* rig = nullptr;
    int      rigSlot[5];         // game slot -> HtvFinger (by joint index)
    // One Euro filter state, per tracked finger
    bool     fInit = false;
    float    fx[5], fdx[5];
    float    fsx[4];
    LONGLONG fQpc = 0;
    float    smooth[5];          // smoothed curl per tracked finger
    float    smoothSplay[4];
    bool     loggedFail = false;
};
static HandCal g_Cal[MAX_INST][2];

static int FingerFromName(const std::string& n) {
    if (n == "thumb") return HTV_THUMB;
    if (n == "index") return HTV_INDEX;
    if (n == "middle") return HTV_MIDDLE;
    if (n == "ring") return HTV_RING;
    if (n == "pinky") return HTV_PINKY;
    return -1;
}

static bool ExplicitOrder(int out[5]) {
    std::string s = g_S.fingerOrder;
    if (s == "auto" || s.empty()) return false;
    std::stringstream ss(s);
    std::string tok;
    int i = 0;
    while (std::getline(ss, tok, ',') && i < 5) {
        int f = FingerFromName(Trim(tok));
        if (f < 0) return false;
        out[i++] = f;
    }
    return i == 5;
}

// Which game slot is which finger, from the knuckle positions alone: the thumb
// is the proximal joint farthest from the centroid of the other four, and the
// rest run index -> pinky in order of distance from the thumb.
static void DetectFingerOrder(const float knuckle[5][3], int out[5]) {
    int thumb = 0;
    float best = -1.0f;
    for (int t = 0; t < 5; ++t) {
        float c[3] = { 0, 0, 0 };
        for (int j = 0; j < 5; ++j) if (j != t) for (int a = 0; a < 3; ++a) c[a] += knuckle[j][a] * 0.25f;
        float d = 0;
        for (int a = 0; a < 3; ++a) d += (knuckle[t][a] - c[a]) * (knuckle[t][a] - c[a]);
        if (d > best) { best = d; thumb = t; }
    }
    int rest[4], n = 0;
    float dist[5];
    for (int j = 0; j < 5; ++j) {
        float d = 0;
        for (int a = 0; a < 3; ++a) d += (knuckle[j][a] - knuckle[thumb][a]) * (knuckle[j][a] - knuckle[thumb][a]);
        dist[j] = d;
        if (j != thumb) rest[n++] = j;
    }
    for (int i = 0; i < 4; ++i)
        for (int k = i + 1; k < 4; ++k)
            if (dist[rest[k]] < dist[rest[i]]) { int t = rest[i]; rest[i] = rest[k]; rest[k] = t; }
    out[thumb] = HTV_THUMB;
    out[rest[0]] = HTV_INDEX;
    out[rest[1]] = HTV_MIDDLE;
    out[rest[2]] = HTV_RING;
    out[rest[3]] = HTV_PINKY;
}

// =============================================================================
// The per-hand work. POD only, so it can sit under __try (see PoseHandSafe).
// =============================================================================
static inline bool ValidJoint(uint32_t j) { return j != 0xffffffffu && j < 1024; }

// The bend axis of every joint, worked out from the hand's own shape so no
// per-rig axis guessing is needed. With the palm normal N pointing INTO the
// palm, rotating a bone b about (b x m) by a positive angle swings it toward m:
//   fingers: m = N          -> the tip curls into the palm
//   thumb:   m = toward the pinky knuckle -> the thumb folds across the palm
// Each world axis is stored in its joint's local frame (v_local = q* v q), so
// it rides along with whatever the hand is doing when the pose is applied.
static inline void ToStd(const float in[4], float out[4], int conj);
static int DetectConvention(const Xform x[5][3], float* scoreStd, float* scoreEng);

static void ComputeAxes(HandCal& cal, const Xform& w, const Xform x[5][3], bool left) {
    int slotOf[5] = { -1, -1, -1, -1, -1 };
    for (int s = 0; s < 5; ++s) slotOf[cal.slotFinger[s]] = s;
    for (int s = 0; s < 5; ++s) for (int k = 0; k < 3; ++k) cal.axisOk[s][k] = false;
    int si = slotOf[HTV_INDEX], sp = slotOf[HTV_PINKY], st = slotOf[HTV_THUMB];
    if (si < 0 || sp < 0 || st < 0) return;

    // across the knuckles, index -> pinky; mirrored for the left hand so both
    // hands share one handedness and one sign curls both inward
    float across[3], mid[3], fwd[3], N[3];
    VSub(x[sp][0].p, x[si][0].p, across);
    if (left) { across[0] = -across[0]; across[1] = -across[1]; across[2] = -across[2]; }
    // "forward" = the middle finger's first bone. NOT wrist -> knuckles: on the
    // chassis rig the hand bone sits well off the wrist, which tilted the palm
    // normal ~36 deg toward the fingers.
    int sm = slotOf[HTV_MIDDLE];
    if (sm < 0) return;
    (void)mid;
    VSub(x[sm][1].p, x[sm][0].p, fwd);
    if (!VNorm(fwd) || !VNorm(across)) return;
    VCross(fwd, across, N);
    if (!VNorm(N)) return;
    // the real index -> pinky direction (NOT mirrored): "toward the pinky side"
    float P[3];
    VSub(x[sp][0].p, x[si][0].p, P);
    if (!VNorm(P)) return;
    for (int s = 0; s < 5; ++s) cal.spreadOk[s] = false;

    for (int s = 0; s < 5; ++s) {
        bool thumb = (s == st);
        for (int k = 0; k < 3; ++k) {
            float b[3];
            if (k < 2) VSub(x[s][k + 1].p, x[s][k].p, b);
            else       VSub(x[s][2].p, x[s][1].p, b);     // distal: reuse the last bone
            if (!VNorm(b)) continue;
            float m[3];
            float tw = thumb ? tanf(g_S.twist[cal.slotFinger[s]] * 0.0174532925f) : 0.0f;
            if (thumb) {
                VSub(x[sp][0].p, x[s][k].p, m);
                if (!VNorm(m)) continue;
                for (int a = 0; a < 3; ++a) m[a] += tw * N[a];
            } else {
                for (int a = 0; a < 3; ++a) m[a] = N[a];   // twist is applied after the snap
            }
            if (!VOrtho(m, b)) continue;
            float aw[3], qc[4], al[3];
            VCross(b, m, aw);
            if (!VNorm(aw)) continue;
            QConj(x[s][k].q, qc);
            QRotate(qc, aw, al);
            if (!VNorm(al)) continue;
            if (!thumb && g_S.snapFingerAxis) {
                int big = 0;
                for (int c = 1; c < 3; ++c) if (fabsf(al[c]) > fabsf(al[big])) big = c;
                float sgn = al[big] < 0.0f ? -1.0f : 1.0f;
                al[0] = al[1] = al[2] = 0.0f;
                al[big] = sgn;
            }
            // Twist: tilt the hinge around the bone so the curl drifts sideways.
            // Positive = toward the pinky side. A rotation of b about axis a moves
            // it along (a x b); spinning a about b spins that motion about b, and
            // the spin direction that adds a +P component is sign(dot(b x u, P)).
            if (!thumb && g_S.twist[cal.slotFinger[s]] != 0.0f) {
                float awS[3], u[3], bu[3], bl[3], rq[4], t[3];
                QRotate(x[s][k].q, al, awS);
                VCross(awS, b, u);
                VCross(b, u, bu);
                float sgn = VDot(bu, P) >= 0.0f ? 1.0f : -1.0f;
                QRotate(qc, b, bl);
                if (VNorm(bl)) {
                    QAxisAngleVec(bl, sgn * g_S.twist[cal.slotFinger[s]], rq);
                    QRotate(rq, al, t);
                    if (VNorm(t)) memcpy(al, t, sizeof(t));
                }
            }
            memcpy(cal.axis[s][k], al, sizeof(al));
            cal.axisOk[s][k] = true;

            if (k == 0) {   // knuckle spread: swing the tip toward the pinky side
                float ps[3] = { P[0], P[1], P[2] }, as[3], sl[3];
                if (VOrtho(ps, b)) {
                    VCross(b, ps, as);
                    if (VNorm(as)) {
                        QRotate(qc, as, sl);
                        if (VNorm(sl)) { memcpy(cal.spreadAxis[s], sl, sizeof(sl)); cal.spreadOk[s] = true; }
                    }
                }
            }
        }
    }
}

static bool Capture(uint8_t* inst, int h, HandCal& cal, bool left) {
    void* pose = *(void**)(inst + INST_POSE);
    uint16_t skel = (uint16_t)*(uint32_t*)(inst + INST_SKEL);
    uint32_t wrist = *(uint32_t*)(inst + INST_WRIST + h * 4);
    if (!pose || !ValidJoint(wrist)) return false;

    Xform w, x[5][3];
    float knuckle[5][3];
    GetJoint(pose, &w, skel, (int32_t)wrist);
    for (int s = 0; s < 5; ++s) {
        const uint32_t* jr = (const uint32_t*)(inst + INST_FINGERS + (h * 5 + s) * 12);
        if (!ValidJoint(jr[0]) || !ValidJoint(jr[1]) || !ValidJoint(jr[2])) return false;
        for (int k = 0; k < 3; ++k) GetJoint(pose, &x[s][k], skel, (int32_t)jr[k]);
    }
    float scStd = 0, scEng = 0;
    int detected = DetectConvention(x, &scStd, &scEng);
    cal.conj = (g_S.convention < 0) ? detected : g_S.convention;
    Log("quaternion convention: bone-axis fit standard %.3f, engine %.3f -> using %s%s",
        scStd, scEng, cal.conj ? "engine (conjugate)" : "standard", g_S.convention < 0 ? " (auto)" : " (forced)");
    { float t[4]; memcpy(t, w.q, 16); ToStd(t, w.q, cal.conj); }
    for (int s = 0; s < 5; ++s) {
        for (int k = 0; k < 3; ++k) { float t[4]; memcpy(t, x[s][k].q, 16); ToStd(t, x[s][k].q, cal.conj); }
        for (int a = 0; a < 3; ++a) knuckle[s][a] = x[s][0].p[a];
        // local = inverse(parent) * joint; parent of the first joint is the wrist
        float inv[4];
        QConj(w.q, inv);       QMul(inv, x[s][0].q, cal.ref[s][0]); QNorm(cal.ref[s][0]);
        QConj(x[s][0].q, inv); QMul(inv, x[s][1].q, cal.ref[s][1]); QNorm(cal.ref[s][1]);
        QConj(x[s][1].q, inv); QMul(inv, x[s][2].q, cal.ref[s][2]); QNorm(cal.ref[s][2]);
    }
    if (!ExplicitOrder(cal.slotFinger)) DetectFingerOrder(knuckle, cal.slotFinger);
    cal.capW = w;
    memcpy(cal.capX, x, sizeof(cal.capX));
    cal.capLeft = left;
    ComputeAxes(cal, w, x, left);
    cal.axisVersion = g_AxisVersion;
    for (int f = 0; f < 5; ++f) cal.smooth[f] = 0.0f;
    for (int f = 0; f < 4; ++f) cal.smoothSplay[f] = 0.0f;
    cal.inst = inst;
    cal.have = true;
    return true;
}

// Engine <-> standard quaternion. The engine's QuaternionRotateVector
// (echovr+0xf93b0) computes v + w*t + t x q with t = 2(v x q): that is rotation
// by the CONJUGATE. So a joint quaternion from GetJoint is the inverse of the
// standard (v' = q v q*) one, and all the math here runs on the standard form.
static inline void ToStd(const float in[4], float out[4], int conj) {
    if (conj) QConj(in, out); else memcpy(out, in, 16);
}

// Which convention makes the finger bones line up with a joint axis? In the
// chassis rig every finger bone runs along its joint's local +-X (bind pose),
// so the right convention gives |local bone| ~ a cardinal axis on every joint.
static int DetectConvention(const Xform x[5][3], float* scoreStd, float* scoreEng) {
    float sc[2] = { 0, 0 };
    int n = 0;
    for (int s = 0; s < 5; ++s)
        for (int k = 0; k < 2; ++k) {
            float b[3];
            VSub(x[s][k + 1].p, x[s][k].p, b);
            if (!VNorm(b)) continue;
            ++n;
            for (int c = 0; c < 2; ++c) {
                float q[4], qc[4], l[3];
                ToStd(x[s][k].q, q, c);
                QConj(q, qc);
                QRotate(qc, b, l);
                float m = fabsf(l[0]);
                if (fabsf(l[1]) > m) m = fabsf(l[1]);
                if (fabsf(l[2]) > m) m = fabsf(l[2]);
                sc[c] += m;
            }
        }
    if (n) { sc[0] /= n; sc[1] /= n; }
    *scoreStd = sc[0]; *scoreEng = sc[1];
    return sc[1] > sc[0] ? 1 : 0;
}

// Splay per tracked finger from OpenVR's four between-finger splays, spreading
// outward from the middle finger.
static float SplayFor(int finger, const float sp[4]) {
    switch (finger) {
        case HTV_INDEX:  return -sp[1];
        case HTV_RING:   return  sp[2];
        case HTV_PINKY:  return  sp[2] + sp[3];
        default:         return  0.0f;
    }
}

static void PoseHand(uint8_t* inst, int h, HandCal& cal, const float curl[5], const float splay[4], bool left) {
    void* pose = *(void**)(inst + INST_POSE);
    uint16_t skel = (uint16_t)*(uint32_t*)(inst + INST_SKEL);
    uint32_t wrist = *(uint32_t*)(inst + INST_WRIST + h * 4);
    if (!pose || !ValidJoint(wrist)) return;

    // Frame-rate independent smoothing: the same feel at 72, 90 or 120 Hz.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = cal.lastQpc ? (float)(now.QuadPart - cal.lastQpc) / (float)freq.QuadPart : 0.0f;
    cal.lastQpc = now.QuadPart;
    if (dt > 0.1f) dt = 0.1f;
    float a = (g_S.smoothingMs > 0.0f) ? expf(-dt * 1000.0f / g_S.smoothingMs) : 0.0f;
    for (int f = 0; f < 5; ++f) {
        float target = curl[f];
        if (fabsf(target - cal.smooth[f]) < g_S.deadband) target = cal.smooth[f];   // sensor noise
        cal.smooth[f] = cal.smooth[f] * a + target * (1.0f - a);
    }
    for (int f = 0; f < 4; ++f) cal.smoothSplay[f] = cal.smoothSplay[f] * a + splay[f] * (1.0f - a);

    if (cal.axisVersion != g_AxisVersion) {     // a twist changed: re-derive, no recapture
        ComputeAxes(cal, cal.capW, cal.capX, cal.capLeft);
        cal.axisVersion = g_AxisVersion;
    }

    Xform w;
    GetJoint(pose, &w, skel, (int32_t)wrist);
    { float t[4]; memcpy(t, w.q, 16); ToStd(t, w.q, cal.conj); }

    for (int s = 0; s < 5; ++s) {
        const uint32_t* jr = (const uint32_t*)(inst + INST_FINGERS + (h * 5 + s) * 12);
        if (!ValidJoint(jr[0]) || !ValidJoint(jr[1]) || !ValidJoint(jr[2])) continue;
        int finger = cal.slotFinger[s];
        bool thumb = (finger == HTV_THUMB);
        float c = cal.smooth[finger];
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        int axis = thumb ? g_S.thumbBendAxis : g_S.bendAxis;
        float sign = thumb ? (left ? g_S.leftThumbSign : g_S.rightThumbSign)
                           : (left ? g_S.leftBendSign  : g_S.rightBendSign);
        const float* maxDeg = thumb ? g_S.thumbMax : g_S.maxCurl;

        float parentQ[4] = { w.q[0], w.q[1], w.q[2], w.q[3] };
        for (int k = 0; k < 3; ++k) {
            // current transform AFTER the previous set moved it (set carries children)
            Xform cur;
            GetJoint(pose, &cur, skel, (int32_t)jr[k]);

            float bend[4], local[4], tmp[4];
            if (axis == AXIS_AUTO && cal.axisOk[s][k]) QAxisAngleVec(cal.axis[s][k], sign * c * maxDeg[k], bend);
            else QAxisAngle(axis == AXIS_AUTO ? 2 : axis, sign * c * maxDeg[k], bend);
            if (k == 0 && cal.spreadOk[s] && g_S.spread[finger] != 0.0f) {
                float sq[4], rs[4];
                QAxisAngleVec(cal.spreadAxis[s], g_S.spread[finger], sq);
                QMul(cal.ref[s][k], sq, rs);
                QMul(rs, bend, local);
            } else {
                QMul(cal.ref[s][k], bend, local);
            }
            if (k == 0 && !thumb && g_S.splayDeg != 0.0f) {
                float sp[4];
                QAxisAngle(g_S.splayAxis, SplayFor(finger, cal.smoothSplay) * g_S.splayDeg * (left ? 1.0f : -1.0f), sp);
                QMul(local, sp, tmp);
                memcpy(local, tmp, sizeof(tmp));
            }
            Xform nx = cur;
            float stdQ[4];
            QMul(parentQ, local, stdQ);
            QNorm(stdQ);
            ToStd(stdQ, nx.q, cal.conj);          // standard -> engine (the conjugate is its own inverse)
            SetJoint(pose, skel, jr[k], &nx);
            memcpy(parentQ, stdQ, sizeof(parentQ));
        }
    }
}

// Reads the game's knuckle positions (before we pose) in wrist space, and the
// incoming curls, to see where frame-to-frame motion comes from.
static void Measure(uint8_t* inst, int h, HandCal& cal, const float curl[5], Stats& st) {
    void* pose = *(void**)(inst + INST_POSE);
    uint16_t skel = (uint16_t)*(uint32_t*)(inst + INST_SKEL);
    uint32_t wrist = *(uint32_t*)(inst + INST_WRIST + h * 4);
    if (!pose || !ValidJoint(wrist)) return;
    Xform w;
    GetJoint(pose, &w, skel, (int32_t)wrist);
    float wq[4], wqc[4];
    ToStd(w.q, wq, cal.conj);
    QConj(wq, wqc);
    float kn[5][3];
    for (int s = 0; s < 5; ++s) {
        const uint32_t* jr = (const uint32_t*)(inst + INST_FINGERS + (h * 5 + s) * 12);
        if (!ValidJoint(jr[0])) return;
        Xform x;
        GetJoint(pose, &x, skel, (int32_t)jr[0]);
        float d[3];
        VSub(x.p, w.p, d);
        QRotate(wqc, d, kn[cal.slotFinger[s]]);
    }
    if (st.haveLast) {
        for (int f = 0; f < 5; ++f) {
            st.curlJump[f] += fabsf(curl[f] - st.lastCurl[f]);
            float d[3];
            VSub(kn[f], st.lastKnuckle[f], d);
            st.knuckleMove[f] += sqrtf(VDot(d, d));
        }
        st.knuckleN++;
    }
    memcpy(st.lastCurl, curl, sizeof(st.lastCurl));
    memcpy(st.lastKnuckle, kn, sizeof(st.lastKnuckle));
    st.haveLast = true;
}
static void MeasureSafe(uint8_t* inst, int h, HandCal* cal, const float* curl, Stats* st) {
    __try { Measure(inst, h, *cal, curl, *st); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// =============================================================================
// Rig mode: pose from the chassis rig's bind pose (plugin/rig_table.h).
// =============================================================================

// The rig hand whose wrist and all fifteen finger joints match this instance's
// joint table -- or null, in which case the captured-pose path is used.
static const RigHand* FindRig(uint8_t* inst, int h, int slotOut[5]) {
    uint32_t wrist = *(uint32_t*)(inst + INST_WRIST + h * 4);
    for (int r = 0; r < 2; ++r) {
        const RigHand& rh = g_Rig[r];
        if (rh.wrist != wrist) continue;
        bool all = true;
        for (int s = 0; s < 5 && all; ++s) {
            const uint32_t* jr = (const uint32_t*)(inst + INST_FINGERS + (h * 5 + s) * 12);
            int found = -1;
            for (int f = 0; f < 5; ++f)
                if (rh.f[f].j[0].joint == jr[0] && rh.f[f].j[1].joint == jr[1] && rh.f[f].j[2].joint == jr[2])
                    found = f;
            if (found < 0) all = false; else slotOut[s] = found;
        }
        return all ? &rh : nullptr;
    }
    return nullptr;
}

static inline float OneEuroAlpha(float cutoffHz, float dt) {
    float tau = 1.0f / (6.2831853f * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}

// One Euro filter (Casiez et al.) over the five curls and four splays.
static void FilterInput(HandCal& cal, const float curl[5], const float splay[4], float outC[5], float outS[4]) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = cal.fQpc ? (float)(now.QuadPart - cal.fQpc) / (float)freq.QuadPart : 0.0f;
    cal.fQpc = now.QuadPart;
    if (!cal.fInit || dt <= 0.0f || dt > 0.25f) {
        for (int f = 0; f < 5; ++f) { cal.fx[f] = curl[f]; cal.fdx[f] = 0.0f; }
        for (int f = 0; f < 4; ++f) cal.fsx[f] = splay[f];
        cal.fInit = true;
    } else {
        float ad = OneEuroAlpha(g_S.filterDCutoff, dt);
        for (int f = 0; f < 5; ++f) {
            float dx = (curl[f] - cal.fx[f]) / dt;
            cal.fdx[f] += ad * (dx - cal.fdx[f]);
            float cutoff = g_S.filterMinCutoff + g_S.filterBeta * fabsf(cal.fdx[f]);
            cal.fx[f] += OneEuroAlpha(cutoff, dt) * (curl[f] - cal.fx[f]);
        }
        float as = OneEuroAlpha(g_S.filterMinCutoff, dt);
        for (int f = 0; f < 4; ++f) cal.fsx[f] += as * (splay[f] - cal.fsx[f]);
    }
    for (int f = 0; f < 5; ++f) outC[f] = cal.fx[f];
    for (int f = 0; f < 4; ++f) outS[f] = cal.fsx[f];
}

// Poses one hand of any skeleton that uses the shared chassis rig -- the local
// player's (through the hand animator) or a remote player's (through their body).
static void PoseFingersRig(void* pose, uint16_t skel, const RigHand* rig, HandCal& cal,
                           const float rawCurl[5], const float rawSplay[4], bool left) {
    if (!pose || !rig) return;
    int conj = (g_S.convention == 1) ? 1 : 0;   // the live build measured standard (fit 1.000)

    float curl[5], splay[4];
    FilterInput(cal, rawCurl, rawSplay, curl, splay);

    for (int finger = 0; finger < 5; ++finger) {
        const RigFinger& rf = rig->f[finger];
        bool thumb = (finger == HTV_THUMB);
        float c = curl[finger];
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        float sign = thumb ? (left ? g_S.leftThumbSign : g_S.rightThumbSign)
                           : (left ? g_S.leftBendSign  : g_S.rightBendSign);
        const float* maxDeg = thumb ? g_S.thumbMax : g_S.maxCurl;

        // the finger's REAL parent (hand, or the ring/pinky metacarpal), as the
        // game animates it this frame -- so the palm keeps its own shape
        Xform par;
        GetJoint(pose, &par, skel, (int32_t)rf.parent);
        float parentQ[4];
        ToStd(par.q, parentQ, conj);

        for (int k = 0; k < 3; ++k) {
            const RigJoint& rj = rf.j[k];
            float deg = c * maxDeg[k];
            if (!thumb && k >= 1) deg -= (1.0f - c) * g_S.openStraighten;
            deg *= sign;

            float axis[3] = { rj.axis[0], rj.axis[1], rj.axis[2] };
            if (!thumb && g_S.twist[finger] != 0.0f) {
                float rq[4], t[3];
                QAxisAngleVec(rj.bone, rj.twistSign * g_S.twist[finger], rq);
                QRotate(rq, axis, t);
                if (VNorm(t)) memcpy(axis, t, sizeof(t));
            }
            float bend[4], local[4], tmp[4];
            QAxisAngleVec(axis, deg, bend);
            memcpy(local, rj.bindQ, sizeof(local));
            if (k == 0 && !thumb) {
                float spreadDeg = g_S.spread[finger] + SplayFor(finger, splay) * g_S.splayDeg;
                if (spreadDeg != 0.0f) {
                    float sq[4];
                    QAxisAngleVec(rf.spreadAxis, spreadDeg, sq);
                    QMul(local, sq, tmp);
                    memcpy(local, tmp, sizeof(tmp));
                }
            }
            QMul(local, bend, tmp);
            memcpy(local, tmp, sizeof(tmp));

            Xform cur;
            GetJoint(pose, &cur, skel, (int32_t)rj.joint);   // position already carried by the parent's set
            float stdQ[4];
            QMul(parentQ, local, stdQ);
            QNorm(stdQ);
            Xform nx = cur;
            ToStd(stdQ, nx.q, conj);
            SetJoint(pose, skel, rj.joint, &nx);
            memcpy(parentQ, stdQ, sizeof(parentQ));
        }
    }
}

static void PoseHandRig(uint8_t* inst, int h, HandCal& cal, const float rawCurl[5], const float rawSplay[4], bool left) {
    (void)h;
    PoseFingersRig(*(void**)(inst + INST_POSE), (uint16_t)*(uint32_t*)(inst + INST_SKEL), cal.rig, cal,
                   rawCurl, rawSplay, left);
}

static void PoseHandRigSafe(uint8_t* inst, int h, HandCal* cal, const float* curl, const float* splay, bool left) {
    __try { PoseHandRig(inst, h, *cal, curl, splay, left); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_Faulted = 1; }
}

// =============================================================================
// Remote players: other people's fingers, from the relay
// =============================================================================
//
// Identity comes from the game's own player table (CR15NetGame): every player
// has a server-assigned slot with a display name and an actor. Posing reaches
// their skeleton through the same chain the engine uses:
//
//   slot -> actor           CPlayerSlot actor ref       echovr+0x1c76b0 (resolves slot+0x180)
//   actor -> remote player  CR15RemotePlayerCS vtbl +0x1a8 (lookup) / +0x1c8 (valid)
//   remote -> body          entry +0x10 into the body CS at cs+0x150 (stride 0x3f70)
//   body -> skeleton        pose +0x3e70, skeleton key +0x3e78 -- same get/set joint calls
//
// The local player is the one occupied slot WITHOUT a remote-player component.

static const uintptr_t RVA_NETGAME_UPDATE   = 0x1bf610;   // CR15NetGame::Update(netgame*)
static const uintptr_t RVA_GET_USERNAME     = 0x1c98c0;   // GetUserName(netgame*, u16 slot)
static const uintptr_t RVA_SLOT_ACTOR       = 0x1c76b0;   // CPlayerSlot actor ref(netgame*, out[2], u16 slot)
static const uintptr_t RVA_REMOTE_CACHEPOSE = 0xd77be0;   // CR15RemotePlayerCS::UpdateCachePoseForPhysics (0x600c)
static const uint8_t SIG_NETGAME_UPDATE[]   = { 0x40,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00 };
static const uint8_t SIG_GET_USERNAME[]     = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x0f,0xb7 };
static const uint8_t SIG_SLOT_ACTOR[]       = { 0x40,0x53,0x48,0x83,0xec,0x20,0x41,0x0f,0xb7,0xc0,0x48,0x8b,0xda };
static const uint8_t SIG_REMOTE_CACHEPOSE[] = { 0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x33,0xdb,0x48,0x8b,0xf9 };

typedef void        (__fastcall* pf_NetGameUpdate)(uint8_t* ng);
typedef const char* (__fastcall* pf_GetUserName)(uint8_t* ng, uint16_t slot);
typedef uint64_t*   (__fastcall* pf_SlotActor)(uint8_t* ng, uint64_t out[2], uint16_t slot);
typedef void        (__fastcall* pf_RemoteCachePose)(uint8_t* cs);
typedef uint32_t*   (__fastcall* pf_CsLookup)(void* cs, uint64_t* out, uint64_t actor, int64_t sub);
typedef int         (__fastcall* pf_CsValid)(void* cs, uint32_t handle);

static pf_NetGameUpdate   Real_NetGameUpdate = nullptr;
static pf_GetUserName     GetUserNameFn = nullptr;
static pf_SlotActor       SlotActorFn = nullptr;
static pf_RemoteCachePose Real_RemoteCachePose = nullptr;

static const int MAX_SLOTS = 32;
// Each slot holds a 16-byte actor reference at +0x180. The engine's slot
// accessor (0x1c76b0) RESOLVES it in place -- and in the live game that gave the
// same value for every player (the owning space, not the player). So the raw
// halves are kept too, and whichever one the remote-player lookup accepts wins.
struct RawSlot { uint16_t slot; char name[64]; uint64_t cand[3]; };
struct RosterEntry { uint16_t slot; std::string name; uint64_t cand[3]; };

static std::mutex g_RosterMx;
static std::vector<RosterEntry> g_Roster;
static ULONGLONG g_RosterTick = 0;

// POD-only so it can run under __try: the player table can change under us.
static int ReadSlots(uint8_t* ng, RawSlot* out) {
    int n = 0;
    __try {
        uint16_t count = *(uint16_t*)(ng + 0xe2);          // first replay slot = number of player slots
        if (count == 0 || count > MAX_SLOTS) count = 16;
        for (uint16_t slot = 0; slot < count && n < MAX_SLOTS; ++slot) {
            if (*(uint64_t*)(ng + (size_t)slot * 0x250 + 0x3c0) == 0) continue;   // empty slot
            const char* nm = GetUserNameFn(ng, slot);
            if (!nm || !nm[0]) continue;
            const uint64_t* raw = (const uint64_t*)(ng + (size_t)slot * 0x250 + 0x180);
            out[n].cand[0] = raw[0];
            out[n].cand[1] = raw[1];
            uint64_t ref[2] = { raw[0], raw[1] };
            SlotActorFn(ng, ref, slot);
            out[n].cand[2] = ref[0];
            out[n].slot = slot;
            strncpy_s(out[n].name, nm, _TRUNCATE);
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}

static void __fastcall Hooked_NetGameUpdate(uint8_t* ng) {
    Real_NetGameUpdate(ng);
    if (!g_S.network || !GetUserNameFn || !SlotActorFn) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_RosterTick < 500) return;
    g_RosterTick = now;
    RawSlot raw[MAX_SLOTS];
    int n = ReadSlots(ng, raw);
    if (n < 0) return;
    std::vector<RosterEntry> fresh;
    for (int i = 0; i < n; ++i) fresh.push_back({ raw[i].slot, raw[i].name, { raw[i].cand[0], raw[i].cand[1], raw[i].cand[2] } });
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_RosterMx);
        changed = fresh.size() != g_Roster.size();
        for (size_t i = 0; !changed && i < fresh.size(); ++i)
            changed = fresh[i].name != g_Roster[i].name || fresh[i].cand[0] != g_Roster[i].cand[0];
        g_Roster = fresh;
    }
    if (changed) {
        Log("players in match: %d", (int)fresh.size());
        for (auto& e : fresh) Log("   slot %2u  %-24s ref %016llx %016llx  resolved %016llx", e.slot, e.name.c_str(),
                                  (unsigned long long)e.cand[0], (unsigned long long)e.cand[1], (unsigned long long)e.cand[2]);
    }
}

struct RemoteAvatar { HandCal cal[2]; };
static std::map<std::string, RemoteAvatar> g_Avatars;       // game-thread only
static std::string g_Me;
static int g_RemotePosed = 0;

// actor -> (pose, skeleton) of that remote player, or false. POD + __try.
static int ResolveRemote(uint8_t* cs, uint64_t actor, void** poseOut, uint16_t* skelOut) {
    __try {
        void** vt = *(void***)cs;
        pf_CsLookup lookup = (pf_CsLookup)vt[0x1a8 / 8];
        pf_CsValid  valid  = (pf_CsValid)vt[0x1c8 / 8];
        uint64_t tmp = 0;
        uint32_t* hp = lookup(cs, &tmp, actor, -1);
        if (!hp) return 0;
        uint32_t h = *hp;
        if (!valid(cs, h)) return 0;
        uint16_t count = *(uint16_t*)(cs + 0xfc);
        uint16_t idx = *(uint16_t*)(*(uint8_t**)(cs + 0xd0) + (size_t)(h & 0xffff) * 4);
        if (idx >= count) return 0;
        uint8_t* inst = *(uint8_t**)(cs + 0x100) + (size_t)idx * 0x340;
        uint8_t* bodyCs = *(uint8_t**)(cs + 0x150);
        if (!bodyCs) return 0;
        uint32_t bh = *(uint32_t*)(inst + 0x10);
        uint16_t bidx = *(uint16_t*)(*(uint8_t**)(bodyCs + 0xc8) + (size_t)(bh & 0xffff) * 4);
        uint8_t* body = *(uint8_t**)(bodyCs + 0xf8) + (size_t)bidx * 0x3f70;
        void* pose = *(void**)(body + 0x3e70);
        if (!pose) return 0;
        *poseOut = pose;
        *skelOut = (uint16_t)*(uint32_t*)(body + 0x3e78);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void PoseFingersRigSafe(void* pose, uint16_t skel, const RigHand* rig, HandCal* cal,
                               const float* curl, const float* splay, bool left) {
    __try { PoseFingersRig(pose, skel, rig, *cal, curl, splay, left); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_Faulted = 1; }
}

static void ApplyRemote(uint8_t* cs) {
    if (!g_S.network || g_Faulted) return;
    std::vector<RosterEntry> roster;
    {
        std::lock_guard<std::mutex> lk(g_RosterMx);
        roster = g_Roster;
    }
    if (roster.empty()) return;

    std::vector<std::string> others, noRemote;
    struct Target { std::string name; void* pose; uint16_t skel; };
    std::vector<Target> targets;
    static int candUsed = -1;
    for (auto& e : roster) {
        void* pose = nullptr; uint16_t skel = 0;
        int r = 0, which = -1;
        for (int c = 0; c < 3 && r <= 0; ++c) {
            if (!e.cand[c] || e.cand[c] == ~0ull) continue;
            r = ResolveRemote(cs, e.cand[c], &pose, &skel);
            if (r > 0) which = c;
        }
        if (r > 0) {
            others.push_back(e.name); targets.push_back({ e.name, pose, skel });
            if (which != candUsed) {
                candUsed = which;
                static const char* nm[3] = { "raw ref[0]", "raw ref[1]", "resolved ref" };
                Log("net: avatar lookup works with the %s actor id", nm[which]);
            }
        }
        else if (r == 0) noRemote.push_back(e.name);
    }
    std::string me = g_S.myName[0] ? std::string(g_S.myName) : (noRemote.size() == 1 ? noRemote[0] : g_Me);
    if (me != g_Me) {
        g_Me = me;
        Log("net: local player is '%s'%s", me.c_str(), g_S.myName[0] ? " (MyName)" : " (the one slot with no remote body)");
    }
    if (!g_Me.empty()) Net_SetRoster(g_Me, others);

    ULONGLONG now = GetTickCount64();
    // TEST mode: every remote avatar copies the local tracking -- exercises the
    // whole slot -> actor -> body -> skeleton path without a second player.
    HtvFrame mirror;
    ULONGLONG mirrorAge = 0;
    bool useMirror = g_S.mirrorToRemotes && LatestFrame(mirror, mirrorAge) && mirrorAge <= (ULONGLONG)g_S.staleMs;
    static size_t lastTargets = (size_t)-1;
    if (g_S.mirrorToRemotes && targets.size() != lastTargets) {
        lastTargets = targets.size();
        Log("mirror test: %u remote avatar(s) resolved", (unsigned)targets.size());
        for (auto& t : targets) Log("   '%s' pose=%p skel=%u", t.name.c_str(), t.pose, (unsigned)t.skel);
    }
    for (auto& t : targets) {
        NetRemote nr;
        if (useMirror) {
            memcpy(nr.curl, mirror.curl, sizeof(nr.curl));
            memcpy(nr.splay, mirror.splay, sizeof(nr.splay));
            memcpy(nr.valid, mirror.valid, sizeof(nr.valid));
        } else if (!Net_GetRemote(t.name, nr) || now - nr.tick > 500) continue;
        RemoteAvatar& av = g_Avatars[t.name];
        for (int side = 0; side < 2; ++side) {
            if (!nr.valid[side]) continue;
            PoseFingersRigSafe(t.pose, t.skel, &g_Rig[side], &av.cal[side], nr.curl[side], nr.splay[side], side == 0);
        }
        ++g_RemotePosed;
    }
}

static void __fastcall Hooked_RemoteCachePose(uint8_t* cs) {
    ApplyRemote(cs);
    Real_RemoteCachePose(cs);
}

// SEH wrappers: a bad pointer in here disables the plugin instead of crashing
// the game. They hold no C++ objects, which __try requires.

static int CaptureSafe(uint8_t* inst, int h, HandCal* cal, bool left) {
    __try { return Capture(inst, h, *cal, left) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_Faulted = 1; return 0; }
}
static void PoseHandSafe(uint8_t* inst, int h, HandCal* cal, const float* curl, const float* splay, bool left) {
    __try { PoseHand(inst, h, *cal, curl, splay, left); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_Faulted = 1; }
}

static const char* FingerName(int f) {
    static const char* n[] = { "thumb", "index", "middle", "ring", "pinky" };
    return (f >= 0 && f < 5) ? n[f] : "?";
}

// =============================================================================
// The hook
// =============================================================================
static int g_LastCount = -1;

static void ApplyTracking(uint8_t* cs) {
    if (!g_S.enabled || g_Faulted) return;
    uint16_t count = *(uint16_t*)(cs + CS_COUNT);
    uint8_t* arr = *(uint8_t**)(cs + CS_ARRAY);
    if (!count || !arr) return;

    if (count != g_LastCount || g_LogInstancesRequest.exchange(0)) {
        g_LastCount = count;
        Log("hand animator instances: %u (TargetInstance=%d)", count, g_S.targetInstance);
        for (int i = 0; i < count && i < MAX_INST; ++i) {
            uint8_t* inst = arr + (size_t)i * INST_STRIDE;
            Log("   [%d] inst=%p pose=%p wrist L/R=%u/%u", i, inst, *(void**)(inst + INST_POSE),
                *(uint32_t*)(inst + INST_WRIST), *(uint32_t*)(inst + INST_WRIST + 4));
        }
    }

    HtvFrame fr;
    ULONGLONG age = 0;
    if (!LatestFrame(fr, age) || age > (ULONGLONG)g_S.staleMs) {
        if (g_FrameTick) { g_Stats[0].skipStale++; g_Stats[1].skipStale++; }
        return;
    }

    bool recal = g_CalibrateRequest.exchange(0) != 0;

    for (int i = 0; i < count && i < MAX_INST; ++i) {
        if (g_S.targetInstance >= 0 && i != g_S.targetInstance) continue;
        uint8_t* inst = arr + (size_t)i * INST_STRIDE;
        for (int h = 0; h < 2; ++h) {
            bool left = (h == g_S.gameHandLeft);
            int side = left ? 0 : 1;
            if (!fr.valid[side]) continue;
            HandCal& cal = g_Cal[i][h];
            if (cal.inst != inst) {
                cal.have = false; cal.loggedFail = false;
                cal.rigChecked = false; cal.rig = nullptr; cal.fInit = false;
                cal.inst = inst;
            }

            if (g_S.respectContact) {
                ULONGLONG tick = GetTickCount64();
                if (*(float*)(inst + INST_CONTACT + h * 12) >= CONTACT_EPS) {
                    cal.contactUntil = tick + (ULONGLONG)g_S.contactHoldMs;
                    g_Stats[side].skipContact++;
                    continue;
                }
                if (tick < cal.contactUntil) { g_Stats[side].skipHold++; continue; }
            }

            if (g_S.useRig) {
                if (!cal.rigChecked) {
                    cal.rigChecked = true;
                    cal.rig = FindRig(inst, h, cal.rigSlot);
                    if (cal.rig)
                        Log("instance %d hand %d (%s): rig mode -- wrist #%u matches the embedded chassis rig; slots = %s,%s,%s,%s,%s",
                            i, h, left ? "left" : "right", (unsigned)cal.rig->wrist,
                            FingerName(cal.rigSlot[0]), FingerName(cal.rigSlot[1]), FingerName(cal.rigSlot[2]),
                            FingerName(cal.rigSlot[3]), FingerName(cal.rigSlot[4]));
                    else
                        Log("instance %d hand %d (%s): joint table does not match the embedded rig -- using the captured-pose fallback",
                            i, h, left ? "left" : "right");
                }
                if (cal.rig) {
                    memcpy(cal.slotFinger, cal.rigSlot, sizeof(cal.slotFinger));   // Measure indexes by slotFinger
                    MeasureSafe(inst, h, &cal, fr.curl[side], &g_Stats[side]);
                    PoseHandRigSafe(inst, h, &cal, fr.curl[side], fr.splay[side], left);
                    g_Stats[side].applied++;
                    continue;
                }
            }

            bool open = true;
            for (int f = 0; f < 5; ++f) if (fr.curl[side][f] > g_S.openThreshold) open = false;
            if (recal || (!cal.have && g_S.autoCalibrate && open)) {
                if (CaptureSafe(inst, h, &cal, left)) {
                    int nAxis = 0;
                    for (int s2 = 0; s2 < 5; ++s2) for (int k2 = 0; k2 < 3; ++k2) nAxis += cal.axisOk[s2][k2] ? 1 : 0;
                    Log("calibrated instance %d hand %d (%s): auto bend axes %d/15", i, h, left ? "left" : "right", nAxis);
                    Log("calibrated instance %d hand %d (%s): slots = %s,%s,%s,%s,%s", i, h, left ? "left" : "right",
                        FingerName(cal.slotFinger[0]), FingerName(cal.slotFinger[1]), FingerName(cal.slotFinger[2]),
                        FingerName(cal.slotFinger[3]), FingerName(cal.slotFinger[4]));
                } else if (!cal.loggedFail) {
                    cal.loggedFail = true;
                    Log("calibration failed on instance %d hand %d (joint table not ready)", i, h);
                }
            }
            if (!cal.have) continue;
            MeasureSafe(inst, h, &cal, fr.curl[side], &g_Stats[side]);
            PoseHandSafe(inst, h, &cal, fr.curl[side], fr.splay[side], left);
            g_Stats[side].applied++;
        }
    }
    if (g_Faulted) Log("FAULT while posing -- hand tracking disabled for this session");
    ULONGLONG tnow = GetTickCount64();
    {
        static ULONGLONG netSince = 0;
        if (!netSince) netSince = tnow;
        if (tnow - netSince >= 5000) {
            netSince = tnow;
            int conn, sent, recv, peers;
            Net_Stats(&conn, &sent, &recv, &peers);
            size_t rs;
            { std::lock_guard<std::mutex> lk(g_RosterMx); rs = g_Roster.size(); }
            if (g_S.network)
                Log("net 5s: %s, me '%s', %u player(s) in match, sent %d, received %d, live peers %d, remote hands posed %d",
                    conn ? "connected" : "NOT connected", g_Me.c_str(), (unsigned)rs, sent, recv, peers, g_RemotePosed);
            g_RemotePosed = 0;
        }
    }
    for (int side = 0; side < 2; ++side) {
        Stats& st = g_Stats[side];
        st.frames++;
        if (!st.since) st.since = tnow;
        if (tnow - st.since < 5000) continue;
        if (st.applied || st.skipContact || st.skipHold) {
            int n = st.applied ? st.applied : 1, kn = st.knuckleN ? st.knuckleN : 1;
            Log("stats %s 5s: posed %d, skipped contact %d / hold %d / stale %d | curl jump per frame "
                "T%.3f I%.3f M%.3f R%.3f P%.3f | game knuckle motion mm/frame T%.2f I%.2f M%.2f R%.2f P%.2f",
                side ? "right" : "left", st.applied, st.skipContact, st.skipHold, st.skipStale,
                st.curlJump[0] / n, st.curlJump[1] / n, st.curlJump[2] / n, st.curlJump[3] / n, st.curlJump[4] / n,
                1000 * st.knuckleMove[0] / kn, 1000 * st.knuckleMove[1] / kn, 1000 * st.knuckleMove[2] / kn,
                1000 * st.knuckleMove[3] / kn, 1000 * st.knuckleMove[4] / kn);
        }
        Stats fresh;
        fresh.since = tnow;
        st = fresh;
    }
}

static void __fastcall Hooked_ThumbPiston(uint8_t* cs) {
    ApplyTracking(cs);
    Real_ThumbPiston(cs);
}

// =============================================================================
// Threads: UDP receiver, config-file watcher, installer
// =============================================================================
static void UdpThread() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { Log("socket failed"); return; }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTV_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // local only
    if (bind(s, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("bind 127.0.0.1:%d failed (%d) -- is another copy loaded?", HTV_PORT, WSAGetLastError());
        closesocket(s);
        return;
    }
    Log("listening on 127.0.0.1:%d", HTV_PORT);
    std::vector<char> buf(4096);
    bool announced = false;
    for (;;) {
        sockaddr_in from; int fl = sizeof(from);
        int n = recvfrom(s, buf.data(), (int)buf.size() - 1, 0, (SOCKADDR*)&from, &fl);
        if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        if (n == (int)sizeof(HtvFrame) && *(uint32_t*)buf.data() == HTV_MAGIC) {
            std::lock_guard<std::mutex> lock(g_FrameMutex);
            memcpy(&g_Frame, buf.data(), sizeof(HtvFrame));
            g_FrameTick = GetTickCount64();
            Net_SetLocal(g_Frame.curl, g_Frame.splay, g_Frame.valid);
            if (!announced) { announced = true; Log("receiving tracking frames from the bridge"); }
            continue;
        }
        buf[n] = 0;
        std::string text(buf.data(), n);
        std::string reply;
        if (Trim(text) == "Ping") reply = "PONG";
        else { int k = ApplyConfigText(text); reply = "OK " + std::to_string(k); Log("live: %s", Trim(text).c_str()); }
        sendto(s, reply.c_str(), (int)reply.size(), 0, (SOCKADDR*)&from, fl);
    }
}

static void LoadConfigFile(bool announce) {
    std::ifstream in(g_Dir + "handtracking_config.txt");
    if (!in.is_open()) return;
    std::stringstream ss; ss << in.rdbuf();
    int k = ApplyConfigText(ss.str());
    if (announce) Log("config: %d key(s) from handtracking_config.txt", k);
}

// Edit the config while the game runs; it is re-read within half a second.
static void ConfigWatchThread() {
    std::string path = g_Dir + "handtracking_config.txt";
    FILETIME last = {};
    for (;;) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) {
            if (CompareFileTime(&d.ftLastWriteTime, &last) != 0) {
                bool first = (last.dwLowDateTime == 0 && last.dwHighDateTime == 0);
                last = d.ftLastWriteTime;
                LoadConfigFile(true);
                if (!first) Log("config reloaded");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// The RVA must fall inside the host image before it is read at all: loaded into
// anything other than echovr.exe, base+RVA can be unmapped memory.
static uintptr_t ImageSize(uintptr_t base) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

static bool SigMatches(uintptr_t addr, const uint8_t* sig, size_t n) {
    __try { return memcmp((const void*)addr, sig, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool CheckSig(uintptr_t addr, const uint8_t* sig, size_t n, const char* what) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (addr + n <= base + ImageSize(base) && SigMatches(addr, sig, n)) return true;
    Log("signature mismatch at %s (echovr+0x%llx) -- not the expected echovr.exe build, not hooking",
        what, (unsigned long long)(addr - (uintptr_t)GetModuleHandleA(nullptr)));
    return false;
}

static void InstallThread() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    Log("HandTrackingValve loaded into %s (base %p)", exe, (void*)base);

    uintptr_t tp = base + RVA_THUMB_PISTON, gj = base + RVA_GET_JOINT, sj = base + RVA_SET_JOINT;
    if (!CheckSig(tp, SIG_THUMB_PISTON, sizeof(SIG_THUMB_PISTON), "UpdateThumbPistonAnimPoses") ||
        !CheckSig(gj, SIG_GET_JOINT, sizeof(SIG_GET_JOINT), "GetJoint") ||
        !CheckSig(sj, SIG_SET_JOINT, sizeof(SIG_SET_JOINT), "SetJoint")) return;

    Real_ThumbPiston = (pf_ThumbPiston)tp;
    GetJoint = (pf_GetJoint)gj;
    SetJoint = (pf_SetJoint)sj;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_ThumbPiston, Hooked_ThumbPiston);
    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) Log("hooked CR15HandAnimatorCS::UpdateThumbPistonAnimPoses");
    else Log("DetourTransactionCommit failed: %ld", err);

    // networking: other players' fingers
    uintptr_t ngu = base + RVA_NETGAME_UPDATE, gun = base + RVA_GET_USERNAME,
              sla = base + RVA_SLOT_ACTOR, rcp = base + RVA_REMOTE_CACHEPOSE;
    if (CheckSig(ngu, SIG_NETGAME_UPDATE, sizeof(SIG_NETGAME_UPDATE), "CR15NetGame::Update") &&
        CheckSig(gun, SIG_GET_USERNAME, sizeof(SIG_GET_USERNAME), "GetUserName") &&
        CheckSig(sla, SIG_SLOT_ACTOR, sizeof(SIG_SLOT_ACTOR), "CPlayerSlot actor ref") &&
        CheckSig(rcp, SIG_REMOTE_CACHEPOSE, sizeof(SIG_REMOTE_CACHEPOSE), "CR15RemotePlayerCS::UpdateCachePoseForPhysics")) {
        GetUserNameFn = (pf_GetUserName)gun;
        SlotActorFn = (pf_SlotActor)sla;
        Real_NetGameUpdate = (pf_NetGameUpdate)ngu;
        Real_RemoteCachePose = (pf_RemoteCachePose)rcp;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Real_NetGameUpdate, Hooked_NetGameUpdate);
        DetourAttach(&(PVOID&)Real_RemoteCachePose, Hooked_RemoteCachePose);
        err = DetourTransactionCommit();
        if (err == NO_ERROR) Log("hooked CR15NetGame::Update + CR15RemotePlayerCS::UpdateCachePoseForPhysics (networking)");
        else Log("network hooks failed: %ld", err);
    }
}

static void ResolveDir(HMODULE self) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p = path;
    size_t slash = p.find_last_of("\\/");
    g_Dir = (slash == std::string::npos) ? "" : p.substr(0, slash + 1);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        ResolveDir(hModule);
        LoadConfigFile(false);
        std::thread(InstallThread).detach();
        std::thread(UdpThread).detach();
        std::thread(ConfigWatchThread).detach();
        Net_Start(Log);
        Net_Configure(g_S.network, g_S.relayUrl, g_S.netSendHz);
    }
    return TRUE;
}
