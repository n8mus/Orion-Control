// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/RigctldServer.h"

#include <QTcpSocket>

namespace ttc {

RigctldServer::RigctldServer(RadioController* radio, QObject* parent)
    : QObject(parent), radio_(radio) {
    connect(&server_, &QTcpServer::newConnection, this, &RigctldServer::onNewConnection);
}

bool RigctldServer::listen(quint16 port) {
    return server_.listen(QHostAddress::Any, port);
}

void RigctldServer::onNewConnection() {
    while (QTcpSocket* s = server_.nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this, &RigctldServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        emit clientConnected(s->peerAddress().toString());
    }
}

static const char* modeToHamlib(Mode m) {
    switch (m) {
        case Mode::USB: return "USB";
        case Mode::LSB: return "LSB";
        case Mode::CWU: return "CW";
        case Mode::CWL: return "CWR";
        case Mode::AM:  return "AM";
        case Mode::FM:  return "FM";
    }
    return "USB";
}

static Mode hamlibToMode(const QByteArray& s) {
    if (s == "LSB") return Mode::LSB;
    if (s == "CW")  return Mode::CWU;
    if (s == "CWR") return Mode::CWL;
    if (s == "AM")  return Mode::AM;
    if (s == "FM")  return Mode::FM;
    return Mode::USB;
}

void RigctldServer::onReadyRead() {
    auto* s = qobject_cast<QTcpSocket*>(sender());
    if (!s) return;
    while (s->canReadLine()) {
        QByteArray line = s->readLine().trimmed();
        if (line.isEmpty()) continue;
        emit commandReceived(QString::fromLatin1(line));
        QByteArray reply = handleLine(line);
        if (!reply.isNull()) s->write(reply);
    }
}

// ---- Hamlib "extended response" protocol -----------------------------------
// cqrlog prefixes every command with '+' and expects replies of the form
//   <command echo>:\n<Name: value>...\nRPRT 0\n
// and sends compound polls as one line ("+f +m"). It also runs an init
// handshake: +\chk_vfo, then +\dump_caps, scanning the caps text for
// "Can get X: Y/N" flags that gate everything it does afterwards.

QByteArray RigctldServer::handleExtended(const QByteArray& cmd,
                                         const QList<QByteArray>& args) {
    QByteArray name = cmd;
    if (name.startsWith('\\')) name.remove(0, 1);

    // Canonical long names for the single-letter forms cqrlog polls with.
    if (name == "f") name = "get_freq";
    else if (name == "m") name = "get_mode";
    else if (name == "v") name = "get_vfo";
    else if (name == "t") name = "get_ptt";
    else if (name == "i") name = "get_split_freq";
    else if (name == "s") name = "get_split_vfo";
    else if (name == "F") name = "set_freq";
    else if (name == "M") name = "set_mode";
    else if (name == "T") name = "set_ptt";
    else if (name == "V") name = "set_vfo";

    QByteArray argEcho;
    for (const QByteArray& a : args) argEcho += " " + a;
    QByteArray r = name + ":" + argEcho + "\n";

    // cqrlog ignores "currVFO"/VFO arguments; so do we (one radio, one VFO).
    QList<QByteArray> a = args;
    while (!a.isEmpty() && (a[0] == "currVFO" || a[0].startsWith("VFO")))
        a.removeFirst();

    if (name == "chk_vfo") {
        r += "ChkVFO: 0\n";                         // no --vfo mode
    } else if (name == "dump_caps") {
        r += "Model name:\tTen-Tec Orion Console\n"
             "Mfg name:\tTen-Tec\n"
             "Backend version:\t1.0\n"
             "Rig type:\tTransceiver\n"
             "Can set Frequency:\tY\n"
             "Can get Frequency:\tY\n"
             "Can set Mode:\tY\n"
             "Can get Mode:\tY\n"
             "Can set VFO:\tY\n"
             "Can get VFO:\tN\n"
             "Can set PTT:\tY\n"
             "Can get PTT:\tY\n"
             "Can get Split VFO:\tY\n"
             "Can get Split Freq:\tY\n"
             "Can set Func:\tN\n"
             "Can get Func:\tN\n"
             "Can set Level:\tN\n"
             "Can get Level:\tN\n"
             "Can set Param:\tN\n"
             "Can get Param:\tN\n"
             "Can ctl Mem/VFO:\tN\n"
             "Can send Morse:\tN\n"
             "Can send Voice:\tN\n"
             "Can get power2mW:\tN\n"
             "Can get mW2power:\tN\n";
    } else if (name == "get_freq") {
        r += "Frequency: " + QByteArray::number(qulonglong(freqHz_)) + "\n";
    } else if (name == "get_mode") {
        r += QByteArray("Mode: ") + modeToHamlib(mode_) + "\n"
             "Passband: " + QByteArray::number(bwHz_) + "\n";
    } else if (name == "get_vfo") {
        r += "VFO: VFOA\n";
    } else if (name == "get_ptt") {
        r += QByteArray("PTT: ") + (ptt_ ? "1" : "0") + "\n";
    } else if (name == "get_split_freq") {
        r += "TX Frequency: " + QByteArray::number(qulonglong(freqHz_)) + "\n";
    } else if (name == "get_split_vfo") {
        r += "Split: 0\nTX VFO: VFOA\n";
    } else if (name == "set_freq") {
        if (!a.isEmpty()) {
            freqHz_ = static_cast<uint64_t>(a[0].toDouble());
            if (radio_) radio_->setFrequencyHz(Rx::Main, freqHz_);
        }
    } else if (name == "set_mode") {
        if (!a.isEmpty()) {
            mode_ = hamlibToMode(a[0]);
            if (radio_) radio_->setMode(Rx::Main, mode_);
            if (a.size() >= 2 && a[1].toInt() > 0) {
                bwHz_ = a[1].toInt();
                if (radio_) radio_->setBandwidthHz(Rx::Main, bwHz_);
            }
        }
    } else if (name == "set_ptt") {
        if (!a.isEmpty()) {
            ptt_ = a[0].toInt() != 0;
            emit pttRequested(ptt_);
        }
    } else if (name == "set_vfo" || name == "set_rit" || name == "set_xit"
               || name == "set_func" || name == "set_level" || name == "set_parm"
               || name == "vfo_op" || name == "set_powerstat") {
        // Accepted no-ops: enough for cqrlog's RIT-clear/tuner/power buttons
        // not to error out. (set_powerstat echo is scanned by cqrlog.)
    } else {
        return r + "RPRT -11\n";                    // unknown extended command
    }
    return r + "RPRT 0\n";
}

// Returns the bytes to send back. "RPRT 0\n" = success; "RPRT -11\n" = unimplemented.
QByteArray RigctldServer::handleLine(const QByteArray& line) {
    // Extended-protocol lines start with '+'; compound polls pack several
    // commands on one line ("+f +m"). Split and answer each in order.
    if (line.startsWith('+')) {
        QByteArray reply;
        QByteArray cmd;
        QList<QByteArray> args;
        const QList<QByteArray> tok = line.split(' ');
        for (const QByteArray& t : tok) {
            if (t.isEmpty()) continue;
            if (t.startsWith('+')) {
                if (!cmd.isEmpty()) reply += handleExtended(cmd, args);
                cmd = t.mid(1);
                args.clear();
            } else {
                args.append(t);
            }
        }
        if (!cmd.isEmpty()) reply += handleExtended(cmd, args);
        return reply;
    }

    const char c = line[0];
    const QList<QByteArray> tok = line.split(' ');

    switch (c) {
        case 'f':                                   // get frequency
            return QByteArray::number(static_cast<qulonglong>(freqHz_)) + "\n";
        case 'F':                                   // set frequency
            if (tok.size() >= 2) {
                freqHz_ = tok[1].toULongLong();
                if (radio_) radio_->setFrequencyHz(Rx::Main, freqHz_);
            }
            return "RPRT 0\n";
        case 'm':                                   // get mode + passband
            return QByteArray(modeToHamlib(mode_)) + "\n" + QByteArray::number(bwHz_) + "\n";
        case 'M':                                   // set mode [passband]
            if (tok.size() >= 2) {
                mode_ = hamlibToMode(tok[1]);
                if (radio_) radio_->setMode(Rx::Main, mode_);
                if (tok.size() >= 3 && tok[2].toInt() > 0) {
                    bwHz_ = tok[2].toInt();
                    if (radio_) radio_->setBandwidthHz(Rx::Main, bwHz_);
                }
            }
            return "RPRT 0\n";
        case 'v':                                   // get current VFO
            return "VFOA\n";
        case 'V':                                   // set VFO (single-VFO rig)
            return "RPRT 0\n";
        case 't':                                   // get PTT (cache tracks T/R
            return ptt_ ? "1\n" : "0\n";            // via the metering replies)
        case 'T':                                   // set PTT
            if (tok.size() >= 2) {
                const bool on = tok[1].toInt() != 0;
                ptt_ = on;                          // optimistic; polls confirm
                emit pttRequested(on);
            }
            return "RPRT 0\n";
        default: break;
    }

    if (line.startsWith("\\chk_vfo")) return "CHKVFO 0\n";
    if (line.startsWith("\\dump_state")) {
        // Minimal protocol-1 dump_state. TODO: advertise real Ten-Tec caps.
        return "0\n2\n2\n150000.000000 56000000.000000 0x1ff -1 -1 0x10000003 0x3\n"
               "0 0 0 0 0 0 0\n0 0 0 0 0 0 0\n0x1ff 1\n0x1ff 0\n0 0\n"
               "0x1e 2400\n0x2 500\n0x1 8000\n0 0\n9990\n0\n0\n0\n0\n0\n0\n";
    }
    if (c == 'q' || c == 'Q') return QByteArray();  // quit: let socket close

    return "RPRT -11\n";                            // unimplemented
}

} // namespace ttc
