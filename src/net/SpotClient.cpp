// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/SpotClient.h"

#include <QRegularExpression>
#include <QDateTime>
#include <cmath>

namespace ttc {

namespace {
constexpr qint64 kSpotTtlSecs = 20 * 60;         // spots fade after 20 minutes
constexpr int    kMaxSpots    = 400;

// "DX de W1AW-#:   14025.0  DL1XYZ   CW 25 dB   0123Z"  (freq is kHz)
const QRegularExpression kSpotRe(
    QStringLiteral(R"(^DX de\s+(\S+?):?\s+([0-9]+(?:\.[0-9]+)?)\s+([A-Z0-9/\-]{3,}))"),
    QRegularExpression::CaseInsensitiveOption);
} // namespace

SpotClient::SpotClient(QObject* parent) : QObject(parent) {
    connect(&sock_, &QTcpSocket::readyRead, this, &SpotClient::onData);
    connect(&sock_, &QTcpSocket::connected, this, [this] {
        loginSent_ = false;
        loginFallback_.start(3000);              // some nodes never say "login:"
        emit statusChanged(QString("spots: connected to %1").arg(host_));
    });
    connect(&sock_, &QTcpSocket::disconnected, this, [this] {
        if (enabled_) reconnect_.start(15000);
        emit statusChanged("spots: disconnected");
    });
    connect(&sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (enabled_) reconnect_.start(15000);
        emit statusChanged("spots: " + sock_.errorString());
    });
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, [this] {
        if (enabled_) openSocket();
    });
    loginFallback_.setSingleShot(true);
    connect(&loginFallback_, &QTimer::timeout, this, [this] {
        if (!loginSent_ && sock_.state() == QAbstractSocket::ConnectedState) {
            sock_.write(login_.toLatin1() + "\r\n");
            loginSent_ = true;
        }
    });
    pruneTimer_.setInterval(60000);
    connect(&pruneTimer_, &QTimer::timeout, this, &SpotClient::prune);
}

void SpotClient::configure(const QString& host, quint16 port, const QString& login) {
    host_  = host;
    port_  = port;
    login_ = login;
}

void SpotClient::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        openSocket();
        pruneTimer_.start();
    } else {
        reconnect_.stop();
        pruneTimer_.stop();
        sock_.abort();
        emit statusChanged("spots: off");
    }
}

void SpotClient::clear() {
    if (byCall_.isEmpty()) return;
    byCall_.clear();
    emit spotsChanged();
}

QVector<Spot> SpotClient::spots() const {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVector<Spot> out;
    out.reserve(byCall_.size());
    for (const Spot& s : byCall_)
        if (now - s.atSecs <= kSpotTtlSecs) out.push_back(s);
    return out;
}

void SpotClient::openSocket() {
    sock_.abort();
    lineBuf_.clear();
    emit statusChanged(QString("spots: connecting to %1:%2").arg(host_).arg(port_));
    sock_.connectToHost(host_, port_);
}

void SpotClient::onData() {
    lineBuf_ += sock_.readAll();
    // Answer the login prompt (VE7CC: "Please enter your call:").
    if (!loginSent_) {
        const QString sofar = QString::fromLatin1(lineBuf_).toLower();
        if (sofar.contains("login") || sofar.contains("call")) {
            sock_.write(login_.toLatin1() + "\r\n");
            loginSent_ = true;
        }
    }
    bool changed = false;
    int nl;
    while ((nl = lineBuf_.indexOf('\n')) >= 0) {
        const QString line = QString::fromLatin1(lineBuf_.left(nl)).trimmed();
        lineBuf_.remove(0, nl + 1);
        const auto m = kSpotRe.match(line);
        if (!m.hasMatch()) continue;
        const qint64 hz = static_cast<qint64>(std::llround(m.captured(2).toDouble() * 1000.0));
        if (hz < 1800000 || hz > 54000000) continue;   // HF/6m sanity
        Spot s;
        s.call   = m.captured(3).toUpper();
        s.hz     = hz;
        s.atSecs = QDateTime::currentSecsSinceEpoch();
        byCall_[s.call] = s;
        changed = true;
    }
    if (byCall_.size() > kMaxSpots) prune();
    if (changed) emit spotsChanged();
}

void SpotClient::prune() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool changed = false;
    for (auto it = byCall_.begin(); it != byCall_.end();) {
        if (now - it->atSecs > kSpotTtlSecs) {
            it = byCall_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    // Still over the cap (a very busy contest weekend): drop the oldest.
    while (byCall_.size() > kMaxSpots) {
        auto oldest = byCall_.begin();
        for (auto it = byCall_.begin(); it != byCall_.end(); ++it)
            if (it->atSecs < oldest->atSecs) oldest = it;
        byCall_.erase(oldest);
        changed = true;
    }
    if (changed) emit spotsChanged();
}

} // namespace ttc
