// SPDX-License-Identifier: GPL-2.0-or-later
// Offline skimmer verification: synthesize a 500 ksps capture with several
// CW stations calling CQ inside the 20 m CW segment, hand the engine the
// kind of averaged-spectrum frames MainWindow would, and check that every
// real callsign is spotted on the right frequency — and nothing else is.
//
//   cmake --build build --target skimtest && ./build/skimtest
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"

using ttc::SkimmerEngine;

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

// Text -> on/off keying envelope at the sample rate.
std::vector<bool> keying(const QString& text, int wpm, double rate) {
    const double dit = 1.2 / wpm;                  // seconds
    const auto samples = [&](double s) { return size_t(s * rate); };
    std::vector<bool> k;
    for (const QChar ch : text) {
        if (ch == ' ') {
            k.insert(k.end(), samples(7 * dit), false);
            continue;
        }
        const QString pat = morse().value(ch.toUpper());
        for (const QChar e : pat) {
            k.insert(k.end(), samples(e == '.' ? dit : 3 * dit), true);
            k.insert(k.end(), samples(dit), false);
        }
        k.insert(k.end(), samples(2 * dit), false); // char gap tops up to 3
    }
    return k;
}

struct Station {
    QString call;
    qint64  hz;          // absolute
    int     wpm;
    float   amp;
    QString text;        // sent loop; empty = the stock "CQ CQ DE ..." form
    std::vector<bool> key;
    size_t  pos = 0;
    std::complex<double> lo{1.0, 0.0}, step;
};

} // namespace

// The CW window is chosen by the dial's BAND, not by how near the dial is
// to CW: working phone up the band, the bank keeps reading the CW end
// (operator, 2026-10-20). Reachability is a separate question — the
// capture is 1 MHz centred on the dial + the LO offset — so this also
// prints, per band, how much of the CW window a phone dial can see.
int checkSegments() {
    struct Case { const char* what; qint64 dial; qint64 lo, hi; bool ok; };
    const Case cases[] = {
        {"40 m phone",   7200000,  7000000,  7070000, true},
        {"40 m CW",      7025000,  7000000,  7070000, true},
        {"75 m phone",   3900000,  3500000,  3570000, true},
        {"20 m phone",  14250000, 14000000, 14070000, true},
        {"20 m band ed",14350000, 14000000, 14070000, true},
        {"15 m phone",  21300000, 21000000, 21070000, true},
        {"10 m FM",     29600000, 28000000, 28070000, true},
        {"6 m SSB",     50125000, 50050000, 50100000, true},
        {"60 m channel", 5357000,        0,        0, false},
        {"WWV",         10000000,        0,        0, false},
        {"11 m",        27185000,        0,        0, false},
    };
    int bad = 0;
    for (const Case& c : cases) {
        qint64 lo = 0, hi = 0;
        const bool got = SkimmerEngine::cwSegment(c.dial, lo, hi);
        if (got != c.ok || (got && (lo != c.lo || hi != c.hi))) {
            printf("  FAIL %-12s dial %.3f -> %s %.3f-%.3f (want %s %.3f-%.3f)\n",
                   c.what, c.dial / 1e6, got ? "seg" : "none", lo / 1e6, hi / 1e6,
                   c.ok ? "seg" : "none", c.lo / 1e6, c.hi / 1e6);
            ++bad;
        }
    }
    // What a 1 MHz capture at LO = dial + 260 kHz actually reaches from a
    // typical phone frequency. Informational: a band whose CW end sits
    // below the capture simply assigns nothing until the dial comes down.
    const qint64 kCap = 1000000, kLoOffHz = 260000;
    const Case phone[] = {
        {"160 m", 1900000, 0, 0, true}, {"75 m",  3900000, 0, 0, true},
        {"40 m",  7200000, 0, 0, true}, {"20 m", 14250000, 0, 0, true},
        {"17 m", 18130000, 0, 0, true}, {"15 m", 21300000, 0, 0, true},
        {"12 m", 24950000, 0, 0, true}, {"10 m", 28400000, 0, 0, true},
    };
    printf("  from phone, 1 MHz capture:\n");
    for (const Case& c : phone) {
        qint64 lo = 0, hi = 0;
        if (!SkimmerEngine::cwSegment(c.dial, lo, hi)) continue;
        const qint64 capLo = c.dial + kLoOffHz - kCap / 2;
        const qint64 capHi = c.dial + kLoOffHz + kCap / 2;
        const qint64 gLo = std::max(lo, capLo), gHi = std::min(hi, capHi);
        if (gHi - gLo < 2000)
            printf("    %-6s %.3f  CW %.3f-%.3f  out of reach\n", c.what,
                   c.dial / 1e6, lo / 1e6, hi / 1e6);
        else
            printf("    %-6s %.3f  CW %.3f-%.3f  reaching %.3f-%.3f%s\n",
                   c.what, c.dial / 1e6, lo / 1e6, hi / 1e6, gLo / 1e6,
                   gHi / 1e6, (gLo > lo || gHi < hi) ? " (partial)" : " (all)");
    }
    return bad;
}


