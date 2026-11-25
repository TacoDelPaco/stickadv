#include "audio_fx.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <M5Cardputer.h>
#endif

void AudioFx::begin(uint8_t volumePct) {
    setVolumePct(volumePct);
}

void AudioFx::setVolumePct(uint8_t volumePct) {
    if (volumePct > 100) volumePct = 100;
    _volumePct = volumePct;
#ifdef ARDUINO
    // Cardputer-Adv's ES8311 + NS4150B 1W speaker is very loud at high values.
    // Map 0..100% smoothly onto 0..76 (out of 255) so 20% (~15/255) is pleasant.
    uint8_t hwVol = static_cast<uint8_t>((static_cast<uint16_t>(_volumePct) * 76u) / 100u);
    if (_volumePct > 0 && hwVol < 6) hwVol = 6;
    M5Cardputer.Speaker.setVolume(hwVol);
#endif
}

void AudioFx::enqueueSequence(const Note* notes, int count) {
    _seqLen = count > 8 ? 8 : count;
    for (int i = 0; i < _seqLen; ++i) {
        _seq[i] = notes[i];
    }
    _seqIdx     = 0;
    _nextNoteMs = 0;
}

void AudioFx::playVolumePip() {
    if (_volumePct == 0) return;
    static const Note seq[] = {{1320, 35}};
    enqueueSequence(seq, 1);
    update();
}

void AudioFx::play(AudioEvent ev) {
    if (_volumePct == 0 || ev == AudioEvent::NONE) return;

    switch (ev) {
        case AudioEvent::ARM_JINGLE: {
            static const Note seq[] = {{1046, 45}, {1318, 45}, {1568, 60}, {2093, 80}};
            enqueueSequence(seq, 4);
            break;
        }
        case AudioEvent::COUNTDOWN_BEEP: {
            static const Note seq[] = {{1500, 75}};
            enqueueSequence(seq, 1);
            break;
        }
        case AudioEvent::RACE_GO: {
            static const Note seq[] = {{2250, 200}};
            enqueueSequence(seq, 1);
            break;
        }
        case AudioEvent::GATE_HIT: {
            static const Note seq[] = {{1000, 45}, {1480, 55}};
            enqueueSequence(seq, 2);
            break;
        }
        case AudioEvent::GATE_MISS: {
            static const Note seq[] = {{480, 120}, {360, 100}};
            enqueueSequence(seq, 2);
            break;
        }
        case AudioEvent::RACE_FINISH: {
            static const Note seq[] = {{1568, 80}, {1318, 80}, {1046, 140}};
            enqueueSequence(seq, 3);
            break;
        }
        case AudioEvent::NEW_BEST: {
            static const Note seq[] = {{1046, 60}, {1318, 60}, {1568, 60}, {2093, 150}};
            enqueueSequence(seq, 4);
            break;
        }
        case AudioEvent::IMU_CAL: {
            static const Note seq[] = {{1760, 40}, {2200, 60}};
            enqueueSequence(seq, 2);
            break;
        }
        default:
            break;
    }
    update();
}

void AudioFx::update() {
#ifdef ARDUINO
    if (_seqIdx >= _seqLen) return;
    uint32_t now = millis();
    if (now < _nextNoteMs) return;

    const Note& n = _seq[_seqIdx++];
    if (n.freqHz > 0 && _volumePct > 0) {
        M5Cardputer.Speaker.tone(n.freqHz, n.durMs);
    }
    _nextNoteMs = now + n.durMs + 15;
#endif
}
