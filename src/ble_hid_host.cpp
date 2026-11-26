#include "ble_hid_host.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const NimBLEUUID UUID_HID_SERVICE(static_cast<uint16_t>(0x1812));
static const NimBLEUUID UUID_HID_REPORT_MAP(static_cast<uint16_t>(0x2A4B));
static const NimBLEUUID UUID_HID_REPORT(static_cast<uint16_t>(0x2A4D));
static const NimBLEUUID UUID_HID_PROTO_MODE(static_cast<uint16_t>(0x2A4E));
static const NimBLEUUID UUID_HID_CTRL_POINT(static_cast<uint16_t>(0x2A4C));

struct BleHostState {
    portMUX_TYPE            lock           = portMUX_INITIALIZER_UNLOCKED;
    NimBLEClient*           client         = nullptr;
    NimBLEAdvertisedDevice* pendingDev     = nullptr;

    volatile bool           initialized    = false;
    volatile bool           scanning       = false;
    volatile bool           connecting     = false;
    volatile bool           connected      = false;
    volatile bool           isElrs         = false;
    volatile bool           wantConnect    = false;
    volatile int            rssiDbm        = -55;
    volatile uint32_t       pktCount       = 0;

    HidReportLayout         layout;
    HidJoystickState        latestState;
    uint32_t                lastPacketMs   = 0;
    uint32_t                lastRssiPollMs = 0;
    char                    peerName[20]   = "";
    char                    statusText[28] = "IDLE";
};

static BleHostState g_ble;

static void setStatus(const char* txt) {
    portENTER_CRITICAL(&g_ble.lock);
    strncpy(g_ble.statusText, txt, sizeof(g_ble.statusText) - 1);
    g_ble.statusText[sizeof(g_ble.statusText) - 1] = '\0';
    portEXIT_CRITICAL(&g_ble.lock);
}

static std::string toUpper(const std::string& s) {
    std::string up = s;
    for (char& c : up) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return up;
}

static bool nameIsElrs(const std::string& name) {
    std::string up = toUpper(name);
    return (up.find("ELRS") != std::string::npos ||
            up.find("EXPRESSLRS") != std::string::npos);
}

static bool nameLooksLikeController(const std::string& name) {
    if (name.empty()) return false;
    std::string up = toUpper(name);
    return (up.find("ELRS") != std::string::npos ||
            up.find("EXPRESSLRS") != std::string::npos ||
            up.find("EDGETX") != std::string::npos ||
            up.find("JOYSTICK") != std::string::npos ||
            up.find("GAMEPAD") != std::string::npos ||
            up.find("CONTROLLER") != std::string::npos ||
            up.find("XBOX") != std::string::npos ||
            up.find("8BITDO") != std::string::npos);
}

static void hidReportNotifyCb(NimBLERemoteCharacteristic* chr,
                              uint8_t* data, size_t length, bool isNotify) {
    (void)chr;
    (void)isNotify;
    if (!data || length == 0) return;

    HidJoystickState decoded;
    bool ok = HidReportParser::decodeReport(g_ble.layout, data, length, decoded);
    if (!ok && g_ble.layout.hasReportId && length >= 16) {
        // Fallback: some BLE stacks strip the Report ID byte from 0x2A4D notifications!
        HidReportLayout noRid = g_ble.layout;
        noRid.hasReportId = false;
        ok = HidReportParser::decodeReport(noRid, data, length, decoded);
        if (ok) {
            g_ble.layout.hasReportId = false;
        }
    }
    if (ok) {
        uint32_t now = millis();
        portENTER_CRITICAL(&g_ble.lock);
        g_ble.latestState  = decoded;
        g_ble.lastPacketMs = now;
        g_ble.pktCount++;
        portEXIT_CRITICAL(&g_ble.lock);
    }
}

class BleClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        (void)client;
        // Do NOT force aggressive conn params here; wait until GATT discovery &
        // pairing complete cleanly so slow discovery never triggers a drop!
    }

    void onDisconnect(NimBLEClient* client) override {
        (void)client;
        g_ble.connected = false;
        g_ble.isElrs    = false;
        portENTER_CRITICAL(&g_ble.lock);
        g_ble.latestState.valid = false;
        g_ble.peerName[0] = '\0';
        portEXIT_CRITICAL(&g_ble.lock);
        if (!g_ble.connecting) {
            setStatus("DISCONNECTED");
        }
    }

    bool onConnParamsUpdateRequest(NimBLEClient* client, const ble_gap_upd_params* params) override {
        (void)client;
        (void)params;
        // Always accept peer's requested connection parameter update so ELRS /
        // gamepads never disconnect us over rejected parameters!
        return true;
    }

    uint32_t onPassKeyRequest() override { return 0; }
    bool onConfirmPIN(uint32_t pass_key) override { (void)pass_key; return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (desc && !desc->sec_state.encrypted) {
            // Do not force disconnect — some controllers work unencrypted
        }
    }
};

class BleScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev || g_ble.connected || g_ble.connecting || g_ble.wantConnect) return;

        std::string nm = dev->getName();
        bool hasHidSvc = dev->isAdvertisingService(UUID_HID_SERVICE);
        bool looksCtrl = nameLooksLikeController(nm);

        // Require either a controller-like name OR HID service with Joystick/Gamepad appearance
        // (0x03C3 = Joystick, 0x03C4 = Gamepad) so we don't accidentally grab nearby mice/keyboards!
        uint16_t app = dev->haveAppearance() ? dev->getAppearance() : 0;
        bool hidCtrlApp = (app == 0x03C3 || app == 0x03C4 || app == 0x03C0);

        if (!looksCtrl && !(hasHidSvc && (hidCtrlApp || !nm.empty()))) {
            return;
        }

        NimBLEDevice::getScan()->stop();
        g_ble.scanning = false;

        if (g_ble.pendingDev) {
            delete g_ble.pendingDev;
            g_ble.pendingDev = nullptr;
        }
        g_ble.pendingDev  = new NimBLEAdvertisedDevice(*dev);
        g_ble.wantConnect = true;
    }
};

static BleClientCallbacks g_clientCbs;
static BleScanCallbacks   g_scanCbs;

