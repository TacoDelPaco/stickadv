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

} // namespace

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
            // Cardputer-Adv BMI270 X-axis is negated relative to screen right:
            // tilting Cardputer RIGHT must produce positive roll (+1.0)!
            float relRoll  = -(_rollDeg - _zeroRollDeg);
            // Tilting top of Cardputer toward user (nose up) -> positive (+1.0 = climb up)
            float relPitch =  (_pitchDeg - _zeroPitchDeg);
            if (settings.imuInvertY) relPitch = -relPitch;

            imuRoll = applyDeadzone(clampf(relRoll  / MAX_TILT_ROLL_DEG,  -1.0f, 1.0f), 0.07f);
            imuTilt = applyDeadzone(clampf(relPitch / MAX_TILT_PITCH_DEG, -1.0f, 1.0f), 0.07f);
        }
    }
#endif

    // Arrow Keys (always super crisp & take priority over IMU when held!)
    bool anyHorizKey = keys.left || keys.right;
    bool anyVertKey  = keys.up   || keys.down;

    float kbRoll = (keys.right ? 0.90f : 0.0f) - (keys.left ? 0.90f : 0.0f);
    float kbVert = (keys.up    ? 0.90f : 0.0f) - (keys.down ? 0.90f : 0.0f);

    float finalRoll = anyHorizKey ? kbRoll : imuRoll;
    float finalThr  = 0.0f;
    float finalPitch = 0.42f; // Brisk auto-cruise forward speed (~65 km/h)

    if (settings.imuMode == ImuMode::TILT_ALT || !settings.imuEnabled || !_imuPresent) {
        // Intuitive Handheld / Arrow-Key Mode:
        //   Left/Right (arrows or tilt) -> X steering
        //   Up/Down    (arrows or tilt) -> Climb / Dive altitude
        //   Space / Shift               -> Turbo forward / Slow brake
        finalThr = anyVertKey ? kbVert : imuTilt;
        if (keys.boost)      finalPitch = 0.95f;
        else if (keys.brake) finalPitch = 0.08f;
    } else {
        // Classic TILT_SPD mode:
        //   Tilt forward/back -> Forward Speed (pitch)
        //   Up/Down arrows    -> Altitude (throttle)
        finalThr   = kbVert;
        finalPitch = clampf(0.42f - imuTilt * 0.58f, 0.02f, 1.0f);
        if (keys.boost)      finalPitch = 1.0f;
        else if (keys.brake) finalPitch = 0.05f;
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
