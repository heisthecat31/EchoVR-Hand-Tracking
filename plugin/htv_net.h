// HandTrackingValve networking -- shares finger tracking with other players
// through the relay (Railway/SparkAPI htv_relay.py) over one WebSocket.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <windows.h>

struct NetRemote {
    float    curl[2][5];     // [0]=left [1]=right, HtvFinger order
    float    splay[2][4];
    uint8_t  valid[2];
    ULONGLONG tick;          // GetTickCount64() when it arrived
};

// Starts the connect/send/receive threads. `log` is the plugin's logger.
void Net_Start(void (*log)(const char* fmt, ...));

// Settings (applied live). An empty URL or enabled=0 disconnects.
void Net_Configure(int enabled, const std::string& url, int sendHz);

// Who we are and who else is in our match (display names). Sent to the relay
// whenever it changes; the relay only pairs players who list each other.
void Net_SetRoster(const std::string& me, const std::vector<std::string>& others);

// The local tracking to share: curls/splays in [0,1], valid per hand.
void Net_SetLocal(const float curl[2][5], const float splay[2][4], const uint8_t valid[2]);

// Latest frame received for `name`; false if none.
bool Net_GetRemote(const std::string& name, NetRemote& out);

// For the log: connected?, frames sent/received since last call.
void Net_Stats(int* connected, int* sent, int* received, int* peers);
