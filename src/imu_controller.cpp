#include "imu_controller.h"
#include <math.h>

#ifdef ARDUINO
#include <M5Unified.h>
#endif

namespace {

inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

inline float applyDeadzone(float v, float dz) {
    if (fabsf(v) < dz) return 0.0f;
    return (v > 0.0f) ? ((v - dz) / (1.0f - dz)) : ((v + dz) / (1.0f - dz));
}

// Responsive handheld tilt range (full deflection at comfortable wrist angles)
constexpr float MAX_TILT_ROLL_DEG  = 22.0f;
constexpr float MAX_TILT_PITCH_DEG = 18.0f;

// Playable digital keyboard sensitivity: gentle peak throw + smooth ramp for
// micro-adjustments through gates, with crisp release so there is no drift.
constexpr float KB_ROLL_MAX      = 0.44f; // ~132 u/s at MED sensitivity
constexpr float KB_VERT_MAX      = 0.28f; // ~119 u/s at MED sensitivity
constexpr float KB_ROLL_RAMP     = 1.85f; // 0 -> KB_ROLL_MAX in ~0.24s
constexpr float KB_VERT_RAMP     = 1.30f; // 0 -> KB_VERT_MAX in ~0.22s
constexpr float KB_RELEASE_DECAY = 14.0f; // Returns to 0 in ~0.03s on key release

inline float stepKeyAxis(float current, bool posHeld, bool negHeld,
                         float maxVal, float rampRate, float dt) {
    if (posHeld && !negHeld) {
        if (current < 0.0f) current = 0.0f;
        return clampf(current + rampRate * dt, 0.0f, maxVal);
    }
    if (negHeld && !posHeld) {
        if (current > 0.0f) current = 0.0f;
        return clampf(current - rampRate * dt, -maxVal, 0.0f);
    }
    float decay = KB_RELEASE_DECAY * dt;
    if (fabsf(current) <= decay) return 0.0f;
    return (current > 0.0f) ? (current - decay) : (current + decay);
}

} // namespace

void ImuController::computeTiltAxes(float rawRollDeg, float rawPitchDeg,
                                    float zeroRollDeg, float zeroPitchDeg,
                                    bool invertY, float& outRoll, float& outTilt) {
    // Cardputer-Adv BMI270 X and Y axes are negated relative to pilot/screen frame:
    //  - tilting right  -> positive roll (+1.0)
    //  - tilting up     -> positive tilt (+1.0 = climb in TILT_ALT)
    float relRoll  = -(rawRollDeg  - zeroRollDeg);
    float relPitch = -(rawPitchDeg - zeroPitchDeg);
    if (invertY) relPitch = -relPitch;

    outRoll = applyDeadzone(clampf(relRoll  / MAX_TILT_ROLL_DEG,  -1.0f, 1.0f), 0.07f);
    outTilt = applyDeadzone(clampf(relPitch / MAX_TILT_PITCH_DEG, -1.0f, 1.0f), 0.07f);
}

bool ImuController::begin() {
#ifdef ARDUINO
    _imuPresent = M5.Imu.isEnabled();
    if (_imuPresent) {
        M5.Imu.update();
        float ax = 0, ay = 0, az = 1;
        M5.Imu.getAccel(&ax, &ay, &az);
        _rollDeg  = atan2f(ax, sqrtf(ay * ay + az * az)) * 180.0f / static_cast<float>(M_PI);
        _pitchDeg = atan2f(ay, sqrtf(ax * ax + az * az)) * 180.0f / static_cast<float>(M_PI);
        _zeroRollDeg  = _rollDeg;
        _zeroPitchDeg = _pitchDeg;
    }
#else
    _imuPresent = false;
#endif
    return _imuPresent;
}

void ImuController::calibrateNeutral() {
    _zeroRollDeg  = _rollDeg;
    _zeroPitchDeg = _pitchDeg;
}

