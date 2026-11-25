#pragma once

#include "sim_engine.h"
#include <stdint.h>

// ============================================================================
// ES8311 Speaker Audio Effects Engine (Cardputer-Adv)
// Gentle volume curve (0..100% -> 0..75/255 on 1W NS4150B amp) with live
// volume control and non-blocking note sequencer.
// ============================================================================

class AudioFx {
public:
    void begin(uint8_t volumePct);
    void setVolumePct(uint8_t volumePct);
    void play(AudioEvent ev);
    void playVolumePip();
    void update();

private:
    struct Note {
        uint16_t freqHz;
        uint16_t durMs;
    };

    void enqueueSequence(const Note* notes, int count);

    uint8_t  _volumePct   = 20;
    Note     _seq[8]      = {};
    int      _seqLen      = 0;
    int      _seqIdx      = 0;
    uint32_t _nextNoteMs  = 0;
};
