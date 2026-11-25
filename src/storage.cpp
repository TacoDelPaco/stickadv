#include "storage.h"

#ifdef ARDUINO
#include <Preferences.h>

namespace {
static Preferences g_prefs;
static bool        g_prefsReady = false;
constexpr const char* NS_NAME = "fpv_sim";
} // namespace

void SimStorage::begin() {
    g_prefsReady = g_prefs.begin(NS_NAME, false);
}

int SimStorage::loadBestScore() {
    if (!g_prefsReady) return 0;
    return g_prefs.getInt("best_score", 0);
}

void SimStorage::saveBestScore(int score) {
    if (!g_prefsReady) return;
    g_prefs.putInt("best_score", score);
}

void SimStorage::loadSettings(SimSettings& s) {
    if (!g_prefsReady) return;
    uint8_t gm = g_prefs.getUChar("game_mode", static_cast<uint8_t>(s.gameMode));
    uint8_t cm = g_prefs.getUChar("ch_map",    static_cast<uint8_t>(s.channelMap));
    uint8_t rp = g_prefs.getUChar("rates",     static_cast<uint8_t>(s.rates));
    uint8_t ob = g_prefs.getUChar("osd_bat",   static_cast<uint8_t>(s.osdBattery));
    uint8_t im = g_prefs.getUChar("imu_mode",  static_cast<uint8_t>(s.imuMode));

    if (gm < static_cast<uint8_t>(GameMode::COUNT))         s.gameMode   = static_cast<GameMode>(gm);
    if (cm < static_cast<uint8_t>(ChannelMap::COUNT))       s.channelMap = static_cast<ChannelMap>(cm);
    if (rp < static_cast<uint8_t>(RateProfile::COUNT))      s.rates      = static_cast<RateProfile>(rp);
    if (ob < static_cast<uint8_t>(OsdBatterySource::COUNT)) s.osdBattery = static_cast<OsdBatterySource>(ob);
    if (im < static_cast<uint8_t>(ImuMode::COUNT))          s.imuMode    = static_cast<ImuMode>(im);

    s.camTilt    = g_prefs.getBool("cam_tilt",  s.camTilt);
    s.analogFx   = g_prefs.getBool("analog_fx", s.analogFx);
    s.imuEnabled = g_prefs.getBool("imu_on",    s.imuEnabled);
    s.imuInvertY = g_prefs.getBool("imu_invy",  s.imuInvertY);
    uint8_t vol  = g_prefs.getUChar("vol_pct",  s.volumePct);
    s.volumePct  = (vol > 100) ? 20 : vol;
}

void SimStorage::saveSettings(const SimSettings& s) {
    if (!g_prefsReady) return;
    g_prefs.putUChar("game_mode", static_cast<uint8_t>(s.gameMode));
    g_prefs.putUChar("ch_map",    static_cast<uint8_t>(s.channelMap));
    g_prefs.putUChar("rates",     static_cast<uint8_t>(s.rates));
    g_prefs.putUChar("osd_bat",   static_cast<uint8_t>(s.osdBattery));
    g_prefs.putUChar("imu_mode",  static_cast<uint8_t>(s.imuMode));
    g_prefs.putBool("cam_tilt",   s.camTilt);
    g_prefs.putBool("analog_fx",  s.analogFx);
    g_prefs.putBool("imu_on",     s.imuEnabled);
    g_prefs.putBool("imu_invy",   s.imuInvertY);
    g_prefs.putUChar("vol_pct",   s.volumePct);
}

#else
void SimStorage::begin() {}
int  SimStorage::loadBestScore() { return 0; }
void SimStorage::saveBestScore(int) {}
void SimStorage::loadSettings(SimSettings&) {}
void SimStorage::saveSettings(const SimSettings&) {}
#endif
