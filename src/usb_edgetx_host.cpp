#include "usb_edgetx_host.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

namespace {

constexpr uint8_t USB_DESC_TYPE_HID        = 0x21;
constexpr uint8_t USB_DESC_TYPE_HID_REPORT = 0x22;
constexpr size_t  USB_INT_BUF_SZ           = 64;
constexpr size_t  USB_CTRL_BUF_SZ          = 512;

struct UsbHostState {
    portMUX_TYPE             lock          = portMUX_INITIALIZER_UNLOCKED;
    usb_host_client_handle_t clientHdl     = nullptr;
    usb_device_handle_t      devHdl        = nullptr;
    usb_transfer_t*          intXfer       = nullptr;
    usb_transfer_t*          ctrlXfer      = nullptr;
    SemaphoreHandle_t        ctrlDoneSem   = nullptr;

    volatile bool            pendingOpen   = false;
    volatile bool            pendingClose  = false;
    volatile uint8_t         pendingAddr   = 0;

    volatile bool            connected     = false;
    volatile bool            isEdgeTx      = false;
    volatile uint32_t        pktCount      = 0;
    uint16_t                 vid           = 0;
    uint16_t                 pid           = 0;
    int                      ifaceNum      = -1;
    uint8_t                  epInAddr      = 0;
    uint16_t                 epInMps       = 0;
    uint16_t                 reportDescLen = 0;

    HidReportLayout          layout;
    HidJoystickState         latestState;
    uint32_t                 lastPacketMs  = 0;
    char                     devLabel[20]  = "";
    char                     statusText[28] = "INIT";
};

static UsbHostState g_usb;

static void setUsbStatus(const char* txt) {
    portENTER_CRITICAL(&g_usb.lock);
    strncpy(g_usb.statusText, txt, sizeof(g_usb.statusText) - 1);
    g_usb.statusText[sizeof(g_usb.statusText) - 1] = '\0';
    portEXIT_CRITICAL(&g_usb.lock);
}

static void ctrlTransferCb(usb_transfer_t* transfer) {
    (void)transfer;
    if (g_usb.ctrlDoneSem) {
        xSemaphoreGive(g_usb.ctrlDoneSem);
    }
}

static void intInTransferCb(usb_transfer_t* transfer) {
    if (!transfer) return;

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        HidJoystickState decoded;
        bool ok = false;
        if (g_usb.isEdgeTx && !g_usb.layout.valid) {
            ok = HidReportParser::decodeEdgeTxRaw(transfer->data_buffer, transfer->actual_num_bytes, decoded);
        } else if (g_usb.layout.valid) {
            ok = HidReportParser::decodeReport(g_usb.layout, transfer->data_buffer, transfer->actual_num_bytes, decoded);
            if (!ok && g_usb.isEdgeTx) {
                ok = HidReportParser::decodeEdgeTxRaw(transfer->data_buffer, transfer->actual_num_bytes, decoded);
            }
        }

        if (ok) {
            uint32_t now = millis();
            portENTER_CRITICAL(&g_usb.lock);
            g_usb.latestState  = decoded;
            g_usb.lastPacketMs = now;
            g_usb.pktCount++;
            portEXIT_CRITICAL(&g_usb.lock);
        }
    }

    if (g_usb.connected && !g_usb.pendingClose &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        usb_host_transfer_submit(transfer);
    }
}

static void sendHidSetIdle(usb_device_handle_t devHdl, int ifaceNum) {
    if (!g_usb.ctrlXfer || !g_usb.ctrlDoneSem) return;
    auto* setup = reinterpret_cast<usb_setup_packet_t*>(g_usb.ctrlXfer->data_buffer);
    setup->bmRequestType = 0x21; // Host-to-Device | Class | Interface
    setup->bRequest      = 0x0A; // HID SET_IDLE
    setup->wValue        = 0x0000;
    setup->wIndex        = static_cast<uint16_t>(ifaceNum);
    setup->wLength       = 0;

    g_usb.ctrlXfer->device_handle    = devHdl;
    g_usb.ctrlXfer->bEndpointAddress = 0;
    g_usb.ctrlXfer->callback         = ctrlTransferCb;
    g_usb.ctrlXfer->context          = nullptr;
    g_usb.ctrlXfer->num_bytes        = sizeof(usb_setup_packet_t);

    xSemaphoreTake(g_usb.ctrlDoneSem, 0);
    if (usb_host_transfer_submit_control(g_usb.clientHdl, g_usb.ctrlXfer) == ESP_OK) {
        xSemaphoreTake(g_usb.ctrlDoneSem, pdMS_TO_TICKS(250));
    }
}

