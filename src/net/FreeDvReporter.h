// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
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
// remove_connection (frequency in Hz). Spots, AetherSDR's rules:
//   tx_report transmitting=true -> the station itself, at its frequency
//   rx_report with a callsign   -> the HEARD station, at the receiver's
//                                  frequency, with the SNR
// freq_change alone never spots (people scan), nor does a bulk_update
// replay of old tx_reports. Kind 'V'; spots age out after 20 minutes.
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

    static constexpr qint64 kSpotLifeSecs = 1200;

signals:
    void spotsChanged();
    void statusChanged(const QString& s);

private:
    struct Station {
        QString call, grid, mode;
        qint64 hz = 0;
    };
    void connectNow();
    void handleEvent(const QString& name, const QJsonObject& o, bool replay);
    void addSpot(const QString& call, qint64 hz, const QString& comment,
                 const QString& grid);
    void expire();

    QWebSocket* ws_ = nullptr;
    QTimer retry_, alive_, expire_;
    int backoffS_ = 5;
    bool enabled_ = false;
    QHash<QString, Station> stations_;             // by sid
    QHash<QString, Spot> spots_;                   // by call
};

} // namespace ttc