// The bank's size is the operator's choice (SKIM ▾ → CW channels) and
// changes while the IQ runs: the engine always holds kMaxChannels decoders
// and uses the first N. Growing must let the next spectrum pass park the
// new ones; shrinking must free what lies past the new count.
int checkChannels() {
    int bad = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) { printf("  FAIL channels: %s\n", what); ++bad; }
    };
    expect(SkimmerEngine::threadsFor(24) == 4, "24 channels -> 4 threads");
    expect(SkimmerEngine::threadsFor(36) == 5, "36 -> 5");
    expect(SkimmerEngine::threadsFor(48) == 6, "48 -> 6");
    expect(SkimmerEngine::threadsFor(64) == 8, "64 -> 8");
    const double rate = 1000000.0;
    const qint64 dial = 14030000;
    const int loOff = 260000, n = 16384;
    const double binHz = rate / n;
    // Forty carriers, 1.5 kHz apart, up the 20 m CW window.
    std::vector<float> db(n, -120.0f);
    for (int i = 0; i < 40; ++i) {
        const qint64 hz = 14003000 + i * 1500;
        const int bin = n / 2 + int(std::lround(double(hz - dial - loOff) / binHz));
        db[bin] = -70.0f;
        db[bin - 1] = db[bin + 1] = -80.0f;
    }
    const auto busy = [](const SkimmerEngine& e) {
        int k = 0;
        for (const auto& c : e.channelInfo()) if (c.active) ++k;
        return k;
    };
    SkimmerEngine eng(rate, 24);
    eng.setEnabled(true);
    expect(eng.channelCount() == 24 && eng.channelInfo().size() == 24, "built at 24");
    eng.updateFromSpectrum(db, int(rate), dial, loOff);
    expect(busy(eng) == 24, "24 channels: 24 of the 40 carriers taken");
    eng.setChannelCount(48);
    expect(eng.channelCount() == 48, "raised to 48");
    eng.updateFromSpectrum(db, int(rate), dial, loOff);
    expect(busy(eng) == 40, "48 channels: all 40 carriers taken");
    eng.setChannelCount(36);
    expect(eng.channelInfo().size() == 36 && busy(eng) == 36, "cut to 36: the rest freed");
    eng.setChannelCount(500);
    expect(eng.channelCount() == SkimmerEngine::kMaxChannels, "clamped at the bank's size");
    eng.setChannelCount(24);
    eng.updateFromSpectrum(db, int(rate), dial, loOff);
    expect(busy(eng) == 24, "back to 24: full, no more");
    return bad;
}

