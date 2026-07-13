// SPDX-License-Identifier: GPL-2.0-or-later
// CwDecoder quality matrix: synthetic CW at various speeds, timing jitter
// and QSB depths; scores how many intact copies of the callsign survive.
//   cmake --build build --target cwtest && ./build/cwtest
// Run before decoder changes for a baseline, after for the comparison.
#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "cw/CwDecoder.h"

namespace {

const QHash<QChar, QString>& morse() {
    static const QHash<QChar, QString> t = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"},{'1', ".----"},
        {'2', "..---"},{'3', "...--"},{'4', "....-"},{'5', "....."},
        {'6', "-...."},{'7', "--..."},{'8', "---.."},{'9', "----."},
    };
    return t;
}

struct Case {
    const char* name;
    int    wpm;
    double jitter;      // sigma of lognormal element-length scatter
    double snrAmp;      // signal amplitude over noise sigma 0.004
    double qsbHz;       // 0 = no fade
    double qsbDepth;    // 0..1
};

// Element-timed keying with optional per-element jitter.
std::vector<bool> keying(const QString& text, int wpm, double jitter,
                         std::mt19937& rng, double rate) {
    const double dit = 1.2 / wpm;
    std::lognormal_distribution<double> j(0.0, jitter);
    const auto scale = [&] { return jitter > 0.0 ? j(rng) : 1.0; };
    std::vector<bool> k;
    const auto add = [&](double secs, bool on) {
        k.insert(k.end(), size_t(secs * rate), on);
    };
    for (const QChar ch : text) {
        if (ch == ' ') { add(7 * dit * scale(), false); continue; }
        const QString pat = morse().value(ch.toUpper());
        for (const QChar e : pat) {
            add((e == '.' ? dit : 3 * dit) * scale(), true);
            add(dit * scale(), false);
        }
        add(2 * dit * scale(), false);
    }
    return k;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    constexpr double kRate = 500000.0;
    constexpr double kOffset = -60000.0;   // where the tuned reader listens
    const QString story = "CQ CQ DE W1AW W1AW K  ";

    const Case cases[] = {
        {"12 wpm clean",   12, 0.00, 0.20, 0.0, 0.0},
        {"20 wpm clean",   20, 0.00, 0.20, 0.0, 0.0},
        {"28 wpm clean",   28, 0.00, 0.20, 0.0, 0.0},
        {"35 wpm clean",   35, 0.00, 0.20, 0.0, 0.0},
        {"40 wpm clean",   40, 0.00, 0.20, 0.0, 0.0},
        {"45 wpm clean",   45, 0.00, 0.20, 0.0, 0.0},
        {"50 wpm clean",   50, 0.00, 0.20, 0.0, 0.0},
        {"20 wpm weak",    20, 0.00, 0.02, 0.0, 0.0},
        {"28 wpm jit .12", 28, 0.12, 0.20, 0.0, 0.0},
        {"28 wpm jit .20", 28, 0.20, 0.20, 0.0, 0.0},
        {"20 wpm qsb 90%", 20, 0.00, 0.20, 0.15, 0.90},
        {"20 wpm qsb 97%", 20, 0.00, 0.20, 0.15, 0.97},
        {"25 wpm qsb 90% weak", 25, 0.05, 0.06, 0.25, 0.90},
    };

    int overallScore = 0, overallMax = 0;
    for (const Case& c : cases) {
        std::mt19937 rng(11);
        std::normal_distribution<float> noise(0.0f, 0.004f);
        ttc::CwDecoder dec(kRate, kOffset);
        dec.setEnabled(true);
        QString got;
        QObject::connect(&dec, &ttc::CwDecoder::textDecoded, &dec,
                         [&](const QString& t) { got += t; },
                         Qt::DirectConnection);
        const std::vector<bool> key = keying(story, c.wpm, c.jitter, rng,
                                             kRate);
        // ~14 repetitions or 60 s, whichever is less.
        const size_t total =
            std::min(key.size() * 14, size_t(60.0 * kRate));
        const int expected = int(total / key.size()) * 2;  // 2 calls/cycle
        std::complex<double> lo(1.0, 0.0);
        const double inc = 2.0 * M_PI * kOffset / kRate;
        const std::complex<double> step(std::cos(inc), std::sin(inc));
        std::vector<std::complex<float>> iq(16384);
        size_t pos = 0;
        double t = 0.0;
        for (size_t done = 0; done < total; done += iq.size()) {
            for (size_t i = 0; i < iq.size(); ++i) {
                double amp = c.snrAmp;
                if (c.qsbHz > 0.0) {
                    const double f =
                        0.5 * (1.0 + std::sin(2.0 * M_PI * c.qsbHz * t));
                    amp *= 1.0 - c.qsbDepth * f;
                }
                std::complex<double> s(noise(rng), noise(rng));
                if (key[pos]) s += amp * lo;
                lo *= step;
                if (++pos >= key.size()) pos = 0;
                t += 1.0 / kRate;
                iq[i] = std::complex<float>(float(s.real()),
                                            float(s.imag()));
            }
            lo /= std::abs(lo);
            dec.processIq(iq.data(), iq.size());
        }
        const int hits = got.count("W1AW");
        const int pct = expected ? 100 * hits / expected : 0;
        overallScore += std::min(hits, expected);
        overallMax += expected;
        printf("%-22s  %3d%%  (%d/%d intact calls)   tail: %s\n",
               c.name, pct, hits, expected,
               qPrintable(got.right(40)));
    }
    printf("TOTAL: %d%% (%d/%d)\n", 100 * overallScore / overallMax,
           overallScore, overallMax);
    return 0;
}
