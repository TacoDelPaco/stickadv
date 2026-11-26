#include "betaflight_osd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {

// MAX7456-inspired color palette (mostly crisp analog white + amber/red/green accents)
constexpr uint16_t OSD_WHITE  = rgb565(248, 250, 255);
constexpr uint16_t OSD_BLACK  = rgb565(0, 0, 0);
constexpr uint16_t OSD_AMBER  = rgb565(255, 205, 50);
constexpr uint16_t OSD_RED    = rgb565(255, 72, 72);
constexpr uint16_t OSD_GREEN  = rgb565(90, 255, 115);
constexpr uint16_t OSD_CYAN   = rgb565(90, 235, 255);
constexpr uint16_t OSD_DIM    = rgb565(190, 200, 210);
constexpr uint16_t OSD_PANEL  = rgb565(8, 12, 18);

// Crisp 5x7 MAX7456-style bold uppercase + digits + symbols bitmap font
// Each glyph is 7 rows, MSB in bit 4 (5 pixels wide: bits 4..0)
struct GlyphEntry {
    char    ch;
    uint8_t rows[7];
};

static const GlyphEntry kFontTable[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x0E,0x10,0x1E,0x11,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},

    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},

    {':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'%', {0x19,0x19,0x02,0x04,0x08,0x13,0x13}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'?', {0x0E,0x11,0x02,0x04,0x04,0x00,0x04}},
    {'[', {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']', {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'=', {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    {'*', {0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00}},
    {'#', {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}},
};

// MAX7456-style 7x7 icons (bits 6..0)
static const uint8_t kIconAntenna[7] = {
    0b0101010,
    0b0011100,
    0b0001000,
    0b0001000,
    0b0001000,
    0b0011100,
    0b0111110,
};

static const uint8_t kIconClock[7] = {
    0b0011100,
    0b0100010,
    0b1001001,
    0b1001101,
    0b1000001,
    0b0100010,
    0b0011100,
};

static const uint8_t kIconGate[7] = {
    0b1111111,
    0b1111111,
    0b1100011,
    0b1100011,
    0b1100011,
    0b1100011,
    0b1100011,
};

static const uint8_t kIconBattery[6][7] = {
    // 0 bars (empty)
    {0b0011100, 0b1111111, 0b1000001, 0b1000001, 0b1000001, 0b1000001, 0b1111111},
    // 1 bar
    {0b0011100, 0b1111111, 0b1000001, 0b1000001, 0b1000001, 0b1111111, 0b1111111},
    // 2 bars
    {0b0011100, 0b1111111, 0b1000001, 0b1000001, 0b1111111, 0b1111111, 0b1111111},
    // 3 bars
    {0b0011100, 0b1111111, 0b1000001, 0b1111111, 0b1111111, 0b1111111, 0b1111111},
    // 4 bars
    {0b0011100, 0b1111111, 0b1111111, 0b1111111, 0b1111111, 0b1111111, 0b1111111},
    // 5 bars (full)
    {0b0011100, 0b1111111, 0b1111111, 0b1111111, 0b1111111, 0b1111111, 0b1111111},
};

const char* inputSourceLabel(InputSource src, bool isElrs, bool imuEnabled = true) {
    switch (src) {
        case InputSource::USB_HID: return "USB";
        case InputSource::BLE_HID: return isElrs ? "ELRS" : "BLE";
        case InputSource::IMU_KB:
        default:                   return imuEnabled ? "IMU" : "KEY";
    }
}

const char* chMapLabel(ChannelMap m) {
    switch (m) {
        case ChannelMap::TAER: return "TAER";
        case ChannelMap::AERT: return "AERT";
        case ChannelMap::AETR:
        default:               return "AETR";
    }
}

const char* rateLabel(RateProfile r) {
    switch (r) {
        case RateProfile::RATE_LOW:  return "LOW";
        case RateProfile::RATE_HIGH: return "HIGH";
        case RateProfile::RATE_MID:
        default:                     return "MED";
    }
}

} // namespace

void BetaflightOsd::putPixel(uint16_t* fb, int x, int y, uint16_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        fb[y * SCREEN_W + x] = color;
    }
}

const uint8_t* BetaflightOsd::getFontGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(kFontTable) / sizeof(kFontTable[0]); ++i) {
        if (kFontTable[i].ch == c) return kFontTable[i].rows;
    }
    return kFontTable[0].rows;
}

