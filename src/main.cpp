// ============================================================================
// StickADV — 3D FPV Racing Simulator (Port of EdgeTX Game-Simulator)
// Target: M5Stack Cardputer-Adv (ESP32-S3FN8, 240x135 ST7789V2, BMI270, ES8311)
//
// Features:
//  - Faithful EdgeTX Game-Simulator 3D FPV race mechanics at 60 FPS
//  - Native USB-OTG Host HID for EdgeTX radios (Classic, 2.11+, Advanced)
//  - Ultra-low-latency (7.5-11ms) BLE HID for ExpressLRS BLE Joystick & Gamepads
//  - Internal BMI270 6-axis IMU + simple Arrow-Key flight controls
//  - Full 3D colors, tilting FPV horizon, analog RF static & Betaflight OSD
// ============================================================================

#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <stdio.h>

#include "config.h"
#include "sim_engine.h"
#include "renderer.h"
#include "betaflight_osd.h"
#include "input_manager.h"
#include "audio_fx.h"
#include "storage.h"

namespace {

enum class AppScreen : uint8_t {
    SIM_VIEW  = 0,
    SETTINGS  = 1,
    STICK_MON = 2,
};

constexpr int SETTINGS_COUNT = 12;

M5Canvas      g_canvas(&M5Cardputer.Display);
uint16_t*     g_fb = nullptr;

SimEngine     g_sim;
SimSettings   g_settings;
PilotInput    g_input;
InputManager  g_inputMgr;
AudioFx       g_audio;
SimStorage    g_storage;
UiMenuState   g_ui;
AppScreen     g_screen = AppScreen::SIM_VIEW;

uint32_t      g_lastFrameUs   = 0;
uint32_t      g_frameCount    = 0;
uint32_t      g_lastBatPollMs = 0;
float         g_realBatVolts  = 4.1f;

bool keyPressedChar(const Keyboard_Class::KeysState& ks, char lower) {
    char upper = (lower >= 'a' && lower <= 'z') ? static_cast<char>(lower - 'a' + 'A') : lower;
    for (char c : ks.word) {
        if (c == lower || c == upper) return true;
    }
    return false;
}

KeyFlightState sampleFlightKeys() {
    KeyFlightState k;
    auto& kb = M5Cardputer.Keyboard;
    auto ks  = kb.keysState();

    // Support Cardputer arrow keys BOTH with Fn (ks.up/down/left/right)
    // AND without Fn (plain ';', '.', ',', '/'), plus WASD!
    k.up    = ks.up    || kb.isKeyPressed(';') || kb.isKeyPressed('w') || kb.isKeyPressed('W');
    k.down  = ks.down  || kb.isKeyPressed('.') || kb.isKeyPressed('s') || kb.isKeyPressed('S');
    k.left  = ks.left  || kb.isKeyPressed(',') || kb.isKeyPressed('a') || kb.isKeyPressed('A');
    k.right = ks.right || kb.isKeyPressed('/') || kb.isKeyPressed('d') || kb.isKeyPressed('D');
    k.boost = ks.space || kb.isKeyPressed(' ');
    k.brake = ks.shift;
    return k;
}

void adjustVolume(int delta) {
    int nv = static_cast<int>(g_settings.volumePct) + delta;
    if (nv < 0)   nv = 0;
    if (nv > 100) nv = 100;
    g_settings.volumePct = static_cast<uint8_t>(nv);
    g_audio.setVolumePct(g_settings.volumePct);
    g_storage.saveSettings(g_settings);

    char msg[20];
    if (g_settings.volumePct == 0) snprintf(msg, sizeof(msg), "VOL: MUTE");
    else                           snprintf(msg, sizeof(msg), "VOL: %d%%", g_settings.volumePct);
    g_sim.notifyToast(msg, 1.0f);
    g_audio.playVolumePip();
}

void cycleVolumePreset() {
    static const uint8_t kSteps[] = {0, 10, 20, 35, 50, 75, 100};
    for (size_t i = 0; i < sizeof(kSteps); ++i) {
        if (g_settings.volumePct < kSteps[i]) {
            g_settings.volumePct = kSteps[i];
            g_audio.setVolumePct(g_settings.volumePct);
            g_storage.saveSettings(g_settings);
            g_audio.playVolumePip();
            return;
        }
    }
    g_settings.volumePct = 0;
    g_audio.setVolumePct(0);
    g_storage.saveSettings(g_settings);
}

void cycleSettingItem(int idx) {
    switch (idx) {
        case 0:
            cycleVolumePreset();
            return;
        case 1:
            g_settings.gameMode = static_cast<GameMode>(
                (static_cast<uint8_t>(g_settings.gameMode) + 1) % static_cast<uint8_t>(GameMode::COUNT));
            break;
        case 2:
            g_settings.imuEnabled = !g_settings.imuEnabled;
            break;
        case 3:
            g_settings.imuMode = static_cast<ImuMode>(
                (static_cast<uint8_t>(g_settings.imuMode) + 1) % static_cast<uint8_t>(ImuMode::COUNT));
            break;
        case 4:
            g_settings.imuInvertY = !g_settings.imuInvertY;
            break;
        case 5:
            g_settings.channelMap = static_cast<ChannelMap>(
                (static_cast<uint8_t>(g_settings.channelMap) + 1) % static_cast<uint8_t>(ChannelMap::COUNT));
            break;
        case 6:
            g_settings.rates = static_cast<RateProfile>(
                (static_cast<uint8_t>(g_settings.rates) + 1) % static_cast<uint8_t>(RateProfile::COUNT));
            break;
        case 7:
            g_settings.camTilt = !g_settings.camTilt;
            break;
        case 8:
            g_settings.analogFx = !g_settings.analogFx;
            break;
        case 9:
            g_settings.osdBattery = static_cast<OsdBatterySource>(
                (static_cast<uint8_t>(g_settings.osdBattery) + 1) % static_cast<uint8_t>(OsdBatterySource::COUNT));
            break;
        case 10:
            if (g_inputMgr.isBleConnected()) {
                g_inputMgr.disconnectBle();
            } else if (g_inputMgr.isBleScanning()) {
                g_inputMgr.stopBleScan();
            } else {
                g_inputMgr.startBleScan();
            }
            return;
        case 11:
            g_inputMgr.clearBleBonds();
            return;
        default:
            return;
    }
    g_storage.saveSettings(g_settings);
}

void handleDiscreteKeys() {
    bool btnAClick = M5Cardputer.BtnA.wasPressed();
    bool kbChanged = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
    auto ks        = M5Cardputer.Keyboard.keysState();

    bool hitEnter = btnAClick || (kbChanged && ks.enter);
    bool hitEsc   = kbChanged && (ks.esc || ks.del || ks.backspace || keyPressedChar(ks, '`'));
    bool hitUp    = kbChanged && (ks.up    || keyPressedChar(ks, ';') || keyPressedChar(ks, 'w'));
    bool hitDown  = kbChanged && (ks.down  || keyPressedChar(ks, '.') || keyPressedChar(ks, 's'));
    bool hitRight = kbChanged && (ks.right || keyPressedChar(ks, '/') || keyPressedChar(ks, 'd') || ks.space);
    bool hitM     = kbChanged && (keyPressedChar(ks, 'm') || ks.tab);
    bool hitK     = kbChanged && keyPressedChar(ks, 'k');
    bool hitC     = kbChanged && keyPressedChar(ks, 'c');
    bool hitB     = kbChanged && keyPressedChar(ks, 'b');
    bool hitI     = kbChanged && keyPressedChar(ks, 'i');
    bool hitX     = kbChanged && keyPressedChar(ks, 'x');
    bool hitVolDn = kbChanged && (keyPressedChar(ks, '-') || keyPressedChar(ks, '_'));
    bool hitVolUp = kbChanged && (keyPressedChar(ks, '=') || keyPressedChar(ks, '+'));

    // Global instant volume control (- / =)
    if (hitVolDn) adjustVolume(-10);
    if (hitVolUp) adjustVolume(+10);

    // Global quick IMU Zero ('C') works on any screen
    if (hitC) {
        g_inputMgr.calibrateImu();
        g_sim.notifyImuZeroed();
    }

    // Global quick IMU ON/OFF toggle ('I') for instant Arrow-Key testing
    if (hitI && g_screen != AppScreen::SETTINGS) {
        g_settings.imuEnabled = !g_settings.imuEnabled;
        g_storage.saveSettings(g_settings);
        g_sim.notifyToast(g_settings.imuEnabled ? "IMU: ON" : "IMU: OFF (KEYS)", 1.2f);
    }

    // Global BLE scan shortcut ('B') when not mid-flight
    if (hitB && (g_screen != AppScreen::SIM_VIEW ||
                 g_sim.phase() == SimPhase::TITLE ||
                 g_sim.phase() == SimPhase::POSTFLIGHT)) {
        if (g_inputMgr.isBleConnected()) {
            g_inputMgr.disconnectBle();
        } else if (g_inputMgr.isBleScanning()) {
            g_inputMgr.stopBleScan();
        } else {
            g_inputMgr.startBleScan();
        }
    }

    if (g_screen == AppScreen::SETTINGS) {
        if (hitUp) {
            g_ui.selectedItem = (g_ui.selectedItem + SETTINGS_COUNT - 1) % SETTINGS_COUNT;
        } else if (hitDown) {
            g_ui.selectedItem = (g_ui.selectedItem + 1) % SETTINGS_COUNT;
        } else if (hitEnter || hitRight) {
            cycleSettingItem(g_ui.selectedItem);
        } else if (hitEsc || hitM) {
            g_screen = AppScreen::SIM_VIEW;
        }
        return;
    }

    if (g_screen == AppScreen::STICK_MON) {
        if (hitX) {
            g_inputMgr.clearBleBonds();
        } else if (hitEsc || hitK || hitM) {
            g_screen = AppScreen::SIM_VIEW;
        }
        return;
    }

    // Main Simulator Screen
    SimPhase ph = g_sim.phase();
    if (ph == SimPhase::TITLE || ph == SimPhase::POSTFLIGHT) {
        if (hitEnter || g_input.armEdge) {
            g_sim.startRace(g_settings);
        } else if (hitM) {
            g_screen = AppScreen::SETTINGS;
        } else if (hitK) {
            g_screen = AppScreen::STICK_MON;
        } else if (hitEsc && ph == SimPhase::POSTFLIGHT) {
            g_sim.abortToTitle();
        }
    } else {
        // COUNTDOWN or FLYING
        if (hitEsc) {
            g_sim.abortToTitle();
        } else if (hitM) {
            g_sim.abortToTitle();
            g_screen = AppScreen::SETTINGS;
        }
    }
}

void refreshUiTelemetry() {
    uint32_t now = millis();
    if (now - g_lastBatPollMs > 500) {
        g_lastBatPollMs = now;
        int32_t mv = M5Cardputer.Power.getBatteryVoltage();
        if (mv > 2000 && mv < 5500) {
            g_realBatVolts = static_cast<float>(mv) * 0.001f;
        }
        g_ui.realBatPct = M5Cardputer.Power.getBatteryLevel();
    }

    g_ui.usbConnected = g_inputMgr.isUsbConnected();
    g_ui.bleConnected = g_inputMgr.isBleConnected();
    g_ui.bleScanning  = g_inputMgr.isBleScanning();
    g_ui.imuPresent   = g_inputMgr.isImuPresent();
    g_ui.imuRollDeg   = g_inputMgr.imuRollDeg();
    g_ui.imuPitchDeg  = g_inputMgr.imuPitchDeg();
    g_ui.usbPkts      = g_inputMgr.usbPacketCount();
    g_ui.blePkts      = g_inputMgr.blePacketCount();
    g_inputMgr.getUsbLabel(g_ui.usbDevName, sizeof(g_ui.usbDevName));
    g_inputMgr.getBleLabel(g_ui.blePeerName, sizeof(g_ui.blePeerName));
    g_inputMgr.getUsbStatus(g_ui.usbStatus, sizeof(g_ui.usbStatus));
    g_inputMgr.getBleStatus(g_ui.bleStatus, sizeof(g_ui.bleStatus));
}

} // namespace

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_spk = true;
    M5Cardputer.begin(cfg, true);

    // M5GFX 0.3.0 sets Cardputer-Adv's default panel offset to landscape at rotation 0
    M5Cardputer.Display.setRotation(0);
    if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) {
        M5Cardputer.Display.setRotation(1);
    }
    M5Cardputer.Display.setBrightness(220);
    M5Cardputer.Display.fillScreen(TFT_BLACK);

    // Allocate 240x135 16-bit RGB565 sprite in internal SRAM (64.8 KB, DMA-ready)
    g_canvas.setColorDepth(16);
    g_canvas.createSprite(SCREEN_W, SCREEN_H);
    g_fb = reinterpret_cast<uint16_t*>(g_canvas.getBuffer());

    // Load saved settings & high score from NVS
    g_storage.begin();
    g_storage.loadSettings(g_settings);
    g_sim.setBestScore(g_storage.loadBestScore());
    g_sim.seedRng(esp_random());

    // Initialize gentle audio & Tri-mode input stack (BMI270 IMU + USB-OTG Host + NimBLE)
    g_audio.begin(g_settings.volumePct);
    g_inputMgr.begin();

    g_lastFrameUs = micros();
}