void ImuController::update(float dt, const KeyFlightState& keys, const SimSettings& settings,
                           PilotInput& outInput) {
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f)   dt = 0.1f;

    float imuRoll = 0.0f;
    float imuTilt = 0.0f;

#ifdef ARDUINO
    if (_imuPresent) {
        M5.Imu.update();
        float ax = 0, ay = 0, az = 1;
        float gx = 0, gy = 0, gz = 0;
        M5.Imu.getAccel(&ax, &ay, &az);
        M5.Imu.getGyro(&gx, &gy, &gz);

        float accRoll  = atan2f(ax, sqrtf(ay * ay + az * az)) * 180.0f / static_cast<float>(M_PI);
        float accPitch = atan2f(ay, sqrtf(ax * ax + az * az)) * 180.0f / static_cast<float>(M_PI);

        // Complementary filter: snappy low-lag gyro + drift-free gravity reference
        constexpr float ALPHA = 0.85f;
        _rollDeg  = ALPHA * (_rollDeg  - gy * dt) + (1.0f - ALPHA) * accRoll;
        _pitchDeg = ALPHA * (_pitchDeg + gx * dt) + (1.0f - ALPHA) * accPitch;

        if (settings.imuEnabled) {
            computeTiltAxes(_rollDeg, _pitchDeg, _zeroRollDeg, _zeroPitchDeg,
                            settings.imuInvertY, imuRoll, imuTilt);
        }
    }
#endif

    // Arrow Keys (smooth, controlled sensitivity; takes priority over IMU when pressed)
    bool anyHorizKey = keys.left || keys.right;
    bool anyVertKey  = keys.up   || keys.down;

    _kbRoll = stepKeyAxis(_kbRoll, keys.right, keys.left, KB_ROLL_MAX, KB_ROLL_RAMP, dt);
    _kbVert = stepKeyAxis(_kbVert, keys.up,    keys.down, KB_VERT_MAX, KB_VERT_RAMP, dt);

    bool useKbRoll = anyHorizKey || (fabsf(_kbRoll) > 0.001f);
    bool useKbVert = anyVertKey  || (fabsf(_kbVert) > 0.001f);

    float finalRoll  = useKbRoll ? _kbRoll : imuRoll;
    float finalThr   = 0.0f;
    float finalPitch = 0.42f; // Brisk auto-cruise forward speed

    if (settings.imuMode == ImuMode::TILT_ALT || !settings.imuEnabled || !_imuPresent) {
        // Intuitive Handheld / Arrow-Key Mode:
        //   Left/Right (arrows or tilt) -> X steering
        //   Up/Down    (arrows or tilt) -> Climb / Dive altitude
        //   Shift                       -> Brake
        finalThr = useKbVert ? _kbVert : imuTilt;
        if (keys.brake) finalPitch = 0.08f;
    } else {
        // Classic TILT_SPD mode:
        //   Tilt forward/back -> Forward Speed (pitch)
        //   Up/Down arrows    -> Altitude (throttle)
        finalThr   = _kbVert;
        finalPitch = clampf(0.42f - imuTilt * 0.58f, 0.02f, 1.0f);
        if (keys.brake) finalPitch = 0.05f;
    }

    outInput.roll      = clampf(finalRoll, -1.0f, 1.0f);
    outInput.pitch     = clampf(finalPitch, -0.2f, 1.0f);
    outInput.throttle  = clampf(finalThr, -1.0f, 1.0f);
    outInput.yaw       = 0.0f;
    outInput.armSwitch = false;
    outInput.armEdge   = false;
    outInput.rawCh[0]  = outInput.roll;
    outInput.rawCh[1]  = outInput.pitch;
    outInput.rawCh[2]  = outInput.throttle;
    outInput.rawCh[3]  = 0.0f;
    outInput.buttons   = 0;
    outInput.source    = InputSource::IMU_KB;
    outInput.rssiPct   = 99;
    outInput.latencyMs = 0;
    outInput.isElrsBle = false;
}