void BetaflightOsd::drawGlyphOutlined(uint16_t* fb, int x, int y, const uint8_t rows[7],
                                      int width, uint16_t fgColor, int scale) {
    // Pass 1: Authentic MAX7456 1px black outline halo around every set pixel
    for (int ry = 0; ry < 7; ++ry) {
        uint8_t bits = rows[ry];
        if (!bits) continue;
        for (int rx = 0; rx < width; ++rx) {
            if (bits & (1u << (width - 1 - rx))) {
                int bx = x + rx * scale;
                int by = y + ry * scale;
                for (int sy = -1; sy <= scale; ++sy) {
                    for (int sx = -1; sx <= scale; ++sx) {
                        putPixel(fb, bx + sx, by + sy, OSD_BLACK);
                    }
                }
            }
        }
    }
    // Pass 2: Crisp foreground pixels
    for (int ry = 0; ry < 7; ++ry) {
        uint8_t bits = rows[ry];
        if (!bits) continue;
        for (int rx = 0; rx < width; ++rx) {
            if (bits & (1u << (width - 1 - rx))) {
                int bx = x + rx * scale;
                int by = y + ry * scale;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        putPixel(fb, bx + sx, by + sy, fgColor);
                    }
                }
            }
        }
    }
}

void BetaflightOsd::drawTextOutlined(uint16_t* fb, int x, int y, const char* text,
                                     uint16_t fgColor, int scale) {
    if (!text) return;
    int cx = x;
    int adv = 6 * scale;
    while (*text) {
        if (*text != ' ') {
            drawGlyphOutlined(fb, cx, y, getFontGlyph(*text), 5, fgColor, scale);
        }
        cx += adv;
        ++text;
    }
}

void BetaflightOsd::drawTextCentered(uint16_t* fb, int cx, int y, const char* text,
                                     uint16_t fgColor, int scale) {
    if (!text) return;
    int len = static_cast<int>(strlen(text));
    int totalW = len * 6 * scale - scale;
    drawTextOutlined(fb, cx - totalW / 2, y, text, fgColor, scale);
}

void BetaflightOsd::drawIconOutlined(uint16_t* fb, int x, int y, OsdIcon icon,
                                     uint16_t fgColor) {
    const uint8_t* rows = kIconAntenna;
    switch (icon) {
        case OsdIcon::ANTENNA:   rows = kIconAntenna;    break;
        case OsdIcon::CLOCK:     rows = kIconClock;      break;
        case OsdIcon::GATE:      rows = kIconGate;       break;
        case OsdIcon::BATTERY_0: rows = kIconBattery[0]; break;
        case OsdIcon::BATTERY_1: rows = kIconBattery[1]; break;
        case OsdIcon::BATTERY_2: rows = kIconBattery[2]; break;
        case OsdIcon::BATTERY_3: rows = kIconBattery[3]; break;
        case OsdIcon::BATTERY_4: rows = kIconBattery[4]; break;
        case OsdIcon::BATTERY_5: rows = kIconBattery[5]; break;
        default:                 rows = kIconAntenna;    break;
    }
    drawGlyphOutlined(fb, x, y, rows, 7, fgColor, 1);
}

void BetaflightOsd::fillRectAlpha(uint16_t* fb, int x, int y, int w, int h,
                                  uint16_t color, uint8_t alpha) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > SCREEN_W) ? SCREEN_W : (x + w);
    int y1 = (y + h > SCREEN_H) ? SCREEN_H : (y + h);
    for (int py = y0; py < y1; ++py) {
        uint16_t* row = &fb[py * SCREEN_W];
        for (int px = x0; px < x1; ++px) {
            row[px] = blend565(row[px], color, alpha);
        }
    }
}

void BetaflightOsd::drawRect(uint16_t* fb, int x, int y, int w, int h, uint16_t color) {
    for (int px = x; px < x + w; ++px) {
        putPixel(fb, px, y, color);
        putPixel(fb, px, y + h - 1, color);
    }
    for (int py = y; py < y + h; ++py) {
        putPixel(fb, x, py, color);
        putPixel(fb, x + w - 1, py, color);
    }
}

