// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
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
    // Display frame rate: every block is FFT'd regardless (Welch); this sets
    // how often an averaged frame goes out, and keeps the temporal smoothing
    // the same in SECONDS (0.5 per frame at 20 fps).
    void setFrameRateHz(int fps) {
        fps = fps < 5 ? 5 : fps > 60 ? 60 : fps;
        // Emits ride block arrivals (one FFT block = 16.4 ms at 1 MS/s): 0.8
        // lets 60 fps take every block instead of every other one (measured
        // ~49 fps at 0.9).
        minInterval_ = 1000.0 / fps * 0.8;
        smooth_ = float(std::pow(0.5, 20.0 / fps));
    }
    int  bins() const { return n_; }

    void addSamples(const IqBlock& iq);

private:
    static double now_ms();

    int n_;
    Fft fft_;
    std::vector<float> window_;                 // Hann
    std::vector<std::complex<float>> buf_;      // pending samples (consumed in blocks)
    std::vector<std::complex<float>> frame_;
    std::vector<float> psAcc_;                  // Welch power accumulator
    int    psCount_     = 0;                    // spectra accumulated this frame
    std::vector<float> outDb_;                  // smoothed output, fftshifted
    std::function<void(const std::vector<float>&)> out_;
    double minInterval_ = 50.0;                 // ~20 fps
    double lastEmitMs_  = 0.0;
    float  smooth_      = 0.5f;                  // temporal averaging factor
};

} // namespace ttc
