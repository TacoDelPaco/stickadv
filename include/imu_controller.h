#pragma once

#include "config.h"
#include <stdint.h>

// ============================================================================
// Cardputer-Adv BMI270 6-Axis IMU + Arrow-Key Flight Controller
// - Arrow Keys (; . , / with or without Fn, or WASD):
//     Left / Right = Roll left/right
//     Up / Down    = Climb / Dive altitude
//     Space        = Speed boost (faster forward)
//     Shift        = Brake / slow down
// - BMI270 IMU (when imuEnabled == true):
//     Tilt Left / Right = Roll left/right (corrected Cardputer-Adv sign!)
//     Tilt Up / Down    = Climb / Dive altitude (TILT_ALT) or Speed (TILT_SPD)
//     Press 'C' anytime to zero comfortable handheld holding angle.
// ============================================================================

struct KeyFlightState {
    bool up    = false; // Up Arrow (Fn+; or ;) or W
    bool down  = false; // Down Arrow (Fn+. or .) or S
    bool left  = false; // Left Arrow (Fn+, or ,) or A
    bool right = false; // Right Arrow (Fn+/ or /) or D
    bool boost = false; // Space (turbo forward)
    bool brake = false; // Shift (slow down)
};

class ImuController {
public:
    bool begin();
    void calibrateNeutral();
    void update(float dt, const KeyFlightState& keys, const SimSettings& settings,
                PilotInput& outInput);

    bool  isPresent() const { return _imuPresent; }
    float rollDeg() const   { return -(_rollDeg - _zeroRollDeg); }
    float pitchDeg() const  { return -(_pitchDeg - _zeroPitchDeg); }

    // Map raw BMI270 Euler deltas into normalized [-1..+1] pilot roll & tilt
    static void computeTiltAxes(float rawRollDeg, float rawPitchDeg,
                                float zeroRollDeg, float zeroPitchDeg,
                                bool invertY, float& outRoll, float& outTilt);

private:
    bool  _imuPresent    = false;
    float _rollDeg       = 0.0f;
    float _pitchDeg      = 0.0f;
    float _zeroRollDeg   = 0.0f;
    float _zeroPitchDeg  = 0.0f;
    float _kbRoll        = 0.0f;
    float _kbVert        = 0.0f;
};