void BetaflightOsd::drawLineOutlined(uint16_t* fb, int x0, int y0, int x1, int y1,
                                     uint16_t fgColor) {
    auto plotPass = [&](uint16_t col, bool halo) {
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int cx = x0, cy = y0;
        for (int guard = 0; guard < 300; ++guard) {
            if (halo) {
                putPixel(fb, cx - 1, cy, OSD_BLACK);
                putPixel(fb, cx + 1, cy, OSD_BLACK);
                putPixel(fb, cx, cy - 1, OSD_BLACK);
                putPixel(fb, cx, cy + 1, OSD_BLACK);
            } else {
                putPixel(fb, cx, cy, col);
            }
            if (cx == x1 && cy == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; cx += sx; }
            if (e2 <= dx) { err += dx; cy += sy; }
        }
    };
    plotPass(OSD_BLACK, true);
    plotPass(fgColor, false);
}

void BetaflightOsd::drawArtificialHorizon(uint16_t* fb, float rollRad, float pitchRad,
                                          int throttlePct, float altM) {
    const int cx = SCREEN_W / 2;
    const int cy = SCREEN_H / 2;

    // 1. Betaflight Center Crosshair:  - ^ -
    drawLineOutlined(fb, cx - 10, cy, cx - 5, cy, OSD_WHITE);
    drawLineOutlined(fb, cx + 5, cy, cx + 10, cy, OSD_WHITE);
    drawLineOutlined(fb, cx - 2, cy + 2, cx, cy, OSD_WHITE);
    drawLineOutlined(fb, cx + 2, cy + 2, cx, cy, OSD_WHITE);

    // 2. Dynamic Artificial Horizon (tilts opposite roll, shifts with pitch)
    float sinR = sinf(-rollRad);
    float cosR = cosf(-rollRad);
    float pitchOffset = pitchRad * 70.0f;

    // Draw 2 clean horizon segments left & right of center crosshair
    auto drawHSeg = [&](float u0, float u1, float vOff, uint16_t col) {
        int ax = static_cast<int>(cx + u0 * cosR - (vOff + pitchOffset) * sinR);
        int ay = static_cast<int>(cy + u0 * sinR + (vOff + pitchOffset) * cosR);
        int bx = static_cast<int>(cx + u1 * cosR - (vOff + pitchOffset) * sinR);
        int by = static_cast<int>(cy + u1 * sinR + (vOff + pitchOffset) * cosR);
        drawLineOutlined(fb, ax, ay, bx, by, col);
    };

    // Main horizon bars
    drawHSeg(-38.0f, -18.0f, 0.0f, OSD_WHITE);
    drawHSeg( 18.0f,  38.0f, 0.0f, OSD_WHITE);
    // Subtle +/- pitch ladder ticks
    drawHSeg(-26.0f, -20.0f, -14.0f, OSD_DIM);
    drawHSeg( 20.0f,  26.0f, -14.0f, OSD_DIM);
    drawHSeg(-26.0f, -20.0f,  14.0f, OSD_DIM);
    drawHSeg( 20.0f,  26.0f,  14.0f, OSD_DIM);

    // 3. Betaflight Left/Right Sidebars (Left = Altitude, Right = Throttle)
    const int barTop = cy - 22;
    const int barH   = 44;
    const int lx     = 14;
    const int rx     = SCREEN_W - 15;

    // Bracket ticks
    drawLineOutlined(fb, lx, barTop, lx + 3, barTop, OSD_DIM);
    drawLineOutlined(fb, lx, barTop + barH, lx + 3, barTop + barH, OSD_DIM);
    drawLineOutlined(fb, lx, cy, lx + 2, cy, OSD_DIM);

    drawLineOutlined(fb, rx - 3, barTop, rx, barTop, OSD_DIM);
    drawLineOutlined(fb, rx - 3, barTop + barH, rx, barTop + barH, OSD_DIM);
    drawLineOutlined(fb, rx - 2, cy, rx, cy, OSD_DIM);

    // Altitude caret on left bar (0..12m)
    float altFrac = altM / 12.0f;
    if (altFrac < 0.0f) altFrac = 0.0f;
    if (altFrac > 1.0f) altFrac = 1.0f;
    int altY = barTop + barH - static_cast<int>(altFrac * barH);
    drawLineOutlined(fb, lx + 1, altY, lx + 5, altY, OSD_CYAN);

    // Throttle caret on right bar (0..100%)
    float thrFrac = static_cast<float>(throttlePct) / 100.0f;
    if (thrFrac < 0.0f) thrFrac = 0.0f;
    if (thrFrac > 1.0f) thrFrac = 1.0f;
    int thrY = barTop + barH - static_cast<int>(thrFrac * barH);
    drawLineOutlined(fb, rx - 5, thrY, rx - 1, thrY, OSD_AMBER);
}