static bool connectToPendingDevice() {
    NimBLEAdvertisedDevice* dev = g_ble.pendingDev;
    g_ble.pendingDev = nullptr;
    if (!dev) return false;

    g_ble.connecting = true;
    std::string devName = dev->getName();
    bool elrsFlag = nameIsElrs(devName);

    char msg[28];
    setStatus("CONNECTING...");

    if (!g_ble.client) {
        g_ble.client = NimBLEDevice::createClient();
        g_ble.client->setClientCallbacks(&g_clientCbs, false);
        g_ble.client->setConnectTimeout(6);
    }

    // Step 1: Connect with generous, reliable discovery params (15-30ms, 6s timeout)
    g_ble.client->setConnectionParams(12, 24, 0, 600);

    bool ok = g_ble.client->connect(dev, true);
    if (!ok) {
        // Retry once after clearing stale bonds in case of LTK mismatch
        NimBLEDevice::deleteAllBonds();
        vTaskDelay(pdMS_TO_TICKS(150));
        ok = g_ble.client->connect(dev, true);
    }
    delete dev;

    if (!ok || !g_ble.client->isConnected()) {
        setStatus("CONN ERR");
        g_ble.connecting = false;
        return false;
    }

    // Small settle delay for peer BLE server (especially ESP32-BLE-Gamepad / ELRS)
    vTaskDelay(pdMS_TO_TICKS(120));

    // Step 2: Optional security elevation (non-fatal if peer doesn't mandate pairing)
    setStatus("PAIRING...");
    if (!g_ble.client->secureConnection()) {
        // Some peers fail on stale bond — clear bonds and continue gracefully if still connected
        if (!g_ble.client->isConnected()) {
            NimBLEDevice::deleteAllBonds();
            setStatus("PAIR ERR");
            g_ble.connecting = false;
            return false;
        }
    }

    // Step 3: Discover HID Service (0x1812)
    setStatus("DISCOVERING...");
    NimBLERemoteService* hidSvc = g_ble.client->getService(UUID_HID_SERVICE);
    if (!hidSvc) {
        setStatus("NO HID ERR");
        g_ble.client->disconnect();
        g_ble.connecting = false;
        return false;
    }

    // Optional: Report Protocol Mode (1) & Exit Suspend (1)
    NimBLERemoteCharacteristic* protoChr = hidSvc->getCharacteristic(UUID_HID_PROTO_MODE);
    if (protoChr && protoChr->canWriteNoResponse()) {
        uint8_t reportMode = 1;
        protoChr->writeValue(&reportMode, 1, false);
    }
    NimBLERemoteCharacteristic* ctrlChr = hidSvc->getCharacteristic(UUID_HID_CTRL_POINT);
    if (ctrlChr && ctrlChr->canWriteNoResponse()) {
        uint8_t exitSuspend = 1;
        ctrlChr->writeValue(&exitSuspend, 1, false);
    }

    // Step 4: Parse HID Report Map (0x2A4B)
    setStatus("READING MAP...");
    HidReportLayout parsedLayout;
    bool gotDesc = false;
    NimBLERemoteCharacteristic* mapChr = hidSvc->getCharacteristic(UUID_HID_REPORT_MAP);
    if (mapChr && mapChr->canRead()) {
        std::string mapData = mapChr->readValue();
        if (!mapData.empty()) {
            gotDesc = HidReportParser::parseDescriptor(
                reinterpret_cast<const uint8_t*>(mapData.data()),
                mapData.size(),
                parsedLayout);
            // NOTE: Over BLE GATT, each Report ID is exposed on its own 0x2A4D characteristic,
            // so notifications on 0x2A4D do NOT include a leading Report ID byte!
            if (gotDesc) {
                parsedLayout.hasReportId = false;
            }
        }
    }

    if (gotDesc) {
        parsedLayout.isElrsOrder = elrsFlag;
        g_ble.layout = parsedLayout;
    } else {
        HidReportParser::makeElrsBleLayout(g_ble.layout);
        g_ble.layout.isElrsOrder = elrsFlag;
    }

    // Step 5: Subscribe to all notify-capable 0x2A4D Report characteristics
    setStatus("SUBSCRIBING...");
    int subscribed = 0;
    const auto* chars = hidSvc->getCharacteristics(true);
    if (chars) {
        for (auto* chr : *chars) {
            if (chr->getUUID() == UUID_HID_REPORT && chr->canNotify()) {
                if (chr->subscribe(true, hidReportNotifyCb, true) ||
                    chr->subscribe(true, hidReportNotifyCb, false)) {
                    ++subscribed;
                }
            }
        }
    }

    if (subscribed == 0 || !g_ble.client->isConnected()) {
        setStatus("SETUP ERR");
        if (g_ble.client->isConnected()) g_ble.client->disconnect();
        g_ble.connecting = false;
        return false;
    }

    // Step 6: NOW switch to ultra-low-latency interval (7.5ms - 11.25ms, latency=0, 4s timeout)
    g_ble.client->updateConnParams(6, 9, 0, 400);

    portENTER_CRITICAL(&g_ble.lock);
    g_ble.isElrs   = elrsFlag;
    g_ble.pktCount = 0;
    if (!devName.empty()) {
        strncpy(g_ble.peerName, devName.c_str(), sizeof(g_ble.peerName) - 1);
    } else {
        strncpy(g_ble.peerName, "BLE HID", sizeof(g_ble.peerName) - 1);
    }
    g_ble.peerName[sizeof(g_ble.peerName) - 1] = '\0';
    g_ble.connected = true;
    portEXIT_CRITICAL(&g_ble.lock);

    snprintf(msg, sizeof(msg), "%s", g_ble.peerName);
    setStatus(msg);
    g_ble.connecting = false;
    return true;
}

