#include "hid_parser.h"
#include <string.h>
#include <math.h>

namespace {

inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

int32_t readItemSigned(const uint8_t* p, uint8_t size) {
    switch (size) {
        case 1: return static_cast<int8_t>(p[0]);
        case 2: return static_cast<int16_t>(p[0] | (p[1] << 8));
        case 4: return static_cast<int32_t>(
            static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24));
        default: return 0;
    }
}

uint32_t readItemUnsigned(const uint8_t* p, uint8_t size) {
    switch (size) {
        case 1: return p[0];
        case 2: return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
        case 4: return static_cast<uint32_t>(p[0]) |
                       (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) |
                       (static_cast<uint32_t>(p[3]) << 24);
        default: return 0;
    }
}

} // namespace

int HidReportParser::usageToSlot(uint16_t usagePage, uint16_t usage) {
    if (usagePage == 0x01) { // Generic Desktop
        if (usage >= 0x30 && usage <= 0x37) {
            return static_cast<int>(usage - 0x30); // X,Y,Z,Rx,Ry,Rz,Slider,Dial
        }
    } else if (usagePage == 0x02) { // Simulation Controls (EdgeTX Advanced Sim mode)
        switch (usage) {
            case 0xB0: return 0; // Aileron
            case 0xB8: return 1; // Elevator
            case 0xBB: return 2; // Throttle
            case 0xBA: return 3; // Rudder
            case 0xC4: return 4; // Accelerator
            case 0xC5: return 5; // Brake
            case 0xC8: return 6; // Steering
            default: break;
        }
    }
    return -1;
}

void HidReportParser::makeEdgeTxLayout(HidReportLayout& outLayout) {
    outLayout = HidReportLayout{};
    outLayout.valid        = true;
    outLayout.hasReportId  = false;
    outLayout.reportId     = 0;
    outLayout.buttonBitOff = 0;
    outLayout.buttonCount  = 24;
    outLayout.isElrsOrder  = false;

    for (int i = 0; i < HID_MAX_AXES; ++i) {
        outLayout.axes[i].present    = true;
        outLayout.axes[i].usage      = static_cast<uint16_t>(0x30 + i);
        outLayout.axes[i].bitOffset  = static_cast<uint16_t>(24 + i * 16);
        outLayout.axes[i].bitSize    = 16;
        outLayout.axes[i].logicalMin = 0;
        outLayout.axes[i].logicalMax = 2047;
        outLayout.axes[i].isSigned   = false;
    }
    outLayout.totalBits = 24 + 8 * 16; // 19 bytes
}

void HidReportParser::makeElrsBleLayout(HidReportLayout& outLayout) {
    // ExpressLRS devBLE.cpp uses ESP32-BLE-Gamepad:
    //   1 byte (8 buttons) + 8 x int16 (0..32767) in usage order:
    //   X, Y, Z, Rz, Rx, Ry, Slider, Dial (or X, Y, Z, Rx, Ry, Rz, S1, S2 depending on version).
    // When report descriptor is parsed from 0x2A4B, exact offsets are discovered automatically.
    // This preset serves as a fallback if 0x2A4B is unavailable.
    outLayout = HidReportLayout{};
    outLayout.valid        = true;
    outLayout.hasReportId  = false;
    outLayout.reportId     = 0;
    outLayout.buttonBitOff = 0;
    outLayout.buttonCount  = 8;
    outLayout.isElrsOrder  = true;

    // Default ESP32-BLE-Gamepad axis order: X, Y, Z, Rz, Rx, Ry, Slider, Dial
    const uint8_t slots[8] = {0, 1, 2, 5, 3, 4, 6, 7};
    for (int i = 0; i < 8; ++i) {
        int s = slots[i];
        outLayout.axes[s].present    = true;
        outLayout.axes[s].usage      = static_cast<uint16_t>(0x30 + s);
        outLayout.axes[s].bitOffset  = static_cast<uint16_t>(8 + i * 16);
        outLayout.axes[s].bitSize    = 16;
        outLayout.axes[s].logicalMin = 0;
        outLayout.axes[s].logicalMax = 32767;
        outLayout.axes[s].isSigned   = false;
    }
    outLayout.totalBits = 8 + 8 * 16; // 17 bytes
}