void BetaflightOsd::drawFlightOsd(uint16_t* fb,
                                  const SimEngine& sim,
                                  const PilotInput& input,
                                  const SimSettings& settings,
                                  uint32_t frameCount) {
    const BetaflightTelemetry& t = sim.telemetry();
    const RunStats& st           = sim.stats();
    char buf[32];

    // ---- Center Betaflight Crosshair & Artificial Horizon ----
    drawArtificialHorizon(fb, t.rollRad, t.pitchRad, t.throttlePct, t.altitudeM);

    // ---- Top-Left: Link Quality & Source + Gate Score ----
    uint16_t lqCol = (t.rssiPct < 45) ? OSD_RED : ((t.rssiPct < 70) ? OSD_AMBER : OSD_WHITE);
    drawIconOutlined(fb, 4, 4, OsdIcon::ANTENNA, lqCol);
    snprintf(buf, sizeof(buf), "%s:%02d", inputSourceLabel(input.source, input.isElrsBle, settings.imuEnabled), t.rssiPct);
    drawTextOutlined(fb, 13, 4, buf, lqCol);

    drawIconOutlined(fb, 4, 15, OsdIcon::GATE, OSD_GREEN);
    if (st.score >= 0) {
        snprintf(buf, sizeof(buf), "%d HI:%d", st.score, st.bestScore);
    } else {
        snprintf(buf, sizeof(buf), "%d HI:%d", st.score, st.bestScore);
    }
    drawTextOutlined(fb, 13, 15, buf, OSD_WHITE);

    // ---- Top-Right: Race Timer + Speed ----
    bool lowTime = (settings.gameMode == GameMode::RACE_30S && sim.remainingSec() < 5.0f &&
                    sim.phase() == SimPhase::FLYING);
    uint16_t timeCol = (lowTime && ((frameCount / 10) & 1)) ? OSD_RED : OSD_WHITE;
    drawIconOutlined(fb, SCREEN_W - 44, 4, OsdIcon::CLOCK, timeCol);
    if (settings.gameMode == GameMode::RACE_30S) {
        int secs = static_cast<int>(sim.remainingSec());
        int tenths = static_cast<int>((sim.remainingSec() - secs) * 10.0f);
        if (tenths < 0) tenths = 0;
        snprintf(buf, sizeof(buf), "%02d.%dS", secs, tenths);
    } else {
        int secs = static_cast<int>(st.flightTimeS);
        snprintf(buf, sizeof(buf), "%02d:%02d", (secs / 60) % 60, secs % 60);
    }
    drawTextOutlined(fb, SCREEN_W - 34, 4, buf, timeCol);

    snprintf(buf, sizeof(buf), "%3dKMH", static_cast<int>(t.airspeedKmh + 0.5f));
    drawTextOutlined(fb, SCREEN_W - 40, 15, buf, OSD_WHITE);

    // ---- Top-Center / Center Countdown & Betaflight Warning Banners ----
    if (sim.phase() == SimPhase::COUNTDOWN) {
        snprintf(buf, sizeof(buf), "%d", sim.countdownNum());
        drawTextCentered(fb, SCREEN_W / 2, 30, buf, OSD_AMBER, 3);
        drawTextCentered(fb, SCREEN_W / 2, 18, "ARMED - READY", OSD_GREEN, 1);
    } else if (sim.goBannerTimer() > 0.0f) {
        drawTextCentered(fb, SCREEN_W / 2, 28, "GO!", OSD_GREEN, 2);
    } else if (sim.statusBannerTimer() > 0.0f && sim.statusBanner()[0] != '\0') {
        uint16_t bCol = OSD_WHITE;
        if (strstr(sim.statusBanner(), "+1")) bCol = OSD_GREEN;
        else if (strstr(sim.statusBanner(), "-1")) bCol = OSD_RED;
        else if (strstr(sim.statusBanner(), "ZERO") || strstr(sim.statusBanner(), "CENTER")) bCol = OSD_CYAN;
        drawTextCentered(fb, SCREEN_W / 2, 26, sim.statusBanner(), bCol, 1);
    } else if (t.lowBattery && ((frameCount / 15) & 1)) {
        drawTextCentered(fb, SCREEN_W / 2, 26, "LOW BATTERY", OSD_RED, 1);
    }

    // ---- Bottom-Left: Betaflight Battery Voltage + Altitude ----
    float cellV = t.cellVoltage;
    int batBars = static_cast<int>((cellV - 3.30f) / (4.15f - 3.30f) * 5.0f + 0.5f);
    if (batBars < 0) batBars = 0;
    if (batBars > 5) batBars = 5;
    uint16_t batCol = t.lowBattery ? OSD_RED : (batBars <= 1 ? OSD_AMBER : OSD_WHITE);

    drawIconOutlined(fb, 4, SCREEN_H - 21,
                     static_cast<OsdIcon>(static_cast<int>(OsdIcon::BATTERY_0) + batBars), batCol);
    snprintf(buf, sizeof(buf), "%4.1fV", t.packVoltage);
    drawTextOutlined(fb, 13, SCREEN_H - 21, buf, batCol);

    snprintf(buf, sizeof(buf), "ALT %3.1fM", t.altitudeM);
    drawTextOutlined(fb, 4, SCREEN_H - 11, buf, OSD_WHITE);

    // ---- Bottom-Center: Betaflight Craft Name & Flight Mode ----
    const char* fmode = "ACRO";
    if (input.source == InputSource::IMU_KB) {
        fmode = settings.imuEnabled ? "TILT" : "KEYS";
    }
    drawTextCentered(fb, SCREEN_W / 2, SCREEN_H - 21, fmode, OSD_CYAN);
    drawTextCentered(fb, SCREEN_W / 2, SCREEN_H - 11, "STICKADV", OSD_WHITE);

    // ---- Bottom-Right: Throttle % & Rate Profile ----
    snprintf(buf, sizeof(buf), "THR:%d%%", t.throttlePct);
    int thrW = static_cast<int>(strlen(buf)) * 6;
    drawTextOutlined(fb, SCREEN_W - 4 - thrW, SCREEN_H - 21, buf, OSD_WHITE);

    snprintf(buf, sizeof(buf), "R:%s", rateLabel(settings.rates));
    int rateW = static_cast<int>(strlen(buf)) * 6;
    drawTextOutlined(fb, SCREEN_W - 4 - rateW, SCREEN_H - 11, buf, OSD_DIM);
}