static void bleConnectTask(void* arg) {
    (void)arg;
    while (true) {
        if (g_ble.wantConnect && !g_ble.connected) {
            g_ble.wantConnect = false;
            connectToPendingDevice();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

} // namespace

bool BleHidHost::begin() {
    if (g_ble.initialized) return true;

    NimBLEDevice::init("StickADV");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    // Bonding=true, MITM=false, SC=true ("Just Works" HID-over-GATT)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&g_scanCbs, false);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);

    g_ble.initialized = true;
    setStatus("READY");
    xTaskCreatePinnedToCore(bleConnectTask, "ble_hid_conn", 5120, nullptr, 3, nullptr, 0);
    return true;
}

void BleHidHost::startScan(uint32_t durationSec) {
    if (!g_ble.initialized || g_ble.connected || g_ble.connecting) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) return;
    g_ble.scanning = true;
    setStatus("SCANNING...");
    scan->start(durationSec, [](NimBLEScanResults) {
        g_ble.scanning = false;
        if (!g_ble.connected && !g_ble.connecting && !g_ble.wantConnect) {
            setStatus("NONE FOUND");
        }
    }, false);
}

void BleHidHost::stopScan() {
    if (!g_ble.initialized) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
        scan->stop();
    }
    g_ble.scanning = false;
}

void BleHidHost::disconnect() {
    stopScan();
    if (g_ble.client && g_ble.client->isConnected()) {
        g_ble.client->disconnect();
    }
    g_ble.connected = false;
    g_ble.isElrs    = false;
    setStatus("IDLE");
}

void BleHidHost::clearBonds() {
    disconnect();
    NimBLEDevice::deleteAllBonds();
    setStatus("UNPAIRED");
}

void BleHidHost::poll(bool allowBackgroundReconnect) {
    (void)allowBackgroundReconnect;
    if (!g_ble.initialized) return;

    if (g_ble.connected && g_ble.client) {
        uint32_t now = millis();
        if (now - g_ble.lastRssiPollMs > 1000) {
            g_ble.lastRssiPollMs = now;
            g_ble.rssiDbm = g_ble.client->getRssi();
        }
    }
}

bool BleHidHost::isConnected() const {
    return g_ble.connected;
}

bool BleHidHost::isScanning() const {
    return g_ble.scanning || g_ble.connecting;
}

bool BleHidHost::isElrsJoystick() const {
    return g_ble.connected && g_ble.isElrs;
}

int BleHidHost::getRssiPct() const {
    if (!g_ble.connected) return 0;
    int rssi = g_ble.rssiDbm;
    int pct = 20 + ((rssi + 95) * 79) / 55;
    if (pct < 15) pct = 15;
    if (pct > 99) pct = 99;
    return pct;
}

uint32_t BleHidHost::getPacketCount() const {
    return g_ble.pktCount;
}

bool BleHidHost::getLatestState(HidJoystickState& outState, uint32_t& outAgeMs) const {
    if (!g_ble.connected) return false;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_ble.lock);
    outState = g_ble.latestState;
    outAgeMs = now - g_ble.lastPacketMs;
    portEXIT_CRITICAL(&g_ble.lock);
    return outState.valid;
}

void BleHidHost::getPeerName(char* buf, size_t bufLen) const {
    if (!buf || bufLen == 0) return;
    portENTER_CRITICAL(&g_ble.lock);
    strncpy(buf, g_ble.peerName, bufLen - 1);
    buf[bufLen - 1] = '\0';
    portEXIT_CRITICAL(&g_ble.lock);
}

void BleHidHost::getStatusText(char* buf, size_t bufLen) const {
    if (!buf || bufLen == 0) return;
    portENTER_CRITICAL(&g_ble.lock);
    strncpy(buf, g_ble.statusText, bufLen - 1);
    buf[bufLen - 1] = '\0';
    portEXIT_CRITICAL(&g_ble.lock);
}

#else
bool BleHidHost::begin() { return false; }
void BleHidHost::startScan(uint32_t) {}
void BleHidHost::stopScan() {}
void BleHidHost::disconnect() {}
void BleHidHost::clearBonds() {}
void BleHidHost::poll(bool) {}
bool BleHidHost::isConnected() const { return false; }
bool BleHidHost::isScanning() const { return false; }
bool BleHidHost::isElrsJoystick() const { return false; }
int  BleHidHost::getRssiPct() const { return 0; }
uint32_t BleHidHost::getPacketCount() const { return 0; }
bool BleHidHost::getLatestState(HidJoystickState&, uint32_t&) const { return false; }
void BleHidHost::getPeerName(char* buf, size_t bufLen) const { if (buf && bufLen) buf[0] = '\0'; }
void BleHidHost::getStatusText(char* buf, size_t bufLen) const { if (buf && bufLen) strcpy(buf, "OFF"); }
#endif
