// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdr/SdrPlaySource.h"

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

bool SdrPlaySource::fail(const char* where, sdrplay_api_ErrT e) {
    err_ = std::string(where) + ": " + sdrplay_api_GetErrorString(e);
    std::fprintf(stderr, "[sdrplay] %s\n", err_.c_str());
    return false;
}

bool SdrPlaySource::start(double centerHz, double sampleRate) {
    if (!apiOpen_) return false;
    if (streaming_) stop();

    sdrplay_api_ErrT e;
    if ((e = sdrplay_api_LockDeviceApi()) != sdrplay_api_Success) return fail("LockDeviceApi", e);

    sdrplay_api_DeviceT devs[8];
    unsigned int nDevs = 0;
    e = sdrplay_api_GetDevices(devs, &nDevs, 8);
    if (e != sdrplay_api_Success) { sdrplay_api_UnlockDeviceApi(); return fail("GetDevices", e); }

    int idx = -1;
    for (unsigned int i = 0; i < nDevs; ++i)
        if (devs[i].hwVer == SDRPLAY_RSP2_ID) { idx = static_cast<int>(i); break; }
    if (idx < 0) { sdrplay_api_UnlockDeviceApi(); err_ = "no RSP2 found"; return false; }

    device_ = devs[idx];
    device_.tuner = sdrplay_api_Tuner_A;
    if ((e = sdrplay_api_SelectDevice(&device_)) != sdrplay_api_Success) {
        sdrplay_api_UnlockDeviceApi(); return fail("SelectDevice (in use?)", e);
    }
    sdrplay_api_UnlockDeviceApi();

    if ((e = sdrplay_api_GetDeviceParams(device_.dev, &params_)) != sdrplay_api_Success)
        return fail("GetDeviceParams", e);

    // Front-end config: zero-IF, 1.536 MHz analog BW, fixed gain (AGC off) for a
    // stable panadapter noise floor.
    params_->devParams->fsFreq.fsHz          = sampleRate;
    auto* rx = params_->rxChannelA;
    rx->tunerParams.rfFreq.rfHz              = centerHz;
    rx->tunerParams.bwType                   = sdrplay_api_BW_1_536;
    rx->tunerParams.ifType                   = sdrplay_api_IF_Zero;
    rx->tunerParams.gain.gRdB                = gRdB_;
    rx->tunerParams.gain.LNAstate            = static_cast<unsigned char>(lnaState_);
    rx->ctrlParams.agc.enable                = sdrplay_api_AGC_DISABLE;
    rx->ctrlParams.decimation.enable         = 0;
    rx->rsp2TunerParams.antennaSel =
        antennaB_ ? sdrplay_api_Rsp2_ANTENNA_B : sdrplay_api_Rsp2_ANTENNA_A;

    sdrplay_api_CallbackFnsT cbs{};
    cbs.StreamACbFn = &SdrPlaySource::streamCb;
    cbs.StreamBCbFn = nullptr;
    cbs.EventCbFn   = &SdrPlaySource::eventCb;

    if ((e = sdrplay_api_Init(device_.dev, &cbs, this)) != sdrplay_api_Success) {
        sdrplay_api_ReleaseDevice(&device_);
        return fail("Init", e);
    }
    streaming_ = true;
    std::printf("[sdrplay] RSP2 streaming: %.0f Hz @ %.0f Ssps, antenna %c\n",
                centerHz, sampleRate, antennaB_ ? 'B' : 'A');
    return true;
}

void SdrPlaySource::stop() {
    if (streaming_) {
        sdrplay_api_Uninit(device_.dev);
        sdrplay_api_ReleaseDevice(&device_);
        streaming_ = false;
    }
    params_ = nullptr;
}

void SdrPlaySource::setCenterFrequency(double hz) {
    if (!streaming_) return;
    params_->rxChannelA->tunerParams.rfFreq.rfHz = hz;
    sdrplay_api_Update(device_.dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
}

void SdrPlaySource::streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                             unsigned int numSamples, unsigned int, void* ctx) {
    auto* self = static_cast<SdrPlaySource*>(ctx);
    if (!self->cb_) return;
    if (self->scratch_.size() < numSamples) self->scratch_.resize(numSamples);
    constexpr float kNorm = 1.0f / 32768.0f;
    for (unsigned int i = 0; i < numSamples; ++i)
        self->scratch_[i] = { xi[i] * kNorm, xq[i] * kNorm };
    // Hand off exactly numSamples (scratch_ may be larger).
    self->cb_(IqBlock(self->scratch_.begin(), self->scratch_.begin() + numSamples));
}

void SdrPlaySource::eventCb(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT,
                            sdrplay_api_EventParamsT*, void*) {
    if (eventId == sdrplay_api_PowerOverloadChange)
        std::fprintf(stderr, "[sdrplay] ADC power overload — reduce gain\n");
    else if (eventId == sdrplay_api_DeviceRemoved)
        std::fprintf(stderr, "[sdrplay] device removed\n");
}

} // namespace ttc
