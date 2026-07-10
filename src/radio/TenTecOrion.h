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

    // Queries (responses arrive asynchronously via the signals below).
    void queryFrequency(Rx rx);
    void queryFilter(Rx rx);
    void queryMode(Rx rx);

signals:
    void frequencyReported(Rx rx, uint64_t hz);
    void bandwidthReported(Rx rx, int bwHz);
    void pbtReported(Rx rx, int pbtHz);
    void modeReported(Rx rx, Mode mode);
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
