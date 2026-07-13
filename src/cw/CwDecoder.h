// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <atomic>
#include <complex>
#include <cstddef>

namespace ttc {

// CW reader fed straight from the SDR IQ stream — no audio cable, works
// with the AF gain at zero. The tuned station's carrier sits at a FIXED
// offset from the SDR LO (-kLoOffsetHz, because the capture LO rides
// 60 kHz above the dial and both radios read the carrier on the dial in
// CW), so the mixer never needs retuning: CW zap puts the carrier at the
// dial, the dial is always -60 kHz from the LO, and this decoder listens
// exactly there in a ~±110 Hz window.
//
// Chain (input 500 ksps, SDR thread): mix +60 kHz to DC -> 250:1 boxcar
// decimate (2 ksps) -> 8-tap complex moving average (~±110 Hz) ->
// magnitude envelope -> adaptive two-level slicer (tracks signal peak and
// noise floor separately) -> mark/space run lengths -> adaptive-dit Morse
// classifier. Characters are emitted via queued signal to the GUI.
class CwDecoder : public QObject {
    Q_OBJECT
public:
    explicit CwDecoder(double inputRate, double offsetHz,
                       QObject* parent = nullptr);

    void setEnabled(bool on);              // atomic; safe from any thread
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Move the listening window to a new offset from the SDR LO (skimmer
    // channels hop between stations). Safe from any thread; the SDR thread
    // applies it at the next block and resets all demod state.
    void retune(double offsetHz);

    // SDR streaming thread.
    void processIq(const std::complex<float>* d, size_t n);

signals:
    void textDecoded(const QString& text); // one or more characters
    void wpmEstimated(int wpm);

private:
    void tick(float mag);                  // one envelope sample (2 kHz)
    void classifyGap(double gapMs);
    void emitSymbol();

    std::atomic<bool> enabled_{false};
    std::atomic<double> pendingOffset_{0.0};
    std::atomic<bool> retunePending_{false};
    double inputRate_ = 500000.0;

    // mixer + decimator (SDR thread only). The LO is a recurrence phasor
    // (one complex multiply per sample, renormalized periodically) — a
    // skimmer bank of these runs where per-sample sin/cos would not.
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int renorm_ = 0;
    std::complex<double> acc_{0.0, 0.0};
    int accN_ = 0, decim_ = 250;
    std::complex<float> ma_[8] = {};       // post-decimation moving average
    int maIdx_ = 0;

    // envelope + slicer
    float env_ = 0.0f;                     // smoothed magnitude
    float peak_ = 0.0f, floor_ = 1e-3f;    // adaptive signal/noise trackers
    bool  key_ = false;
    double runMs_ = 0.0;                   // current mark/space duration
    double tickMs_ = 0.5;                  // 2 kHz envelope rate

    // Morse timing
    double ditMs_ = 60.0;                  // adaptive (starts ~20 WPM)
    double lastSpaceMs_ = 0.0;             // for bridging over noise blips
    QString sym_;                          // dits/dahs of the current char
    bool wordGapSent_ = true;
    int lastWpm_ = 0;
};

} // namespace ttc
