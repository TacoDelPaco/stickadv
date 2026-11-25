#pragma once

#include "config.h"
#include "hid_parser.h"
#include <stdint.h>

// ============================================================================
// Ultra-Low-Latency BLE HID Joystick Client (NimBLE-Arduino 1.4.x)
// Connects to ExpressLRS BLE Joystick ("ELRS Joystick") & generic BLE HID
// gamepads (Service 0x1812). Uses generous discovery params during GATT +
// pairing, auto-recovers from stale bonds, then switches to 7.5-11.25ms
// low-latency notifications (0x2A4D) on Core 0.
// ============================================================================

class BleHidHost {
public:
    bool begin();
    void startScan(uint32_t durationSec = 10);
    void stopScan();
    void disconnect();
    void clearBonds();

    void poll(bool allowBackgroundReconnect);

    bool isConnected() const;
    bool isScanning() const;
    bool isElrsJoystick() const;
    int  getRssiPct() const;
    uint32_t getPacketCount() const;
    bool getLatestState(HidJoystickState& outState, uint32_t& outAgeMs) const;
    void getPeerName(char* buf, size_t bufLen) const;
    void getStatusText(char* buf, size_t bufLen) const;
};
