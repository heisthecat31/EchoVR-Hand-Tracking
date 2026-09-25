// HandTrackingValve networking: a WinHTTP WebSocket client to the relay.
//
// Wire format (see Railway/SparkAPI/htv_relay.py):
//   -> text   {"v":1,"name":"<me>","roster":["<other>",...]}      on connect / roster change
//   -> binary "HTF1" flags curl[2][5] splay[2][4]  (u8 each)       ~30 Hz while tracking is live
//   <- binary "HTR1" len name  flags curl[2][5] splay[2][4]         from players in our match
// flags: bit0 = left hand valid, bit1 = right hand valid.
//
// WinHTTP ships with Windows, so nothing extra is installed. One thread
// connects and sends; a second receives; both stop on error and the first
// reconnects with backoff.

#define WIN32_LEAN_AND_MEAN
#include "htv_net.h"
#include <winhttp.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

static void (*g_NetLog)(const char*, ...) = nullptr;
#define NLOG(...) do { if (g_NetLog) g_NetLog(__VA_ARGS__); } while (0)

static std::mutex g_Mx;
static int         g_Enabled = 1;
static std::string g_Url;
static int         g_SendHz = 30;
static std::string g_Me;
static std::vector<std::string> g_Others;
static int         g_RosterVersion = 0;

static float    g_LocalCurl[2][5], g_LocalSplay[2][4];
static uint8_t  g_LocalValid[2];
static ULONGLONG g_LocalTick = 0;

static std::map<std::string, NetRemote> g_Remote;
static std::atomic<int> g_Connected(0), g_Sent(0), g_Recv(0);
static std::atomic<int> g_ConfigVersion(0);

void Net_Configure(int enabled, const std::string& url, int sendHz) {
    std::lock_guard<std::mutex> lk(g_Mx);
    if (enabled != g_Enabled || url != g_Url) ++g_ConfigVersion;
    g_Enabled = enabled;
    g_Url = url;
    g_SendHz = sendHz < 5 ? 5 : (sendHz > 60 ? 60 : sendHz);
}

void Net_SetRoster(const std::string& me, const std::vector<std::string>& others) {
    std::lock_guard<std::mutex> lk(g_Mx);
    if (me == g_Me && others == g_Others) return;
    g_Me = me;
    g_Others = others;
    ++g_RosterVersion;
}

void Net_SetLocal(const float curl[2][5], const float splay[2][4], const uint8_t valid[2]) {
    std::lock_guard<std::mutex> lk(g_Mx);
    memcpy(g_LocalCurl, curl, sizeof(g_LocalCurl));
    memcpy(g_LocalSplay, splay, sizeof(g_LocalSplay));
    memcpy(g_LocalValid, valid, sizeof(g_LocalValid));
    g_LocalTick = GetTickCount64();
}

bool Net_GetRemote(const std::string& name, NetRemote& out) {
    std::lock_guard<std::mutex> lk(g_Mx);
    auto it = g_Remote.find(name);
    if (it == g_Remote.end()) return false;
    out = it->second;
    return true;
}

void Net_Stats(int* connected, int* sent, int* received, int* peers) {
    *connected = g_Connected;
    *sent = g_Sent.exchange(0);
    *received = g_Recv.exchange(0);
    std::lock_guard<std::mutex> lk(g_Mx);
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (auto& kv : g_Remote) if (now - kv.second.tick < 2000) ++n;
    *peers = n;
}

static std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static inline uint8_t Q(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static void HandleIncoming(const uint8_t* d, DWORD n) {
    // "HTR1" len name flags curl[10] splay[8]
    if (n < 5 || memcmp(d, "HTR1", 4) != 0) return;
    DWORD nl = d[4];
    if (n != 5 + nl + 19) return;
    std::string name((const char*)d + 5, nl);
    const uint8_t* p = d + 5 + nl;
    NetRemote r;
    r.valid[0] = (p[0] & 1) ? 1 : 0;
    r.valid[1] = (p[0] & 2) ? 1 : 0;
    for (int h = 0; h < 2; ++h) for (int f = 0; f < 5; ++f) r.curl[h][f] = p[1 + h * 5 + f] / 255.0f;
    for (int h = 0; h < 2; ++h) for (int f = 0; f < 4; ++f) r.splay[h][f] = p[11 + h * 4 + f] / 255.0f;
    r.tick = GetTickCount64();
    bool first;
    {
        std::lock_guard<std::mutex> lk(g_Mx);
        first = g_Remote.find(name) == g_Remote.end();
        g_Remote[name] = r;
        ++g_Recv;
    }
    if (first) NLOG("net: first frame from '%s' (L %s, R %s)", name.c_str(), r.valid[0] ? "on" : "off", r.valid[1] ? "on" : "off");
}

static void ReceiveLoop(HINTERNET ws, std::atomic<int>* alive) {
    uint8_t buf[512];
    std::vector<uint8_t> msg;
    while (*alive) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &got, &type);
        if (err != ERROR_SUCCESS) break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        msg.insert(msg.end(), buf, buf + got);
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            if (msg.size() > 4096) msg.clear();
            continue;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            HandleIncoming(msg.data(), (DWORD)msg.size());
        msg.clear();
    }
    *alive = 0;
}

