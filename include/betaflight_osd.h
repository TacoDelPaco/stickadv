#pragma once

#include "config.h"
#include "sim_engine.h"
#include <stdint.h>

// ============================================================================
// Authentic Betaflight Analog OSD (MAX7456 Style) Renderer
// Renders crisp outlined white-on-black-halo characters, MAX7456 icons,
// dynamic tilting Artificial Horizon, center crosshair, telemetry, and
// Betaflight-style Post-Flight Stats / Menu / Stick Monitor screens.
// ============================================================================

enum class OsdIcon : uint8_t {
    ANTENNA = 0,
    CLOCK,
    GATE,
    BATTERY_0,
    BATTERY_1,
    BATTERY_2,
    BATTERY_3,
    BATTERY_4,
    BATTERY_5,
    CROSSHAIR_L,
    CROSSHAIR_C,
    CROSSHAIR_R,
};

struct UiMenuState {
    int      selectedItem    = 0;
    bool     bleScanning     = false;
    bool     bleConnected    = false;
    bool     usbConnected    = false;
    char     blePeerName[20] = "";
    char     usbDevName[20]  = "";
    char     bleStatus[28]   = "";
    char     usbStatus[28]   = "";
    uint32_t usbPkts         = 0;
    uint32_t blePkts         = 0;
    float    imuRollDeg      = 0.0f;
    float    imuPitchDeg     = 0.0f;
    bool     imuPresent      = true;
    int      realBatPct      = 100;
};

class BetaflightOsd {
public:
    // Draw live in-flight Betaflight Analog OSD over the 3D framebuffer
    static void drawFlightOsd(uint16_t* fb,
                              const SimEngine& sim,
                              const PilotInput& input,
                              const SimSettings& settings,
                              uint32_t frameCount);

    // Draw Title / Disarmed FPV goggle standby overlay
    static void drawTitleOverlay(uint16_t* fb,
                                 const SimEngine& sim,
                                 const PilotInput& input,
                                 const SimSettings& settings,
                                 const UiMenuState& ui,
                                 uint32_t frameCount);

    // Draw Betaflight-style Post-Flight Stats Summary after 30s run
    static void drawPostFlightStats(uint16_t* fb,
                                    const SimEngine& sim,
                                    const PilotInput& input,
                                    const SimSettings& settings,
                                    uint32_t frameCount);

    // Draw Betaflight OSD-style Settings Menu (CMS menu!)
    static void drawSettingsMenu(uint16_t* fb,
                                 const SimSettings& settings,
                                 const UiMenuState& ui);

    // Draw Live 8-Channel USB/BLE Stick & BMI270 IMU Monitor
    static void drawStickMonitor(uint16_t* fb,
                                 const PilotInput& input,
                                 const SimSettings& settings,
                                 const UiMenuState& ui);

    // Low-level MAX7456-style outlined text & icon drawing helpers
    static void drawTextOutlined(uint16_t* fb, int x, int y, const char* text,
                                 uint16_t fgColor, int scale = 1);
    static void drawTextCentered(uint16_t* fb, int cx, int y, const char* text,
                                 uint16_t fgColor, int scale = 1);
    static void drawIconOutlined(uint16_t* fb, int x, int y, OsdIcon icon,
                                 uint16_t fgColor);
    static void fillRectAlpha(uint16_t* fb, int x, int y, int w, int h,
                              uint16_t color, uint8_t alpha);
    static void drawRect(uint16_t* fb, int x, int y, int w, int h, uint16_t color);

private:
    static void putPixel(uint16_t* fb, int x, int y, uint16_t color);
    static void drawGlyphOutlined(uint16_t* fb, int x, int y, const uint8_t rows[7],
                                  int width, uint16_t fgColor, int scale);
    static void drawArtificialHorizon(uint16_t* fb, float rollRad, float pitchRad,
                                      int throttlePct, float altM);
    static void drawLineOutlined(uint16_t* fb, int x0, int y0, int x1, int y1,
                                 uint16_t fgColor);
    static const uint8_t* getFontGlyph(char c);
};
