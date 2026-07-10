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
// Compact fixed geometry: labels row on top, bar below, readout to the right.
constexpr int kLabelH = 12;
constexpr int kBarH   = 10;
constexpr int kBarX0  = 6;
constexpr int kBarX1  = 300;   // bar ends here; text readout to the right
} // namespace

SMeterWidget::SMeterWidget(QWidget* parent) : QWidget(parent) {
    setFixedSize(364, kLabelH + kBarH + 8);
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

    const int barTop = kLabelH + 2;
    auto dbToX = [&](double db) {
        const double f = (db - kDbMin) / (kDbMax - kDbMin);
        return kBarX0 + static_cast<int>(std::clamp(f, 0.0, 1.0) * (kBarX1 - kBarX0));
    };

    // Bar trough and fill FIRST, labels after, so the scale is never overdrawn.
    p.fillRect(QRect(kBarX0, barTop, kBarX1 - kBarX0, kBarH), QColor(30, 36, 44));
    if (haveReading_) {
        const int xS9  = dbToX(0.0);
        const int xLvl = dbToX(dbS9_);
        p.fillRect(QRect(kBarX0, barTop, std::min(xLvl, xS9) - kBarX0, kBarH),
                   QColor(70, 200, 110));
        if (xLvl > xS9)
            p.fillRect(QRect(xS9, barTop, xLvl - xS9, kBarH), QColor(230, 80, 60));
        const int xPk = dbToX(peakDb_);
        p.fillRect(QRect(xPk - 1, barTop, 2, kBarH), QColor(240, 240, 240));
    }

    // Scale: S1..S9 every other unit, then +20/+40/+60, in the label row above.
    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    p.setPen(QColor(160, 170, 180));
    for (int s = 1; s <= 9; s += 2) {
        const int x = dbToX((s - 9) * 6.0);
        p.drawText(x - 8, 0, 16, kLabelH - 2, Qt::AlignCenter, QString::number(s));
        p.drawLine(x, kLabelH - 2, x, barTop);
    }
    for (int over = 20; over <= 60; over += 20) {
        const int x = dbToX(over);
        p.drawText(x - 14, 0, 28, kLabelH - 2, Qt::AlignCenter, QString("+%1").arg(over));
        p.drawLine(x, kLabelH - 2, x, barTop);
    }

    // Text readout, e.g. "S7" or "S9+23", right of the bar.
    QString txt = "--";
    if (haveReading_) {
        if (dbS9_ >= 0.5) txt = QString("S9+%1").arg(static_cast<int>(std::lround(dbS9_)));
        else {
            const int s = std::clamp(static_cast<int>(std::lround(9.0 + dbS9_ / 6.0)), 0, 9);
            txt = QString("S%1").arg(s);
        }
    }
    f.setPixelSize(13); f.setBold(true); p.setFont(f);
    p.setPen(QColor(220, 230, 240));
    p.drawText(QRect(kBarX1 + 8, 0, width() - kBarX1 - 10, height()),
               Qt::AlignVCenter | Qt::AlignLeft, txt);
}

} // namespace ttc
