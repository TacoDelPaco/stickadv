#include "renderer.h"
#include <math.h>
#include <stdlib.h>

namespace {

inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Vibrant FPV outdoor race color palette
constexpr uint16_t COL_CURB_RED      = rgb565(228, 48, 42);
constexpr uint16_t COL_CURB_WHITE    = rgb565(238, 240, 232);
constexpr uint16_t COL_GRID_LINE     = rgb565(36, 118, 46);
constexpr uint16_t COL_CENTER_STRIPE = rgb565(64, 152, 68);
constexpr uint16_t COL_HORIZON_GLOW  = rgb565(188, 224, 245);

// Gate & Flag colors
constexpr uint16_t COL_GATE_GND_MAIN = rgb565(255, 86, 28);   // Bright FPV racing orange
constexpr uint16_t COL_GATE_GND_TRIM = rgb565(255, 245, 220); // White stripe
constexpr uint16_t COL_GATE_AIR_MAIN = rgb565(0, 215, 255);   // MultiGP cyan
constexpr uint16_t COL_GATE_AIR_TRIM = rgb565(255, 228, 40);  // Neon yellow banner
constexpr uint16_t COL_GATE_POLE     = rgb565(185, 195, 205); // Aluminum support leg
constexpr uint16_t COL_FLAG_L_MAIN   = rgb565(240, 45, 185);  // Magenta feather flag
constexpr uint16_t COL_FLAG_L_TRIM   = rgb565(255, 255, 255);
constexpr uint16_t COL_FLAG_R_MAIN   = rgb565(255, 200, 24);  // Amber/Gold feather flag
constexpr uint16_t COL_FLAG_R_TRIM   = rgb565(235, 42, 42);
constexpr uint16_t COL_MARKER_LIME   = rgb565(80, 255, 95);
constexpr uint16_t COL_MARKER_CORE   = rgb565(255, 255, 255);
constexpr uint16_t COL_FLASH_HIT     = rgb565(50, 255, 100);
constexpr uint16_t COL_FLASH_MISS    = rgb565(255, 40, 40);

// Sky & grass gradient LUTs indexed by signed perpendicular distance from horizon
uint16_t skyColorForDist(int dUp) {
    // dUp: 0 at horizon .. ~100 at zenith
    int t = clampi(dUp * 255 / 95, 0, 255);
    uint8_t r = static_cast<uint8_t>(168 - (t * 144) / 255);
    uint8_t g = static_cast<uint8_t>(216 - (t * 136) / 255);
    uint8_t b = static_cast<uint8_t>(248 - (t * 68)  / 255);
    return rgb565(r, g, b);
}

uint16_t grassColorForDist(int dDown) {
    // dDown: 0 at horizon (hazy green) .. ~100 close to camera (rich turf green)
    int t = clampi(dDown * 255 / 90, 0, 255);
    uint8_t r = static_cast<uint8_t>(78  - (t * 54) / 255);
    uint8_t g = static_cast<uint8_t>(142 - (t * 48) / 255);
    uint8_t b = static_cast<uint8_t>(68  - (t * 42) / 255);
    return rgb565(r, g, b);
}

// Slight atmospheric haze blend for distant gates/flags so depth pops immediately
uint16_t shadeByDepth(uint16_t col, float dz) {
    if (dz < 1800.0f) return col;
    float frac = clampf((dz - 1800.0f) / 4800.0f, 0.0f, 0.55f);
    return blend565(col, COL_HORIZON_GLOW, static_cast<uint8_t>(frac * 255.0f));
}

} // namespace

void FpvRenderer::putPixel(uint16_t* fb, int x, int y, uint16_t col) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        fb[y * SCREEN_W + x] = col;
    }
}

