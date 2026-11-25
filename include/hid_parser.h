#pragma once

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Unified USB & BLE HID Joystick Report Descriptor + Report Parser
// Supports:
//  - EdgeTX Classic USB Joystick (19B: 24 buttons + 8 x 11-bit uint16 axes)
//  - EdgeTX Standard 2.11+ USB Joystick (20B: + 1B battery %)
//  - EdgeTX Advanced / Custom HID Report Descriptors
//  - ExpressLRS BLE Joystick ("ELRS Joystick" via BleGamepad: X,Y,Z,Rz,Rx,Ry,S1,S2)
//  - Generic USB / BLE HID Gamepads & Joysticks
// ============================================================================

constexpr int HID_MAX_AXES = 8;

struct HidAxisField {
    bool     present    = false;
    uint16_t usage      = 0;      // Generic Desktop usage (0x30=X..0x37=Dial)
    uint16_t bitOffset  = 0;      // Bit offset inside report payload (after optional Report ID)
    uint8_t  bitSize    = 0;      // Width in bits (e.g. 8, 10, 11, 12, 16)
    int32_t  logicalMin = 0;
    int32_t  logicalMax = 2047;
    bool     isSigned   = false;
};

struct HidReportLayout {
    bool         valid          = false;
    bool         hasReportId    = false;
    uint8_t      reportId       = 0;
    uint16_t     totalBits      = 0;
    uint16_t     buttonBitOff   = 0;
    uint8_t      buttonCount    = 0;
    HidAxisField axes[HID_MAX_AXES]; // Indexed 0..7 in standard usage order (X,Y,Z,Rx,Ry,Rz,Slider,Dial)
    bool         isElrsOrder    = false; // True when ELRS BLE Joystick maps CH1..8 -> X,Y,Rx,Ry,Z,Rz,S1,S2
};

struct HidJoystickState {
    float    axes[HID_MAX_AXES] = {0}; // Normalized [-1.0 .. +1.0], mapped to logical CH1..CH8
    uint32_t buttons            = 0;   // Up to 24 buttons
    int8_t   batteryPct         = -1;  // -1 if not reported
    bool     valid              = false;
};

class HidReportParser {
public:
    // Parse a raw USB or BLE HID Report Descriptor (0x22 / 0x2A4B)
    // Returns true if at least 2 analog axes were discovered.
    static bool parseDescriptor(const uint8_t* desc, size_t len, HidReportLayout& outLayout);

    // Populate the known EdgeTX Classic / Standard USB Joystick layout directly
    static void makeEdgeTxLayout(HidReportLayout& outLayout);

    // Populate the known ExpressLRS BLE Joystick (ESP32-BLE-Gamepad) layout directly
    static void makeElrsBleLayout(HidReportLayout& outLayout);

    // Decode a raw HID Input Report using a parsed (or preset) layout
    static bool decodeReport(const HidReportLayout& layout,
                             const uint8_t* report, size_t len,
                             HidJoystickState& outState);

    // Fast-path decoder specifically for EdgeTX Classic (19B) / 2.11+ (20B) reports
    static bool decodeEdgeTxRaw(const uint8_t* report, size_t len, HidJoystickState& outState);

private:
    static int32_t extractBits(const uint8_t* data, size_t lenBytes,
                               uint16_t bitOffset, uint8_t bitSize, bool isSigned);
    static int usageToSlot(uint16_t usagePage, uint16_t usage);
};
