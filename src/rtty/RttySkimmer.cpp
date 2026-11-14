// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttySkimmer.h"
#include "rtty/RttyDemod.h"

#include <QDateTime>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace ttc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kShiftHz = 170.0;
constexpr double kBaud = 45.45;
// The part of the decimated slice the bank may use: past this the
// anti-alias filter droops and (further out) folds.
constexpr double kUseHalfHz = 47500.0;
// Capture edges are the SDR's own filter skirts.
constexpr qint64 kCaptureMarginHz = 20000;

// Where RTTY actually lives, per band, sized for a contest weekend (20 m
// runs to 14.150 then, 40 m from 7.030 to 7.125). Each row carries the
// whole band: the window follows the band the dial is in, phone end
// included, exactly as the CW skimmer's does. No row for 160 m or 6 m.
struct Seg { qint64 bandLo, bandHi, lo, hi; };
const Seg kRttySegs[] = {
    { 3500000,  4000000,  3560000,  3600000},   // 80 m
    { 7000000,  7300000,  7030000,  7125000},   // 40 m
    {10100000, 10150000, 10130000, 10150000},   // 30 m
    {14000000, 14350000, 14080000, 14150000},   // 20 m
    {18068000, 18168000, 18100000, 18110000},   // 17 m
    {21000000, 21450000, 21080000, 21150000},   // 15 m
    {24890000, 24990000, 24910000, 24930000},   // 12 m
    {28000000, 29700000, 28080000, 28170000},   // 10 m
};

// The deck's callsign shape (RttyEngine): portable forms included.
const QRegularExpression& callRe() {
    static const QRegularExpression re(
        "^[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z](/[A-Z0-9]+)?$|"
        "^[A-Z0-9]+/[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z]$");
    return re;
}

bool dbg() {
    static const bool on = std::getenv("TTC_RSKIMDBG") != nullptr;
    return on;
}

}  // namespace

RttySkimmer::RttySkimmer(double inputRate, int channels, QObject* parent)
    : QObject(parent), inputRate_(inputRate), fft_(kFft), window_(kFft),
      ring_(kFft), frame_(kFft), pw_(kFft, 0.0f) {
    const int n = std::clamp(channels, 1, kMaxChannels);
    ch_.resize(n);
    on_.assign(n, 0);
    wgen_.assign(n, 0);
    pend_.resize(n);
    for (int i = 0; i < n; ++i) {
        dem_.push_back(std::make_unique<RttyDemod>());
        dem_[i]->onChar = [this, i](char c) { pend_[i] += c; };   // mux_ held
    }
    for (int i = 0; i < kFft; ++i)
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * float(kPi) * i / (kFft - 1)));
    configureRates();
}

RttySkimmer::~RttySkimmer() = default;

qint64 RttySkimmer::nowMs() const {
    return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch();
}

void RttySkimmer::configureRates() {
    decim_ = std::max(1, int(std::lround(inputRate_ / 125000.0)));
    outRate_ = inputRate_ / decim_;
    // Windowed-sinc anti-alias FIR, computed at the decimated instants
    // only. Twelve taps per decimation step (the SKIM view's waterfall
    // gets by on six — its aliases are cosmetic; here an alias would be a
    // station decoded on the wrong frequency): -6 dB at 0.44 x the output
    // rate, the stopband reached before anything can fold into the part
    // of the slice the bank uses.
    const int n = 12 * decim_;
    taps_.resize(n);
    const double fc = 0.44 * outRate_;
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
        const double t = k - (n - 1) / 2.0;
        const double x = 2.0 * kPi * fc / inputRate_ * t;
        const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * k / (n - 1));
        taps_[k] = float(sinc * w);
        sum += taps_[k];
    }
    for (float& t : taps_) t = float(t / sum);
    size_t sz = 1;
    while (sz < taps_.size() + 1) sz <<= 1;
    dline_.assign(sz, {0.0f, 0.0f});
    dpos_ = 0;
    phase_ = 0;
    resetSpectrum();
}

void RttySkimmer::setInputRate(double rate) {
    freeAll();
    std::lock_guard<std::mutex> lk(mux_);
    inputRate_ = rate;
    configureRates();
    setMixer(mixOff_);
}

void RttySkimmer::setMixer(double offsetHz) {
    const double inc = -2.0 * kPi * offsetHz / inputRate_;
    loStep_ = {std::cos(inc), std::sin(inc)};
}

void RttySkimmer::resetSpectrum() {
    std::fill(pw_.begin(), pw_.end(), 0.0f);
    pwFrames_ = 0;
    primed_ = kFft;
    hopFill_ = 0;
}

