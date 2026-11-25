#pragma once

#include "config.h"
#include "sim_engine.h"
#include <stdint.h>

// ============================================================================
// 60 FPS Colored 3D FPV World Renderer (240x135 RGB565 Software Rasterizer)
// Renders rotating sky/horizon gradient, perspective FPV grass track with
// speed grid & red/white curbs, depth-sorted colorful 3D gates & flags,
// active target reticle + offscreen guidance arrow, and analog RF static FX.
// ============================================================================

class FpvRenderer {
public:
    // Render complete 3D FPV scene into 240x135 RGB565 framebuffer
    static void renderScene(uint16_t* fb,
                            const SimEngine& sim,
                            const SimSettings& settings,
                            uint32_t frameCount);

private:
    struct Camera {
        float droneX;
        float droneY;
        float droneZ;
        float sinR;
        float cosR;
        float pitchPx;
    };

    static void projectPoint(const Camera& cam, float wx, float wy, float wz,
                             float& sx, float& sy);
    static void drawSkyAndGround(uint16_t* fb, const Camera& cam);
    static void drawTrackCorridor(uint16_t* fb, const Camera& cam);
    static void drawObstacle(uint16_t* fb, const Camera& cam,
                             const Obstacle& ob, bool isClosest, uint32_t frameCount);
    static void drawTargetMarker(uint16_t* fb, float sx, float sy, uint32_t frameCount);
    static void applyAnalogFx(uint16_t* fb, const SimEngine& sim,
                              const SimSettings& settings, uint32_t frameCount);

    // Fast clipped 2D raster primitives
    static void putPixel(uint16_t* fb, int x, int y, uint16_t col);
    static void drawLine(uint16_t* fb, int x0, int y0, int x1, int y1, uint16_t col);
    static void drawThickLine(uint16_t* fb, int x0, int y0, int x1, int y1,
                              int thickness, uint16_t col);
    static void fillConvexQuad(uint16_t* fb,
                               float x0, float y0, float x1, float y1,
                               float x2, float y2, float x3, float y3,
                               uint16_t col);
};