void BetaflightOsd::drawTitleOverlay(uint16_t* fb,
                                     const SimEngine& sim,
                                     const PilotInput& input,
                                     const SimSettings& settings,
                                     const UiMenuState& ui,
                                     uint32_t frameCount) {
    char buf[44];

    // Translucent dark overlay panel
    fillRectAlpha(fb, 10, 5, SCREEN_W - 20, SCREEN_H - 10, OSD_PANEL, 196);
    drawRect(fb, 10, 5, SCREEN_W - 20, SCREEN_H - 10, OSD_CYAN);

    // Two-tone StickADV hero title ("STICK" cyan + "ADV" amber)
    constexpr int kTitleScale = 3;
    constexpr int kTitleW     = 8 * 6 * kTitleScale - kTitleScale; // "STICKADV" = 141 px
    constexpr int kTitleX     = (SCREEN_W - kTitleW) / 2;
    drawTextOutlined(fb, kTitleX, 11, "STICK", OSD_CYAN, kTitleScale);
    drawTextOutlined(fb, kTitleX + 5 * 6 * kTitleScale, 11, "ADV", OSD_AMBER, kTitleScale);

    // Clean, single-line active controller status
    uint16_t srcCol = (input.source == InputSource::USB_HID) ? OSD_GREEN :
                      (input.source == InputSource::BLE_HID) ? OSD_CYAN  : OSD_AMBER;

    if (input.source == InputSource::USB_HID) {
        snprintf(buf, sizeof(buf), "USB: %s", ui.usbDevName[0] ? ui.usbDevName : "RADIO");
    } else if (input.source == InputSource::BLE_HID) {
        snprintf(buf, sizeof(buf), "BLE: %s", ui.blePeerName[0] ? ui.blePeerName : "RADIO");
    } else if (ui.bleScanning || (ui.bleStatus[0] && strcmp(ui.bleStatus, "READY") != 0 &&
                                  strcmp(ui.bleStatus, "IDLE") != 0)) {
        snprintf(buf, sizeof(buf), "BLE: %s", ui.bleStatus);
        srcCol = strstr(ui.bleStatus, "ERR") || strstr(ui.bleStatus, "DISC") ? OSD_RED : OSD_CYAN;
    } else if (settings.imuEnabled && ui.imuPresent) {
        snprintf(buf, sizeof(buf), "INPUT: TILT + KEYS");
    } else {
        snprintf(buf, sizeof(buf), "INPUT: KEYS ONLY");
    }
    drawTextCentered(fb, SCREEN_W / 2, 39, buf, srcCol, 1);

    // Race Mode & Best Score
    snprintf(buf, sizeof(buf), "%s   BEST: %d",
             settings.gameMode == GameMode::RACE_30S ? "30S RACE" : "ENDLESS",
             sim.getBestScore());
    drawTextCentered(fb, SCREEN_W / 2, 52, buf, OSD_WHITE, 1);

    // Primary Call-to-Action or live feedback toast
    if (sim.statusBannerTimer() > 0.0f && sim.statusBanner()[0] != '\0') {
        drawTextCentered(fb, SCREEN_W / 2, 69, sim.statusBanner(), OSD_CYAN, 1);
    } else {
        bool pulse = ((frameCount / 22) & 1) != 0;
        fillRectAlpha(fb, 40, 66, SCREEN_W - 80, 13, OSD_GREEN, pulse ? 55 : 28);
        drawTextCentered(fb, SCREEN_W / 2, 69, "PRESS [OK] TO FLY",
                         pulse ? OSD_GREEN : OSD_WHITE, 1);
    }

    // Subtle divider between main info and menu shortcuts
    for (int px = 22; px < SCREEN_W - 22; ++px) {
        putPixel(fb, px, 86, blend565(fb[86 * SCREEN_W + px], OSD_CYAN, 90));
    }

    // Clean 2x2 plain-English shortcut grid
    drawTextOutlined(fb, 24,  94, "[M] SETTINGS",  OSD_WHITE);
    drawTextOutlined(fb, 128, 94, "[B] BLE RADIO", OSD_WHITE);
    drawTextOutlined(fb, 24, 108, "[C] ZERO TILT", OSD_DIM);
    drawTextOutlined(fb, 128, 108, "[K] INPUTS",   OSD_DIM);
}

