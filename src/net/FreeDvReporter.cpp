// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/FreeDvReporter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>

namespace ttc {

namespace {
const char* kUrl = "wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket";
qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }
}  // namespace

FreeDvReporter::FreeDvReporter(QObject* parent) : QObject(parent) {
    send = [this](const QString& t) { if (ws_) ws_->sendTextMessage(t); };
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &FreeDvReporter::connectNow);
    // No server ping within pingInterval + pingTimeout = a dead link
    // (freedv-backend's rule); re-armed on every frame.
    alive_.setSingleShot(true);
    alive_.setInterval(60000);
    connect(&alive_, &QTimer::timeout, this, [this] {
        if (ws_) ws_->abort();                     // -> disconnected -> retry
    });
    expire_.setInterval(30000);
    connect(&expire_, &QTimer::timeout, this, &FreeDvReporter::expire);
}

FreeDvReporter::~FreeDvReporter() { delete ws_; }

void FreeDvReporter::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        backoffS_ = 5;
        expire_.start();
        connectNow();
        return;
    }
    retry_.stop();
    alive_.stop();
    expire_.stop();
    if (ws_) {
        ws_->disconnect(this);
        ws_->close();
        ws_->deleteLater();
        ws_ = nullptr;
    }
    stations_.clear();
    if (!spots_.isEmpty()) {
        spots_.clear();
        emit spotsChanged();
    }
    emit statusChanged("FreeDV Reporter: off");
}

void FreeDvReporter::connectNow() {
    if (!enabled_) return;
    if (!ws_) {
        ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(ws_, &QWebSocket::textMessageReceived, this, &FreeDvReporter::handleFrame);
        connect(ws_, &QWebSocket::connected, this, [this] {
            backoffS_ = 5;
            alive_.start();
        });
        connect(ws_, &QWebSocket::disconnected, this, [this] {
            alive_.stop();
            stations_.clear();                     // a reconnect re-sends it all
            if (!enabled_) return;
            emit statusChanged(QString("FreeDV Reporter: disconnected, retry in %1 s")
                                   .arg(backoffS_));
            retry_.start(backoffS_ * 1000);
            backoffS_ = std::min(backoffS_ * 2, 60);
        });
    }
    ws_->open(QUrl(QString::fromLatin1(kUrl)));
}

void FreeDvReporter::handleFrame(const QString& f) {
    alive_.start();
    if (f.isEmpty()) return;
    switch (f[0].toLatin1()) {
    case '0':                                      // EIO open -> SIO connect
        send(QStringLiteral(R"(40{"role":"view","protocol_version":2})"));
        return;
    case '2':                                      // ping
        send(QStringLiteral("3"));
        return;
    case '4':
        break;
    default:                                       // 1 close, 3 pong, 6 noop
        return;
    }
    if (f.startsWith(QLatin1String("40"))) {
        emit statusChanged("FreeDV Reporter: connected");
        return;
    }
    if (f.startsWith(QLatin1String("41"))) return; // server-side disconnect
    if (!f.startsWith(QLatin1String("42"))) return;
    const QJsonDocument d = QJsonDocument::fromJson(f.mid(2).toUtf8());
    const QJsonArray a = d.array();
    if (a.size() < 1) return;
    const QString name = a.at(0).toString();
    const size_t before = size_t(spots_.size());
    if (name == QLatin1String("bulk_update")) {
        // [[name,{...}], ...] — the snapshot on connect, replayed through
        // the same handlers, minus old tx_reports making spots.
        for (const QJsonValue& v : a.at(1).toArray()) {
            const QJsonArray p = v.toArray();
            handleEvent(p.at(0).toString(), p.at(1).toObject(), true);
        }
    } else {
        handleEvent(name, a.at(1).toObject(), false);
    }
    if (size_t(spots_.size()) != before || name == QLatin1String("tx_report")
        || name == QLatin1String("rx_report"))
        emit spotsChanged();
}

void FreeDvReporter::handleEvent(const QString& name, const QJsonObject& o, bool replay) {
    const QString sid = o.value("sid").toString();
    if (name == QLatin1String("new_connection")) {
        Station& s = stations_[sid];
        s.call = o.value("callsign").toString().toUpper();
        s.grid = o.value("grid_square").toString();
    } else if (name == QLatin1String("remove_connection")) {
        stations_.remove(sid);
    } else if (name == QLatin1String("freq_change")) {
        Station& s = stations_[sid];
        if (s.call.isEmpty()) s.call = o.value("callsign").toString().toUpper();
        if (s.grid.isEmpty()) s.grid = o.value("grid_square").toString();
        s.hz = qint64(o.value("freq").toDouble());
    } else if (name == QLatin1String("tx_report")) {
        Station& s = stations_[sid];
        if (s.call.isEmpty()) s.call = o.value("callsign").toString().toUpper();
        s.mode = o.value("mode").toString();
        if (!replay && o.value("transmitting").toBool())
            addSpot(s.call, s.hz, s.mode, s.grid);
    } else if (name == QLatin1String("rx_report")) {
        const QString heard = o.value("callsign").toString().toUpper();
        const auto it = stations_.constFind(sid);  // the RECEIVER's sid
        if (heard.isEmpty() || it == stations_.constEnd()) return;
        const QString rxCall = o.value("receiver_callsign").toString().toUpper();
        addSpot(heard, it->hz,
                QString("%1 heard by %2 %3 dB").arg(o.value("mode").toString(), rxCall)
                    .arg(o.value("snr").toDouble(), 0, 'f', 0),
                QString());
    }
}

void FreeDvReporter::addSpot(const QString& call, qint64 hz, const QString& comment,
                             const QString& grid) {
    // HF/6 m sanity, as the cluster parser and POTA clamp.
    if (call.isEmpty() || hz < 1800000 || hz > 54000000) return;
    Spot s;
    s.call = call;
    s.hz = hz;
    s.atSecs = nowSecs();
    s.kind = 'V';
    s.tag = QStringLiteral("FreeDV");
    s.grid = grid;
    s.spotter = QStringLiteral("FreeDV Reporter");
    s.comment = comment;
    spots_[call] = s;
}

void FreeDvReporter::expire() {
    const qint64 now = nowSecs();
    bool gone = false;
    for (auto it = spots_.begin(); it != spots_.end();) {
        if (now - it->atSecs > kSpotLifeSecs) { it = spots_.erase(it); gone = true; }
        else ++it;
    }
    if (gone) emit spotsChanged();
}

QVector<Spot> FreeDvReporter::spots() const {
    QVector<Spot> v;
    v.reserve(spots_.size());
    for (const Spot& s : spots_) v.push_back(s);
    return v;
}

} // namespace ttc
