// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "sdr/SdrSource.h"
#include <sdrplay_api.h>
#include <string>

namespace ttc {

// SDRplay RSP2 IQ source via the SDRplay API (libsdrplay_api). Compiled only when
// -DBUILD_SDRPLAY=ON. Streams IQ from the Orion's spare-antenna jack; the IQ
// callback runs on the SDRplay streaming thread (consumers must be quick / hand off).
class SdrPlaySource : public SdrSource {
public:
    SdrPlaySource();
    ~SdrPlaySource() override;

    bool apiOk() const { return apiOpen_; }

    bool start(double centerHz, double sampleRate) override;
    void stop() override;
    void setCenterFrequency(double hz) override;
    void setIqCallback(std::function<void(const IqBlock&)> cb) override { cb_ = std::move(cb); }

    // Which RSP2 SMA the spare-jack coax is on. Default Antenna A.
    void setAntennaB(bool b) { antennaB_ = b; }

    // Front-end gain. The shared-antenna feed is hot, so more attenuation than
    // usual: raise LNAstate (0..8 on RSP2) and gRdB (20..59) to clear ADC overload.
    void setGain(int gRdB, int lnaState) { gRdB_ = gRdB; lnaState_ = lnaState; }

    std::string lastError() const { return err_; }

private:
    static void streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                         unsigned int numSamples, unsigned int reset, void* ctx);
    static void eventCb(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                        sdrplay_api_EventParamsT* params, void* ctx);
    bool fail(const char* where, sdrplay_api_ErrT e);

    std::function<void(const IqBlock&)> cb_;
    std::string err_;
    bool apiOpen_   = false;
    bool streaming_ = false;
    bool antennaB_  = false;
    int  gRdB_      = 40;   // tuned for the hot shared-antenna feed (see docs/phase0-sdr.md)
    int  lnaState_  = 6;

    sdrplay_api_DeviceT       device_{};
    sdrplay_api_DeviceParamsT* params_ = nullptr;
    IqBlock scratch_;                       // reused per-callback buffer
};

} // namespace ttc
