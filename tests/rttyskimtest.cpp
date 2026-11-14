// SPDX-License-Identifier: GPL-3.0-or-later
// RTTY skimmer, offline: a synthetic 1 MS/s capture of the 20 m RTTY
// window — five stations keying real ITA2 at 45.45 Bd / 170 Hz, one of
// them barely over the noise, plus the things that must NOT be spotted
// (two steady carriers 170 Hz apart, the same again at unequal levels, a
// keyed lone carrier) — through RttySkimmer exactly as the console feeds
// it. Every call must land on the band map at its MARK frequency, one of
// them flagged as calling us, nothing else may appear, the false pairs
// must give their channels back, a dial move mid-run must not move a
// spot, and a dead band must stay silent. Prints the bank's CPU cost.
//
//   cmake --build build --target rttyskimtest && ./build/rttyskimtest
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rtty/Ita2.h"
#include "rtty/RttySkimmer.h"

using ttc::RttySkimmer;

namespace {

constexpr double kRate = 1e6;
constexpr double kPi = 3.14159265358979323846;
constexpr double kBaud = 45.45;

// Cheap Gaussian-ish noise: the sum of four uniforms (xorshift).
struct Noise {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    float u() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return float(s >> 40) * (1.0f / 16777216.0f) - 0.5f;
    }
    float g() { return (u() + u() + u() + u()) * 1.7320508f; }   // unit variance
};

enum : int8_t { Off = 0, Mark = 1, Space = 2 };

struct Station {
    const char* call;        // nullptr = an interferer
    qint64 markHz;
    double amp;
    std::vector<std::pair<double, int8_t>> plan;   // (samples, state), cycled
    // generator state
    size_t at = 0;
    double left = 0.0;
    int8_t state = Off;
    std::complex<double> ph{1.0, 0.0}, rotM, rotS;
};

// An over: half a second of mark, the text, a mark tail, then key up.
void addOver(Station& st, const std::string& text, double offSecs) {
    const double bit = kRate / kBaud;
    st.plan.emplace_back(0.5 * kRate, Mark);
    for (uint8_t code : ttc::ita2::encodeText(text, true)) {
        st.plan.emplace_back(bit, Space);                              // start
        for (int i = 0; i < 5; ++i)
            st.plan.emplace_back(bit, ((code >> i) & 1) ? Mark : Space);
        st.plan.emplace_back(1.5 * bit, Mark);                         // stop
    }
    st.plan.emplace_back(0.2 * kRate, Mark);
    if (offSecs > 0.0) st.plan.emplace_back(offSecs * kRate, Off);
}

void retune(std::vector<Station>& sts, qint64 loAbs) {
    for (Station& s : sts) {
        s.rotM = std::polar(1.0, 2.0 * kPi * double(s.markHz - loAbs) / kRate);
        s.rotS = std::polar(1.0, 2.0 * kPi * double(s.markHz - 170 - loAbs) / kRate);
    }
}

void render(std::vector<Station>& sts, Noise& nz, float sigma,
            std::vector<std::complex<float>>& blk) {
    for (auto& z : blk) z = {sigma * nz.g(), sigma * nz.g()};
    for (Station& s : sts) {
        for (auto& z : blk) {
            if (s.left <= 0.0) {
                s.state = s.plan[s.at].second;
                s.left += s.plan[s.at].first;
                s.at = (s.at + 1) % s.plan.size();
            }
            s.left -= 1.0;
            s.ph *= (s.state == Space) ? s.rotS : s.rotM;
            if (s.state != Off)
                z += std::complex<float>(float(s.amp * s.ph.real()), float(s.amp * s.ph.imag()));
        }
        s.ph /= std::abs(s.ph);
    }
}

