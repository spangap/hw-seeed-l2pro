/**
 * l2pro_audio.h — the Wio Tracker L2 Pro's audio codecs, as a Service.
 *
 * L2proAudio::onInit registers the ES8311 (speaker) and ES7243E (mic) control
 * plane with the audio straddle; the audio task applies it as each direction
 * comes up. when:-gated on spangap/audio via the straddle.yaml `services:`
 * entry, so this slice compiles only when audio is staged.
 */
#pragma once

#include "service.h"

class L2proAudio : public Service {
public:
    void onInit() override;
};
