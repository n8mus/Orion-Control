// SPDX-License-Identifier: GPL-2.0-or-later
#include "dsp/SpectrumComputer.h"

#include <cmath>
#include <ctime>

namespace ttc {

SpectrumComputer::SpectrumComputer(int fftSize)
    : n_(fftSize), fft_(fftSize), window_(fftSize), frame_(fftSize),
      outDb_(fftSize, -120.0f) {
    for (int i = 0; i < n_; ++i)                 // Hann window
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * i / (n_ - 1)));
    buf_.reserve(n_ * 2);
}

double SpectrumComputer::now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void SpectrumComputer::addSamples(const IqBlock& iq) {
    buf_.insert(buf_.end(), iq.begin(), iq.end());
    if (static_cast<int>(buf_.size()) < n_) return;
    if (static_cast<int>(buf_.size()) > n_)      // keep only the latest n_ samples
        buf_.erase(buf_.begin(), buf_.end() - n_);

    const double t = now_ms();
    if (t - lastEmitMs_ < minInterval_) return;  // throttle to target frame rate
    lastEmitMs_ = t;

    for (int i = 0; i < n_; ++i) frame_[i] = buf_[i] * window_[i];
    fft_.forward(frame_);

    const float norm = 1.0f / static_cast<float>(n_);
    for (int i = 0; i < n_; ++i) {
        const int k = (i + n_ / 2) % n_;          // fftshift: DC to center
        const std::complex<float> c = frame_[k];
        const float p  = (c.real() * c.real() + c.imag() * c.imag()) * norm * norm;
        const float db = 10.0f * std::log10(p + 1e-12f);
        outDb_[i] = smooth_ * outDb_[i] + (1.0f - smooth_) * db;  // temporal smoothing
    }
    if (out_) out_(outDb_);
}

} // namespace ttc
