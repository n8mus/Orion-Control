// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SMeterWidget.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
// Meter span in dB relative to S9: S1 (-48) .. S9+60.
constexpr double kDbMin = -54.0;
constexpr double kDbMax = +60.0;
constexpr int    kPeakHoldMs = 1500;
} // namespace

SMeterWidget::SMeterWidget(QWidget* parent) : QWidget(parent) {
    setFixedHeight(46);
    setMinimumWidth(320);
}

// Hamlib TT565_STR_CAL_V2 (rigs/tentec/orion.h): raw ?S units -> dB rel S9,
// valid for firmware 2.x and later (Jon's Orion runs v3).
double SMeterWidget::rawToDbS9(int raw) {
    struct Pt { double raw, db; };
    static const Pt cal[] = {
        {10, -48}, {24, -42}, {38, -36}, {47, -30}, {61, -24}, {70, -18},
        {79, -12}, {84, -6},  {94, 0},   {103, 10}, {118, 20}, {134, 30},
        {147, 40}, {161, 50},
    };
    constexpr int n = static_cast<int>(std::size(cal));
    if (raw <= cal[0].raw)     return cal[0].db;
    if (raw >= cal[n - 1].raw) return cal[n - 1].db;
    for (int i = 1; i < n; ++i) {
        if (raw <= cal[i].raw) {
            const double f = (raw - cal[i - 1].raw) / (cal[i].raw - cal[i - 1].raw);
            return cal[i - 1].db + f * (cal[i].db - cal[i - 1].db);
        }
    }
    return cal[n - 1].db;
}

void SMeterWidget::setRawLevel(int raw) {
    dbS9_ = rawToDbS9(raw);
    haveReading_ = true;
    if (dbS9_ >= peakDb_ || !sincePeak_.isValid()
        || sincePeak_.elapsed() > kPeakHoldMs) {
        peakDb_ = dbS9_;
        sincePeak_.restart();
    }
    update();
}

void SMeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));

    const int barTop = 18, barH = 14;
    const int x0 = 8, x1 = width() - 78;              // room for the text readout
    auto dbToX = [&](double db) {
        const double f = (db - kDbMin) / (kDbMax - kDbMin);
        return x0 + static_cast<int>(std::clamp(f, 0.0, 1.0) * (x1 - x0));
    };

    // Scale: S1..S9 every other unit, then +20/+40/+60.
    p.setPen(QColor(160, 170, 180));
    QFont f = p.font(); f.setPixelSize(10); p.setFont(f);
    for (int s = 1; s <= 9; s += 2) {
        const int x = dbToX((s - 9) * 6.0);
        p.drawLine(x, barTop - 4, x, barTop);
        p.drawText(x - 8, barTop - 6, 16, 10, Qt::AlignCenter, QString::number(s));
    }
    for (int over = 20; over <= 60; over += 20) {
        const int x = dbToX(over);
        p.drawLine(x, barTop - 4, x, barTop);
        p.drawText(x - 14, barTop - 6, 28, 10, Qt::AlignCenter, QString("+%1").arg(over));
    }

    // Bar trough.
    p.fillRect(QRect(x0, barTop, x1 - x0, barH), QColor(30, 36, 44));

    if (haveReading_) {
        // Green to S9, red above — classic meter split at the S9 tick.
        const int xS9  = dbToX(0.0);
        const int xLvl = dbToX(dbS9_);
        p.fillRect(QRect(x0, barTop, std::min(xLvl, xS9) - x0, barH), QColor(70, 200, 110));
        if (xLvl > xS9)
            p.fillRect(QRect(xS9, barTop, xLvl - xS9, barH), QColor(230, 80, 60));

        // Peak-hold marker.
        const int xPk = dbToX(peakDb_);
        p.fillRect(QRect(xPk - 1, barTop, 2, barH), QColor(240, 240, 240));

        // Text readout, e.g. "S7" or "S9+23".
        QString txt;
        if (dbS9_ >= 0.5) txt = QString("S9+%1").arg(static_cast<int>(std::lround(dbS9_)));
        else {
            const int s = std::clamp(static_cast<int>(std::lround(9.0 + dbS9_ / 6.0)), 0, 9);
            txt = QString("S%1").arg(s);
        }
        f.setPixelSize(15); f.setBold(true); p.setFont(f);
        p.setPen(QColor(220, 230, 240));
        p.drawText(QRect(x1 + 6, barTop - 4, width() - x1 - 8, barH + 8),
                   Qt::AlignVCenter | Qt::AlignLeft, txt);
    } else {
        p.setPen(QColor(120, 130, 140));
        p.drawText(QRect(x0, barTop, x1 - x0, barH), Qt::AlignCenter, "no signal data");
    }
}

} // namespace ttc