void FpvRenderer::drawLine(uint16_t* fb, int x0, int y0, int x1, int y1, uint16_t col) {
    // Liang-Barsky / Cohen-Sutherland quick reject + clip to screen rect
    auto outCode = [](int x, int y) -> uint8_t {
        uint8_t c = 0;
        if (x < 0)             c |= 1;
        else if (x >= SCREEN_W) c |= 2;
        if (y < 0)             c |= 4;
        else if (y >= SCREEN_H) c |= 8;
        return c;
    };

    uint8_t c0 = outCode(x0, y0);
    uint8_t c1 = outCode(x1, y1);
    for (int iter = 0; iter < 8; ++iter) {
        if (!(c0 | c1)) break;
        if (c0 & c1) return;
        uint8_t out = c0 ? c0 : c1;
        int x = 0, y = 0;
        if (out & 8) {
            if (y1 == y0) return;
            x = x0 + static_cast<int>(static_cast<int64_t>(x1 - x0) * (SCREEN_H - 1 - y0) / (y1 - y0));
            y = SCREEN_H - 1;
        } else if (out & 4) {
            if (y1 == y0) return;
            x = x0 + static_cast<int>(static_cast<int64_t>(x1 - x0) * (0 - y0) / (y1 - y0));
            y = 0;
        } else if (out & 2) {
            if (x1 == x0) return;
            y = y0 + static_cast<int>(static_cast<int64_t>(y1 - y0) * (SCREEN_W - 1 - x0) / (x1 - x0));
            x = SCREEN_W - 1;
        } else {
            if (x1 == x0) return;
            y = y0 + static_cast<int>(static_cast<int64_t>(y1 - y0) * (0 - x0) / (x1 - x0));
            x = 0;
        }
        if (out == c0) { x0 = x; y0 = y; c0 = outCode(x0, y0); }
        else           { x1 = x; y1 = y; c1 = outCode(x1, y1); }
    }
    if (c0 | c1) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        fb[y0 * SCREEN_W + x0] = col;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void FpvRenderer::drawThickLine(uint16_t* fb, int x0, int y0, int x1, int y1,
                                int thickness, uint16_t col) {
    drawLine(fb, x0, y0, x1, y1, col);
    if (thickness <= 1) return;
    int half = thickness / 2;
    bool steep = abs(y1 - y0) > abs(x1 - x0);
    for (int d = -half; d <= half; ++d) {
        if (d == 0) continue;
        if (steep) drawLine(fb, x0 + d, y0, x1 + d, y1, col);
        else       drawLine(fb, x0, y0 + d, x1, y1 + d, col);
    }
}

void FpvRenderer::fillConvexQuad(uint16_t* fb,
                                 float x0, float y0, float x1, float y1,
                                 float x2, float y2, float x3, float y3,
                                 uint16_t col) {
    float xs[4] = {x0, x1, x2, x3};
    float ys[4] = {y0, y1, y2, y3};
    float minYf = ys[0], maxYf = ys[0];
    for (int i = 1; i < 4; ++i) {
        if (ys[i] < minYf) minYf = ys[i];
        if (ys[i] > maxYf) maxYf = ys[i];
    }
    int yStart = clampi(static_cast<int>(ceilf(minYf)), 0, SCREEN_H - 1);
    int yEnd   = clampi(static_cast<int>(floorf(maxYf)), 0, SCREEN_H - 1);
    if (yStart > yEnd) return;

    for (int y = yStart; y <= yEnd; ++y) {
        float fy = static_cast<float>(y);
        float minX = 1e6f, maxX = -1e6f;
        for (int e = 0; e < 4; ++e) {
            int n = (e + 1) & 3;
            float ay = ys[e], by = ys[n];
            if ((ay <= fy && by >= fy) || (by <= fy && ay >= fy)) {
                float dy = by - ay;
                float ix = (fabsf(dy) < 1e-4f) ? xs[e] : (xs[e] + (fy - ay) * (xs[n] - xs[e]) / dy);
                if (ix < minX) minX = ix;
                if (ix > maxX) maxX = ix;
            }
        }
        int xStart = clampi(static_cast<int>(ceilf(minX)), 0, SCREEN_W - 1);
        int xEnd   = clampi(static_cast<int>(floorf(maxX)), 0, SCREEN_W - 1);
        if (xStart <= xEnd) {
            uint16_t* row = &fb[y * SCREEN_W];
            for (int x = xStart; x <= xEnd; ++x) {
                row[x] = col;
            }
        }
    }
}

void FpvRenderer::projectPoint(const Camera& cam, float wx, float wy, float wz,
                               float& sx, float& sy) {
    float dz = wz - cam.droneZ;
    if (dz < 15.0f) dz = 15.0f;
    float u = ((wx - cam.droneX) * SimEngine::Z_SCALE) / dz;
    float v = ((wy - cam.droneY) * SimEngine::Z_SCALE) / dz + cam.pitchPx;

    // Rotate around screen center by camera roll
    sx = (SCREEN_W * 0.5f) + u * cam.cosR - v * cam.sinR;
    sy = (SCREEN_H * 0.5f) + u * cam.sinR + v * cam.cosR;
}

void FpvRenderer::drawSkyAndGround(uint16_t* fb, const Camera& cam) {
    // Precompute small lookup tables for ultra-fast sky/grass gradient rasterization
    static bool lutInit = false;
    static uint16_t skyLut[160];
    static uint16_t grassLut[160];
    if (!lutInit) {
        for (int i = 0; i < 160; ++i) {
            skyLut[i]   = skyColorForDist(i);
            grassLut[i] = grassColorForDist(i);
        }
        lutInit = true;
    }

    const float cx = SCREEN_W * 0.5f;
    const float cy = SCREEN_H * 0.5f;

    // Fast path when flat (no roll)
    if (fabsf(cam.sinR) < 0.002f) {
        int horizonY = static_cast<int>(cy + cam.pitchPx);
        for (int y = 0; y < SCREEN_H; ++y) {
            uint16_t* row = &fb[y * SCREEN_W];
            int d = y - horizonY;
            uint16_t col;
            if (d == 0) {
                col = COL_HORIZON_GLOW;
            } else if (d < 0) {
                int idx = -d;
                col = skyLut[idx < 160 ? idx : 159];
            } else {
                col = grassLut[d < 160 ? d : 159];
            }
            for (int x = 0; x < SCREEN_W; ++x) row[x] = col;
        }
        return;
    }

    // Tilted horizon fast integer DDA per scanline
    // Signed perpendicular distance from horizon line (positive = ground, negative = sky):
    //   d(x, y) = -(x - cx) * sinR + (y - cy) * cosR - pitchPx
    const int32_t stepX = static_cast<int32_t>(-cam.sinR * 65536.0f);
    for (int y = 0; y < SCREEN_H; ++y) {
        float dStart = (cx * cam.sinR) + (static_cast<float>(y) - cy) * cam.cosR - cam.pitchPx;
        int32_t dFix = static_cast<int32_t>(dStart * 65536.0f);
        uint16_t* row = &fb[y * SCREEN_W];
        for (int x = 0; x < SCREEN_W; ++x) {
            int d = dFix >> 16;
            if (d == 0) {
                row[x] = COL_HORIZON_GLOW;
            } else if (d < 0) {
                int idx = -d;
                row[x] = skyLut[idx < 160 ? idx : 159];
            } else {
                row[x] = grassLut[d < 160 ? d : 159];
            }
            dFix += stepX;
        }
    }
}

void FpvRenderer::drawTrackCorridor(uint16_t* fb, const Camera& cam) {
    // Draw perspective speed grid lines & red/white FPV race track sidelines
    const float trackHalfW = SimEngine::TRACK_W * 2.0f; // matches simulator.lua (w = track.w * 2)
    constexpr float segStep = 320.0f;
    const float zBase = floorf(cam.droneZ / segStep) * segStep;

    // 1. Cross-track ground speed lines rushing past camera
    for (int s = 1; s <= 12; ++s) {
        float wz = zBase + static_cast<float>(s) * segStep;
        float dz = wz - cam.droneZ;
        if (dz < 60.0f) continue;

        float lx, ly, rx, ry;
        projectPoint(cam, -trackHalfW, 0.0f, wz, lx, ly);
        projectPoint(cam,  trackHalfW, 0.0f, wz, rx, ry);

        uint16_t gridCol = shadeByDepth(COL_GRID_LINE, dz);
        drawLine(fb, static_cast<int>(lx), static_cast<int>(ly),
                     static_cast<int>(rx), static_cast<int>(ry), gridCol);
    }

    // 2. Center dashed racing line & red/white alternating corridor borders
    for (int s = 0; s < 13; ++s) {
        float z0 = zBase + static_cast<float>(s) * segStep;
        float z1 = z0 + segStep;
        if (z1 <= cam.droneZ + 45.0f) continue;
        if (z0 <  cam.droneZ + 45.0f) z0 = cam.droneZ + 45.0f;

        int segIdx = static_cast<int>(floorf(z0 / segStep));
        bool redSeg = (segIdx & 1) == 0;
        uint16_t curbCol = shadeByDepth(redSeg ? COL_CURB_RED : COL_CURB_WHITE, z0 - cam.droneZ);
        int thick = (z0 - cam.droneZ < 700.0f) ? 2 : 1;

        float x0, y0, x1, y1;
        // Left border
        projectPoint(cam, -trackHalfW, 0.0f, z0, x0, y0);
        projectPoint(cam, -trackHalfW, 0.0f, z1, x1, y1);
        drawThickLine(fb, static_cast<int>(x0), static_cast<int>(y0),
                          static_cast<int>(x1), static_cast<int>(y1), thick, curbCol);

        // Right border
        projectPoint(cam, trackHalfW, 0.0f, z0, x0, y0);
        projectPoint(cam, trackHalfW, 0.0f, z1, x1, y1);
        drawThickLine(fb, static_cast<int>(x0), static_cast<int>(y0),
                          static_cast<int>(x1), static_cast<int>(y1), thick, curbCol);

        // Center runway dashes
        if (redSeg) {
            projectPoint(cam, 0.0f, 0.0f, z0, x0, y0);
            projectPoint(cam, 0.0f, 0.0f, z0 + segStep * 0.55f, x1, y1);
            drawLine(fb, static_cast<int>(x0), static_cast<int>(y0),
                         static_cast<int>(x1), static_cast<int>(y1),
                         shadeByDepth(COL_CENTER_STRIPE, z0 - cam.droneZ));
        }
    }
}

void FpvRenderer::drawTargetMarker(uint16_t* fb, float sx, float sy, uint32_t frameCount) {
    bool pulse = ((frameCount / 6) & 1) != 0;
    uint16_t col = pulse ? COL_MARKER_LIME : COL_MARKER_CORE;

    bool offscreen = (sx < 6.0f || sx > SCREEN_W - 7.0f || sy < 14.0f || sy > SCREEN_H - 18.0f);
    int mx = clampi(static_cast<int>(sx), 7, SCREEN_W - 8);
    int my = clampi(static_cast<int>(sy), 22, SCREEN_H - 26);

    if (offscreen) {
        // Draw edge chevron pointing toward off-screen next gate
        drawLine(fb, mx - 3, my, mx, my - 3, COL_MARKER_LIME);
        drawLine(fb, mx, my - 3, mx + 3, my, COL_MARKER_LIME);
        drawLine(fb, mx + 3, my, mx, my + 3, COL_MARKER_LIME);
        drawLine(fb, mx, my + 3, mx - 3, my, COL_MARKER_LIME);
        putPixel(fb, mx, my, COL_MARKER_CORE);
    } else {
        // Pulsing diamond target reticle inside gate opening
        int r = pulse ? 4 : 3;
        drawLine(fb, mx - r, my, mx, my - r, col);
        drawLine(fb, mx, my - r, mx + r, my, col);
        drawLine(fb, mx + r, my, mx, my + r, col);
        drawLine(fb, mx, my + r, mx - r, my, col);
        putPixel(fb, mx, my, COL_MARKER_CORE);
    }
}

void FpvRenderer::drawObstacle(uint16_t* fb, const Camera& cam,
                               const Obstacle& ob, bool isClosest, uint32_t frameCount) {
    float dz = ob.z - cam.droneZ;
    if (dz < 30.0f) return;

    // Pole thickness scales with proximity for true 3D depth
    int thick = (dz < 650.0f) ? 3 : ((dz < 1700.0f) ? 2 : 1);
    float postW = 2.4f;

    float markX = 0.0f, markY = 0.0f;

    if (ob.type == ObstacleType::GATE_GROUND) {
        float lx = ob.x - SimEngine::GATE_W * 0.5f;
        float rx = ob.x + SimEngine::GATE_W * 0.5f;
        float topY = -SimEngine::GATE_H;
        float botY = 0.0f;

        uint16_t mainCol = shadeByDepth(COL_GATE_GND_MAIN, dz);
        uint16_t trimCol = shadeByDepth(COL_GATE_GND_TRIM, dz);

        // Filled top banner
        float tlx, tly, trx, try_, blx, bly, brx, bry;
        projectPoint(cam, lx - postW, topY - 4.0f, ob.z, tlx, tly);
        projectPoint(cam, rx + postW, topY - 4.0f, ob.z, trx, try_);
        projectPoint(cam, rx + postW, topY + 1.0f, ob.z, brx, bry);
        projectPoint(cam, lx - postW, topY + 1.0f, ob.z, blx, bly);
        fillConvexQuad(fb, tlx, tly, trx, try_, brx, bry, blx, bly, mainCol);

        // Left & right gate posts (with white accent stripe at mid-height)
        float lbx, lby, rbx, rby, lmx, lmy, rmx, rmy;
        projectPoint(cam, lx, botY, ob.z, lbx, lby);
        projectPoint(cam, lx, topY, ob.z, tlx, tly);
        projectPoint(cam, lx, topY * 0.5f, ob.z, lmx, lmy);
        projectPoint(cam, rx, botY, ob.z, rbx, rby);
        projectPoint(cam, rx, topY, ob.z, trx, try_);
        projectPoint(cam, rx, topY * 0.5f, ob.z, rmx, rmy);

        drawThickLine(fb, static_cast<int>(lbx), static_cast<int>(lby),
                          static_cast<int>(tlx), static_cast<int>(tly), thick, mainCol);
        drawThickLine(fb, static_cast<int>(rbx), static_cast<int>(rby),
                          static_cast<int>(trx), static_cast<int>(try_), thick, mainCol);
        drawThickLine(fb, static_cast<int>(lmx), static_cast<int>(lmy),
                          static_cast<int>(tlx), static_cast<int>(tly), thick > 1 ? thick - 1 : 1, trimCol);
        drawThickLine(fb, static_cast<int>(rmx), static_cast<int>(rmy),
                          static_cast<int>(trx), static_cast<int>(try_), thick > 1 ? thick - 1 : 1, trimCol);

        // Highlight top lip of active gate
        if (isClosest) {
            drawLine(fb, static_cast<int>(tlx), static_cast<int>(tly),
                         static_cast<int>(trx), static_cast<int>(try_), COL_MARKER_LIME);
        }

        projectPoint(cam, ob.x, -SimEngine::GATE_H * 0.5f, ob.z, markX, markY);

    } else if (ob.type == ObstacleType::GATE_AIR) {
        float lx   = ob.x - SimEngine::GATE_W * 0.5f;
        float rx   = ob.x + SimEngine::GATE_W * 0.5f;
        float topY = -SimEngine::GATE_H * 2.0f;
        float midY = -SimEngine::GATE_H;
        float botY = 0.0f;

        uint16_t mainCol = shadeByDepth(COL_GATE_AIR_MAIN, dz);
        uint16_t trimCol = shadeByDepth(COL_GATE_AIR_TRIM, dz);
        uint16_t legCol  = shadeByDepth(COL_GATE_POLE, dz);

        // Support legs (ground to mid bar)
        float lbx, lby, lmx, lmy, rbx, rby, rmx, rmy, ltx, lty, rtx, rty;
        projectPoint(cam, lx, botY, ob.z, lbx, lby);
        projectPoint(cam, lx, midY, ob.z, lmx, lmy);
        projectPoint(cam, lx, topY, ob.z, ltx, lty);
        projectPoint(cam, rx, botY, ob.z, rbx, rby);
        projectPoint(cam, rx, midY, ob.z, rmx, rmy);
        projectPoint(cam, rx, topY, ob.z, rtx, rty);

        drawLine(fb, static_cast<int>(lbx), static_cast<int>(lby),
                     static_cast<int>(lmx), static_cast<int>(lmy), legCol);
        drawLine(fb, static_cast<int>(rbx), static_cast<int>(rby),
                     static_cast<int>(rmx), static_cast<int>(rmy), legCol);

        // Top yellow banner quad
        float b0x, b0y, b1x, b1y, b2x, b2y, b3x, b3y;
        projectPoint(cam, lx - postW, topY - 4.0f, ob.z, b0x, b0y);
        projectPoint(cam, rx + postW, topY - 4.0f, ob.z, b1x, b1y);
        projectPoint(cam, rx + postW, topY + 1.0f, ob.z, b2x, b2y);
        projectPoint(cam, lx - postW, topY + 1.0f, ob.z, b3x, b3y);
        fillConvexQuad(fb, b0x, b0y, b1x, b1y, b2x, b2y, b3x, b3y, trimCol);

        // Elevated gate sides + bottom sill (bright cyan)
        drawThickLine(fb, static_cast<int>(lmx), static_cast<int>(lmy),
                          static_cast<int>(ltx), static_cast<int>(lty), thick, mainCol);
        drawThickLine(fb, static_cast<int>(rmx), static_cast<int>(rmy),
                          static_cast<int>(rtx), static_cast<int>(rty), thick, mainCol);
        drawThickLine(fb, static_cast<int>(lmx), static_cast<int>(lmy),
                          static_cast<int>(rmx), static_cast<int>(rmy), thick, mainCol);

        if (isClosest) {
            drawLine(fb, static_cast<int>(ltx), static_cast<int>(lty),
                         static_cast<int>(rtx), static_cast<int>(rty), COL_MARKER_LIME);
        }

        projectPoint(cam, ob.x, -SimEngine::GATE_H * 1.5f, ob.z, markX, markY);

    } else {
        // FLAG_LEFT (pass to RIGHT) or FLAG_RIGHT (pass to LEFT)
        bool isLeft = (ob.type == ObstacleType::FLAG_LEFT);
        float lx   = ob.x - SimEngine::FLAG_W * 0.5f;
        float rx   = ob.x + SimEngine::FLAG_W * 0.5f;
        float topY = -SimEngine::GATE_H * 2.0f;
        float midY = -SimEngine::GATE_H * 0.85f;
        float botY = 0.0f;

        uint16_t mainCol = shadeByDepth(isLeft ? COL_FLAG_L_MAIN : COL_FLAG_R_MAIN, dz);
        uint16_t trimCol = shadeByDepth(isLeft ? COL_FLAG_L_TRIM : COL_FLAG_R_TRIM, dz);
        uint16_t poleCol = shadeByDepth(COL_GATE_POLE, dz);

        // Filled racing feather flag banner
        float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
        projectPoint(cam, lx, topY, ob.z, p0x, p0y);
        projectPoint(cam, rx, topY, ob.z, p1x, p1y);
        projectPoint(cam, rx, midY, ob.z, p2x, p2y);
        projectPoint(cam, lx, midY, ob.z, p3x, p3y);
        fillConvexQuad(fb, p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, mainCol);

        // Diagonal accent stripe across banner
        drawLine(fb, static_cast<int>(p0x), static_cast<int>(p0y),
                     static_cast<int>(p2x), static_cast<int>(p2y), trimCol);

        // Tall flag pole anchored to ground on inner side (matching simulator.lua)
        float poleX = isLeft ? rx : lx;
        float pbx, pby, ptx, pty;
        projectPoint(cam, poleX, botY, ob.z, pbx, pby);
        projectPoint(cam, poleX, topY, ob.z, ptx, pty);
        drawThickLine(fb, static_cast<int>(pbx), static_cast<int>(pby),
                          static_cast<int>(ptx), static_cast<int>(pty),
                          thick > 1 ? 2 : 1, poleCol);

        float markerWorldX = isLeft ? (ob.x + SimEngine::FLAG_W * 2.2f)
                                    : (ob.x - SimEngine::FLAG_W * 2.2f);
        projectPoint(cam, markerWorldX, -SimEngine::GATE_H * 1.35f, ob.z, markX, markY);
    }

    if (isClosest) {
        drawTargetMarker(fb, markX, markY, frameCount);
    }
}

void FpvRenderer::applyAnalogFx(uint16_t* fb, const SimEngine& sim,
                                const SimSettings& settings, uint32_t frameCount) {
    // 1. Subtle CRT/NTSC scanline dimming on every 3rd row (keeps 1.14" IPS crisp!)
    if (settings.analogFx) {
        for (int y = 2; y < SCREEN_H; y += 3) {
            uint16_t* row = &fb[y * SCREEN_W];
            for (int x = 0; x < SCREEN_W; ++x) {
                row[x] = scanlineDim565(row[x]);
            }
        }
    }

    // 2. Dynamic 5.8GHz Analog RF Static / Snow bursts (on gate miss or low RSSI)
    float noiseLvl = sim.telemetry().rfStaticLevel;
    if (settings.analogFx && noiseLvl > 0.02f) {
        uint32_t rng = 0x9E3779B9u ^ (frameCount * 2654435761u);
        int streaks = static_cast<int>(noiseLvl * 28.0f) + 2;
        for (int s = 0; s < streaks; ++s) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int y = static_cast<int>(rng % SCREEN_H);
            int x = static_cast<int>((rng >> 8) % (SCREEN_W - 24));
            int w = 6 + static_cast<int>((rng >> 16) % 26);
            uint8_t br = static_cast<uint8_t>(140 + ((rng >> 24) & 0x7F));
            uint16_t snow = rgb565(br, br, br);
            uint16_t* row = &fb[y * SCREEN_W];
            for (int i = 0; i < w && (x + i) < SCREEN_W; ++i) {
                if ((i & 1) == 0) row[x + i] = snow;
            }
        }
    }

    // 3. Subtle gate hit / miss edge border pulse
    if (sim.hitFlashTimer() > 0.0f) {
        uint8_t a = static_cast<uint8_t>(clampf(sim.hitFlashTimer() / 0.22f, 0.0f, 1.0f) * 140.0f);
        for (int b = 0; b < 3; ++b) {
            for (int x = 0; x < SCREEN_W; ++x) {
                fb[b * SCREEN_W + x] = blend565(fb[b * SCREEN_W + x], COL_FLASH_HIT, a);
                fb[(SCREEN_H - 1 - b) * SCREEN_W + x] =
                    blend565(fb[(SCREEN_H - 1 - b) * SCREEN_W + x], COL_FLASH_HIT, a);
            }
        }
    } else if (sim.missFlashTimer() > 0.0f) {
        uint8_t a = static_cast<uint8_t>(clampf(sim.missFlashTimer() / 0.35f, 0.0f, 1.0f) * 170.0f);
        for (int b = 0; b < 3; ++b) {
            for (int x = 0; x < SCREEN_W; ++x) {
                fb[b * SCREEN_W + x] = blend565(fb[b * SCREEN_W + x], COL_FLASH_MISS, a);
                fb[(SCREEN_H - 1 - b) * SCREEN_W + x] =
                    blend565(fb[(SCREEN_H - 1 - b) * SCREEN_W + x], COL_FLASH_MISS, a);
            }
        }
    }
}

