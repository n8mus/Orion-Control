// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "sdr/SdrSource.h"      // IqBlock
#include "dsp/Fft.h"
#include <functional>
#include <vector>

namespace ttc {

// Turns a stream of IQ blocks into a display spectrum: Hann window -> FFT ->
// power(dB) -> fftshift (DC centered) -> temporal smoothing. Throttled to a target
// frame rate so we FFT ~20x/sec, not per callback. Called on the SDR thread; the
// output callback fires there too (marshal to the GUI thread in the consumer).
class SpectrumComputer {
public:
    explicit SpectrumComputer(int fftSize = 2048);

    void setOutput(std::function<void(const std::vector<float>&)> cb) { out_ = std::move(cb); }
    void setFrameIntervalMs(double ms) { minInterval_ = ms; }
    int  bins() const { return n_; }

    void addSamples(const IqBlock& iq);

private:
    static double now_ms();

    int n_;
    Fft fft_;
    std::vector<float> window_;                 // Hann
    std::vector<std::complex<float>> buf_;      // rolling accumulator (kept at <= n_)
    std::vector<std::complex<float>> frame_;
    std::vector<float> outDb_;                  // smoothed output, fftshifted
    std::function<void(const std::vector<float>&)> out_;
    double minInterval_ = 50.0;                 // ~20 fps
    double lastEmitMs_  = 0.0;
    float  smooth_      = 0.5f;                  // temporal averaging factor
};

} // namespace ttc
