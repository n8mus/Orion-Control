// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdr/SdrPlaySource.h"

#include <sdrplay_api.h>
#include <cstdio>

namespace ttc {

SdrPlaySource::SdrPlaySource() {
    if (sdrplay_api_Open() == sdrplay_api_Success) {
        apiOpen_ = true;
        float ver = 0.0f;
        sdrplay_api_ApiVersion(&ver);
        std::printf("[sdrplay] API opened, version %.2f\n", ver);
    } else {
        err_ = "sdrplay_api_Open failed (is the sdrplay API service running?)";
    }
}

SdrPlaySource::~SdrPlaySource() {
    stop();
    if (apiOpen_) sdrplay_api_Close();
}

bool SdrPlaySource::start(double centerHz, double sampleRate) {
    if (!apiOpen_) return false;
    // TODO(Phase 0): sdrplay_api_LockDeviceApi -> GetDevices -> SelectDevice ->
    // configure RSP2 tuner (centerHz, sampleRate, IF/LO, gain) -> register
    // sdrplay_api_CallbackFnsT streaming callbacks that fill IqBlock and invoke cb_.
    (void)centerHz; (void)sampleRate;
    err_ = "streaming not yet implemented (skeleton)";
    return false;
}

void SdrPlaySource::stop() { /* TODO(Phase 0): sdrplay_api_Uninit */ }

void SdrPlaySource::setCenterFrequency(double hz) {
    // TODO(Phase 0): sdrplay_api_Update with tuner frequency change.
    (void)hz;
}

} // namespace ttc