static bool fetchHidReportDescriptor(usb_device_handle_t devHdl, int ifaceNum, uint16_t descLen,
                                     HidReportLayout& outLayout) {
    if (!g_usb.ctrlXfer || !g_usb.ctrlDoneSem || descLen == 0) return false;
    if (descLen > USB_CTRL_BUF_SZ - sizeof(usb_setup_packet_t)) {
        descLen = USB_CTRL_BUF_SZ - sizeof(usb_setup_packet_t);
    }

    auto* setup = reinterpret_cast<usb_setup_packet_t*>(g_usb.ctrlXfer->data_buffer);
    setup->bmRequestType = 0x81; // Device-to-Host | Standard | Interface
    setup->bRequest      = 0x06; // GET_DESCRIPTOR
    setup->wValue        = static_cast<uint16_t>((USB_DESC_TYPE_HID_REPORT << 8) | 0x00);
    setup->wIndex        = static_cast<uint16_t>(ifaceNum);
    setup->wLength       = descLen;

    g_usb.ctrlXfer->device_handle    = devHdl;
    g_usb.ctrlXfer->bEndpointAddress = 0;
    g_usb.ctrlXfer->callback         = ctrlTransferCb;
    g_usb.ctrlXfer->context          = nullptr;
    g_usb.ctrlXfer->num_bytes        = sizeof(usb_setup_packet_t) + descLen;

    xSemaphoreTake(g_usb.ctrlDoneSem, 0);
    if (usb_host_transfer_submit_control(g_usb.clientHdl, g_usb.ctrlXfer) != ESP_OK) {
        return false;
    }
    if (xSemaphoreTake(g_usb.ctrlDoneSem, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    if (g_usb.ctrlXfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        g_usb.ctrlXfer->actual_num_bytes <= sizeof(usb_setup_packet_t)) {
        return false;
    }

    const uint8_t* payload = g_usb.ctrlXfer->data_buffer + sizeof(usb_setup_packet_t);
    size_t payloadLen      = g_usb.ctrlXfer->actual_num_bytes - sizeof(usb_setup_packet_t);
    return HidReportParser::parseDescriptor(payload, payloadLen, outLayout);
}

static bool findHidEndpoint(const usb_config_desc_t* cfgDesc,
                            int& outIface, uint8_t& outEpAddr, uint16_t& outMps,
                            uint16_t& outReportDescLen) {
    outIface = -1;
    outEpAddr = 0;
    outMps = 0;
    outReportDescLen = 0;

    if (!cfgDesc) return false;
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(cfgDesc);
    const uint8_t* end = p + cfgDesc->wTotalLength;

    bool inHidIface = false;
    int  curIface   = -1;
    uint16_t curRptLen = 0;

    while (p + 2 <= end) {
        uint8_t bLen  = p[0];
        uint8_t bType = p[1];
        if (bLen < 2 || p + bLen > end) break;

        if (bType == USB_B_DESCRIPTOR_TYPE_INTERFACE && bLen >= sizeof(usb_intf_desc_t)) {
            const auto* intf = reinterpret_cast<const usb_intf_desc_t*>(p);
            inHidIface = (intf->bInterfaceClass == USB_CLASS_HID);
            curIface   = intf->bInterfaceNumber;
            curRptLen  = 0;
        } else if (inHidIface && bType == USB_DESC_TYPE_HID && bLen >= 9) {
            curRptLen = static_cast<uint16_t>(p[7] | (p[8] << 8));
        } else if (inHidIface && bType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && bLen >= sizeof(usb_ep_desc_t)) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(p);
            bool isIn  = (ep->bEndpointAddress & 0x80) != 0;
            bool isInt = (ep->bmAttributes & 0x03) == 0x03;
            if (isIn && isInt) {
                outIface         = curIface;
                outEpAddr        = ep->bEndpointAddress;
                outMps           = ep->wMaxPacketSize;
                outReportDescLen = curRptLen;
                return true;
            }
        }
        p += bLen;
    }
    return false;
}

static void closeActiveDevice() {
    g_usb.connected = false;
    g_usb.isEdgeTx  = false;
    if (g_usb.devHdl) {
        if (g_usb.ifaceNum >= 0) {
            usb_host_endpoint_halt(g_usb.devHdl, g_usb.epInAddr);
            usb_host_endpoint_flush(g_usb.devHdl, g_usb.epInAddr);
            usb_host_interface_release(g_usb.clientHdl, g_usb.devHdl, g_usb.ifaceNum);
        }
        usb_host_device_close(g_usb.clientHdl, g_usb.devHdl);
        g_usb.devHdl = nullptr;
    }
    g_usb.ifaceNum = -1;
    g_usb.epInAddr = 0;
    g_usb.layout   = HidReportLayout{};
    portENTER_CRITICAL(&g_usb.lock);
    g_usb.latestState.valid = false;
    g_usb.devLabel[0] = '\0';
    portEXIT_CRITICAL(&g_usb.lock);
    setUsbStatus("WAIT (5V OTG)");
}

static void openAttachedDevice(uint8_t devAddr) {
    if (g_usb.devHdl) {
        closeActiveDevice();
    }

    char msg[28];
    setUsbStatus("USB:OPENING");

    usb_device_handle_t hdl = nullptr;
    if (usb_host_device_open(g_usb.clientHdl, devAddr, &hdl) != ESP_OK || !hdl) {
        setUsbStatus("ERR:OPEN");
        return;
    }

    const usb_device_desc_t* devDesc = nullptr;
    const usb_config_desc_t* cfgDesc = nullptr;
    if (usb_host_get_device_descriptor(hdl, &devDesc) != ESP_OK ||
        usb_host_get_active_config_descriptor(hdl, &cfgDesc) != ESP_OK) {
        setUsbStatus("ERR:DESC");
        usb_host_device_close(g_usb.clientHdl, hdl);
        return;
    }

    int iface = -1;
    uint8_t epAddr = 0;
    uint16_t mps = 0;
    uint16_t rptDescLen = 0;
    if (!findHidEndpoint(cfgDesc, iface, epAddr, mps, rptDescLen)) {
        snprintf(msg, sizeof(msg), "NO HID %04X", devDesc->idVendor);
        setUsbStatus(msg);
        usb_host_device_close(g_usb.clientHdl, hdl);
        return;
    }

    if (usb_host_interface_claim(g_usb.clientHdl, hdl, iface, 0) != ESP_OK) {
        setUsbStatus("ERR:CLAIM");
        usb_host_device_close(g_usb.clientHdl, hdl);
        return;
    }

    g_usb.devHdl        = hdl;
    g_usb.vid           = devDesc->idVendor;
    g_usb.pid           = devDesc->idProduct;
    g_usb.isEdgeTx      = (g_usb.vid == EDGETX_USB_VID && g_usb.pid == EDGETX_USB_PID);
    g_usb.ifaceNum      = iface;
    g_usb.epInAddr      = epAddr;
    g_usb.epInMps       = (mps > USB_INT_BUF_SZ || mps == 0) ? USB_INT_BUF_SZ : mps;
    g_usb.reportDescLen = rptDescLen;
    g_usb.layout        = HidReportLayout{};
    g_usb.pktCount      = 0;

    // Standard HID class SET_IDLE(0,0) before polling Interrupt IN
    sendHidSetIdle(hdl, iface);

    bool parsed = false;
    if (rptDescLen > 0) {
        parsed = fetchHidReportDescriptor(hdl, iface, rptDescLen, g_usb.layout);
    }
    if (!parsed && g_usb.isEdgeTx) {
        HidReportParser::makeEdgeTxLayout(g_usb.layout);
    }

    portENTER_CRITICAL(&g_usb.lock);
    if (g_usb.isEdgeTx) {
        strncpy(g_usb.devLabel, "EDGETX USB", sizeof(g_usb.devLabel) - 1);
    } else {
        snprintf(g_usb.devLabel, sizeof(g_usb.devLabel), "HID %04X:%04X", g_usb.vid, g_usb.pid);
    }
    g_usb.devLabel[sizeof(g_usb.devLabel) - 1] = '\0';
    portEXIT_CRITICAL(&g_usb.lock);

    g_usb.connected = true;
    g_usb.intXfer->device_handle    = hdl;
    g_usb.intXfer->bEndpointAddress = epAddr;
    g_usb.intXfer->callback         = intInTransferCb;
    g_usb.intXfer->context          = nullptr;
    g_usb.intXfer->num_bytes        = g_usb.epInMps;

    if (usb_host_transfer_submit(g_usb.intXfer) != ESP_OK) {
        setUsbStatus("ERR:XFER");
        closeActiveDevice();
        return;
    }

    snprintf(msg, sizeof(msg), "OK:%04X:%04X", g_usb.vid, g_usb.pid);
    setUsbStatus(msg);
}

static void clientEventCb(const usb_host_client_event_msg_t* eventMsg, void* arg) {
    (void)arg;
    if (!eventMsg) return;
    if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        g_usb.pendingAddr = eventMsg->new_dev.address;
        g_usb.pendingOpen = true;
    } else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        g_usb.pendingClose = true;
    }
}