int fails = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- the window follows the band ----
    {
        qint64 lo = 0, hi = 0;
        check(RttySkimmer::rttySegment(14250000, lo, hi) && lo == 14080000 && hi == 14150000,
              "20 m phone dial -> the 20 m RTTY window");
        check(RttySkimmer::rttySegment(7040000, lo, hi) && lo == 7030000 && hi == 7125000,
              "40 m -> 7.030-7.125");
        check(!RttySkimmer::rttySegment(1830000, lo, hi), "160 m: no window");
        check(!RttySkimmer::rttySegment(5000000, lo, hi), "off the ham bands: no window");
    }

    // Noise 0.05 per component over 1 MHz; "dB" below is tone power over
    // the noise in 3 kHz, the rttytest ruler.
    const float sigma = 0.05f;
    const double n3k = 2.0 * sigma * sigma * 3000.0 / kRate;
    const auto ampFor = [&](double db) { return std::sqrt(n3k * std::pow(10.0, db / 10.0)); };

    std::vector<Station> sts;
    const auto station = [&](const char* call, qint64 markHz, double db, const std::string& text,
                             double offSecs) {
        Station s;
        s.call = call;
        s.markHz = markHz;
        s.amp = ampFor(db);
        addOver(s, text, offSecs);
        sts.push_back(std::move(s));
    };
    station("W1AW", 14085200, 20, "CQ CQ CQ DE W1AW W1AW W1AW K\r\n", 2.0);
    station("DL1ABC", 14092340, 15, "CQ TEST DL1ABC DL1ABC CQ ", 1.5);
    station("JA1XYZ", 14140100, 12, "CQ CQ DE JA1XYZ JA1XYZ PSE K\r\n", 2.5);
    station("K3LR", 14101000, 18, "N8EM DE K3LR K3LR UR 599 599 OH OH BK\r\n", 3.0);
    station("VE3ABC", 14096000, 3, "CQ CQ CQ DE VE3ABC VE3ABC VE3ABC K\r\n", 1.0);
    // Not RTTY: two carriers 170 Hz apart, equal; the same 10 dB apart;
    // a keyed lone carrier (CW-ish, 60 ms elements).
    const auto carrier = [&](qint64 hz, double db) {
        Station s;
        s.call = nullptr;
        s.markHz = hz;
        s.amp = ampFor(db);
        s.plan.emplace_back(kRate, Mark);
        sts.push_back(std::move(s));
    };
    carrier(14120000, 20);
    carrier(14120000 - 170, 20);
    carrier(14130000, 22);
    carrier(14130000 - 170, 12);
    {
        Station s;
        s.call = nullptr;
        s.markHz = 14110000;
        s.amp = ampFor(20);
        for (int i = 0; i < 7; ++i) {
            s.plan.emplace_back((i % 3 ? 0.06 : 0.18) * kRate, Mark);
            s.plan.emplace_back(0.06 * kRate, Off);
        }
        sts.push_back(std::move(s));
    }

    // `rttyskimtest --write <file.tciq> [secs]`: the same scene as a
    // capture file, for replaying through the console itself
    // (TTC_IQFILE=<file> with skim/rtty=true in a scratch config).
    if (argc > 2 && QString(argv[1]) == "--write") {
        const double secs = argc > 3 ? std::atof(argv[3]) : 30.0;
        const double lo = 14090000.0 + 260000.0, dialHz = 14090000.0;
        retune(sts, qint64(lo));
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) { std::perror(argv[2]); return 2; }
        char hdr[40] = {};
        std::memcpy(hdr, "TTCIQ02", 8);
        std::memcpy(hdr + 8, &kRate, 8);
        std::memcpy(hdr + 16, &lo, 8);
        std::memcpy(hdr + 32, &dialHz, 8);
        std::fwrite(hdr, 1, sizeof hdr, f);
        Noise wn;
        std::vector<std::complex<float>> wb(16384);
        std::vector<int16_t> raw(wb.size() * 2);
        for (size_t b = 0; b < size_t(secs * kRate / wb.size()); ++b) {
            render(sts, wn, sigma, wb);
            for (size_t i = 0; i < wb.size(); ++i) {
                raw[2 * i] = int16_t(std::lround(wb[i].real() * 32767.0f));
                raw[2 * i + 1] = int16_t(std::lround(wb[i].imag() * 32767.0f));
            }
            std::fwrite(raw.data(), 2, raw.size(), f);
        }
        std::fclose(f);
        std::printf("wrote %.0f s to %s\n", secs, argv[2]);
        return 0;
    }

    qint64 streamMs = 1000000;                  // the test's clock
    RttySkimmer skim(kRate, 16);
    skim.setClock([&] { return streamMs; });
    skim.setMyCall("N8EM");
    skim.setKnownCalls(QSet<QString>{"W1AW", "DL1ABC", "K3LR"});
    skim.setEnabled(true);

    qint64 dial = 14090000;
    const int loOff = 260000, span = 1000000;
    retune(sts, dial + loOff);
    skim.update(dial, loOff, span);

    Noise nz;
    std::vector<std::complex<float>> blk(16384);
    QElapsedTimer cpu;
    qint64 cpuNs = 0;
    const double runSecs = 75.0;
    const size_t blocks = size_t(runSecs * kRate / blk.size());
    double nextUpdate = 1.0;
    bool moved = false;
    int falseBusyAt20 = -1;
    for (size_t b = 0; b < blocks; ++b) {
        const double t = double(b) * blk.size() / kRate;
        if (!moved && t >= 38.0) {               // the operator tunes 3 kHz up
            moved = true;
            dial += 3000;
            retune(sts, dial + loOff);
            skim.update(dial, loOff, span);
        }
        render(sts, nz, sigma, blk);
        cpu.start();
        skim.processIq(blk.data(), blk.size());
        cpuNs += cpu.nsecsElapsed();
        streamMs = 1000000 + qint64(t * 1000.0);
        app.processEvents();
        if (t >= nextUpdate) {
            nextUpdate += 1.0;
            skim.update(dial, loOff, span);
            if (falseBusyAt20 < 0 && t >= 20.0) {
                falseBusyAt20 = 0;
                for (const auto& c : skim.channelInfo())
                    if (c.active && std::llabs(c.hz - 14120000) < 400) ++falseBusyAt20;
            }
        }
    }
    app.processEvents();

    std::printf("\nband map after %.0f s:\n", runSecs);
    const auto spots = skim.spots();
    for (const auto& s : spots)
        std::printf("  %-8s %.2f kHz%s\n", qPrintable(s.call), s.hz / 1000.0,
                    s.toMe ? "  (calling us)" : "");
    std::printf("channels:\n");
    for (const auto& c : skim.channelInfo())
        if (c.active)
            std::printf("  %.2f kHz %-8s %s %s\n", c.hz / 1000.0, qPrintable(c.call),
                        c.open ? "open" : "shut", qPrintable(c.text.right(24)));
    std::printf("\n");

    int found = 0;
    for (const Station& st : sts) {
        if (!st.call) continue;
        bool ok = false;
        for (const auto& s : spots)
            if (s.call == QLatin1String(st.call) && std::llabs(s.hz - st.markHz) <= 30) ok = true;
        const std::string what = std::string(st.call) + " spotted on its mark frequency";
        check(ok, what.c_str());
        if (ok) ++found;
    }
    check(int(spots.size()) == found, "nothing else on the band map");
    bool toMe = false;
    for (const auto& s : spots)
        if (s.call == QLatin1String("K3LR") && s.toMe) toMe = true;
    check(toMe, "K3LR flagged as calling us");
    check(falseBusyAt20 == 0, "equal carrier pair: channel given back inside 20 s");
    int falseBusy = 0;
    for (const auto& c : skim.channelInfo())
        if (c.active && (std::llabs(c.hz - 14120000) < 400 || std::llabs(c.hz - 14130000) < 400
                         || std::llabs(c.hz - 14110000) < 400))
            ++falseBusy;
    check(falseBusy == 0, "no channel left on a carrier pair or the keyed carrier");
    std::printf("\nbank cost: %.1f %% of one core (16 channels + slice + finder)\n",
                100.0 * double(cpuNs) / 1e9 / runSecs);

    // ---- a dead band stays silent ----
    {
        RttySkimmer dead(kRate, 16);
        qint64 ms = 5000000;
        dead.setClock([&] { return ms; });
        dead.setEnabled(true);
        dead.update(14090000, loOff, span);
        std::vector<Station> none;
        int busy = 0;
        const size_t nb = size_t(25.0 * kRate / blk.size());
        for (size_t b = 0; b < nb; ++b) {
            render(none, nz, sigma, blk);
            dead.processIq(blk.data(), blk.size());
            ms += qint64(blk.size() * 1000.0 / kRate);
            app.processEvents();
            if (b % 61 == 60) {
                dead.update(14090000, loOff, span);
                for (const auto& c : dead.channelInfo())
                    if (c.active) ++busy;
            }
        }
        check(dead.spots().isEmpty(), "dead band: no spots");
        check(busy == 0, "dead band: no channel ever parked on noise");
    }

    // ---- out of reach: 10 m phone dial, window 28.08-28.17 ----
    {
        RttySkimmer far(kRate, 4);
        far.setEnabled(true);
        far.update(28900000, loOff, span);       // capture 28.66-29.66
        const auto r = far.reach();
        check(r.segLo == 28080000 && r.gotHi == 0, "10 m phone dial: window named, none of it reached");
    }

    std::printf("\n%s\n", fails ? "RTTY SKIMMER: FAIL" : "RTTY SKIMMER: PASS");
    return fails ? 1 : 0;
}