void RttySkimmer::setEnabled(bool on) {
    if (on == enabled()) return;
    enabled_.store(on, std::memory_order_relaxed);
    if (!on) {
        feed_.store(false, std::memory_order_relaxed);
        freeAll();
        cold_.clear();
        sliceHz_ = 0;                  // the next update starts from scratch
        reach_ = {};
    }
}

bool RttySkimmer::rttySegment(qint64 dialHz, qint64& lo, qint64& hi) {
    for (const Seg& s : kRttySegs)
        if (dialHz >= s.bandLo - 50000 && dialHz <= s.bandHi + 50000) {
            lo = s.lo;
            hi = s.hi;
            return true;
        }
    return false;
}

// ---- IQ worker --------------------------------------------------------------

void RttySkimmer::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (!feed_.load(std::memory_order_relaxed)) return;
    std::vector<std::tuple<int, quint32, std::string>> out;
    {
        std::lock_guard<std::mutex> lk(mux_);
        // Mix the slice centre to 0 Hz, FIR-decimate to ~125 ksps.
        dec_.clear();
        const size_t mask = dline_.size() - 1;
        for (size_t i = 0; i < n; ++i) {
            dline_[dpos_ & mask] =
                std::complex<float>(std::complex<double>(d[i]) * lo_);
            ++dpos_;
            lo_ *= loStep_;
            if (++renorm_ >= 4096) {
                renorm_ = 0;
                lo_ /= std::abs(lo_);
            }
            if (++phase_ < decim_) continue;
            phase_ = 0;
            std::complex<float> acc(0.0f, 0.0f);
            size_t idx = (dpos_ - 1) & mask;
            for (const float t : taps_) {
                acc += t * dline_[idx];
                idx = (idx - 1) & mask;
            }
            dec_.push_back(acc);
        }
        // The finder's spectrum: a Hann-windowed 8192-point FFT every
        // 4096 samples (~30 a second), power averaged ~half a second.
        for (const std::complex<float>& y : dec_) {
            ring_[rpos_] = y;
            rpos_ = (rpos_ + 1) % kFft;
            if (primed_ > 0) --primed_;
            if (++hopFill_ < kHop) continue;
            hopFill_ = 0;
            if (primed_ > 0) continue;
            for (int k = 0; k < kFft; ++k)
                frame_[k] = ring_[(rpos_ + k) % kFft] * window_[k];
            fft_.forward(frame_);
            ++pwFrames_;
            const float a = std::max(0.06f, 1.0f / float(pwFrames_));
            for (int k = 0; k < kFft; ++k) {
                const std::complex<float> c = frame_[(k + kFft / 2) % kFft];
                const float p = c.real() * c.real() + c.imag() * c.imag();
                pw_[k] += a * (p - pw_[k]);
            }
        }
        // The bank.
        for (size_t i = 0; i < dem_.size(); ++i) {
            if (!on_[i]) continue;
            dem_[i]->process(dec_.data(), dec_.size());
            if (!pend_[i].empty()) {
                out.emplace_back(int(i), wgen_[i], std::move(pend_[i]));
                pend_[i].clear();
            }
        }
    }
    for (auto& [idx, gen, text] : out) {
        const QString t = QString::fromLatin1(text.data(), qsizetype(text.size()));
        QMetaObject::invokeMethod(
            this, [this, idx = idx, gen = gen, t] { onText(idx, gen, t); },
            Qt::QueuedConnection);
    }
}

// ---- GUI thread -------------------------------------------------------------

void RttySkimmer::freeChannel(int i, bool cold) {
    Chan& c = ch_[i];
    if (!c.active) return;
    if (cold) cold_.push_back({c.offHz + c.afcHz, nowMs() + 120000});
    {
        std::lock_guard<std::mutex> lk(mux_);
        on_[i] = 0;
        pend_[i].clear();
    }
    if (dbg())
        std::fprintf(stderr, "[rskim] ch%d free %.2f kHz%s\n", i,
                     markHz(c) / 1000.0, cold ? " (cold)" : "");
    c = Chan{};
}

void RttySkimmer::freeAll() {
    for (int i = 0; i < int(ch_.size()); ++i) freeChannel(i, false);
}

