#pragma once

#include "config.h"
#include "hid_parser.h"
#include <stdint.h>

// ============================================================================
// ESP32-S3 Native USB-OTG Host HID Joystick Driver for EdgeTX Radio Controllers
// Runs on Core 0 FreeRTOS tasks using ESP-IDF usb/usb_host.h.
// Supports hot-plug, HID SET_IDLE, EdgeTX VID:1209 PID:4F54 (Classic 19B,
// 2.11+ 20B, Advanced), generic USB HID Joysticks, and live status diagnostics.
// ============================================================================

class UsbEdgeTxHost {
public:
    bool begin();
    bool isConnected() const;
    bool isEdgeTxRadio() const;
    uint32_t getPacketCount() const;
    bool getLatestState(HidJoystickState& outState, uint32_t& outAgeMs) const;
    void getDeviceLabel(char* buf, size_t bufLen) const;
    void getStatusText(char* buf, size_t bufLen) const;
};
