// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TenTecOrion.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    // Hard safety gate: a malformed CAT/rigctld request (e.g. "*AF0") would
    // otherwise send the radio to 0 Hz and off frequency. Never let anything
    // outside the Orion's real RX range reach the hardware.
    if (hz < 100000 || hz > 60000000) {
        if (std::getenv("TTC_TRACE"))
            std::fprintf(stderr, "[cat] REJECTED out-of-range setFrequency %llu\n",
                         static_cast<unsigned long long>(hz));
        return;
    }
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
    // Rev 1.2 documents *R.NN for NR, but v3 firmware silently ignores it —
    // live probe found NR moved to letter 'R' (set/read-back verified).
    send(QByteArray("*R") + rxLetter(rx) + "NR" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::setNoiseBlanker(Rx rx, int level) {
    send(QByteArray("*R") + rxLetter(rx) + "NB" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::setAutoNotch(Rx rx, int level) {
    send(QByteArray("*R") + rxLetter(rx) + "NA" + QByteArray::number(clampi(level, 0, 9)));
}

void TenTecOrion::setPreamp(Rx rx, bool on) {
    send(QByteArray("*R") + rxLetter(rx) + "E" + (on ? "1" : "0"));
}

void TenTecOrion::setNotchCenter(Rx rx, int hz) {
    send(QByteArray("*R") + rxLetter(rx) + "NC" + QByteArray::number(clampi(hz, 20, 4000)));
}

void TenTecOrion::setNotchWidth(Rx rx, int hz) {
    send(QByteArray("*R") + rxLetter(rx) + "NW" + QByteArray::number(clampi(hz, 10, 300)));
}

void TenTecOrion::setNotchEngaged(Rx rx, bool on) {
    send(QByteArray("*R") + rxLetter(rx) + "NM" + (on ? "1" : "0"));
}

void TenTecOrion::setSaf(Rx rx, bool on) {
    send(QByteArray("*R") + rxLetter(rx) + "NS" + (on ? "1" : "0"));
}

void TenTecOrion::setPtt(bool on)        { send(on ? "*TK" : "*TU"); }
void TenTecOrion::setTxPower(int pct)    { send("*TP" + QByteArray::number(clampi(pct, 0, 100))); }
void TenTecOrion::setMicGain(int pct)    { send("*TM" + QByteArray::number(clampi(pct, 0, 100))); }
void TenTecOrion::setSpeechProc(int lvl) { send("*TS" + QByteArray::number(clampi(lvl, 0, 9))); }
void TenTecOrion::setAuxInputGain(int p) { send("*TI" + QByteArray::number(clampi(p, 0, 100))); }
void TenTecOrion::setMonitor(int pct)    { send("*TO" + QByteArray::number(clampi(pct, 0, 100))); }
void TenTecOrion::setAfVolume(Rx rx, int pct) {
    // Docs claim 0-100 but the real scale is a byte: *UM100 lands at ~40% of
    // max audio (100/255), measured on the v3 radio. Map percent -> 0-255.
    send(QByteArray("*U") + (rx == Rx::Main ? 'M' : 'S')
         + QByteArray::number(clampi(pct, 0, 100) * 255 / 100));
}

void TenTecOrion::queryAfVolume(Rx rx) {
    send(QByteArray("?U") + (rx == Rx::Main ? 'M' : 'S'));
}

void TenTecOrion::setAudioRouting(char left, char right, char speaker) {
    send(QByteArray("*UC") + left + right + speaker);
}

void TenTecOrion::queryAudioRouting() { send("?UC"); }
void TenTecOrion::setTunerEnabled(bool on) { send(on ? "*TT1" : "*TT0"); }
void TenTecOrion::startTune()            { send("*TTT"); }

void TenTecOrion::queryTxPower()         { send("?TP"); }
void TenTecOrion::queryTuner()           { send("?TT"); }
void TenTecOrion::queryAuxInputGain()    { send("?TI"); }
void TenTecOrion::queryTxAudio() {
    send("?TM");
    send("?TS");
    send("?TO");
    send("?UM");                                   // volume query is speculative
}

void TenTecOrion::querySMeter()          { send("?S"); }
void TenTecOrion::queryAgc(Rx rx)        { send(QByteArray("?R") + rxLetter(rx) + "A"); }
void TenTecOrion::queryRfGain(Rx rx)     { send(QByteArray("?R") + rxLetter(rx) + "G"); }
void TenTecOrion::queryAttenuator(Rx rx) { send(QByteArray("?R") + rxLetter(rx) + "T"); }
void TenTecOrion::queryPreamp(Rx rx)     { send(QByteArray("?R") + rxLetter(rx) + "E"); }

void TenTecOrion::queryNotch(Rx rx) {
    send(QByteArray("?R") + rxLetter(rx) + "NC");
    send(QByteArray("?R") + rxLetter(rx) + "NW");
    send(QByteArray("?R") + rxLetter(rx) + "NM");
}

void TenTecOrion::setHardwareNb(Rx rx, bool on) {
    send(QByteArray("*R") + rxLetter(rx) + "NH" + (on ? "1" : "0"));
}

void TenTecOrion::queryDspLevels(Rx rx) {
    send(QByteArray("?R") + rxLetter(rx) + "NR");   // v3 NR letter (NN is dead)
    send(QByteArray("?R") + rxLetter(rx) + "NB");
    send(QByteArray("?R") + rxLetter(rx) + "NA");
    send(QByteArray("?R") + rxLetter(rx) + "NH");   // hardware blanker on/off
}

void TenTecOrion::setVfoAssignment(char mainRx, char subRx, char tx) {
    send(QByteArray("*KV") + mainRx + subRx + tx);
}

void TenTecOrion::queryVfoAssignment() { send("?KV"); }

void TenTecOrion::setAntennaRouting(char ant1, char ant2, char rxAnt) {
    send(QByteArray("*KA") + ant1 + ant2 + rxAnt);
}

void TenTecOrion::queryAntennaRouting() { send("?KA"); }

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
            if (rPos > 0 && sPos > 0) {
                // v3 firmware sends SWR as 8.8 fixed point (x256): live trace
                // "@STF024R004S608" = 24 W fwd, 4 W ref, 608/256 = SWR 2.38,
                // which matches the SWR computed from those powers. The manual
                // shows a decimal ("S1.1") — accept that too if a '.' appears.
                const QByteArray swrField = line.mid(sPos + 1);
                double swr = std::atof(swrField.constData());
                if (!swrField.contains('.')) swr /= 256.0;
                emit txMeterReported(std::atof(line.mid(4, rPos - 4).constData()),
                                     std::atof(line.mid(rPos + 1, sPos - rPos - 1).constData()),
                                     swr);
            }
        }
        return;
    }
    if (line[1] == 'T' && line.size() >= 4) {         // transmitter group @T<P/M/O/T>
        if (line[2] == 'P' && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 100)
            emit txPowerReported(static_cast<int>(v));
        else if (line[2] == 'M' && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 100)
            emit micGainReported(static_cast<int>(v));
        else if (line[2] == 'S' && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 9)
            emit speechProcReported(static_cast<int>(v));
        else if (line[2] == 'I' && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 100)
            emit auxInputGainReported(static_cast<int>(v));
        else if (line[2] == 'O' && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 100)
            emit monitorReported(static_cast<int>(v));
        else if (line[2] == 'T' && (line[3] == '0' || line[3] == '1'))
            emit tunerReported(line[3] == '1');
        return;
    }
    if (line[1] == 'K' && line.size() >= 6) {         // @KV<m><s><t> / @KA<1><2><rx>
        auto in = [](char c, const char* set) { return std::strchr(set, c) != nullptr; };
        if (line[2] == 'V' && in(line[3], "AB")
            && in(line[4], "ABN") && in(line[5], "ABN"))
            emit vfoAssignmentReported(line[3], line[4], line[5]);
        else if (line[2] == 'A' && in(line[3], "MSBN")
                 && in(line[4], "MSBN") && in(line[5], "MSBN"))
            emit antennaRoutingReported(line[3], line[4], line[5]);
        return;
    }
    if (line[1] == 'U' && line.size() >= 4) {         // audio group @U<M/S/B/C>...
        if (line[2] == 'C') {                          // @UC<left><right><spk>
            auto in = [](char c) { return c == 'M' || c == 'S' || c == 'B'; };
            if (line.size() >= 6 && in(line[3]) && in(line[4]) && in(line[5]))
                emit audioRoutingReported(line[3], line[4], line[5]);
            return;
        }
        if ((line[2] == 'M' || line[2] == 'S' || line[2] == 'B')
            && parseLeadingInt(line.mid(3), v) && v >= 0 && v <= 255) {
            const int pct = static_cast<int>(v * 100 / 255);   // byte -> percent
            if (line[2] != 'S') emit afVolumeReported(Rx::Main, pct);
            if (line[2] != 'M') emit afVolumeReported(Rx::Sub, pct);
            // Combined form @UM<m>S<s> (documented "both" response).
            if (line[2] == 'M') {
                const int sPos = line.indexOf('S', 3);
                qlonglong v2 = 0;
                if (sPos > 0 && parseLeadingInt(line.mid(sPos + 1), v2)
                    && v2 >= 0 && v2 <= 255)
                    emit afVolumeReported(Rx::Sub, static_cast<int>(v2 * 100 / 255));
            }
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
        else if (line[3] == 'E' && line.size() >= 5 && (line[4] == '0' || line[4] == '1'))
            emit preampReported(rx, line[4] == '1');
        else if (line[3] == 'N' && line.size() >= 6) {  // @R.N<C/W/M/S/N/B/A><val>
            const char sub = line[4];
            const bool num = parseLeadingInt(line.mid(5), v);
            if (sub == 'C' && num && v >= 20 && v <= 4000)
                emit notchCenterReported(rx, static_cast<int>(v));
            else if (sub == 'W' && num && v >= 10 && v <= 300)
                emit notchWidthReported(rx, static_cast<int>(v));
            else if (sub == 'M' && num && (v == 0 || v == 1))
                emit notchEngagedReported(rx, v == 1);
            else if (sub == 'S' && num && (v == 0 || v == 1))
                emit safReported(rx, v == 1);
            else if ((sub == 'R' || sub == 'N') && num && v >= 0 && v <= 9)
                emit nrReported(rx, static_cast<int>(v));   // 'R' on v3, 'N' pre-v3
            else if (sub == 'B' && num && v >= 0 && v <= 9)
                emit nbReported(rx, static_cast<int>(v));
            else if (sub == 'A' && num && v >= 0 && v <= 9)
                emit autoNotchReported(rx, static_cast<int>(v));
            else if (sub == 'H' && num && (v == 0 || v == 1))
                emit hardwareNbReported(rx, v == 1);
        }
    }
}

} // namespace ttc
