// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "sdr/SdrSource.h"
#include <string>

namespace ttc {

// SDRplay RSP2 IQ source via the SDRplay API (libsdrplay_api). Compiled only
// when -DBUILD_SDRPLAY=ON. Phase 0 target: open the RSP2 and stream IQ from the
// Orion's spare-antenna jack into the panadapter.
class SdrPlaySource : public SdrSource {
public:
    SdrPlaySource();
    ~SdrPlaySource() override;

    bool start(double centerHz, double sampleRate) override;
    void stop() override;
    void setCenterFrequency(double hz) override;
    void setIqCallback(std::function<void(const IqBlock&)> cb) override { cb_ = std::move(cb); }

    std::string lastError() const { return err_; }

private:
    std::function<void(const IqBlock&)> cb_;
    std::string err_;
    bool apiOpen_ = false;
};

} // namespace ttc