void FpvRenderer::renderScene(uint16_t* fb,
                              const SimEngine& sim,
                              const SimSettings& settings,
                              uint32_t frameCount) {
    Camera cam;
    cam.droneX  = sim.droneX();
    cam.droneY  = sim.droneY();
    cam.droneZ  = sim.droneZ();
    cam.sinR    = sinf(-sim.telemetry().rollRad);
    cam.cosR    = cosf(-sim.telemetry().rollRad);
    cam.pitchPx = sim.telemetry().pitchRad * 64.0f;

    // 1. Sky & Ground gradient
    drawSkyAndGround(fb, cam);

    // 2. Racing corridor sidelines & rushing ground speed grid
    drawTrackCorridor(fb, cam);

    // 3. Depth-sort obstacles far-to-near (painter's algorithm)
    int order[SimEngine::MAX_OBSTACLES];
    for (int i = 0; i < SimEngine::MAX_OBSTACLES; ++i) order[i] = i;
    const Obstacle* obs = sim.obstacles();
    for (int i = 0; i < SimEngine::MAX_OBSTACLES - 1; ++i) {
        for (int j = i + 1; j < SimEngine::MAX_OBSTACLES; ++j) {
            if (obs[order[j]].z > obs[order[i]].z) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    int closest = sim.closestIdx();
    for (int k = 0; k < SimEngine::MAX_OBSTACLES; ++k) {
        int idx = order[k];
        drawObstacle(fb, cam, obs[idx], idx == closest, frameCount);
    }

    // 4. Analog FPV scanlines, RF noise & gate pass flash
    applyAnalogFx(fb, sim, settings, frameCount);
}