void BetaflightOsd::drawPostFlightStats(uint16_t* fb,
                                        const SimEngine& sim,
                                        const PilotInput& input,
                                        const SimSettings& settings,
                                        uint32_t frameCount) {
    const RunStats& st = sim.stats();
    char buf[40];

    fillRectAlpha(fb, 14, 8, SCREEN_W - 28, SCREEN_H - 16, OSD_PANEL, 205);
    drawRect(fb, 14, 8, SCREEN_W - 28, SCREEN_H - 16, st.isNewBest ? OSD_GREEN : OSD_WHITE);

    drawTextCentered(fb, SCREEN_W / 2, 13, "RACE COMPLETE", OSD_AMBER, 1);

    if (st.isNewBest && ((frameCount / 15) & 1)) {
        drawTextCentered(fb, SCREEN_W / 2, 25, "* NEW BEST SCORE! *", OSD_GREEN, 1);
    } else {
        snprintf(buf, sizeof(buf), "SCORE: %d   BEST: %d", st.score, st.bestScore);
        drawTextCentered(fb, SCREEN_W / 2, 25, buf, OSD_WHITE, 1);
    }

    int total = st.gatesHit + st.gatesMissed;
    int acc   = total > 0 ? (st.gatesHit * 100 / total) : 0;

    snprintf(buf, sizeof(buf), "GATES   : %d / %d (%d%%)", st.gatesHit, total, acc);
    drawTextOutlined(fb, 26, 40, buf, OSD_WHITE);

    snprintf(buf, sizeof(buf), "STREAK  : %d", st.maxStreak);
    drawTextOutlined(fb, 26, 52, buf, OSD_WHITE);

    snprintf(buf, sizeof(buf), "TOP SPD : %d KM/H", static_cast<int>(st.maxSpeedKmh + 0.5f));
    drawTextOutlined(fb, 26, 64, buf, OSD_WHITE);

    if (input.source == InputSource::IMU_KB) {
        snprintf(buf, sizeof(buf), "INPUT   : %s", settings.imuEnabled ? "TILT + KEYS" : "KEYS ONLY");
    } else {
        snprintf(buf, sizeof(buf), "INPUT   : %s (%s)",
                 inputSourceLabel(input.source, input.isElrsBle),
                 chMapLabel(settings.channelMap));
    }
    drawTextOutlined(fb, 26, 76, buf, OSD_CYAN);

    snprintf(buf, sizeof(buf), "BATTERY : %d MAH", static_cast<int>(sim.telemetry().mahUsed + 0.5f));
    drawTextOutlined(fb, 26, 88, buf, OSD_DIM);

    drawTextCentered(fb, SCREEN_W / 2, 108, "[OK] FLY AGAIN  [M] SETTINGS", OSD_GREEN, 1);
}

