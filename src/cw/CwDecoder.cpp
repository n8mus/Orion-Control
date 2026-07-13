// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwDecoder.h"

#include <QHash>
#include <cmath>

namespace ttc {

namespace {
// ITU Morse, plus the prosigns that matter on the air.
const QHash<QString, QChar>& morseTable() {
    static const QHash<QString, QChar> t = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
        {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
        {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'},
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
        {"-...-", '='}, {".-.-.", '+'}, {"-....-", '-'}, {".--.-.", '@'},
        {"---...", ':'}, {"-.-.-.", ';'}, {".-..-.", '"'}, {"..--.-", '_'},
        {"-.--.", '('}, {"-.--.-", ')'}, {"...-..-", '$'}, {".-...", '&'},
    };
    return t;
}
} // namespace

CwDecoder::CwDecoder(double inputRate, double offsetHz, QObject* parent)
    : QObject(parent) {
    // Mix the carrier at offsetHz (negative = below the LO) up to DC.
    phaseInc_ = -2.0 * M_PI * offsetHz / inputRate;
    decim_ = static_cast<int>(inputRate / 2000.0);   // -> 2 ksps envelope
    tickMs_ = 1000.0 * decim_ / inputRate;
}

void CwDecoder::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}

void CwDecoder::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    for (size_t i = 0; i < n; ++i) {
        const std::complex<double> lo(std::cos(phase_), std::sin(phase_));
        phase_ += phaseInc_;
        if (phase_ > M_PI)  phase_ -= 2.0 * M_PI;
        if (phase_ < -M_PI) phase_ += 2.0 * M_PI;
        acc_ += std::complex<double>(d[i]) * lo;
        if (++accN_ < decim_) continue;
        // one decimated complex sample
        std::complex<float> z(float(acc_.real() / decim_),
                              float(acc_.imag() / decim_));
        acc_ = {0.0, 0.0};
        accN_ = 0;
        ma_[maIdx_] = z;
        maIdx_ = (maIdx_ + 1) & 7;
        std::complex<float> s{0.0f, 0.0f};
        for (const auto& v : ma_) s += v;
        tick(std::abs(s) / 8.0f);
    }
}

void CwDecoder::tick(float mag) {
    // Envelope smoothing ~4 ms.
    env_ += 0.12f * (mag - env_);
    // Track key-down level and noise floor — each learns ONLY during its
    // own state. Letting the floor rise during a long dah collapses the
    // SNR gate mid-element and chops it (found via envelope dump: the
    // floor climbed to half the signal within 300 ms of key-down).
    if (key_) {
        if (env_ > peak_) peak_ += 0.20f * (env_ - peak_);
        else              peak_ += 0.01f * (env_ - peak_);
    } else {
        if (env_ < floor_) floor_ += 0.20f * (env_ - floor_);
        else               floor_ += 0.01f * (env_ - floor_);
        // A plausible signal onset must charge the peak FAST or the very
        // first element gets bitten off waiting for the SNR gate (C decoded
        // as R, every time). Plausible = well clear of the noise floor —
        // otherwise ordinary noise wiggle pumps the peak up and the gate
        // opens on a dead frequency (live test: an empty spot babbled E's).
        if (env_ > peak_ && env_ > 3.0f * floor_)
            peak_ += 0.20f * (env_ - peak_);
        else
            peak_ += 0.0003f * (env_ - peak_);
    }
    if (floor_ < 1e-6f) floor_ = 1e-6f;

    bool key = key_;
    if (peak_ > 3.0f * floor_) {           // usable SNR only
        const float span = peak_ - floor_;
        const float on  = floor_ + 0.55f * span;
        const float off = floor_ + 0.35f * span;
        if (!key_ && env_ > on)  key = true;
        if (key_  && env_ < off) key = false;
    } else {
        key = false;
    }

    if (key == key_) {
        runMs_ += tickMs_;
        // Long silence closes out the pending character and word.
        if (!key_ && !sym_.isEmpty() && runMs_ > 2.5 * ditMs_) emitSymbol();
        if (!key_ && !wordGapSent_ && runMs_ > 6.0 * ditMs_) {
            wordGapSent_ = true;
            emit textDecoded(" ");
        }
        return;
    }
    // Edge: classify the run that just ended.
    const double ms = runMs_;
    runMs_ = 0.0;
    key_ = key;
    if (key) {
        // A space ended. Short spaces are element gaps = exactly one dit —
        // they carry the sender's dit clock even when the marks are being
        // misclassified, which is what breaks the 35 WPM acquisition trap
        // (dahs captured as dits keep the estimate high; the 34 ms gaps
        // pull it right back down).
        if (ms > 0.4 * ditMs_ && ms < 2.0 * ditMs_)
            ditMs_ += 0.35 * (ms - ditMs_);
        lastSpaceMs_ = ms;
        return;
    }
    // A mark ended: dit or dah, and adapt the dit clock.
    if (ms < 0.35 * ditMs_) {
        // Impulse blip, not an element — and it must not SPLIT the gap it
        // landed in, or characters never close in noise: resume the space
        // as if the blip never happened.
        runMs_ = lastSpaceMs_ + ms;
        return;
    }
    if (ms < 2.0 * ditMs_) {
        sym_ += '.';
        if (ms > 0.4 * ditMs_) {           // too short to trust for timing
            ditMs_ += 0.35 * (ms - ditMs_);
            const int wpm = int(std::lround(1200.0 / ditMs_));
            if (wpm != lastWpm_ && wpm >= 5 && wpm <= 60) {
                lastWpm_ = wpm;
                emit wpmEstimated(wpm);
            }
        }
    } else {
        sym_ += '-';
        ditMs_ += 0.35 * (ms / 3.0 - ditMs_);
    }
    if (sym_.size() > 8) sym_.clear();     // garbage guard
}

void CwDecoder::emitSymbol() {
    const auto it = morseTable().constFind(sym_);
    sym_.clear();
    wordGapSent_ = false;
    if (it != morseTable().constEnd())
        emit textDecoded(QString(it.value()));
    else
        emit textDecoded("*");             // unreadable character
}

} // namespace ttc
