// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QHash>
#include <QVector>

namespace ttc {

// One DX-cluster spot, newest sighting per callsign.
struct Spot {
    QString call;
    qint64  hz     = 0;
    qint64  atSecs = 0;      // epoch seconds of the last sighting
};

// Minimal DX-cluster telnet client (the same feed KE9NS PowerSDR's Spotter
// uses): connects to a cluster node, answers the login prompt with the
// operator's callsign, and parses "DX de SPOTTER: 14025.0 CALL ..." lines.
// Spots are deduped per call (latest freq wins) and expire after 20 minutes.
// Reconnects with backoff while enabled; fully quiet when disabled.
class SpotClient : public QObject {
    Q_OBJECT
public:
    explicit SpotClient(QObject* parent = nullptr);

    void configure(const QString& host, quint16 port, const QString& login);
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    void clear();

    QVector<Spot> spots() const;                 // unexpired, unsorted

signals:
    void spotsChanged();
    void statusChanged(const QString& s);        // for the status bar

private:
    void openSocket();
    void onData();
    void prune();

    QTcpSocket sock_;
    QTimer     reconnect_;                       // retry while enabled
    QTimer     pruneTimer_;
    QTimer     loginFallback_;                   // send login even w/o a prompt
    QString    host_  = "dxc.ve7cc.net";
    quint16    port_  = 23;
    QString    login_;
    bool       enabled_   = false;
    bool       loginSent_ = false;
    QByteArray lineBuf_;
    QHash<QString, Spot> byCall_;
};

} // namespace ttc