void BetaflightOsd::drawSettingsMenu(uint16_t* fb,
                                     const SimSettings& settings,
                                     const UiMenuState& ui) {
    fillRectAlpha(fb, 0, 0, SCREEN_W, SCREEN_H, OSD_PANEL, 235);
    drawRect(fb, 4, 4, SCREEN_W - 8, SCREEN_H - 8, OSD_CYAN);

    constexpr int TOTAL_ITEMS = 12;
    char hdr[28];
    snprintf(hdr, sizeof(hdr), "SETTINGS  %d/%d", ui.selectedItem + 1, TOTAL_ITEMS);
    drawTextCentered(fb, SCREEN_W / 2, 9, hdr, OSD_AMBER, 1);

    const char* labels[TOTAL_ITEMS] = {
        "VOLUME",
        "GAME MODE",
        "TILT FLY",
        "TILT UP/DOWN",
        "INVERT TILT",
        "SENSITIVITY",
        "CAMERA TILT",
        "CRT EFFECT",
        "OSD BATTERY",
        "CHANNEL MAP",
        "BLE RADIO",
        "BLE PAIRING",
    };

    char vals[TOTAL_ITEMS][20];
    if (settings.volumePct == 0) snprintf(vals[0], sizeof(vals[0]), "MUTE");
    else                         snprintf(vals[0], sizeof(vals[0]), "%d%%", settings.volumePct);
    snprintf(vals[1], sizeof(vals[1]), "%s", settings.gameMode == GameMode::RACE_30S ? "30S RACE" : "ENDLESS");
    snprintf(vals[2], sizeof(vals[2]), "%s", settings.imuEnabled ? "ON" : "OFF");
    snprintf(vals[3], sizeof(vals[3]), "%s", settings.imuMode == ImuMode::TILT_ALT ? "ALTITUDE" : "SPEED");
    snprintf(vals[4], sizeof(vals[4]), "%s", settings.imuInvertY ? "ON" : "OFF");
    snprintf(vals[5], sizeof(vals[5]), "%s", rateLabel(settings.rates));
    snprintf(vals[6], sizeof(vals[6]), "%s", settings.camTilt ? "ON" : "OFF");
    snprintf(vals[7], sizeof(vals[7]), "%s", settings.analogFx ? "ON" : "OFF");
    snprintf(vals[8], sizeof(vals[8]), "%s", settings.osdBattery == OsdBatterySource::SIM_4S ? "SIM 4S" : "DEVICE");
    snprintf(vals[9], sizeof(vals[9]), "%s", chMapLabel(settings.channelMap));
    if (ui.bleConnected)      snprintf(vals[10], sizeof(vals[10]), "DISCONNECT");
    else if (ui.bleScanning)  snprintf(vals[10], sizeof(vals[10]), "SCANNING");
    else                      snprintf(vals[10], sizeof(vals[10]), "SCAN");
    snprintf(vals[11], sizeof(vals[11]), "CLEAR");

    constexpr int VISIBLE = 7;
    int topIdx = ui.selectedItem - 3;
    if (topIdx < 0) topIdx = 0;
    if (topIdx > TOTAL_ITEMS - VISIBLE) topIdx = TOTAL_ITEMS - VISIBLE;

    for (int row = 0; row < VISIBLE; ++row) {
        int idx = topIdx + row;
        int y = 23 + row * 13;
        bool sel = (idx == ui.selectedItem);
        if (sel) {
            fillRectAlpha(fb, 10, y - 2, SCREEN_W - 26, 11, OSD_CYAN, 60);
            drawTextOutlined(fb, 13, y, ">", OSD_GREEN);
        }
        drawTextOutlined(fb, 23, y, labels[idx], sel ? OSD_WHITE : OSD_DIM);
        drawTextOutlined(fb, 138, y, vals[idx], sel ? OSD_AMBER : OSD_CYAN);
    }

    // Subtle vertical scroll track & thumb on right edge
    constexpr int trackX = SCREEN_W - 11;
    constexpr int trackY = 22;
    constexpr int trackH = 88;
    fillRectAlpha(fb, trackX, trackY, 3, trackH, OSD_DIM, 55);
    int thumbH = (trackH * VISIBLE) / TOTAL_ITEMS;
    int thumbY = trackY + ((trackH - thumbH) * topIdx) / (TOTAL_ITEMS - VISIBLE);
    fillRectAlpha(fb, trackX, thumbY, 3, thumbH, OSD_CYAN, 210);

    drawTextCentered(fb, SCREEN_W / 2, SCREEN_H - 14, "ARROWS:MOVE  OK:SET  DEL:BACK", OSD_DIM, 1);
}

