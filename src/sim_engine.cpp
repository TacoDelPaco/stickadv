#include "sim_engine.h"
#include <math.h>
#include <string.h>

namespace {

inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Original EdgeTX simulator.lua constants normalized to ~10 Hz reference tick rate
constexpr float TICK_RATE       = 10.0f;
constexpr float ROLL_SCALE      = 28.0f;
constexpr float PITCH_SCALE     = 5.0f;
constexpr float THROTTLE_SCALE  = 24.0f;
constexpr float MIN_SPEED_TICK  = 3.0f;
constexpr float STICK_FULL      = 1024.0f;

} // namespace

SimEngine::SimEngine() {
    // Initialize demo obstacles for live attract-mode background on Title screen
    _objectCounter = 0;
    for (int i = 0; i < MAX_OBSTACLES; ++i) {
        _obstacles[i] = spawnNextObstacle();
    }
    _droneX = 0.0f;
    _droneY = -28.0f;
    _droneZ = 0.0f;
}

void SimEngine::setBestScore(int best) {
    if (best > _stats.bestScore) {
        _stats.bestScore = best;
    }
}

uint32_t SimEngine::nextRand() {
    // Fast xorshift32
    uint32_t x = _rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rngState = x;
    return x;
}

float SimEngine::applyExpo(float v, float expo) const {
    float c = clampf(v, -1.0f, 1.0f);
    return c * (1.0f - expo) + (c * c * c) * expo;
}

float SimEngine::rateMultiplier(RateProfile r) const {
    switch (r) {
        case RateProfile::RATE_LOW:  return 0.72f;
        case RateProfile::RATE_HIGH: return 1.32f;
        case RateProfile::RATE_MID:
        default:                     return 1.00f;
    }
}

void SimEngine::pushAudio(AudioEvent ev) {
    int next = (_audioHead + 1) & 7;
    if (next != _audioTail) {
        _audioQueue[_audioHead] = ev;
        _audioHead = next;
    }
}

AudioEvent SimEngine::popAudioEvent() {
    if (_audioTail == _audioHead) return AudioEvent::NONE;
    AudioEvent ev = _audioQueue[_audioTail];
    _audioTail = (_audioTail + 1) & 7;
    return ev;
}

void SimEngine::setBanner(const char* text, float duration) {
    strncpy(_statusBanner, text, sizeof(_statusBanner) - 1);
    _statusBanner[sizeof(_statusBanner) - 1] = '\0';
    _statusBannerTimer = duration;
}

Obstacle SimEngine::spawnNextObstacle() {
    ++_objectCounter;
    Obstacle ob;
    ob.id = _objectCounter;
    ob.z  = static_cast<float>(_objectCounter) * Z_STEP;
    ob.y  = 0.0f;

    // Random x in [-TRACK_W, +TRACK_W] matching simulator.lua
    float rx = (static_cast<float>(nextRand() % 1001) / 500.0f - 1.0f) * TRACK_W;
    uint32_t typeId = (nextRand() % 6) + 1; // 1..6

    if (typeId == 1 || typeId == 2) {
        ob.type = ObstacleType::GATE_GROUND;
        ob.x    = rx;
    } else if (typeId == 3 || typeId == 4) {
        ob.type = ObstacleType::GATE_AIR;
        ob.x    = rx;
    } else if (typeId == 5) {
        ob.type = ObstacleType::FLAG_RIGHT;
        ob.x    = -fabsf(rx) - TRACK_W;
    } else {
        ob.type = ObstacleType::FLAG_LEFT;
        ob.x    = fabsf(rx) + TRACK_W;
    }
    return ob;
}

void SimEngine::startRace(const SimSettings& settings) {
    _droneX = 0.0f;
    _droneY = -18.0f; // Start hovering slightly off ground for immediate clean view
    _droneZ = 0.0f;
    _speedX = 0.0f;
    _speedY = 0.0f;
    _speedZ = 0.0f;

    _objectCounter = 0;
    for (int i = 0; i < MAX_OBSTACLES; ++i) {
        _obstacles[i] = spawnNextObstacle();
    }
    _closestIdx = 0;

    int prevBest = _stats.bestScore;
    _stats = RunStats{};
    _stats.bestScore = prevBest;

    _countdownTimer    = COUNTDOWN_SEC;
    _countdownNum      = 3;
    _lastCountdownBeep = 4;
    _remainingSec      = RACE_DURATION;
    _goBannerTimer     = 0.0f;
    _hitFlashTimer     = 0.0f;
    _missFlashTimer    = 0.0f;
    _statusBannerTimer = 0.0f;
    _statusBanner[0]   = '\0';

    _telem.packVoltage   = 16.6f; // Freshly charged 4S LiPo (4.15V/cell)
    _telem.cellVoltage   = 4.15f;
    _telem.mahUsed       = 0.0f;
    _telem.rollRad       = 0.0f;
    _telem.pitchRad      = 0.0f;
    _telem.rfStaticLevel = 0.0f;
    _telem.armed         = true;

    if (settings.gameMode == GameMode::ENDLESS) {
        _phase = SimPhase::FLYING;
        _goBannerTimer = 1.0f;
        setBanner("FREE FLIGHT", 1.5f);
        pushAudio(AudioEvent::ARM_JINGLE);
    } else {
        _phase = SimPhase::COUNTDOWN;
        pushAudio(AudioEvent::ARM_JINGLE);
    }
}

