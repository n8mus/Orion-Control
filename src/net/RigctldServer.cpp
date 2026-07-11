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

// Returns the bytes to send back. "RPRT 0\n" = success; "RPRT -11\n" = unimplemented.
QByteArray RigctldServer::handleLine(const QByteArray& line) {
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