void BetaflightOsd::drawStickMonitor(uint16_t* fb,
                                     const PilotInput& input,
                                     const SimSettings& settings,
                                     const UiMenuState& ui) {
    fillRectAlpha(fb, 0, 0, SCREEN_W, SCREEN_H, OSD_PANEL, 235);
    drawRect(fb, 4, 4, SCREEN_W - 8, SCREEN_H - 8, OSD_GREEN);

    char buf[64];
    if (input.source == InputSource::IMU_KB) {
        snprintf(buf, sizeof(buf), "INPUTS - %s", settings.imuEnabled ? "TILT + KEYS" : "KEYS ONLY");
    } else {
        snprintf(buf, sizeof(buf), "INPUTS - %s (%s)",
                 inputSourceLabel(input.source, input.isElrsBle),
                 chMapLabel(settings.channelMap));
    }
    drawTextCentered(fb, SCREEN_W / 2, 8, buf, OSD_AMBER, 1);

    // Left column: Mapped flight axes
    const char* axisName[4] = {"ROLL", "PITCH", "THR", "YAW"};
    const float axisVal[4]  = {input.roll, input.pitch, input.throttle, input.yaw};

    for (int i = 0; i < 4; ++i) {
        int y = 21 + i * 13;
        drawTextOutlined(fb, 10, y, axisName[i], OSD_WHITE);
        int bx = 44, bw = 72, bh = 8;
        drawRect(fb, bx, y, bw, bh, OSD_DIM);
        int mid = bx + bw / 2;
        putPixel(fb, mid, y + 1, OSD_WHITE);
        putPixel(fb, mid, y + bh - 2, OSD_WHITE);

        int fillW = static_cast<int>(axisVal[i] * (bw / 2 - 2));
        if (fillW > 0) {
            fillRectAlpha(fb, mid, y + 1, fillW, bh - 2, OSD_GREEN, 220);
        } else if (fillW < 0) {
            fillRectAlpha(fb, mid + fillW, y + 1, -fillW, bh - 2, OSD_CYAN, 220);
        }
    }

    // Right column: Raw CH1..CH4 in microseconds
    for (int i = 0; i < 4; ++i) {
        int y = 21 + i * 13;
        int us = 1500 + static_cast<int>(input.rawCh[i] * 500.0f);
        snprintf(buf, sizeof(buf), "CH%d:%4d", i + 1, us);
        drawTextOutlined(fb, 124, y, buf, OSD_WHITE);
    }

    // Live Tilt + ARM
    snprintf(buf, sizeof(buf), "TILT R:%+3.0f  P:%+3.0f  ARM:%s",
             ui.imuRollDeg, ui.imuPitchDeg, input.armSwitch ? "ON" : "OFF");
    drawTextOutlined(fb, 10, 76, buf, ui.imuPresent ? OSD_CYAN : OSD_RED);

    // Live USB Host Status & Packet Count
    snprintf(buf, sizeof(buf), "USB: %s (%lu)", ui.usbStatus, static_cast<unsigned long>(ui.usbPkts));
    drawTextOutlined(fb, 10, 88, buf, ui.usbConnected ? OSD_GREEN : OSD_AMBER);

    // Live BLE Host Status & Packet Count
    snprintf(buf, sizeof(buf), "BLE: %s (%lu)", ui.bleStatus, static_cast<unsigned long>(ui.blePkts));
    drawTextOutlined(fb, 10, 100, buf, ui.bleConnected ? OSD_GREEN : OSD_CYAN);

    drawTextCentered(fb, SCREEN_W / 2, SCREEN_H - 14, "[C]ZERO [B]BLE [X]UNPAIR [DEL]BACK", OSD_DIM, 1);
}
