// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include "radio/RadioController.h"
#include "radio/SerialPort.h"

namespace ttc {

// Ten-Tec Orion 565/566 (firmware v3) driver. ASCII CAT over 57600 8N1.
// Command encoders below are the real protocol; see docs/cat-command-reference.md.
class TenTecOrion : public QObject, public RadioController {
    Q_OBJECT
public:
    explicit TenTecOrion(QObject* parent = nullptr);

    bool open(const std::string& device);   // e.g. "/dev/ttyS0"
    void close();

    const CapabilityProfile& caps() const override { return caps_; }
    bool connected() const override { return serial_.isOpen(); }

    void setFrequencyHz(Rx rx, uint64_t hz) override;
    void setMode(Rx rx, Mode mode) override;
    void setPassband(Rx rx, int loEdgeHz, int hiEdgeHz) override;
    void setBandwidthHz(Rx rx, int bwHz) override;
    void setPbtHz(Rx rx, int pbtHz) override;

    // Control-surface knobs. AGC is the radio's own letter code:
    // 'F'ast 'M'edium 'S'low 'P'rogram 'O'ff. Attenuator steps 0..3 = off/6/12/18 dB.
    void setAgc(Rx rx, char agc);                 // *R<M/S>A<F/M/S/P/O>
    void setRfGain(Rx rx, int gain);              // *R<M/S>G<0-100>
    void setAttenuator(Rx rx, int step);          // *R<M/S>T<0-3>
    // DSP helpers share one command group: *R<M/S>N<A/B/N><0-9>, 0 = off.
    void setNoiseReduction(Rx rx, int level);     // *R<M/S>NN<val>
    void setNoiseBlanker(Rx rx, int level);       // *R<M/S>NB<val>
    void setAutoNotch(Rx rx, int level);          // *R<M/S>NA<val>

    // Manual notch — UNDOCUMENTED commands, live-probe verified on the v3
    // firmware (see docs/cat-command-reference.md). The firmware silently
    // REJECTS out-of-range values, so these clamp before sending.
    void setNotchCenter(Rx rx, int hz);           // *R<M/S>NC<20-4000>
    void setNotchWidth(Rx rx, int hz);            // *R<M/S>NW<10-300>
    void setNotchEngaged(Rx rx, bool on);         // *R<M/S>NM<0/1>
    void setSaf(Rx rx, bool on);                  // *R<M/S>NS<0/1>

    // Queries (responses arrive asynchronously via the signals below).
    void queryFrequency(Rx rx);
    void queryFilter(Rx rx);
    void queryMode(Rx rx);
    void querySMeter();                           // ?S -> @SRM<m>S<s> (or @STF.. in TX)
    void queryAgc(Rx rx);                         // ?R<M/S>A
    void queryRfGain(Rx rx);                      // ?R<M/S>G
    void queryAttenuator(Rx rx);                  // ?R<M/S>T
    void queryNotch(Rx rx);                       // ?R.NC / ?R.NW / ?R.NM (undocumented)
    // Speculative: NR/NB/AN level queries appear in no document, but neither
    // did the notch ones and the radio answered those. Unanswered queries are
    // harmless; if replies come back the *Reported signals below fire.
    void queryDspLevels(Rx rx);                   // ?R.NN / ?R.NB / ?R.NA

signals:
    void frequencyReported(Rx rx, uint64_t hz);
    void bandwidthReported(Rx rx, int bwHz);
    void pbtReported(Rx rx, int pbtHz);
    void modeReported(Rx rx, Mode mode);
    void sMeterReported(int mainRaw, int subRaw); // raw units (hamlib cal table scale)
    void txMeterReported(double fwdWatts, double refWatts, double swr);
    void agcReported(Rx rx, char agc);
    void rfGainReported(Rx rx, int gain);
    void attenReported(Rx rx, int step);
    void notchCenterReported(Rx rx, int hz);
    void notchWidthReported(Rx rx, int hz);
    void notchEngagedReported(Rx rx, bool on);
    void safReported(Rx rx, bool on);
    void nrReported(Rx rx, int level);
    void nbReported(Rx rx, int level);
    void autoNotchReported(Rx rx, int level);
    void rawLine(const QByteArray& line);

private slots:
    void onLine(const QByteArray& line);

private:
    void send(const QByteArray& cmd);            // appends CR, writes
    static char rxLetter(Rx rx) { return rx == Rx::Main ? 'M' : 'S'; }

    SerialPort serial_;
    CapabilityProfile caps_;
};

} // namespace ttc