bool HidReportParser::parseDescriptor(const uint8_t* desc, size_t len, HidReportLayout& outLayout) {
    if (!desc || len < 4) return false;

    outLayout = HidReportLayout{};

    uint16_t usagePage   = 0;
    int32_t  logicalMin  = 0;
    int32_t  logicalMax  = 0;
    uint32_t reportSize  = 0;
    uint32_t reportCount = 0;
    uint8_t  curReportId = 0;
    bool     seenReportId = false;

    // Local state
    uint16_t usages[32];
    int      usageCount = 0;
    uint16_t usageMin   = 0;
    uint16_t usageMax   = 0;
    bool     hasUsageRange = false;

    // Track bit offset per report ID (we keep the first gamepad/joystick input report)
    uint16_t bitPos = 0;
    uint8_t  targetReportId = 0;
    bool     lockedReportId = false;
    int      axesFound = 0;

    size_t idx = 0;
    while (idx < len) {
        uint8_t prefix = desc[idx++];
        if (prefix == 0xFE) {
            // Long item: skip
            if (idx + 1 >= len) break;
            uint8_t dataLen = desc[idx];
            idx += 2 + dataLen;
            continue;
        }

        uint8_t sizeCode = prefix & 0x03;
        uint8_t itemSize = (sizeCode == 3) ? 4 : sizeCode;
        uint8_t itemType = (prefix >> 2) & 0x03; // 0=Main, 1=Global, 2=Local
        uint8_t itemTag  = (prefix >> 4) & 0x0F;

        if (idx + itemSize > len) break;
        const uint8_t* itemData = &desc[idx];
        idx += itemSize;

        if (itemType == 1) { // Global
            switch (itemTag) {
                case 0x0: usagePage   = static_cast<uint16_t>(readItemUnsigned(itemData, itemSize)); break;
                case 0x1: logicalMin  = readItemSigned(itemData, itemSize); break;
                case 0x2: logicalMax  = readItemSigned(itemData, itemSize); break;
                case 0x7: reportSize  = readItemUnsigned(itemData, itemSize); break;
                case 0x8: {
                    uint8_t rid = static_cast<uint8_t>(readItemUnsigned(itemData, itemSize));
                    if (rid != curReportId) {
                        curReportId = rid;
                        if (!lockedReportId) {
                            bitPos = 0;
                        }
                    }
                    seenReportId = true;
                    break;
                }
                case 0x9: reportCount = readItemUnsigned(itemData, itemSize); break;
                default: break;
            }
        } else if (itemType == 2) { // Local
            switch (itemTag) {
                case 0x0:
                    if (usageCount < 32) {
                        usages[usageCount++] = static_cast<uint16_t>(readItemUnsigned(itemData, itemSize));
                    }
                    break;
                case 0x1:
                    usageMin = static_cast<uint16_t>(readItemUnsigned(itemData, itemSize));
                    hasUsageRange = true;
                    break;
                case 0x2:
                    usageMax = static_cast<uint16_t>(readItemUnsigned(itemData, itemSize));
                    hasUsageRange = true;
                    break;
                default: break;
            }
        } else if (itemType == 0) { // Main
            if (itemTag == 0x8) { // Input item
                uint32_t flags = readItemUnsigned(itemData, itemSize);
                bool isConstant = (flags & 0x01) != 0;

                // Only parse fields for the primary joystick report ID
                bool matchesReport = (!lockedReportId || curReportId == targetReportId);
                if (matchesReport && !isConstant && reportSize > 0 && reportCount > 0) {
                    if (usagePage == 0x09 && reportSize == 1) {
                        // Buttons
                        if (outLayout.buttonCount == 0) {
                            outLayout.buttonBitOff = bitPos;
                            outLayout.buttonCount  = static_cast<uint8_t>(reportCount > 24 ? 24 : reportCount);
                            if (seenReportId && !lockedReportId) {
                                lockedReportId = true;
                                targetReportId = curReportId;
                            }
                        }
                    } else if (usagePage == 0x01 || usagePage == 0x02) {
                        for (uint32_t i = 0; i < reportCount; ++i) {
                            uint16_t u = 0;
                            if (hasUsageRange) {
                                uint32_t candidate = static_cast<uint32_t>(usageMin) + i;
                                u = static_cast<uint16_t>(candidate <= usageMax ? candidate : usageMax);
                            } else if (usageCount > 0) {
                                u = usages[i < static_cast<uint32_t>(usageCount) ? i : (usageCount - 1)];
                            }

                            int slot = usageToSlot(usagePage, u);
                            if (slot >= 0 && slot < HID_MAX_AXES && !outLayout.axes[slot].present && reportSize <= 32) {
                                HidAxisField& f = outLayout.axes[slot];
                                f.present    = true;
                                f.usage      = u;
                                f.bitOffset  = static_cast<uint16_t>(bitPos + i * reportSize);
                                f.bitSize    = static_cast<uint8_t>(reportSize);
                                f.logicalMin = logicalMin;
                                f.logicalMax = logicalMax;
                                f.isSigned   = (logicalMin < 0);
                                ++axesFound;
                                if (seenReportId && !lockedReportId) {
                                    lockedReportId = true;
                                    targetReportId = curReportId;
                                }
                            }
                        }
                    }
                }
                if (matchesReport) {
                    bitPos = static_cast<uint16_t>(bitPos + reportSize * reportCount);
                }
            }
            // Clear local items after every Main item per HID 1.11 spec
            usageCount    = 0;
            hasUsageRange = false;
            usageMin      = 0;
            usageMax      = 0;
        }
    }

    if (axesFound >= 2) {
        outLayout.valid       = true;
        outLayout.hasReportId = lockedReportId && (targetReportId != 0);
        outLayout.reportId    = targetReportId;
        outLayout.totalBits   = bitPos;
        return true;
    }
    return false;
}