// `skimtest bench`: what a FULL bank costs at each size, 1 MS/s — one
// block of noise + 64 carriers fed over and over (the decoders' cost is
// the per-sample mixer and filter, whatever is keyed). Prints how much of
// real time the feed thread needs; under 100 % or the IQ ring overflows.
int bench() {
    const double rate = 1000000.0;
    const qint64 dial = 14030000;
    const int loOff = 260000, n = 16384;
    const double binHz = rate / n;
    std::vector<float> db(n, -120.0f);
    std::vector<std::complex<float>> blk(16384);
    std::mt19937 rng(3);
    std::normal_distribution<float> nz(0.0f, 0.004f);
    for (auto& z : blk) z = {nz(rng), nz(rng)};
    for (int i = 0; i < 64; ++i) {
        const qint64 hz = 14002000 + i * 1050;
        const double off = double(hz - dial - loOff);
        const int bin = n / 2 + int(std::lround(off / binHz));
        db[bin] = -70.0f;
        db[bin - 1] = db[bin + 1] = -80.0f;
        for (size_t k = 0; k < blk.size(); ++k) {
            const double ph = 2.0 * M_PI * off * double(k) / rate;
            blk[k] += std::complex<float>(float(0.05 * std::cos(ph)), float(0.05 * std::sin(ph)));
        }
    }
    printf("  full bank at 1 MS/s (feed time as a share of real time):\n");
    for (const int count : {24, 36, 48, 64}) {
        SkimmerEngine eng(rate, count);
        eng.setEnabled(true);
        eng.updateFromSpectrum(db, int(rate), dial, loOff);
        const double secs = 8.0;
        const size_t blocks = size_t(secs * rate / blk.size());
        QElapsedTimer t;
        t.start();
        for (size_t b = 0; b < blocks; ++b) {
            eng.processIq(blk.data(), blk.size());
            if (b % 64 == 0) QCoreApplication::processEvents();
        }
        const double used = t.nsecsElapsed() / 1e9;
        int active = 0;
        for (const auto& c : eng.channelInfo()) if (c.active) ++active;
        printf("    %2d channels (%2d busy), %d threads: %5.1f %%\n", count, active,
               SkimmerEngine::threadsFor(count), 100.0 * used / secs);
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && QString(argv[1]) == "bench") return bench();
    const int segFails = checkSegments() + checkChannels();
    // "phone": the same 20 m CW stations, but the dial parked up on SSB
    // at today's 1 MHz capture — the channels then sit 350-400 kHz off the
    // LO instead of a few tens, which is the case the band-wide CW window
    // (2026-10-20) created. Reproduces "waterfall shows CW, no decodes".
    const bool phone = argc > 1 && QString(argv[1]) == "phone";
    const double kRate = phone ? 1000000.0 : 500000.0;
    const qint64 kDial = phone ? 14150000 : 14030000;
    const int    kLoOff = phone ? 260000 : 60000;
    const qint64 kLoAbs = kDial + kLoOff;
    const int        kN = phone ? 16384 : 8192;                    // spectrum bins
    const double binHz = kRate / kN;

    std::vector<Station> st = {
        {"W1AW",  14012000, 22, 0.20f},
        {"DL1ABC",14025500, 28, 0.12f},
        {"JA2XYZ",14040100, 17, 0.06f},
        {"VE3KI", 14055700, 25, 0.10f},
        // Not a callsign — must never be spotted.
        {"TEST",  14070300, 20, 0.15f},
        // Second shift, exercising the wider bank (and 45 WPM post-v2).
        {"K1TTT", 14003200, 45, 0.15f},
        // Contest cadence, NO "DE": exercises the distinct-sightings
        // confirm path and the junk gate on a T/E-heavy call — the old
        // per-character junk metric scored this exact loop 0.65 and would
        // have blocked it once sighting counts became honest (2026-08-08).
        {"N8TT",  14008700, 33, 0.12f, "CQ TEST N8TT N8TT  "},
        {"VE7CC", 14018700, 30, 0.08f},
        {"DL8AAM",14033300, 24, 0.10f},
        {"JA7QQQ",14047900, 20, 0.07f},
        {"W9RE",  14061100, 35, 0.12f},
    };
    for (auto& s : st) {
        s.key = keying(s.text.isEmpty()
                           ? QString("CQ CQ DE %1 %1 K  ").arg(s.call)
                           : s.text,
                       s.wpm, kRate);
        const double inc = 2.0 * M_PI * double(s.hz - kLoAbs) / kRate;
        s.step = {std::cos(inc), std::sin(inc)};
    }

    if (argc > 1 && QString(argv[1]) == "probe") {
        // Single-decoder sanity check on station 0, direct connection.
        ttc::CwDecoder dec(kRate, double(st[0].hz - kLoAbs));
        dec.setEnabled(true);
        QObject::connect(&dec, &ttc::CwDecoder::textDecoded, &dec,
                         [](const QString& t) {
                             printf("%s", qPrintable(t));
                             fflush(stdout);
                         },
                         Qt::DirectConnection);
        std::mt19937 prng(7);
        std::normal_distribution<float> pn(0.0f, 0.004f);
        std::vector<std::complex<float>> piq(16384);
        for (size_t done = 0; done < size_t(20.0 * kRate); done += piq.size()) {
            for (size_t i = 0; i < piq.size(); ++i) {
                std::complex<double> acc(pn(prng), pn(prng));
                for (auto& s : st) {
                    if (s.key[s.pos]) acc += double(s.amp) * s.lo;
                    s.lo *= s.step;
                    if (++s.pos >= s.key.size()) s.pos = 0;
                }
                piq[i] = std::complex<float>(float(acc.real()),
                                             float(acc.imag()));
            }
            for (auto& s : st) s.lo /= std::abs(s.lo);
            dec.processIq(piq.data(), piq.size());
        }
        printf("\n");
        return 0;
    }

    SkimmerEngine eng(kRate, 24);
    // Prefix validator standing in for cty.dat: accept the known-real ones.
    eng.setCallValidator([](const QString& c) {
        for (const char* p : {"W1", "W9", "DL", "JA", "VE", "K1", "N8"})
            if (c.startsWith(p)) return true;
        return false;
    });
    eng.setEnabled(true);

    QHash<QString, qint64> found;                  // call -> reported hz
    QObject::connect(&eng, &SkimmerEngine::spotFound, &eng,
                     [&](const QString& call, qint64 hz, int wpm) {
                         printf("  spot: %-7s %8.1f kHz  %d WPM\n",
                                qPrintable(call), hz / 1000.0, wpm);
                         found[call] = hz;
                     });

    // The averaged spectrum MainWindow would hold: flat floor + peaks.
    std::vector<float> db(kN, -120.0f);
    for (const auto& s : st) {
        const int bin = kN / 2
            + int(std::lround(double(s.hz - kLoAbs) / binHz));
        for (int k = -2; k <= 2; ++k)
            db[bin + k] = std::max(db[bin + k],
                                   -75.0f + 20.0f * std::log10(s.amp / 0.2f)
                                       - 6.0f * std::abs(k));
    }

    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.004f);
    constexpr size_t kBlock = 16384;
    std::vector<std::complex<float>> iq(kBlock);
    const size_t total = size_t(45.0 * kRate);     // 45 s on the air
    size_t sinceSpec = 0;
    for (size_t done = 0; done < total; done += kBlock) {
        for (size_t i = 0; i < kBlock; ++i) {
            std::complex<double> acc(noise(rng), noise(rng));
            for (auto& s : st) {
                if (s.key[s.pos]) acc += double(s.amp) * s.lo;
                s.lo *= s.step;
                if (++s.pos >= s.key.size()) s.pos = 0;
            }
            iq[i] = std::complex<float>(float(acc.real()), float(acc.imag()));
        }
        // Renormalize the station phasors now and then.
        for (auto& s : st) s.lo /= std::abs(s.lo);
        eng.processIq(iq.data(), kBlock);
        sinceSpec += kBlock;
        if (sinceSpec >= size_t(2.0 * kRate)) {    // every 2 simulated secs
            sinceSpec = 0;
            eng.updateFromSpectrum(db, int(kRate), kDial, kLoOff);
            QCoreApplication::processEvents();     // queued decoder signals
        }
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    int fails = segFails;
    for (const auto& s : st) {
        if (s.call == "TEST") {
            if (found.contains("TEST")) {
                printf("FAIL: bogus TEST was spotted\n");
                ++fails;
            }
            continue;
        }
        if (!found.contains(s.call)) {
            printf("FAIL: %s never spotted\n", qPrintable(s.call));
            ++fails;
        } else if (std::llabs(found[s.call] - s.hz) > 60) {
            printf("FAIL: %s off frequency: %lld vs %lld\n",
                   qPrintable(s.call),
                   (long long)found[s.call], (long long)s.hz);
            ++fails;
        }
    }
    for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
        bool real = false;
        for (const auto& s : st)
            if (s.call != "TEST" && s.call == it.key()) real = true;
        if (!real) {
            printf("FAIL: phantom spot %s\n", qPrintable(it.key()));
            ++fails;
        }
    }
    printf(fails ? "SKIMTEST: %d FAILURE(S)\n" : "SKIMTEST: PASS (all %d "
                                                 "stations, no phantoms)\n",
           fails ? fails : int(st.size()) - 1);
    return fails ? 1 : 0;
}
