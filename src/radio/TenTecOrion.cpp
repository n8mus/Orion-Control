// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TenTecOrion.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
    if (std::getenv("TTC_TRACE"))
        std::fprintf(stderr, "[cat->] %s\n", cmd.constData());
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

void TenTecOrion::setAgc(Rx rx, char agc) {
    if (agc != 'F' && agc != 'M' && agc != 'S' && agc != 'P' && agc != 'O') return;
    send(QByteArray("*R") + rxLetter(rx) + "A" + agc);
}

void TenTecOrion::setRfGain(Rx rx, int gain) {
    send(QByteArray("*R") + rxLetter(rx) + "G" + QByteArray::number(clampi(gain, 0, 100)));
}

void TenTecOrion::setAttenuator(Rx rx, int step) {
    send(QByteArray("*R") + rxLetter(rx) + "T" + QByteArray::number(clampi(step, 0, 3)));
}

void TenTecOrion::setNoiseReduction(Rx rx, int level) {
    send(QByteArray("*R") + rxLetter(rx) + "NN" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::setNoiseBlanker(Rx rx, int level) {
    send(QByteArray("*R") + rxLetter(rx) + "NB" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::setAutoNotch(Rx rx, int level) {
    send(QByteArray("*R") + rxLetter(rx) + "NA" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::querySMeter()          { send("?S"); }
void TenTecOrion::queryAgc(Rx rx)        { send(QByteArray("?R") + rxLetter(rx) + "A"); }
void TenTecOrion::queryRfGain(Rx rx)     { send(QByteArray("?R") + rxLetter(rx) + "G"); }
void TenTecOrion::queryAttenuator(Rx rx) { send(QByteArray("?R") + rxLetter(rx) + "T"); }

void TenTecOrion::queryFrequency(Rx rx) {
    const char v = (rx == Rx::Main) ? 'A' : 'B';
    send(QByteArray("?") + v + "F");
}

void TenTecOrion::queryFilter(Rx rx) {
    send(QByteArray("?R") + rxLetter(rx) + "F");
    send(QByteArray("?R") + rxLetter(rx) + "P");
}

void TenTecOrion::queryMode(Rx rx) {
    send(QByteArray("?R") + rxLetter(rx) + "M");
}

// Parse a leading run of digits (optionally signed), ignoring any trailing junk.
// Responses occasionally arrive glued together on the wire; toULongLong() would
// return 0 for the whole line, which upstream must never mistake for a frequency.
static bool parseLeadingInt(const QByteArray& s, qlonglong& out) {
    int i = 0, digits = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    qlonglong v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; ++digits; }
    if (digits == 0) return false;
    out = neg ? -v : v;
    return true;
}

void TenTecOrion::onLine(const QByteArray& line) {
    if (std::getenv("TTC_TRACE"))
        std::fprintf(stderr, "[cat<-] %s\n", line.constData());
    emit rawLine(line);
    // Responses are '@'-prefixed, e.g. @AF14250000, @RMF2400, @RMP-200.
    if (line.size() < 3 || line[0] != '@') return;

    qlonglong v = 0;
    if (line[1] == 'A' || line[1] == 'B') {          // @AF<hz> / @BF<hz>
        if (line[2] == 'F' && parseLeadingInt(line.mid(3), v)
            && v >= 100000 && v <= 60000000) {        // sanity: 100 kHz .. 60 MHz
            Rx rx = (line[1] == 'A') ? Rx::Main : Rx::Sub;
            emit frequencyReported(rx, static_cast<uint64_t>(v));
        }
        return;
    }
    if (line[1] == 'S' && line.size() >= 4) {         // ?S reply
        if (line[2] == 'R' && line[3] == 'M') {       // RX: @SRM<main>S<sub>
            qlonglong mainRaw = 0, subRaw = 0;
            if (parseLeadingInt(line.mid(4), mainRaw) && mainRaw >= 0 && mainRaw <= 300) {
                const int sPos = line.indexOf('S', 4);
                if (sPos > 0 && parseLeadingInt(line.mid(sPos + 1), subRaw)
                    && subRaw >= 0 && subRaw <= 300)
                    emit sMeterReported(static_cast<int>(mainRaw), static_cast<int>(subRaw));
            }
        } else if (line[2] == 'T' && line[3] == 'F') { // TX: @STF<fwd>R<ref>S<swr>
            const int rPos = line.indexOf('R', 4);
            const int sPos = (rPos > 0) ? line.indexOf('S', rPos + 1) : -1;
            if (rPos > 0 && sPos > 0)
                emit txMeterReported(std::atof(line.mid(4, rPos - 4).constData()),
                                     std::atof(line.mid(rPos + 1, sPos - rPos - 1).constData()),
                                     std::atof(line.mid(sPos + 1).constData()));
        }
        return;
    }
    if (line[1] == 'R' && line.size() >= 4) {         // @R[M/S][F/P/M/A/G/T]<val>
        Rx rx = (line[2] == 'M') ? Rx::Main : Rx::Sub;
        if (line[3] == 'F' && parseLeadingInt(line.mid(4), v) && v >= 100 && v <= 6000)
            emit bandwidthReported(rx, static_cast<int>(v));
        else if (line[3] == 'P' && parseLeadingInt(line.mid(4), v) && v >= -8000 && v <= 8000)
            emit pbtReported(rx, static_cast<int>(v));
        else if (line[3] == 'M' && line.size() >= 5 && line[4] >= '0' && line[4] <= '5') {
            static const Mode modes[] = { Mode::USB, Mode::LSB, Mode::CWU,
                                          Mode::CWL, Mode::AM,  Mode::FM };
            emit modeReported(rx, modes[line[4] - '0']);
        }
        else if (line[3] == 'A' && line.size() >= 5) { // AGC letter (P may trail data)
            const char a = line[4];
            if (a == 'F' || a == 'M' || a == 'S' || a == 'P' || a == 'O')
                emit agcReported(rx, a);
        }
        else if (line[3] == 'G' && parseLeadingInt(line.mid(4), v) && v >= 0 && v <= 100)
            emit rfGainReported(rx, static_cast<int>(v));
        else if (line[3] == 'T' && line.size() >= 5 && line[4] >= '0' && line[4] <= '3')
            emit attenReported(rx, line[4] - '0');
    }
}

} // namespace ttc