int32_t HidReportParser::extractBits(const uint8_t* data, size_t lenBytes,
                                     uint16_t bitOffset, uint8_t bitSize, bool isSigned) {
    if (!data || bitSize == 0 || bitSize > 32) return 0;
    if (static_cast<size_t>((bitOffset + bitSize + 7) / 8) > lenBytes) return 0;

    uint32_t raw = 0;
    for (uint8_t b = 0; b < bitSize; ++b) {
        uint16_t pos = static_cast<uint16_t>(bitOffset + b);
        uint8_t byteVal = data[pos >> 3];
        if (byteVal & (1u << (pos & 7))) {
            raw |= (1u << b);
        }
    }

    if (isSigned && bitSize < 32) {
        uint32_t signBit = 1u << (bitSize - 1);
        if (raw & signBit) {
            raw |= (~0u << bitSize);
        }
        return static_cast<int32_t>(raw);
    }
    return static_cast<int32_t>(raw);
}

bool HidReportParser::decodeReport(const HidReportLayout& layout,
                                   const uint8_t* report, size_t len,
                                   HidJoystickState& outState) {
    if (!layout.valid || !report || len == 0) return false;

    const uint8_t* payload = report;
    size_t payloadLen = len;

    if (layout.hasReportId) {
        if (report[0] != layout.reportId) {
            return false; // Different report ID (e.g., keyboard or battery report)
        }
        payload    = report + 1;
        payloadLen = len - 1;
    }

    // Buttons
    uint32_t btns = 0;
    for (uint8_t b = 0; b < layout.buttonCount && b < 24; ++b) {
        int32_t bit = extractBits(payload, payloadLen,
                                  static_cast<uint16_t>(layout.buttonBitOff + b), 1, false);
        if (bit) btns |= (1u << b);
    }
    outState.buttons = btns;

    // Extract normalized axes in raw usage slots:
    // slot 0=X, 1=Y, 2=Z, 3=Rx, 4=Ry, 5=Rz, 6=Slider, 7=Dial
    float slotVal[HID_MAX_AXES] = {0};
    for (int i = 0; i < HID_MAX_AXES; ++i) {
        const HidAxisField& f = layout.axes[i];
        if (!f.present) continue;
        int32_t v = extractBits(payload, payloadLen, f.bitOffset, f.bitSize, f.isSigned);
        int32_t span = f.logicalMax - f.logicalMin;
        if (span > 0) {
            float norm = (static_cast<float>(v - f.logicalMin) / static_cast<float>(span)) * 2.0f - 1.0f;
            slotVal[i] = clampf(norm, -1.0f, 1.0f);
        }
    }

    if (layout.isElrsOrder) {
        // ExpressLRS devBLE.cpp:
        // bleGamepad->setAxes(ch0, ch1, ch4, ch5, ch2, ch3, ch6, ch7)
        // i.e. X=CH1, Y=CH2, Z=CH5, Rz=CH6, Rx=CH3, Ry=CH4, S1=CH7, S2=CH8
        outState.axes[0] = slotVal[0]; // CH1 <- X
        outState.axes[1] = slotVal[1]; // CH2 <- Y
        outState.axes[2] = slotVal[3]; // CH3 <- Rx
        outState.axes[3] = slotVal[4]; // CH4 <- Ry
        outState.axes[4] = slotVal[2]; // CH5 <- Z
        outState.axes[5] = slotVal[5]; // CH6 <- Rz
        outState.axes[6] = slotVal[6]; // CH7 <- Slider
        outState.axes[7] = slotVal[7]; // CH8 <- Dial
    } else {
        for (int i = 0; i < HID_MAX_AXES; ++i) {
            outState.axes[i] = slotVal[i];
        }
    }

    outState.batteryPct = -1;
    outState.valid      = true;
    return true;
}

bool HidReportParser::decodeEdgeTxRaw(const uint8_t* report, size_t len, HidJoystickState& outState) {
    // EdgeTX Classic report is 19 bytes; EdgeTX 2.11+ Standard report is 20 bytes (+battery byte).
    // Some stacks prepend a 1-byte Report ID if defined, so handle 19 or 20 bytes cleanly.
    if (!report || len < 19) return false;

    outState.buttons = static_cast<uint32_t>(report[0]) |
                       (static_cast<uint32_t>(report[1]) << 8) |
                       (static_cast<uint32_t>(report[2]) << 16);

    for (int i = 0; i < 8; ++i) {
        uint16_t raw = static_cast<uint16_t>(report[3 + i * 2]) |
                       (static_cast<uint16_t>(report[4 + i * 2]) << 8);
        // EdgeTX sends 0..2047 (Classic) or 0..2048 (Standard), center 1024
        float norm = (static_cast<float>(raw) - 1024.0f) / 1024.0f;
        outState.axes[i] = clampf(norm, -1.0f, 1.0f);
    }

    if (len >= 20 && report[19] <= 100) {
        outState.batteryPct = static_cast<int8_t>(report[19]);
    } else {
        outState.batteryPct = -1;
    }
    outState.valid = true;
    return true;
}
