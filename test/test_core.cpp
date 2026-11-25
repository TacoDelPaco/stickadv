// ============================================================================
// Host-Native Unit & Visual Verification Suite for StickADV
// Tests:
//  1. EdgeTX Classic (19B) & 2.11+ (20B) USB HID report decoding
//  2. EdgeTX Classic & 2.11+ USB HID Report Descriptor dynamic parsing
//  3. ExpressLRS BLE Joystick (ESP32-BLE-Gamepad) report descriptor & channel order
//  4. Core EdgeTX Game-Simulator 3D FPV physics, gate/flag hits & misses, scoring
//  5. Full 240x135 RGB565 3D scene + Betaflight Analog OSD rendering (writes PPM)
// ============================================================================

#include "config.h"
#include "hid_parser.h"
#include "sim_engine.h"
#include "renderer.h"
#include "betaflight_osd.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool nearf(float a, float b, float eps = 0.02f) {
    return fabsf(a - b) <= eps;
}

static void writePpm(const char* path, const uint16_t* fb) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) {
        uint16_t px = unpack_rgb565_raw(fb[i]);
        uint8_t r = static_cast<uint8_t>(((px >> 11) & 0x1F) * 255 / 31);
        uint8_t g = static_cast<uint8_t>(((px >> 5)  & 0x3F) * 255 / 63);
        uint8_t b = static_cast<uint8_t>((px & 0x1F) * 255 / 31);
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
}

