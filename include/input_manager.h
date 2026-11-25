#pragma once

#include "config.h"
#include "usb_edgetx_host.h"
#include "ble_hid_host.h"
#include "imu_controller.h"

// ============================================================================
// Unified Tri-Mode Input Arbiter
// Automatic seamless priority switching:
//   1. USB-OTG EdgeTX / HID Radio Controller (highest priority, ~1-4ms)
//   2. BLE ExpressLRS / HID Gamepad (low-latency 7.5-11ms NimBLE)
//   3. Internal Cardputer-Adv BMI270 IMU + Arrow-Key fallback (instant)
// ============================================================================

class InputManager {
public:
    void begin();
    void update(float dt, const KeyFlightState& keys, const SimSettings& settings,
                bool inActiveFlight, PilotInput& outInput);

    void calibrateImu()            { _imu.calibrateNeutral(); }
    void startBleScan()            { _ble.startScan(10); }
    void stopBleScan()             { _ble.stopScan(); }
    void disconnectBle()           { _ble.disconnect(); }
    void clearBleBonds()           { _ble.clearBonds(); }

    bool isUsbConnected() const    { return _usb.isConnected(); }
    bool isBleConnected() const    { return _ble.isConnected(); }
    bool isBleScanning() const     { return _ble.isScanning(); }
    bool isImuPresent() const      { return _imu.isPresent(); }
    float imuRollDeg() const       { return _imu.rollDeg(); }
    float imuPitchDeg() const      { return _imu.pitchDeg(); }
    uint32_t usbPacketCount() const { return _usb.getPacketCount(); }
    uint32_t blePacketCount() const { return _ble.getPacketCount(); }

    void getUsbLabel(char* buf, size_t len) const  { _usb.getDeviceLabel(buf, len); }
    void getBleLabel(char* buf, size_t len) const  { _ble.getPeerName(buf, len); }
    void getUsbStatus(char* buf, size_t len) const { _usb.getStatusText(buf, len); }
    void getBleStatus(char* buf, size_t len) const { _ble.getStatusText(buf, len); }

private:
    void mapChannels(const HidJoystickState& hid, ChannelMap mapMode, PilotInput& out);

    UsbEdgeTxHost _usb;
    BleHidHost    _ble;
    ImuController _imu;
    bool          _prevArmSwitch = false;
};
