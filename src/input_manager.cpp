#include "input_manager.h"
#include <math.h>

namespace {

inline float applyStickDeadzone(float v, float dz) {
    if (fabsf(v) < dz) return 0.0f;
    return (v > 0.0f) ? ((v - dz) / (1.0f - dz)) : ((v + dz) / (1.0f - dz));
}

} // namespace

void InputManager::begin() {
    _imu.begin();
    _usb.begin();
    _ble.begin();
}

void InputManager::mapChannels(const HidJoystickState& hid, ChannelMap mapMode, PilotInput& out) {
    for (int i = 0; i < 8; ++i) {
        out.rawCh[i] = hid.axes[i];
    }

    float ail = 0.0f, ele = 0.0f, thr = 0.0f, rud = 0.0f;
    switch (mapMode) {
        case ChannelMap::TAER:
            thr = hid.axes[0];
            ail = hid.axes[1];
            ele = hid.axes[2];
            rud = hid.axes[3];
            break;
        case ChannelMap::AERT:
            ail = hid.axes[0];
            ele = hid.axes[1];
            rud = hid.axes[2];
            thr = hid.axes[3];
            break;
        case ChannelMap::AETR:
        default:
            ail = hid.axes[0];
            ele = hid.axes[1];
            thr = hid.axes[2];
            rud = hid.axes[3];
            break;
    }

    out.roll     = applyStickDeadzone(ail, 0.025f);
    out.pitch    = applyStickDeadzone(ele, 0.025f);
    out.throttle = thr; // Throttle should not have center deadzone
    out.yaw      = applyStickDeadzone(rud, 0.025f);
    out.buttons  = hid.buttons;

    // AUX1 (CH5 > +0.30) or Button 1 acts as EdgeTX Arm / Start switch
    bool armNow    = (hid.axes[4] > 0.30f) || ((hid.buttons & 0x01u) != 0);
    out.armSwitch  = armNow;
    out.armEdge    = (armNow && !_prevArmSwitch);
    _prevArmSwitch = armNow;
}

void InputManager::update(float dt, const KeyFlightState& keys, const SimSettings& settings,
                          bool inActiveFlight, PilotInput& outInput) {
    // Pause BLE scanning during active flight or when USB is connected to avoid any RF jitter
    if ((inActiveFlight || _usb.isConnected()) && _ble.isScanning()) {
        _ble.stopScan();
    }
    _ble.poll(!inActiveFlight && !_usb.isConnected());

    // Priority 1: Native USB-OTG EdgeTX / HID Radio Controller
    HidJoystickState hid;
    uint32_t ageMs = 0;
    if (_usb.isConnected() && _usb.getLatestState(hid, ageMs) && ageMs < 1500) {
        mapChannels(hid, settings.channelMap, outInput);
        outInput.source    = InputSource::USB_HID;
        outInput.rssiPct   = 99;
        outInput.latencyMs = static_cast<uint16_t>(ageMs > 999 ? 999 : ageMs);
        outInput.isElrsBle = false;
        return;
    }

    // Priority 2: Ultra-low-latency BLE ExpressLRS / HID Gamepad
    if (_ble.isConnected() && _ble.getLatestState(hid, ageMs) && ageMs < 1500) {
        mapChannels(hid, settings.channelMap, outInput);
        outInput.source    = InputSource::BLE_HID;
        outInput.rssiPct   = _ble.getRssiPct();
        outInput.latencyMs = static_cast<uint16_t>(ageMs > 999 ? 999 : ageMs);
        outInput.isElrsBle = _ble.isElrsJoystick();
        return;
    }

    // Priority 3: Cardputer-Adv Internal BMI270 IMU + Keyboard fallback
    _imu.update(dt, keys, settings, outInput);
    _prevArmSwitch = false;
}
