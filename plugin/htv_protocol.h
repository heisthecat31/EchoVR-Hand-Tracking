// HandTrackingValve wire format -- shared by the bridge (sender) and the plugin
// (receiver). Loopback UDP only.
//
// Two kinds of datagram arrive on HTV_PORT:
//   * a binary HtvFrame, recognised by its magic, sent ~120 times a second;
//   * plain "Key = Value" text, the same syntax as handtracking_config.txt,
//     applied immediately (the bridge's --set option sends these). Two text queries
//     are answered instead: "Ping" -> "PONG", and "Uptime" -> "UP <ms since the
//     plugin loaded>", which the bridge uses to spot a fresh Echo launch.
#pragma once
#include <stdint.h>

#define HTV_PORT 8768
#define HTV_MAGIC 0x31565448u  // "HTV1" little-endian

// Finger order used on the wire. This is OpenVR's EVRFinger order, and it is
// deliberately NOT the game's order -- the plugin works out which of the game's
// five finger slots is which (see DetectFingerOrder in handtracking.cpp).
enum HtvFinger { HTV_THUMB = 0, HTV_INDEX = 1, HTV_MIDDLE = 2, HTV_RING = 3, HTV_PINKY = 4 };

#pragma pack(push, 1)
struct HtvFrame {
    uint32_t magic;          // HTV_MAGIC
    uint32_t seq;            // increments per frame
    uint8_t  valid[2];       // [0]=left, [1]=right: 1 if that hand's data is live
    uint8_t  pad[2];
    float    curl[2][5];     // 0 = straight, 1 = fully curled (HtvFinger order)
    float    splay[2][4];    // thumb-index, index-middle, middle-ring, ring-pinky
};
#pragma pack(pop)
