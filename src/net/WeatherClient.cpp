// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/WeatherClient.h"
#include "util/CtyLookup.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

namespace ttc {

WeatherClient::WeatherClient(QObject* parent) : QObject(parent) {
    timer_.setInterval(15 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this, &WeatherClient::poll);
}

void WeatherClient::start() {
    timer_.start();
    poll();
}

void WeatherClient::poll() {
    QSettings s;
    const QString zip = s.value("station/zip").toString().trimmed();
    const quint64 gen = ++gen_;
    if (!zip.isEmpty()) {
        const QString cc = s.value("station/zipCountry", "us").toString().trimmed().toLower();
        const QString key = cc + "/" + zip;
        if (s.value("weather/zipKey").toString() == key) {   // resolved before
            src_ = "ZIP " + zip;
            fetchWeather(s.value("weather/lat").toDouble(),
                         s.value("weather/lon").toDouble());
            return;
        }
        QNetworkRequest req{QUrl(QString("https://api.zippopotam.us/%1/%2")
                                     .arg(QString(QUrl::toPercentEncoding(cc)),
                                          QString(QUrl::toPercentEncoding(zip))))};
        req.setTransferTimeout(15000);
        QNetworkReply* r = nam_.get(req);
        connect(r, &QNetworkReply::finished, this, [this, r, gen, key, zip] {
            r->deleteLater();
            if (gen != gen_ || r->error() != QNetworkReply::NoError) return;
            const auto places = QJsonDocument::fromJson(r->readAll())
                                    .object().value("places").toArray();
            if (places.isEmpty()) return;
            const auto p = places.at(0).toObject();
            const double lat = p.value("latitude").toString().toDouble();
            const double lon = p.value("longitude").toString().toDouble();
            QSettings st;
            st.setValue("weather/zipKey", key);
            st.setValue("weather/lat", lat);
            st.setValue("weather/lon", lon);
            src_ = "ZIP " + zip;
            fetchWeather(lat, lon);
        });
        return;
    }
    double lat = 0, lon = 0;
    if (!CtyLookup::gridToLatLon(s.value("station/grid").toString(), lat, lon)) {
        d_ = WeatherData();                       // nothing to look up
        emit updated();
        return;
    }
    src_ = "grid " + s.value("station/grid").toString();
    fetchWeather(lat, lon);
}

void WeatherClient::fetchWeather(double lat, double lon) {
    const quint64 gen = gen_;
    QUrl u("https://api.open-meteo.com/v1/forecast");
    QUrlQuery q;
    q.addQueryItem("latitude", QString::number(lat, 'f', 3));
    q.addQueryItem("longitude", QString::number(lon, 'f', 3));
    q.addQueryItem("current", "temperature_2m,weather_code,wind_speed_10m,"
                              "wind_direction_10m,wind_gusts_10m");
    q.addQueryItem("wind_speed_unit", "kmh");
    q.addQueryItem("timezone", "auto");
    u.setQuery(q);
    QNetworkRequest req(u);
    req.setTransferTimeout(15000);
    QNetworkReply* r = nam_.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, gen] {
        r->deleteLater();
        if (gen != gen_ || r->error() != QNetworkReply::NoError) return;
        const auto c = QJsonDocument::fromJson(r->readAll())
                           .object().value("current").toObject();
        if (c.isEmpty()) return;
        d_.tempC   = c.value("temperature_2m").toDouble();
        d_.windKmh = c.value("wind_speed_10m").toDouble();
        d_.gustKmh = c.value("wind_gusts_10m").toDouble();
        d_.windDeg = qRound(c.value("wind_direction_10m").toDouble());
        d_.code    = c.value("weather_code").toInt();
        d_.when    = c.value("time").toString().replace('T', ' ');
        emit updated();
    });
}

QString WeatherClient::icon(int c) {
    if (c == 0) return QString::fromUtf8("☀");
    if (c <= 2) return QString::fromUtf8("⛅");
    if (c == 3 || c == 45 || c == 48) return QString::fromUtf8("☁");
    if (c >= 95) return QString::fromUtf8("⛈");
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return QString::fromUtf8("❄");
    return QString::fromUtf8("🌧");
}

QString WeatherClient::text() const {
    if (!d_.valid()) return QString();
    const QString up = QSettings().value("station/units", "auto").toString();
    const bool imp = up == "mi"
        || (up == "auto" && QLocale().measurementSystem() != QLocale::MetricSystem);
    static const char* pts[] = {"N","NE","E","SE","S","SW","W","NW"};
    const QString dir = pts[((d_.windDeg + 22) % 360) / 45];
    const double t = imp ? d_.tempC * 9 / 5 + 32 : d_.tempC;
    const double f = imp ? 0.621371 : 1.0;
    const int w = qRound(d_.windKmh * f), g = qRound(d_.gustKmh * f);
    QString s = QString::fromUtf8("%1 %2°%3  %4 %5")
                    .arg(icon(d_.code)).arg(qRound(t)).arg(imp ? "F" : "C")
                    .arg(dir).arg(w);
    if (g > w + 2) s += QString(" G%1").arg(g);
    return s + (imp ? " mph" : " km/h");
}

} // namespace ttc
