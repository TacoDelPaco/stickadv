#pragma once

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// StickADV — 3D FPV Racing Simulator (EdgeTX Game-Simulator Port)
// Target: M5Stack Cardputer-Adv (Stamp-S3A / ESP32-S3FN8, ST7789V2 240x135)
// ============================================================================

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int TARGET_FPS = 60;
constexpr float TARGET_DT = 1.0f / static_cast<float>(TARGET_FPS);

// EdgeTX USB HID Joystick Identifiers (https://manual.edgetx.org)
constexpr uint16_t EDGETX_USB_VID = 0x1209;
constexpr uint16_t EDGETX_USB_PID = 0x4F54;

// Color endianness helper:
// M5Canvas (LovyanGFX 16-bit sprite) stores pixels in big-endian RGB565
// (swap565_t) so SPI DMA pushes directly to ST7789V2 with zero CPU swap.
#ifndef FB_SWAP_BYTES
  #ifdef ARDUINO
    #define FB_SWAP_BYTES 1
  #else
    #define FB_SWAP_BYTES 0
  #endif
#endif

static inline constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
#if FB_SWAP_BYTES
    return static_cast<uint16_t>((c >> 8) | (c << 8));
#else
    return c;
#endif
}

static inline constexpr uint16_t unpack_rgb565_raw(uint16_t px) {
#if FB_SWAP_BYTES
    return static_cast<uint16_t>((px >> 8) | (px << 8));
#else
    return px;
#endif
}

static inline constexpr uint16_t pack_rgb565_raw(uint16_t raw) {
#if FB_SWAP_BYTES
    return static_cast<uint16_t>((raw >> 8) | (raw << 8));
#else
    return raw;
#endif
}

// Fast 50% darken / scanline blend in native framebuffer endianness
static inline uint16_t darken565(uint16_t px, uint8_t shift = 1) {
    uint16_t raw = unpack_rgb565_raw(px);
    uint16_t r = ((raw >> 11) & 0x1F) >> shift;
    uint16_t g = ((raw >> 5)  & 0x3F) >> shift;
    uint16_t b = (raw & 0x1F) >> shift;
    return pack_rgb565_raw(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

// Dim pixel by ~20% for subtle analog CRT scanlines
static inline uint16_t scanlineDim565(uint16_t px) {
    uint16_t raw = unpack_rgb565_raw(px);
    uint16_t r = ((raw >> 11) & 0x1F);
    uint16_t g = ((raw >> 5)  & 0x3F);
    uint16_t b = (raw & 0x1F);
    r = (r * 27) >> 5;
    g = (g * 27) >> 5;
    b = (b * 27) >> 5;
    return pack_rgb565_raw(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha_0_255) {
    uint16_t b_raw = unpack_rgb565_raw(bg);
    uint16_t f_raw = unpack_rgb565_raw(fg);
    uint16_t inv = 255 - alpha_0_255;
    uint16_t r = ((((b_raw >> 11) & 0x1F) * inv) + (((f_raw >> 11) & 0x1F) * alpha_0_255)) >> 8;
    uint16_t g = ((((b_raw >> 5)  & 0x3F) * inv) + (((f_raw >> 5)  & 0x3F) * alpha_0_255)) >> 8;
    uint16_t b = (((b_raw & 0x1F) * inv) + ((f_raw & 0x1F) * alpha_0_255)) >> 8;
    return pack_rgb565_raw(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

// ============================================================================
// Game & Input Enums / Settings
// ============================================================================

enum class InputSource : uint8_t {
    IMU_KB = 0,
    BLE_HID = 1,
    USB_HID = 2,
};

enum class ChannelMap : uint8_t {
    AETR = 0,   // EdgeTX / Betaflight standard: CH1=Roll, CH2=Pitch, CH3=Thr, CH4=Yaw
    TAER = 1,   // Spektrum / JR: CH1=Thr, CH2=Roll, CH3=Pitch, CH4=Yaw
    AERT = 2,   // Futaba / Mode variant: CH1=Roll, CH2=Pitch, CH3=Yaw, CH4=Thr
    COUNT = 3
};

enum class RateProfile : uint8_t {
    RATE_LOW  = 0,
    RATE_MID  = 1,
    RATE_HIGH = 2,
    COUNT     = 3
};

enum class GameMode : uint8_t {
    RACE_30S = 0,   // Classic EdgeTX Game-Simulator 30s timed race
    ENDLESS  = 1,   // Endless practice / streak mode
    COUNT    = 2
};

enum class OsdBatterySource : uint8_t {
    SIM_4S   = 0,   // Simulated 4S LiPo with realistic throttle voltage sag
    REAL_BAT = 1,   // Real Cardputer-Adv battery voltage from GPIO10 ADC
    COUNT    = 2
};

enum class ImuMode : uint8_t {
    TILT_ALT = 0,   // Intuitive handheld: tilt L/R = X, tilt Up/Down = Altitude Y, auto-cruise
    TILT_SPD = 1,   // Classic: tilt L/R = X, tilt Fwd/Back = Speed
    COUNT    = 2
};

struct SimSettings {
    GameMode         gameMode     = GameMode::RACE_30S;
    ChannelMap       channelMap   = ChannelMap::AETR;
    RateProfile      rates        = RateProfile::RATE_LOW;
    bool             camTilt      = true;   // Dynamic roll/pitch FPV camera horizon
    bool             analogFx     = true;   // Subtle CRT scanlines + RF static bursts
    uint8_t          volumePct    = 20;     // 0..100% (0 = mute), gentle default for ES8311
    bool             imuEnabled   = true;   // Toggle BMI270 tilt input (OFF = pure Arrow keys)
    ImuMode          imuMode      = ImuMode::TILT_ALT;
    bool             imuInvertY   = false;  // Flip tilt up/down direction if preferred
    bool             imuInvertX   = false;  // Flip tilt left/right direction if preferred
    OsdBatterySource osdBattery   = OsdBatterySource::SIM_4S;
};

// Normalized pilot control inputs for one simulation frame
struct PilotInput {
    float       roll      = 0.0f;    // [-1.0 .. +1.0] (left .. right)
    float       pitch     = 0.0f;    // [-1.0 .. +1.0] (back/slow .. forward/fast)
    float       throttle  = 0.0f;    // [-1.0 .. +1.0] (down/zero .. up/full)
    float       yaw       = 0.0f;    // [-1.0 .. +1.0] (left .. right)
    bool        armSwitch = false;   // AUX1 / CH5 / Button 1 arm signal
    bool        armEdge   = false;   // Rising edge on arm switch
    float       rawCh[8]  = {0};     // Raw normalized [-1..+1] channels 1..8 for monitor
    uint32_t    buttons   = 0;       // Up to 24 digital buttons
    InputSource source    = InputSource::IMU_KB;
    int         rssiPct   = 99;      // Link quality 0..99
    uint16_t    latencyMs = 0;       // Input update age in ms
    bool        isElrsBle = false;   // Connected via ELRS BLE Joystick
};