void RttySkimmer::assign(int i, double offHz) {
    Chan& c = ch_[i];
    c = Chan{};
    c.active = true;
    c.gen = nextGen_++;
    c.offHz = offHz;
    c.assignedMs = c.winMs = nowMs();
    RttyDemod::Params p;
    p.sampleRate = outRate_;
    p.markHz = offHz + kShiftHz / 2.0;         // mark is the higher RF
    p.spaceHz = offHz - kShiftHz / 2.0;
    p.baud = kBaud;
    p.iq = true;
    // The finder is good to a bin or two; the AFC only has to trim.
    p.afcRangeHz = 60.0;
    p.afcTrackHz = 60.0;
    std::lock_guard<std::mutex> lk(mux_);
    dem_[i]->setParams(p);
    pend_[i].clear();
    wgen_[i] = c.gen;
    on_[i] = 1;
}

qint64 RttySkimmer::markHz(const Chan& c) const {
    return sliceHz_ + qint64(std::lround(c.offHz + c.afcHz + kShiftHz / 2.0));
}

void RttySkimmer::update(qint64 dialHz, int loOffsetHz, int spanHz) {
    if (!enabled()) return;
    const qint64 loAbs = dialHz + loOffsetHz;
    const qint64 now = nowMs();
    qint64 segLo = 0, segHi = 0;
    const bool onBand = rttySegment(dialHz, segLo, segHi);
    // What of the window the capture reaches from here.
    const qint64 capLo = loAbs - spanHz / 2 + kCaptureMarginHz;
    const qint64 capHi = loAbs + spanHz / 2 - kCaptureMarginHz;
    const qint64 slice = (segLo + segHi) / 2;
    const qint64 gotLo = std::max({segLo, capLo, slice - qint64(kUseHalfHz)});
    const qint64 gotHi = std::min({segHi, capHi, slice + qint64(kUseHalfHz)});
    if (!onBand || gotHi - gotLo < 2000) {
        feed_.store(false, std::memory_order_relaxed);
        freeAll();
        sliceHz_ = 0;
        reach_ = onBand ? Reach{segLo, segHi, 0, 0} : Reach{};
        return;
    }
    reach_ = {segLo, segHi, gotLo, gotHi};

    const double mix = double(slice - loAbs);
    if (slice != sliceHz_) {                   // another band: start over
        freeAll();
        cold_.clear();
        std::lock_guard<std::mutex> lk(mux_);
        sliceHz_ = slice;
        mixOff_ = mix;
        setMixer(mix);
        std::fill(dline_.begin(), dline_.end(), std::complex<float>{0.0f, 0.0f});
        resetSpectrum();
    } else if (mix != mixOff_) {               // the LO moved under the slice
        std::lock_guard<std::mutex> lk(mux_);
        mixOff_ = mix;
        setMixer(mix);
        resetSpectrum();                       // smeared while it moved
    }
    feed_.store(true, std::memory_order_relaxed);

    // Snapshot the worker's side.
    std::vector<float> pw;
    unsigned frames = 0;
    {
        std::lock_guard<std::mutex> lk(mux_);
        frames = pwFrames_;
        if (frames >= 15) pw = pw_;
        for (size_t i = 0; i < ch_.size(); ++i) {
            if (!ch_[i].active) continue;
            const RttyDemod::Status st = dem_[i]->status();
            ch_[i].open = st.open;
            ch_[i].afcHz = dem_[i]->afcHz();
        }
    }

    // Expire.
    const double offLo = double(gotLo - slice), offHi = double(gotHi - slice);
    for (int i = 0; i < int(ch_.size()); ++i) {
        Chan& c = ch_[i];
        if (!c.active) continue;
        if (c.open) { c.everOpen = true; c.lastOpenMs = now; }
        if (c.offHz < offLo || c.offHz > offHi) { freeChannel(i, false); continue; }
        // Never keyed: not RTTY, whatever its spectrum looked like.
        if (!c.everOpen && now - c.assignedMs > 15000) { freeChannel(i, true); continue; }
        // Gone quiet: a station listening to a long over comes back, so
        // the channel (and what it knows about him) waits 90 s.
        if (c.everOpen && now - c.lastOpenMs > 90000) { freeChannel(i, false); continue; }
        // Open all along and next to nothing printed in 45 s: a carrier
        // with a weaker one 170 Hz away reads as "one tone on" to the
        // squelch, and noise on the weak one trickles out a character now
        // and then. A keyed station prints six a second.
        ++c.winTicks;
        if (c.open) ++c.winOpen;
        if (now - c.winMs > 45000) {
            if (c.winOpen * 5 >= c.winTicks * 4 && c.winChars < 10) {
                freeChannel(i, true);
                continue;
            }
            c.winMs = now;
            c.winTicks = c.winOpen = c.winChars = 0;
        }
    }
    for (int i = cold_.size() - 1; i >= 0; --i)
        if (cold_[i].untilMs <= now) cold_.removeAt(i);

    if (pw.empty()) return;

    // ---- the finder ----
    const double binHz = outRate_ / kFft;
    const int sh = std::max(2, int(std::lround(kShiftHz / binHz)));   // 11
    std::vector<float> S(kFft, -200.0f);
    for (int k = 1; k + 1 < kFft; ++k)
        S[k] = 10.0f * std::log10(pw[k - 1] + pw[k] + pw[k + 1] + 1e-20f);
    const auto binOf = [&](double off) { return kFft / 2 + int(std::lround(off / binHz)); };
    const int guard = sh + 8;
    const int k0 = std::max(binOf(offLo), guard);
    const int k1 = std::min(binOf(offHi), kFft - 1 - guard);
    if (k1 - k0 < 4 * sh) return;
    std::vector<float> sorted(S.begin() + k0, S.begin() + k1 + 1);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float floorDb = sorted[sorted.size() / 2];
    // The SDR's own DC line, wherever the mixer put it.
    const int dcBin = binOf(-mixOff_);
    const int dcGuard = int(std::lround(1500.0 / binHz));

    // score(k): mark hump at k, space hump at k - sh.
    const auto score = [&](int k) { return std::min(S[k], S[k - sh]); };
    struct Find { int k; float db; };
    std::vector<Find> finds;
    for (int k = k0 + sh; k <= k1; ++k) {
        const float sc = score(k);
        if (sc < floorDb + 10.0f) continue;
        if (std::abs(k - dcBin) < dcGuard || std::abs(k - sh - dcBin) < dcGuard) continue;
        if (std::abs(S[k] - S[k - sh]) > 12.0f) continue;
        // The valley between the humps and the flanks outside them: a
        // wide signal (SSB, a PSK or FT8 pile) has neither.
        float valley = -200.0f;
        for (int j = k - sh + 4; j <= k - 4; ++j) valley = std::max(valley, S[j]);
        if (valley > sc - 3.0f) continue;
        if (S[k + 6] > sc - 6.0f || S[k - sh - 6] > sc - 6.0f) continue;
        bool best = true;
        for (int j = -5; j <= 5 && best; ++j)
            if (j != 0 && score(k + j) > sc) best = false;
        if (best) finds.push_back({k, sc});
    }
    std::sort(finds.begin(), finds.end(), [](const Find& a, const Find& b) { return a.db > b.db; });

    for (const Find& f : finds) {
        const double off = (f.k - sh / 2.0 - kFft / 2) * binHz;   // pair centre
        bool skip = false;
        for (const Chan& c : ch_)
            if (c.active && std::abs(c.offHz + c.afcHz - off) < 120.0) skip = true;
        for (const Cold& cd : cold_)
            if (std::abs(cd.offHz - off) < 120.0) skip = true;
        if (skip) continue;
        int free = -1;
        for (int i = 0; i < int(ch_.size()); ++i)
            if (!ch_[i].active) { free = i; break; }
        if (free < 0) break;                   // bank is full
        assign(free, off);
        if (dbg())
            std::fprintf(stderr, "[rskim] ch%d -> %.2f kHz (%.0f dB over the floor)\n",
                         free, markHz(ch_[free]) / 1000.0, double(f.db - floorDb));
    }
}