static bool CrackUrl(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& secure) {
    std::wstring w(url.begin(), url.end());
    // accept ws:// wss:// http:// https://
    if (w.compare(0, 6, L"wss://") == 0) w = L"https://" + w.substr(6);
    else if (w.compare(0, 5, L"ws://") == 0) w = L"http://" + w.substr(5);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t h[256] = {}, p[1024] = {};
    uc.lpszHostName = h; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = p; uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
    host = h;
    path = p[0] ? p : L"/";
    port = uc.nPort;
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return !host.empty();
}

static void NetMain() {
    int backoffMs = 1000;
    for (;;) {
        std::string url;
        int enabled, cfgv;
        {
            std::lock_guard<std::mutex> lk(g_Mx);
            url = g_Url; enabled = g_Enabled; cfgv = g_ConfigVersion;
        }
        if (!enabled || url.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }

        std::wstring host, path;
        INTERNET_PORT port = 0;
        bool secure = false;
        if (!CrackUrl(url, host, path, port, secure)) {
            NLOG("net: bad RelayUrl '%s'", url.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        HINTERNET s = WinHttpOpen(L"HandTrackingValve/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET c = s ? WinHttpConnect(s, host.c_str(), port, 0) : nullptr;
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        HINTERNET ws = nullptr;
        if (r && WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            ws = WinHttpWebSocketCompleteUpgrade(r, 0);
        }
        DWORD le = GetLastError();
        if (r) WinHttpCloseHandle(r);

        if (!ws) {
            NLOG("net: could not connect to %s (error %lu) -- retrying in %d s", url.c_str(), le, backoffMs / 1000);
            if (c) WinHttpCloseHandle(c);
            if (s) WinHttpCloseHandle(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = backoffMs * 2 > 30000 ? 30000 : backoffMs * 2;
            continue;
        }
        NLOG("net: connected to %s", url.c_str());
        backoffMs = 1000;
        g_Connected = 1;
        std::atomic<int> alive(1);
        std::thread rx(ReceiveLoop, ws, &alive);

        int sentRoster = -1;
        ULONGLONG lastSend = 0;
        while (alive) {
            std::string hello;
            uint8_t frame[23];
            bool haveFrame = false;
            int hz;
            {
                std::lock_guard<std::mutex> lk(g_Mx);
                if (g_ConfigVersion != cfgv || !g_Enabled) { alive = 0; break; }
                hz = g_SendHz;
                if (g_RosterVersion != sentRoster && !g_Me.empty()) {
                    hello = "{\"v\":1,\"name\":\"" + JsonEscape(g_Me) + "\",\"roster\":[";
                    for (size_t i = 0; i < g_Others.size(); ++i)
                        hello += (i ? ",\"" : "\"") + JsonEscape(g_Others[i]) + "\"";
                    hello += "]}";
                    sentRoster = g_RosterVersion;
                }
                ULONGLONG now = GetTickCount64();
                if (!g_Me.empty() && now - g_LocalTick < 250 && (g_LocalValid[0] || g_LocalValid[1]) &&
                    now - lastSend >= (ULONGLONG)(1000 / hz)) {
                    memcpy(frame, "HTF1", 4);
                    frame[4] = (g_LocalValid[0] ? 1 : 0) | (g_LocalValid[1] ? 2 : 0);
                    for (int h = 0; h < 2; ++h) for (int f = 0; f < 5; ++f) frame[5 + h * 5 + f] = Q(g_LocalCurl[h][f]);
                    for (int h = 0; h < 2; ++h) for (int f = 0; f < 4; ++f) frame[15 + h * 4 + f] = Q(g_LocalSplay[h][f]);
                    haveFrame = true;
                    lastSend = now;
                }
            }
            if (!hello.empty()) {
                if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (PVOID)hello.data(), (DWORD)hello.size()) != ERROR_SUCCESS) break;
                NLOG("net: roster sent (%s)", hello.c_str());
            }
            if (haveFrame) {
                if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, frame, sizeof(frame)) != ERROR_SUCCESS) break;
                ++g_Sent;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        alive = 0;
        g_Connected = 0;
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);           // unblocks the receiver
        rx.join();
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
        NLOG("net: disconnected");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void Net_Start(void (*log)(const char* fmt, ...)) {
    g_NetLog = log;
    std::thread(NetMain).detach();
}
