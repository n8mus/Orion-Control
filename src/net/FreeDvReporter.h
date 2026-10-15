// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QJsonValue>
#include <functional>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "net/SpotClient.h"   // Spot

class QWebSocket;

namespace ttc {

// FreeDV Reporter (qso.freedv.org) as a spot source, view-only: who is on
// FreeDV/RADE right now and where. The protocol is Socket.IO v4 over a
// WebSocket, spoken by hand as AetherSDR's FreeDvClient (GPL-3, KK7GWY and
// contributors) and freedv-backend's SocketIoClient (BSD-2) do:
//   server "0{sid,pingInterval,...}"  -> we "40{role:view,protocol_version:2}"
//   server "2" (ping)                 -> we "3" (pong); EIO v4 pings are
//                                        the server's, the client only answers
//   server '42["event",{...}]'        -> station table / spots
// A station table keyed by sid follows new_connection / freq_change /
// tx_report / remove_connection (frequency in Hz, last_tx ISO time).
// Every online station with a callsign and an HF/6 m frequency is a spot
// (kind 'V') — the spot table's "where is FreeDV right now", every band:
//   ON AIR          tx_report transmitting=true      (band map too)
//   TX n min ago    last_tx within 20 min            (band map too)
//   listening       on a frequency, quiet longer     (spot table only —
//                   a dozen of them parked on 14.236 would bury the map)
// plus, AetherSDR's rule, rx_report with a decoded callsign -> the HEARD
// station at the receiver's frequency with the SNR (20 min). Unlike
// AetherSDR the snapshot on connect counts too: last_tx carries the real
// time, so the table is full the moment it connects (measured: 33
// stations with a frequency on a weekday morning, two on the air).
class FreeDvReporter : public QObject {
    Q_OBJECT
public:
    explicit FreeDvReporter(QObject* parent = nullptr);
    ~FreeDvReporter() override;

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    QVector<Spot> spots() const;
    int stationCount() const { return int(stations_.size()); }

    // One Engine.IO text frame (public for freedvtest). Replies go out
    // through send(); events update the table and emit spotsChanged.
    void handleFrame(const QString& frame);
    std::function<void(const QString&)> send;      // default: the socket

    static constexpr qint64 kSpotLifeSecs = 1200;  // heard spots; "lately" on the map
    // Test clock (epoch secs); null = the real one.
    std::function<qint64()> now;

signals:
    void spotsChanged();
    void statusChanged(const QString& s);

private:
    struct Station {
        QString call, grid, mode;
        qint64 hz = 0;
        qint64 lastTx = 0, lastUpdate = 0;         // epoch secs, 0 = never
        bool transmitting = false, rxOnly = false;
    };
    void connectNow();
    void handleEvent(const QString& name, const QJsonObject& o, bool replay);
    void addHeard(const QString& call, qint64 hz, const QString& comment);
    static qint64 isoSecs(const QJsonValue& v);
    void expire();

    QWebSocket* ws_ = nullptr;
    QTimer retry_, alive_, expire_;
    int backoffS_ = 5;
    bool enabled_ = false;
    QHash<QString, Station> stations_;             // by sid
    QHash<QString, Spot> heard_;                   // rx_report finds, by call
};

} // namespace ttc
