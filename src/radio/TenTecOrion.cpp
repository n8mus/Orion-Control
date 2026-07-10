// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TenTecOrion.h"

#include <algorithm>

namespace ttc {

static int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

TenTecOrion::TenTecOrion(QObject* parent) : QObject(parent) {
    caps_.name             = "Ten-Tec Orion 565/566";
    caps_.bandwidthMinHz   = 100;
    caps_.bandwidthMaxHz   = 6000;
    caps_.bandwidthStepHz  = 1;       // on-the-fly, 1 Hz resolution
    caps_.pbtRangeHz       = 8000;
    caps_.continuousFilter = true;    // the flagship: smooth passband drag
    caps_.dualReceiver     = true;
    caps_.needsHwHandshake = false;

    connect(&serial_, &SerialPort::lineReceived, this, &TenTecOrion::onLine);
}

bool TenTecOrion::open(const std::string& device) {
    return serial_.open(device, 57600, caps_.needsHwHandshake);
}

void TenTecOrion::close() { serial_.close(); }

void TenTecOrion::send(const QByteArray& cmd) {
    QByteArray out = cmd;
    out.append('\r');                 // ASCII CR terminates every Orion command
    serial_.write(out);
}

void TenTecOrion::setFrequencyHz(Rx rx, uint64_t hz) {
    // *AF<hz> for VFO A (main), *BF<hz> for VFO B (sub), frequency in Hz.
    const char v = (rx == Rx::Main) ? 'A' : 'B';
    send(QByteArray("*") + v + "F" + QByteArray::number(static_cast<qulonglong>(hz)));
}

void TenTecOrion::setMode(Rx rx, Mode mode) {
    // *RMM<code> / *RSM<code>. Codes: USB0 LSB1 UCW2 LCW3 AM4 FM5.
    int code = 0;
    switch (mode) {
        case Mode::USB: code = 0; break;
        case Mode::LSB: code = 1; break;
        case Mode::CWU: code = 2; break;   // UCW
        case Mode::CWL: code = 3; break;   // LCW
        case Mode::AM:  code = 4; break;
        case Mode::FM:  code = 5; break;
    }
    send(QByteArray("*R") + rxLetter(rx) + "M" + QByteArray::number(code));
}

void TenTecOrion::setBandwidthHz(Rx rx, int bwHz) {
    bwHz = clampi(bwHz, caps_.bandwidthMinHz, caps_.bandwidthMaxHz);
    send(QByteArray("*R") + rxLetter(rx) + "F" + QByteArray::number(bwHz));
}

void TenTecOrion::setPbtHz(Rx rx, int pbtHz) {
    pbtHz = clampi(pbtHz, -caps_.pbtRangeHz, caps_.pbtRangeHz);
    send(QByteArray("*R") + rxLetter(rx) + "P" + QByteArray::number(pbtHz));
}

void TenTecOrion::setPassband(Rx rx, int loEdgeHz, int hiEdgeHz) {
    if (hiEdgeHz < loEdgeHz) std::swap(loEdgeHz, hiEdgeHz);
    // The bijection: two edges -> width + center offset (see cat-command-reference.md).
    const int width  = hiEdgeHz - loEdgeHz;
    const int center = (hiEdgeHz + loEdgeHz) / 2;
    setBandwidthHz(rx, width);
    setPbtHz(rx, center);
}

void TenTecOrion::queryFrequency(Rx rx) {
    const char v = (rx == Rx::Main) ? 'A' : 'B';
    send(QByteArray("?") + v + "F");
}

void TenTecOrion::queryFilter(Rx rx) {
    send(QByteArray("?R") + rxLetter(rx) + "F");
    send(QByteArray("?R") + rxLetter(rx) + "P");
}

void TenTecOrion::onLine(const QByteArray& line) {
    emit rawLine(line);
    // Responses are '@'-prefixed, e.g. @AF14250000, @RMF2400, @RMP-200.
    if (line.size() < 3 || line[0] != '@') return;

    if (line[1] == 'A' || line[1] == 'B') {          // @AF<hz> / @BF<hz>
        if (line[2] == 'F') {
            Rx rx = (line[1] == 'A') ? Rx::Main : Rx::Sub;
            emit frequencyReported(rx, line.mid(3).toULongLong());
        }
        return;
    }
    if (line[1] == 'R' && line.size() >= 4) {         // @R[M/S][F/P]<val>
        Rx rx = (line[2] == 'M') ? Rx::Main : Rx::Sub;
        if (line[3] == 'F') emit bandwidthReported(rx, line.mid(4).toInt());
        else if (line[3] == 'P') emit pbtReported(rx, line.mid(4).toInt());
    }
}

} // namespace ttc