void SimEngine::abortToTitle() {
    _phase = SimPhase::TITLE;
    _telem.armed = false;
    _telem.rfStaticLevel = 0.0f;
}

void SimEngine::notifyImuZeroed() {
    setBanner("IMU ZEROED", 1.2f);
    pushAudio(AudioEvent::IMU_CAL);
}

void SimEngine::notifyToast(const char* text, float duration) {
    setBanner(text, duration);
}

bool SimEngine::update(float dt, const PilotInput& input, const SimSettings& settings,
                       float realBatVolts) {
    if (dt < 0.0005f) dt = 0.0005f;
    if (dt > 0.1f)    dt = 0.1f;

    bool newBestAchieved = false;

    if (_hitFlashTimer > 0.0f)     _hitFlashTimer     = fmaxf(0.0f, _hitFlashTimer - dt);
    if (_missFlashTimer > 0.0f)    _missFlashTimer    = fmaxf(0.0f, _missFlashTimer - dt);
    if (_goBannerTimer > 0.0f)     _goBannerTimer     = fmaxf(0.0f, _goBannerTimer - dt);
    if (_statusBannerTimer > 0.0f) _statusBannerTimer = fmaxf(0.0f, _statusBannerTimer - dt);

    // ------------------------------------------------------------------------
    // TITLE / POSTFLIGHT ATTRACT AUTOPILOT BACKGROUND
    // ------------------------------------------------------------------------
    if (_phase == SimPhase::TITLE || _phase == SimPhase::POSTFLIGHT) {
        _demoTime += dt;
        _droneZ += 720.0f * dt;
        _droneX = sinf(_demoTime * 0.7f) * 32.0f;
        _droneY = -28.0f + sinf(_demoTime * 1.1f) * 14.0f;
        _telem.rollRad  = settings.camTilt ? (cosf(_demoTime * 0.7f) * 0.16f) : 0.0f;
        _telem.pitchRad = settings.camTilt ? 0.08f : 0.0f;
        _telem.armed    = false;
        _telem.airspeedKmh = 42.0f;
        _telem.altitudeM   = -_droneY * 0.15f;
        _telem.throttlePct = 0;
        _telem.rssiPct     = input.rssiPct;
        _telem.rfStaticLevel = 0.0f;

        if (settings.osdBattery == OsdBatterySource::REAL_BAT) {
            _telem.packVoltage = realBatVolts;
            _telem.cellVoltage = realBatVolts;
        } else {
            _telem.packVoltage = 16.4f;
            _telem.cellVoltage = 4.10f;
        }

        // Recycle obstacles ahead in attract mode
        float closestDist = _droneZ + Z_STEP * (MAX_OBSTACLES + 1);
        _closestIdx = 0;
        for (int i = 0; i < MAX_OBSTACLES; ++i) {
            if (_droneZ >= _obstacles[i].z) {
                _obstacles[i] = spawnNextObstacle();
            }
            if (_obstacles[i].z < closestDist && _obstacles[i].z > _droneZ) {
                closestDist = _obstacles[i].z;
                _closestIdx = i;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // COUNTDOWN PHASE (3 - 2 - 1 - GO!)
    // ------------------------------------------------------------------------
    if (_phase == SimPhase::COUNTDOWN) {
        _countdownTimer -= dt;
        int cnt = static_cast<int>(ceilf(_countdownTimer));
        if (cnt < 1) cnt = 1;
        _countdownNum = cnt;

        if (cnt < _lastCountdownBeep && cnt >= 1) {
            _lastCountdownBeep = cnt;
            pushAudio(AudioEvent::COUNTDOWN_BEEP);
        }

        // Let pilot preview camera tilt on sticks during countdown
        float targetRoll  = settings.camTilt ? (input.roll * 0.22f) : 0.0f;
        float targetPitch = settings.camTilt ? (input.pitch * 0.12f) : 0.0f;
        _telem.rollRad  += (targetRoll  - _telem.rollRad)  * clampf(dt * 12.0f, 0.0f, 1.0f);
        _telem.pitchRad += (targetPitch - _telem.pitchRad) * clampf(dt * 12.0f, 0.0f, 1.0f);
        _telem.throttlePct = static_cast<int>(clampf((input.throttle + 1.0f) * 50.0f, 0.0f, 100.0f));
        _telem.rssiPct     = input.rssiPct;
        _telem.altitudeM   = -_droneY * 0.15f;
        _telem.airspeedKmh = 0.0f;

        if (_countdownTimer <= 0.0f) {
            _phase = SimPhase::FLYING;
            _goBannerTimer = 1.0f;
            pushAudio(AudioEvent::RACE_GO);
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // FLYING PHASE (Active Race or Endless)
    // ------------------------------------------------------------------------
    _stats.flightTimeS += dt;
    if (settings.gameMode == GameMode::RACE_30S) {
        _remainingSec -= dt;
        if (_remainingSec <= 0.0f) {
            _remainingSec = 0.0f;
            if (_stats.score > _stats.bestScore) {
                _stats.bestScore = _stats.score;
                _stats.isNewBest = true;
                newBestAchieved  = true;
                pushAudio(AudioEvent::NEW_BEST);
            } else {
                pushAudio(AudioEvent::RACE_FINISH);
            }
            _phase = SimPhase::POSTFLIGHT;
            _telem.armed = false;
            return newBestAchieved;
        }
    }

    const float rate = rateMultiplier(settings.rates);
    const float rollCmd  = applyExpo(input.roll, 0.22f) * rate;
    const float pitchCmd = applyExpo(input.pitch, 0.15f) * rate;
    const float thrCmd   = clampf(input.throttle, -1.0f, 1.0f) * rate;

    // Faithful EdgeTX simulator.lua kinematics scaled by TICK_RATE * dt
    const float tickDt = TICK_RATE * dt;
    _speedX = (rollCmd * STICK_FULL) / ROLL_SCALE;
    _speedZ = (pitchCmd * STICK_FULL) / PITCH_SCALE + MIN_SPEED_TICK;
    _speedY = (thrCmd * STICK_FULL) / THROTTLE_SCALE;

    if (_speedZ < 0.0f) _speedZ = 0.0f;

    // Integrate altitude (y=0 is ground, negative y is UP)
    _droneY -= _speedY * tickDt;
    if (_droneY >= 0.0f) {
        _droneY = 0.0f;
        _speedZ = 0.0f;
        _speedX = 0.0f;
    }
    if (_droneY < -TRACK_H) {
        _droneY = -TRACK_H;
    }

    // Integrate horizontal & forward motion
    _droneZ += _speedZ * tickDt;
    _droneX += _speedX * tickDt;

    if (_droneX >  TRACK_W * 3.0f) _droneX =  TRACK_W * 3.0f;
    if (_droneX < -TRACK_W * 3.0f) _droneX = -TRACK_W * 3.0f;

    // Smooth camera roll/pitch tilt for immersive FPV horizon & Betaflight AHI
    float targetRoll  = settings.camTilt ? clampf(rollCmd * 0.34f, -0.45f, 0.45f) : 0.0f;
    float targetPitch = settings.camTilt ? clampf(pitchCmd * 0.20f, -0.25f, 0.25f) : 0.0f;
    if (_droneY >= -0.5f) {
        targetRoll  = 0.0f;
        targetPitch = 0.0f;
    }
    _telem.rollRad  += (targetRoll  - _telem.rollRad)  * clampf(dt * 14.0f, 0.0f, 1.0f);
    _telem.pitchRad += (targetPitch - _telem.pitchRad) * clampf(dt * 12.0f, 0.0f, 1.0f);

    // Obstacle collision & closest-target selection
    for (int i = 0; i < MAX_OBSTACLES; ++i) {
        if (_droneZ >= _obstacles[i].z) {
            bool success = false;
            const Obstacle& ob = _obstacles[i];

            switch (ob.type) {
                case ObstacleType::GATE_GROUND:
                    if (fabsf(ob.x - _droneX) <= GATE_W * 0.5f && _droneY > -GATE_H) {
                        success = true;
                    }
                    break;
                case ObstacleType::GATE_AIR:
                    if (fabsf(ob.x - _droneX) <= GATE_W * 0.5f &&
                        _droneY < -GATE_H && _droneY > -2.0f * GATE_H) {
                        success = true;
                    }
                    break;
                case ObstacleType::FLAG_LEFT:
                    if (ob.x < _droneX && _droneY > -2.0f * GATE_H) {
                        success = true;
                    }
                    break;
                case ObstacleType::FLAG_RIGHT:
                    if (ob.x > _droneX && _droneY > -2.0f * GATE_H) {
                        success = true;
                    }
                    break;
            }

            if (success) {
                ++_stats.score;
                ++_stats.gatesHit;
                ++_stats.streak;
                if (_stats.streak > _stats.maxStreak) _stats.maxStreak = _stats.streak;
                _hitFlashTimer = 0.22f;
                setBanner("GATE +1", 0.55f);
                pushAudio(AudioEvent::GATE_HIT);

                if (settings.gameMode == GameMode::ENDLESS && _stats.score > _stats.bestScore) {
                    _stats.bestScore = _stats.score;
                    _stats.isNewBest = true;
                    newBestAchieved  = true;
                }
            } else {
                --_stats.score;
                ++_stats.gatesMissed;
                _stats.streak = 0;
                _missFlashTimer = 0.35f;
                setBanner("MISS -1", 0.65f);
                pushAudio(AudioEvent::GATE_MISS);
            }

            _obstacles[i] = spawnNextObstacle();
        }
    }

    // Find closest upcoming obstacle
    float closestDist = _droneZ + Z_STEP * (MAX_OBSTACLES + 2);
    _closestIdx = 0;
    for (int i = 0; i < MAX_OBSTACLES; ++i) {
        if (_obstacles[i].z < closestDist && _obstacles[i].z > (_droneZ + _speedZ * tickDt)) {
            closestDist = _obstacles[i].z;
            _closestIdx = i;
        }
    }

    // Simulated Betaflight Telemetry
    float thrNorm = clampf((input.throttle + 1.0f) * 0.5f, 0.0f, 1.0f);
    if (_droneY >= -0.1f && input.throttle <= -0.8f) thrNorm = 0.0f;

    _telem.throttlePct = static_cast<int>(thrNorm * 100.0f + 0.5f);
    _telem.airspeedKmh = clampf(_speedZ * 0.58f, 0.0f, 145.0f);
    if (_telem.airspeedKmh > _stats.maxSpeedKmh) _stats.maxSpeedKmh = _telem.airspeedKmh;
    _telem.altitudeM   = -_droneY * 0.15f; // 80 units ≈ 12.0 meters

    _telem.currentAmps = 4.5f + thrNorm * thrNorm * 38.0f + fabsf(pitchCmd) * 12.0f;
    _telem.mahUsed    += (_telem.currentAmps * 1000.0f * dt) / 3600.0f;

    if (settings.osdBattery == OsdBatterySource::REAL_BAT) {
        _telem.packVoltage = realBatVolts;
        _telem.cellVoltage = realBatVolts;
        _telem.lowBattery  = (realBatVolts > 2.5f && realBatVolts < 3.45f);
    } else {
        // Realistic 4S LiPo discharge + dynamic throttle voltage sag
        float restingPack = clampf(16.6f - _telem.mahUsed * 0.0045f, 13.6f, 16.8f);
        float sag         = (_telem.currentAmps * 0.022f);
        float targetPack  = clampf(restingPack - sag, 12.8f, 16.8f);
        _telem.packVoltage += (targetPack - _telem.packVoltage) * clampf(dt * 4.0f, 0.0f, 1.0f);
        _telem.cellVoltage  = _telem.packVoltage * 0.25f;
        _telem.lowBattery   = (_telem.cellVoltage < 3.55f);
    }

    // Analog 5.8GHz RSSI & RF static simulation:
    // Drops slightly if pilot flies far outside track corridor or right after missing a gate
    float outOfBounds = fmaxf(0.0f, (fabsf(_droneX) - TRACK_W * 1.4f) / (TRACK_W * 1.6f));
    int simRssi = static_cast<int>(input.rssiPct - outOfBounds * 28.0f - (_missFlashTimer > 0.0f ? 18.0f : 0.0f));
    _telem.rssiPct = simRssi < 15 ? 15 : (simRssi > 99 ? 99 : simRssi);

    float targetStatic = 0.0f;
    if (settings.analogFx) {
        if (_missFlashTimer > 0.0f) {
            targetStatic = fmaxf(targetStatic, (_missFlashTimer / 0.35f) * 0.65f);
        }
        if (outOfBounds > 0.1f) {
            targetStatic = fmaxf(targetStatic, outOfBounds * 0.40f);
        }
    }
    _telem.rfStaticLevel = targetStatic;

    return newBestAchieved;
}