static void usbHostDaemonTask(void* arg) {
    (void)arg;
    while (true) {
        uint32_t eventFlags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
        if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usbHostClientTask(void* arg) {
    (void)arg;
    while (true) {
        usb_host_client_handle_events(g_usb.clientHdl, pdMS_TO_TICKS(20));

        if (g_usb.pendingClose) {
            g_usb.pendingClose = false;
            closeActiveDevice();
        }
        if (g_usb.pendingOpen) {
            uint8_t addr = g_usb.pendingAddr;
            g_usb.pendingOpen = false;
            openAttachedDevice(addr);
        }
    }
}

} // namespace

bool UsbEdgeTxHost::begin() {
    usb_host_config_t hostCfg = {};
    hostCfg.skip_phy_setup = false;
    hostCfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;

    esp_err_t err = usb_host_install(&hostCfg);
    if (err != ESP_OK) {
        char msg[28];
        snprintf(msg, sizeof(msg), "HOST ERR %d", static_cast<int>(err));
        setUsbStatus(msg);
        return false;
    }

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous    = false;
    clientCfg.max_num_event_msg = 5;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg          = nullptr;

    if (usb_host_client_register(&clientCfg, &g_usb.clientHdl) != ESP_OK) {
        setUsbStatus("CLIENT ERR");
        return false;
    }

    if (usb_host_transfer_alloc(USB_INT_BUF_SZ, 0, &g_usb.intXfer) != ESP_OK ||
        usb_host_transfer_alloc(USB_CTRL_BUF_SZ, 0, &g_usb.ctrlXfer) != ESP_OK) {
        setUsbStatus("ALLOC ERR");
        return false;
    }
    g_usb.ctrlDoneSem = xSemaphoreCreateBinary();

    setUsbStatus("WAIT (5V OTG)");
    xTaskCreatePinnedToCore(usbHostDaemonTask, "usb_daemon", 3072, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(usbHostClientTask, "usb_client", 4096, nullptr, 5, nullptr, 0);
    return true;
}

bool UsbEdgeTxHost::isConnected() const {
    return g_usb.connected;
}

bool UsbEdgeTxHost::isEdgeTxRadio() const {
    return g_usb.connected && g_usb.isEdgeTx;
}

uint32_t UsbEdgeTxHost::getPacketCount() const {
    return g_usb.pktCount;
}

bool UsbEdgeTxHost::getLatestState(HidJoystickState& outState, uint32_t& outAgeMs) const {
    if (!g_usb.connected) return false;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_usb.lock);
    outState = g_usb.latestState;
    outAgeMs = now - g_usb.lastPacketMs;
    portEXIT_CRITICAL(&g_usb.lock);
    return outState.valid;
}

void UsbEdgeTxHost::getDeviceLabel(char* buf, size_t bufLen) const {
    if (!buf || bufLen == 0) return;
    portENTER_CRITICAL(&g_usb.lock);
    strncpy(buf, g_usb.devLabel, bufLen - 1);
    buf[bufLen - 1] = '\0';
    portEXIT_CRITICAL(&g_usb.lock);
}

void UsbEdgeTxHost::getStatusText(char* buf, size_t bufLen) const {
    if (!buf || bufLen == 0) return;
    portENTER_CRITICAL(&g_usb.lock);
    strncpy(buf, g_usb.statusText, bufLen - 1);
    buf[bufLen - 1] = '\0';
    portEXIT_CRITICAL(&g_usb.lock);
}

#else
bool UsbEdgeTxHost::begin() { return false; }
bool UsbEdgeTxHost::isConnected() const { return false; }
bool UsbEdgeTxHost::isEdgeTxRadio() const { return false; }
uint32_t UsbEdgeTxHost::getPacketCount() const { return 0; }
bool UsbEdgeTxHost::getLatestState(HidJoystickState&, uint32_t&) const { return false; }
void UsbEdgeTxHost::getDeviceLabel(char* buf, size_t bufLen) const { if (buf && bufLen) buf[0] = '\0'; }
void UsbEdgeTxHost::getStatusText(char* buf, size_t bufLen) const { if (buf && bufLen) strcpy(buf, "OFF"); }
#endif