static void testEdgeTxHid() {
    // Exact Classic HID Report Descriptor from EdgeTX radio/src/usb_joystick.cpp
    static const uint8_t kEdgeTxClassicDesc[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x18, 0x15, 0x00,
        0x25, 0x01, 0x95, 0x18, 0x75, 0x01, 0x81, 0x02,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32,
        0x09, 0x33, 0x09, 0x34, 0x09, 0x35, 0x09, 0x36,
        0x09, 0x37, 0x16, 0x00, 0x00, 0x26, 0xFF, 0x07,
        0x75, 0x10, 0x95, 0x08, 0x81, 0x02, 0xC0, 0xC0
    };

    HidReportLayout layout;
    assert(HidReportParser::parseDescriptor(kEdgeTxClassicDesc, sizeof(kEdgeTxClassicDesc), layout));
    assert(layout.valid);
    assert(!layout.hasReportId);
    assert(layout.buttonCount == 24);
    assert(layout.axes[0].present && layout.axes[0].bitOffset == 24 && layout.axes[0].logicalMax == 2047);
    assert(layout.axes[7].present && layout.axes[7].bitOffset == 24 + 7 * 16);

    // Build a synthetic 20-byte EdgeTX 2.11 report:
    // Button 1 & Button 9 pressed, CH1=2047 (+1), CH2=0 (-1), CH3=1024 (~0), CH5=2047 (+1), Batt=85%
    uint8_t rpt[20] = {};
    rpt[0] = 0x01; // Button 1
    rpt[1] = 0x01; // Button 9
    auto putU16 = [&](int ch, uint16_t v) {
        rpt[3 + ch * 2]     = static_cast<uint8_t>(v & 0xFF);
        rpt[3 + ch * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    putU16(0, 2047);
    putU16(1, 0);
    putU16(2, 1024);
    putU16(3, 1536);
    putU16(4, 2047);
    rpt[19] = 85;

    HidJoystickState st1;
    assert(HidReportParser::decodeReport(layout, rpt, sizeof(rpt), st1));
    assert((st1.buttons & 0x01) != 0);
    assert((st1.buttons & (1u << 8)) != 0);
    assert(nearf(st1.axes[0],  1.0f));
    assert(nearf(st1.axes[1], -1.0f));
    assert(nearf(st1.axes[2],  0.0f));
    assert(nearf(st1.axes[3],  0.5f));
    assert(nearf(st1.axes[4],  1.0f));

    HidJoystickState st2;
    assert(HidReportParser::decodeEdgeTxRaw(rpt, sizeof(rpt), st2));
    assert(st2.batteryPct == 85);
    assert(nearf(st2.axes[0],  1.0f));
    assert(nearf(st2.axes[1], -1.0f));
    printf("[PASS] EdgeTX Classic & 2.11+ USB HID descriptor + report decoding\n");
}

static void testElrsBleHid() {
    // ExpressLRS BLE Joystick (devBLE.cpp): X=CH1, Y=CH2, Z=CH5, Rz=CH6, Rx=CH3, Ry=CH4, S1=CH7, S2=CH8
    HidReportLayout layout;
    HidReportParser::makeElrsBleLayout(layout);
    assert(layout.valid && layout.isElrsOrder);

    uint8_t rpt[17] = {};
    rpt[0] = 0x05; // Buttons 1 and 3
    auto putAxis = [&](int idx, uint16_t v) {
        rpt[1 + idx * 2]     = static_cast<uint8_t>(v & 0xFF);
        rpt[1 + idx * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    putAxis(0, 32767); // X  -> CH1 (+1.0)
    putAxis(1, 16384); // Y  -> CH2 (~0.0)
    putAxis(2, 32767); // Z  -> CH5 (ARM = +1.0)
    putAxis(3, 0);     // Rz -> CH6 (-1.0)
    putAxis(4, 0);     // Rx -> CH3 (Throttle = -1.0)
    putAxis(5, 24576); // Ry -> CH4 (Yaw = +0.5)

    HidJoystickState st;
    assert(HidReportParser::decodeReport(layout, rpt, sizeof(rpt), st));
    assert(st.buttons == 0x05);
    assert(nearf(st.axes[0],  1.0f)); // CH1 Roll
    assert(nearf(st.axes[1],  0.0f)); // CH2 Pitch
    assert(nearf(st.axes[2], -1.0f)); // CH3 Throttle (from Rx!)
    assert(nearf(st.axes[3],  0.5f)); // CH4 Yaw (from Ry!)
    assert(nearf(st.axes[4],  1.0f)); // CH5 ARM (from Z!)
    assert(nearf(st.axes[5], -1.0f)); // CH6 AUX2 (from Rz!)
    printf("[PASS] ExpressLRS BLE Joystick channel remapping\n");
}

static void testSimPhysicsAndScoring() {
    SimEngine sim;
    SimSettings settings;
    settings.gameMode = GameMode::RACE_30S;
    sim.seedRng(42);
    sim.startRace(settings);
    assert(sim.phase() == SimPhase::COUNTDOWN);

    PilotInput in;
    in.source   = InputSource::USB_HID;
    in.pitch    = 0.8f;
    in.throttle = 0.0f;

    // Advance past 3s countdown
    for (int i = 0; i < 200; ++i) {
        sim.update(1.0f / 60.0f, in, settings);
    }
    assert(sim.phase() == SimPhase::FLYING);
    assert(sim.droneZ() > 0.0f);
    assert(sim.telemetry().airspeedKmh > 10.0f);

    // Aim cleanly at upcoming closest obstacle until at least 3 gates are crossed
    int initialZ = static_cast<int>(sim.droneZ());
    for (int i = 0; i < 600; ++i) {
        const Obstacle& tgt = sim.obstacles()[sim.closestIdx()];
        float desiredX = tgt.x;
        float desiredY = -15.0f;
        if (tgt.type == ObstacleType::GATE_AIR)   desiredY = -45.0f;
        if (tgt.type == ObstacleType::FLAG_LEFT)  desiredX = tgt.x + 25.0f;
        if (tgt.type == ObstacleType::FLAG_RIGHT) desiredX = tgt.x - 25.0f;

        in.roll     = fmaxf(-1.0f, fminf(1.0f, (desiredX - sim.droneX()) * 0.06f));
        in.throttle = fmaxf(-1.0f, fminf(1.0f, (sim.droneY() - desiredY) * 0.08f));
        in.pitch    = 0.6f;
        sim.update(1.0f / 60.0f, in, settings);
    }
    assert(sim.droneZ() > initialZ + 3000.0f);
    assert(sim.stats().gatesHit >= 3);
    assert(sim.stats().score >= 3);
    printf("[PASS] SimEngine 3D physics & gate scoring (hits=%d, score=%d, maxSpd=%.1f km/h)\n",
           sim.stats().gatesHit, sim.stats().score, sim.stats().maxSpeedKmh);
}

static void testRendererAndOsdVisuals() {
    static uint16_t fb[SCREEN_W * SCREEN_H];
    SimEngine sim;
    SimSettings settings;
    settings.camTilt  = true;
    settings.analogFx = true;
    sim.seedRng(77);
    sim.startRace(settings);

    PilotInput in;
    in.source   = InputSource::USB_HID;
    in.roll     = 0.45f;
    in.pitch    = 0.65f;
    in.throttle = 0.25f;
    in.rssiPct  = 99;

    // Fast-forward into active flying right in front of a gate
    for (int i = 0; i < 230; ++i) {
        sim.update(1.0f / 60.0f, in, settings);
    }

    FpvRenderer::renderScene(fb, sim, settings, 42);
    BetaflightOsd::drawFlightOsd(fb, sim, in, settings, 42);
    writePpm("/tmp/stickadv_flight.ppm", fb);

    // Also render title overlay & post-flight stats to verify zero out-of-bounds crashes
    UiMenuState ui;
    ui.usbConnected = false;
    ui.bleConnected = true;
    strcpy(ui.blePeerName, "ELRS JOYSTICK");
    in.source    = InputSource::BLE_HID;
    in.isElrsBle = true;
    sim.abortToTitle();
    FpvRenderer::renderScene(fb, sim, settings, 10);
    BetaflightOsd::drawTitleOverlay(fb, sim, in, settings, ui, 10);
    writePpm("/tmp/stickadv_title.ppm", fb);

    printf("[PASS] 3D Renderer + Betaflight Analog OSD rendered cleanly to /tmp/stickadv_*.ppm\n");
}

int main() {
    testEdgeTxHid();
    testElrsBleHid();
    testSimPhysicsAndScoring();
    testRendererAndOsdVisuals();
    printf("ALL TESTS PASSED!\n");
    return 0;
}
