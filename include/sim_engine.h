#pragma once

#include "config.h"
#include <stdint.h>

// ============================================================================
// Core EdgeTX Game-Simulator 3D FPV Physics & Race Engine
// Ported from Alexey Stankevich's OpenTX/EdgeTX Game-simulator.lua
// with 60 FPS delta-time integration, camera roll/pitch dynamics,
// simulated Betaflight telemetry, and audio event queue.
// ============================================================================

enum class ObstacleType : uint8_t {
    GATE_GROUND = 0,
    GATE_AIR    = 1,
    FLAG_LEFT   = 2,
    FLAG_RIGHT  = 3,
};

struct Obstacle {
    float        x = 0.0f;
    float        y = 0.0f;
    float        z = 0.0f;
    ObstacleType type = ObstacleType::GATE_GROUND;
    uint32_t     id   = 0;
};

enum class SimPhase : uint8_t {
    TITLE      = 0,
    COUNTDOWN  = 1,
    FLYING     = 2,
    POSTFLIGHT = 3,
};

enum class AudioEvent : uint8_t {
    NONE = 0,
    ARM_JINGLE,
    COUNTDOWN_BEEP,
    RACE_GO,
    GATE_HIT,
    GATE_MISS,
    RACE_FINISH,
    NEW_BEST,
    IMU_CAL,
};

struct BetaflightTelemetry {
    float    packVoltage   = 16.4f;  // Simulated 4S LiPo (V) or real battery
    float    cellVoltage   = 4.10f;  // Per-cell voltage
    float    currentAmps   = 0.0f;   // Simulated amp draw
    float    mahUsed       = 0.0f;   // mAh consumed during flight
    float    airspeedKmh   = 0.0f;   // Virtual FPV airspeed (km/h)
    float    altitudeM     = 0.0f;   // Altitude in meters
    int      throttlePct   = 0;      // 0..100%
    float    rollRad       = 0.0f;   // Visual camera / horizon roll angle (radians)
    float    pitchRad      = 0.0f;   // Visual camera / horizon pitch angle (radians)
    int      rssiPct       = 99;     // Simulated 5.8GHz / ELRS link quality (0..99)
    float    rfStaticLevel = 0.0f;   // 0..1 analog video noise/glitch intensity
    bool     lowBattery    = false;
    bool     armed         = false;
};

struct RunStats {
    int   score        = 0;
    int   bestScore    = 0;
    int   gatesHit     = 0;
    int   gatesMissed  = 0;
    int   streak       = 0;
    int   maxStreak    = 0;
    float maxSpeedKmh  = 0.0f;
    float flightTimeS  = 0.0f;
    bool  isNewBest    = false;
};

class SimEngine {
public:
    static constexpr int   MAX_OBSTACLES = 4;
    static constexpr float GATE_W        = 30.0f;
    static constexpr float GATE_H        = 30.0f;
    static constexpr float FLAG_W        = 6.0f;
    static constexpr float FLAG_H        = 30.0f;
    static constexpr float TRACK_W       = 50.0f;
    static constexpr float TRACK_H       = 80.0f;
    static constexpr float Z_STEP        = 1500.0f;
    static constexpr float Z_SCALE       = 600.0f;  // Scaled for 240x135 (2x of 128x64)
    static constexpr float RACE_DURATION = 30.0f;
    static constexpr float COUNTDOWN_SEC = 3.0f;

    SimEngine();

    void setBestScore(int best);
    int  getBestScore() const { return _stats.bestScore; }

    void startRace(const SimSettings& settings);
    void abortToTitle();
    void notifyImuZeroed();
    void notifyToast(const char* text, float duration = 1.1f);

    // Advance physics by dt seconds. Returns true if a new best score was just saved.
    bool update(float dt, const PilotInput& input, const SimSettings& settings,
                float realBatVolts = 4.1f);

    // Pop next queued audio cue (returns AudioEvent::NONE when empty)
    AudioEvent popAudioEvent();

    // State getters for renderer & OSD
    SimPhase                 phase() const      { return _phase; }
    float                    droneX() const     { return _droneX; }
    float                    droneY() const     { return _droneY; }
    float                    droneZ() const     { return _droneZ; }
    float                    speedZ() const     { return _speedZ; }
    const Obstacle*          obstacles() const  { return _obstacles; }
    int                      closestIdx() const { return _closestIdx; }
    float                    remainingSec() const { return _remainingSec; }
    int                      countdownNum() const { return _countdownNum; }
    float                    goBannerTimer() const { return _goBannerTimer; }
    float                    hitFlashTimer() const { return _hitFlashTimer; }
    float                    missFlashTimer() const { return _missFlashTimer; }
    const char*              statusBanner() const { return _statusBanner; }
    float                    statusBannerTimer() const { return _statusBannerTimer; }
    const BetaflightTelemetry& telemetry() const { return _telem; }
    const RunStats&          stats() const      { return _stats; }

    // Seed PRNG (deterministic in unit tests)
    void seedRng(uint32_t seed) { _rngState = seed ? seed : 0x12345678u; }

private:
    void       pushAudio(AudioEvent ev);
    void       setBanner(const char* text, float duration);
    Obstacle   spawnNextObstacle();
    uint32_t   nextRand();
    float      applyExpo(float v, float expo) const;
    float      rateMultiplier(RateProfile r) const;

    SimPhase   _phase = SimPhase::TITLE;

    // Drone world coordinates (EdgeTX convention: y=0 is ground, y<0 is altitude UP)
    float      _droneX = 0.0f;
    float      _droneY = -20.0f;
    float      _droneZ = 0.0f;
    float      _speedX = 0.0f;
    float      _speedY = 0.0f;
    float      _speedZ = 0.0f;

    Obstacle   _obstacles[MAX_OBSTACLES];
    uint32_t   _objectCounter = 0;
    int        _closestIdx    = 0;

    float      _countdownTimer    = 0.0f;
    int        _countdownNum      = 3;
    int        _lastCountdownBeep = 4;
    float      _remainingSec      = RACE_DURATION;
    float      _goBannerTimer     = 0.0f;
    float      _hitFlashTimer     = 0.0f;
    float      _missFlashTimer    = 0.0f;

    char       _statusBanner[24]  = "";
    float      _statusBannerTimer = 0.0f;

    BetaflightTelemetry _telem;
    RunStats            _stats;

    // Small ring buffer of audio events
    AudioEvent _audioQueue[8] = {};
    int        _audioHead     = 0;
    int        _audioTail     = 0;

    uint32_t   _rngState      = 0xA5A51234u;
    float      _demoTime      = 0.0f;
};
