// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dsp/Fft.h"

namespace ttc {

class RttyDemod;

// RTTY skimmer: the CW skimmer's idea for 45.45 Bd / 170 Hz RTTY. A slice
// of the SDR capture covering the band's RTTY window is mixed to 0 Hz and
// decimated to ~125 ksps ONCE; a bank of RttyDemod channels (the deck's
// own demodulator, IQ-fed: mark +85 Hz, space -85 Hz about the channel)
// then runs on that slow stream, so the whole bank costs a fraction of a
// core. Calls mined from the print land on the band map as 'R' spots at
// the MARK's RF (the cluster convention), like the deck's own decodes.
//
// Finding stations: an 8192-point spectrum of the slice (15 Hz/bin),
// averaged ~half a second, searched for the RTTY SHAPE — two humps
// 170 Hz apart with a valley between and quiet flanks. A channel parked
// on a find must then open the demodulator's keying-contrast squelch
// within 15 s or it is freed and that frequency ignored for two minutes
// (two steady carriers 170 Hz apart pass the shape test, never the
// squelch).
//
// Threading: processIq() runs on the IQ worker thread, update() and the
// rest on the GUI thread; one mutex guards the DSP state (a block is well
// under a millisecond of work). Decoded text crosses over as queued calls
// stamped with the channel's tenancy, so a late delivery from a freed
// channel is dropped.
class RttySkimmer : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxChannels = 32;
    explicit RttySkimmer(double inputRate, int channels = 16,
                         QObject* parent = nullptr);
    ~RttySkimmer() override;

    void setEnabled(bool on);
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    int channelCount() const { return int(ch_.size()); }

    // Capture-file replay at another rate. Before streaming starts only.
    void setInputRate(double rate);

    // The band's RTTY window, chosen by the band the dial is in (as the
    // CW skimmer's is); false off the ham bands and on bands with no RTTY
    // to speak of (160 m, 6 m).
    static bool rttySegment(qint64 dialHz, qint64& lo, qint64& hi);

    void setCallValidator(std::function<bool(const QString&)> v) {
        validate_ = std::move(v);
    }
    // MASTER.SCP: a listed call confirms on fewer sightings.
    void setKnownCalls(QSet<QString> calls) { known_ = std::move(calls); }
    void setMyCall(const QString& call) { myCall_ = call.toUpper(); }

    // IQ worker thread.
    void processIq(const std::complex<float>* d, size_t n);

    // GUI thread, about once a second: follow the dial/LO, expire
    // channels, park free ones on new finds.
    void update(qint64 dialHz, int loOffsetHz, int spanHz);

    struct ChanInfo {
        bool    active = false;
        bool    open = false;              // squelch open right now
        qint64  hz = 0;                    // the mark's RF
        QString call;
        QString text;                      // recent print
    };
    QVector<ChanInfo> channelInfo() const;

    struct Spot {
        QString call;
        qint64  hz = 0;                    // the mark's RF
        qint64  atSecs = 0;
        bool    toMe = false;              // "<my call> DE <call>"
    };
    const QVector<Spot>& spots();          // pruned to the last 10 minutes

    // For the SKIM menu's readout: the window, and how much of it the
    // capture reaches from this dial (got* both 0 = none of it).
    struct Reach { qint64 segLo = 0, segHi = 0, gotLo = 0, gotHi = 0; };
    Reach reach() const { return reach_; }

    // Tests drive the clock with stream time (ms since some epoch).
    void setClock(std::function<qint64()> c) { clock_ = std::move(c); }

signals:
    void spotFound(const QString& call, qint64 hz, bool toMe);
    void spotsChanged();

private:
    static constexpr int kFft = 8192;
    static constexpr int kHop = 4096;

    // ctx: heard after DE (anyone signing). cq: heard with CQ / TEST /
    // QRZ / TU around it — the station RUNNING the frequency.
    struct Sight { int count = 0; qint64 lastMs = 0; bool ctx = false, cq = false; };
    struct Chan {
        bool    active = false;
        quint32 gen = 0;                   // tenancy stamp
        double  offHz = 0.0;               // channel centre about the slice
        double  afcHz = 0.0;               // the demod's trim, last update
        bool    open = false;
        bool    everOpen = false;
        qint64  assignedMs = 0, lastOpenMs = 0;
        // A 45 s window: updates seen, updates with the squelch open, and
        // characters printed — an open channel that prints next to
        // nothing is not RTTY.
        qint64  winMs = 0;
        int     winTicks = 0, winOpen = 0, winChars = 0;
        QString call;
        int     grade = 0;                 // 3 running (CQ), 2 after DE, 1 bare
        qint64  lastCharMs = 0;            // for the pause between overs
        qint64  callMs = 0;
        QString text;                      // rolling print, for the readout
        QString partial;                   // the word being received
        QStringList words;                 // the last completed words
        QHash<QString, Sight> sight;
    };
    struct Cold { double offHz; qint64 untilMs; };

    qint64 nowMs() const;
    void configureRates();                 // mux_ held (or before streaming)
    void setMixer(double offsetHz);        // mux_ held
    void resetSpectrum();                  // mux_ held
    void freeChannel(int i, bool cold);    // GUI thread; takes mux_
    void freeAll();
    void assign(int i, double offHz);
    void onText(int idx, quint32 gen, const QString& t);
    void onWord(int idx);
    void consider(int idx, const QString& tok, bool toMe);   // spot it if earned
    qint64 markHz(const Chan& c) const;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> feed_{false};        // the slice is inside the capture

    // ---- DSP state, guarded by mux_ ----
    mutable std::mutex mux_;
    double inputRate_;
    int    decim_ = 8;
    double outRate_ = 125000.0;
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int    renorm_ = 0;
    std::vector<float> taps_;
    std::vector<std::complex<float>> dline_;     // power-of-2 ring
    size_t dpos_ = 0;
    int    phase_ = 0;
    std::vector<std::complex<float>> dec_;       // this block, decimated
    Fft fft_;
    std::vector<float> window_;
    std::vector<std::complex<float>> ring_, frame_;
    size_t rpos_ = 0;
    int    hopFill_ = 0;
    size_t primed_ = kFft;
    std::vector<float> pw_;                      // averaged power, fftshifted
    unsigned pwFrames_ = 0;
    std::vector<std::unique_ptr<RttyDemod>> dem_;
    std::vector<uint8_t> on_;                    // worker's view of `active`
    std::vector<quint32> wgen_;
    std::vector<std::string> pend_;              // chars decoded this block

    // ---- GUI-thread state ----
    std::vector<Chan> ch_;
    QVector<Cold> cold_;
    QVector<Spot> spots_;
    qint64 sliceHz_ = 0;                         // slice centre, absolute
    double mixOff_ = 0.0;                        // slice centre - SDR LO
    Reach  reach_;
    quint32 nextGen_ = 1;
    std::function<bool(const QString&)> validate_;
    std::function<qint64()> clock_;
    QSet<QString> known_;
    QString myCall_;
};

}  // namespace ttc