void RttySkimmer::onText(int idx, quint32 gen, const QString& t) {
    if (idx < 0 || idx >= int(ch_.size())) return;
    Chan& c = ch_[idx];
    if (!c.active || c.gen != gen) return;     // late, from an old tenancy
    for (const QChar qc : t) {
        const QChar u = qc.toUpper();
        const bool gap = (u == ' ' || u == '\n');
        c.text += gap ? QChar(' ') : u;
        if (!gap) { c.partial += u; ++c.winChars; continue; }
        if (c.partial.isEmpty()) continue;
        c.words.push_back(c.partial);
        c.partial.clear();
        if (c.words.size() > 12) c.words.removeFirst();
        onWord(idx);
        if (!c.active) return;                 // folded into another channel
    }
    if (c.partial.size() > 24) c.partial.clear();      // a run of garbage
    if (c.text.size() > 160) c.text = c.text.right(120);
}

// A completed word. It is a call if it has the shape and a country; it is
// SPOTTED once it has been seen enough: straight away after CQ / DE / TEST
// / QRZ / TU for a MASTER.SCP call, twice in that context otherwise, and
// a bare repeated call ("W1AW W1AW", a caller's whole transmission) twice
// if listed, three times if not. One decode with a flipped bit still has
// a call's shape — the repeats are what a bust does not have.
void RttySkimmer::onWord(int idx) {
    Chan& c = ch_[idx];
    QString tok = c.words.last();
    tok.remove(QRegularExpression(QStringLiteral("[^A-Z0-9/]")));
    if (tok.size() < 3 || tok.size() > 12) return;
    if (!callRe().match(tok).hasMatch()) return;
    if (!myCall_.isEmpty() && (tok == myCall_ || tok.section('/', 0, 0) == myCall_)) return;
    if (validate_ && !validate_(tok)) return;

    const qint64 now = nowMs();
    const int n = c.words.size();
    const auto word = [&](int back) { return back < n ? c.words[n - 1 - back] : QString(); };
    static const QSet<QString> kCtx = {"CQ", "DE", "TEST", "QRZ", "QRZ?", "TU"};
    const bool afterDe = word(1) == QLatin1String("DE");
    bool ctx = afterDe;
    for (int b = 1; b <= 3 && !ctx; ++b)
        if (kCtx.contains(word(b))) ctx = true;
    // endsWith: the first word of an over often carries a character or
    // two of key-down garbage in front ("SIN8EM DE K3LR").
    const bool toMe = afterDe && !myCall_.isEmpty() && word(2).endsWith(myCall_);

    for (auto it = c.sight.begin(); it != c.sight.end();)
        it = (now - it->lastMs > 120000) ? c.sight.erase(it) : std::next(it);
    Sight& s = c.sight[tok];
    ++s.count;
    s.lastMs = now;
    s.ctx = s.ctx || ctx;
    const bool listed = !known_.isEmpty() && known_.contains(tok);
    const int need = s.ctx ? (listed ? 1 : 2) : (listed ? 2 : 3);
    if (dbg())
        std::fprintf(stderr, "[rskim] ch%d %-10s n%d need %d%s\n", idx, qPrintable(tok),
                     s.count, need, s.ctx ? " ctx" : "");
    if (s.count < need) return;
    const int grade = s.ctx ? 2 : 1;

    // The same station on another channel: a duplicate listener next door
    // folds; anywhere else it is the far end of someone's QSO, not his
    // frequency.
    for (int i = 0; i < int(ch_.size()); ++i) {
        const Chan& o = ch_[i];
        if (i == idx || !o.active || o.call != tok) continue;
        if (std::llabs(markHz(o) - markHz(c)) < 300) freeChannel(idx, false);
        return;
    }

    const qint64 secs = now / 1000;
    const qint64 hz = markHz(c);
    if (tok == c.call) {                       // still there
        c.grade = std::max(c.grade, grade);
        c.callMs = now;
        for (Spot& sp : spots_)
            if (sp.call == tok) { sp.atSecs = secs; sp.hz = hz; sp.toMe = sp.toMe || toMe; }
        emit spotFound(tok, hz, toMe);
        emit spotsChanged();
        return;
    }
    // A frequency has one owner on the map. A bare call does not unseat a
    // station heard calling CQ there (it is his caller) unless he has
    // been silent for three minutes.
    if (!c.call.isEmpty() && grade < c.grade && now - c.callMs < 180000) return;
    if (!c.call.isEmpty()) {
        const QString old = c.call;
        spots_.erase(std::remove_if(spots_.begin(), spots_.end(),
                                    [&old](const Spot& sp) { return sp.call == old; }),
                     spots_.end());
    }
    c.call = tok;
    c.grade = grade;
    c.callMs = now;
    bool known = false;
    for (Spot& sp : spots_)
        if (sp.call == tok) { sp.hz = hz; sp.atSecs = secs; sp.toMe = toMe; known = true; }
    if (!known) spots_.push_back({tok, hz, secs, toMe});
    if (dbg())
        std::fprintf(stderr, "[rskim] SPOT %s %.2f kHz%s\n", qPrintable(tok), hz / 1000.0,
                     toMe ? " (to me)" : "");
    emit spotFound(tok, hz, toMe);
    emit spotsChanged();
}

const QVector<RttySkimmer::Spot>& RttySkimmer::spots() {
    const qint64 secs = nowMs() / 1000;
    spots_.erase(std::remove_if(spots_.begin(), spots_.end(),
                                [secs](const Spot& s) { return secs - s.atSecs > 600; }),
                 spots_.end());
    return spots_;
}

QVector<RttySkimmer::ChanInfo> RttySkimmer::channelInfo() const {
    QVector<ChanInfo> out;
    out.reserve(int(ch_.size()));
    for (const Chan& c : ch_)
        out.push_back({c.active, c.open, c.active ? markHz(c) : 0, c.call, c.text.right(28)});
    return out;
}

}  // namespace ttc