void loop() {
    // Frame pacing toward 60 FPS (16,667 us per frame)
    uint32_t nowUs = micros();
    uint32_t dtUs  = nowUs - g_lastFrameUs;
    if (dtUs < 14000) {
        delayMicroseconds(14000 - dtUs);
        nowUs = micros();
        dtUs  = nowUs - g_lastFrameUs;
    }
    g_lastFrameUs = nowUs;
    float dt = static_cast<float>(dtUs) * 1e-6f;
    if (dt > 0.08f) dt = 0.08f;

    // 1. Poll Cardputer-Adv keyboard & button hardware
    M5Cardputer.update();

    // 2. Sample unified inputs (Priority: USB EdgeTX > BLE ELRS/HID > BMI270 IMU + Arrows)
    KeyFlightState flightKeys = sampleFlightKeys();
    bool inActiveFlight = (g_screen == AppScreen::SIM_VIEW &&
                           (g_sim.phase() == SimPhase::COUNTDOWN ||
                            g_sim.phase() == SimPhase::FLYING));
    g_inputMgr.update(dt, flightKeys, g_settings, inActiveFlight, g_input);

    // 3. Process discrete UI / Arm / Volume / Menu keys
    handleDiscreteKeys();

    // 4. Advance EdgeTX Game-Simulator 3D FPV physics & telemetry
    refreshUiTelemetry();
    if (g_sim.update(dt, g_input, g_settings, g_realBatVolts)) {
        g_storage.saveBestScore(g_sim.getBestScore());
    }

    // 5. Pump non-blocking ES8311 speaker audio cues
    AudioEvent ev;
    while ((ev = g_sim.popAudioEvent()) != AudioEvent::NONE) {
        g_audio.play(ev);
    }
    g_audio.update();

    // 6. Render 3D Colored FPV World + Authentic Betaflight Analog OSD into RGB565 sprite
    if (g_fb) {
        FpvRenderer::renderScene(g_fb, g_sim, g_settings, g_frameCount);

        if (g_screen == AppScreen::SETTINGS) {
            BetaflightOsd::drawSettingsMenu(g_fb, g_settings, g_ui);
        } else if (g_screen == AppScreen::STICK_MON) {
            BetaflightOsd::drawStickMonitor(g_fb, g_input, g_settings, g_ui);
        } else {
            switch (g_sim.phase()) {
                case SimPhase::TITLE:
                    BetaflightOsd::drawTitleOverlay(g_fb, g_sim, g_input, g_settings, g_ui, g_frameCount);
                    break;
                case SimPhase::POSTFLIGHT:
                    BetaflightOsd::drawPostFlightStats(g_fb, g_sim, g_input, g_settings, g_frameCount);
                    break;
                case SimPhase::COUNTDOWN:
                case SimPhase::FLYING:
                default:
                    BetaflightOsd::drawFlightOsd(g_fb, g_sim, g_input, g_settings, g_frameCount);
                    break;
            }
        }

        // Single DMA SPI push to ST7789V2
        g_canvas.pushSprite(0, 0);
    }

    ++g_frameCount;
}
