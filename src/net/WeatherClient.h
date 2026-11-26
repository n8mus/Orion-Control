// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ttc {

struct WeatherData {
    double tempC = 0, windKmh = 0, gustKmh = 0;
    int windDeg = 0, code = -1;
    QString when;                       // observation time, as Open-Meteo says
    bool valid() const { return code >= 0; }
};

// Current conditions for the clock block: Open-Meteo (free, no key), every
// 15 min. Position = station/zip (Zippopotam.us, cached in weather/*) when
// set, else the centre of station/grid. Failures keep the last values.
class WeatherClient : public QObject {
    Q_OBJECT
public:
    explicit WeatherClient(QObject* parent = nullptr);
    void start();                       // also call after Setup is saved
    WeatherData data() const { return d_; }
    QString source() const { return src_; }
    // "☁ 41°F  W 12 G22 mph" per station/units; empty until a reading.
    QString text() const;
    static QString icon(int wmoCode);

signals:
    void updated();

private:
    void poll();
    void fetchWeather(double lat, double lon);

    QNetworkAccessManager nam_;
    QTimer timer_;
    WeatherData d_;
    QString src_;
    quint64 gen_ = 0;                   // drops replies from a stale location
};

} // namespace ttc
