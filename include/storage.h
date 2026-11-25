#pragma once

#include "config.h"

// ============================================================================
// Persistent Storage (ESP32 NVS Preferences)
// Saves Best Score and all Betaflight OSD / Input / Rate settings across reboots.
// ============================================================================

class SimStorage {
public:
    void begin();
    int  loadBestScore();
    void saveBestScore(int score);
    void loadSettings(SimSettings& outSettings);
    void saveSettings(const SimSettings& settings);
};
