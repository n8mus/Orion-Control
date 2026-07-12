// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SMeterWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
// Meter span in dB relative to S9: S1 (-48) .. S9+60.
constexpr double kDbMin = -54.0;
constexpr double kDbMax = +60.0;
constexpr int    kPeakHoldMs = 1500;
constexpr double kMaxW = 120.0;          // TX scale top (100 W radio)
// Bar style fixed geometry (the original compact strip).
constexpr int kLabelH = 12;
constexpr int kBarH   = 10;
constexpr int kBarX0  = 6;
constexpr int kBarX1  = 300;

// Meter-face palette (shared by Analog and Edge).
const QColor kIvoryHi(246, 239, 221);
const QColor kIvoryLo(231, 219, 188);
const QColor kBezel(40, 46, 56);
const QColor kInk(30, 28, 24);           // scale/needle black
const QColor kRed(196, 40, 32);          // over-S9 / over-100W zone
const QColor kGhost(120, 116, 104);      // peak ghost needle
} // namespace

SMeterWidget::SMeterWidget(QWidget* parent) : QWidget(parent) {
    QSettings s;
    style_ = std::clamp(s.value("display/meterStyle", int(Analog)).toInt(),
                        0, int(kStyleCount) - 1);
    mode_  = std::clamp(s.value("display/meterMode", int(Sig)).toInt(),
                        0, int(kModeCount) - 1);
    applyStyle();
    // Needle ballistics: fast attack, slow decay, ~30 fps — poll data arrives
    // at only 2 Hz, this is what makes it move like a real movement.
    anim_ = new QTimer(this);
    anim_->setInterval(33);
    connect(anim_, &QTimer::timeout, this, [this] {
        const auto step = [](double& cur, double tgt, double tauUp, double tauDn) {
            const double tau = tgt > cur ? tauUp : tauDn;
            cur += (tgt - cur) * std::min(1.0, 0.033 / tau);
        };
        step(needleDb_, haveReading_ ? displayDb() : kDbMin, 0.10, 0.60);
        step(needleW_, tx_ ? fwdW_ : 0.0, 0.08, 0.35);
        if (style_ == Analog || style_ == Edge) update();
    });
    anim_->start();
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
    tx_ = false;                          // radio answered in receive
    dbS9_ = rawToDbS9(raw);
    haveReading_ = true;
    if (!emaInit_) { emaDb_ = dbS9_; emaInit_ = true; }
    else emaDb_ += 0.3 * (dbS9_ - emaDb_);
    if (dbS9_ >= peakDb_ || !sincePeak_.isValid()
        || sincePeak_.elapsed() > kPeakHoldMs) {
        peakDb_ = dbS9_;
        sincePeak_.restart();
    }
    update();
}

void SMeterWidget::setTxLevel(double fwdWatts, double refWatts, double swr) {
    tx_ = true;                           // radio answered in transmit
    fwdW_ = fwdWatts;
    refW_ = refWatts;
    swr_  = swr;
    if (fwdW_ >= peakW_ || !sinceTxPeak_.isValid()
        || sinceTxPeak_.elapsed() > kPeakHoldMs) {
        peakW_ = fwdW_;
        sinceTxPeak_.restart();
    }
    update();
}

double SMeterWidget::displayDb() const {
    return mode_ == Avg ? emaDb_ : mode_ == Peak ? peakDb_ : dbS9_;
}

// dB -> scale fraction. Real meter faces give S1..S9 the wider share of the
// arc and compress the over-S9 decades: 58 % / 42 % split here.
double SMeterWidget::scaleFrac(double db) const {
    db = std::clamp(db, kDbMin, kDbMax);
    if (db <= 0.0) return (db - kDbMin) / (0.0 - kDbMin) * 0.58;
    return 0.58 + db / kDbMax * 0.42;
}

QString SMeterWidget::sUnitsText(double db) const {
    if (!haveReading_) return "--";
    if (db >= 0.5) return QString("S9+%1").arg(static_cast<int>(std::lround(db)));
    const int s = std::clamp(static_cast<int>(std::lround(9.0 + db / 6.0)), 0, 9);
    return QString("S%1").arg(s);
}

void SMeterWidget::applyStyle() {
    switch (style_) {
        case Bar:     setFixedSize(364, kLabelH + kBarH + 8); break;
        case Analog:  setFixedSize(230, 96); break;
        case Edge:    setFixedSize(300, 60); break;
        case Digital: setFixedSize(250, 54); break;
    }
    static const char* styleName[] = {"Bar", "Analog", "Edge", "Digital"};
    static const char* modeName[]  = {"Signal", "Average", "Peak"};
    setToolTip(QString("S-meter — style: %1, reading: %2\n"
                       "click: next style (Bar / Analog / Edge / Digital)\n"
                       "right-click: reading mode (Signal / Average / Peak)")
                   .arg(styleName[style_]).arg(modeName[mode_]));
}

void SMeterWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton)
        style_ = (style_ + 1) % kStyleCount;
    else if (e->button() == Qt::RightButton)
        mode_ = (mode_ + 1) % kModeCount;
    else
        return;
    QSettings s;
    s.setValue("display/meterStyle", style_);
    s.setValue("display/meterMode", mode_);
    applyStyle();
    update();
}

void SMeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));
    switch (style_) {
        case Bar:     tx_ ? paintBarTx(p) : paintBarRx(p); break;
        case Analog:  paintAnalog(p); break;
        case Edge:    paintEdge(p); break;
        case Digital: paintDigital(p); break;
    }
}

// ---------------------------------------------------------------- Bar style

void SMeterWidget::paintBarRx(QPainter& p) {
    const int barTop = kLabelH + 2;
    auto dbToX = [&](double db) {
        const double f = (db - kDbMin) / (kDbMax - kDbMin);
        return kBarX0 + static_cast<int>(std::clamp(f, 0.0, 1.0) * (kBarX1 - kBarX0));
    };

    // Bar trough and fill FIRST, labels after, so the scale is never overdrawn.
    p.fillRect(QRect(kBarX0, barTop, kBarX1 - kBarX0, kBarH), QColor(30, 36, 44));
    if (haveReading_) {
        const double db = displayDb();
        const int xS9  = dbToX(0.0);
        const int xLvl = dbToX(db);
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

    QString txt = sUnitsText(displayDb());
    f.setPixelSize(13); f.setBold(true); p.setFont(f);
    p.setPen(QColor(220, 230, 240));
    p.drawText(QRect(kBarX1 + 8, 0, width() - kBarX1 - 10, height()),
               Qt::AlignVCenter | Qt::AlignLeft, txt);
}

void SMeterWidget::paintBarTx(QPainter& p) {
    const int barTop = kLabelH + 2;
    auto wToX = [&](double w) {
        return kBarX0 + static_cast<int>(std::clamp(w / kMaxW, 0.0, 1.0)
                                         * (kBarX1 - kBarX0));
    };

    p.fillRect(QRect(kBarX0, barTop, kBarX1 - kBarX0, kBarH), QColor(30, 36, 44));
    p.fillRect(QRect(kBarX0, barTop, wToX(fwdW_) - kBarX0, kBarH), QColor(240, 160, 40));
    const int xPk = wToX(peakW_);
    p.fillRect(QRect(xPk - 1, barTop, 2, kBarH), QColor(240, 240, 240));

    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    p.setPen(QColor(160, 170, 180));
    for (int w = 0; w <= 100; w += 25) {
        const int x = wToX(w);
        p.drawText(x - 10, 0, 20, kLabelH - 2, Qt::AlignCenter, QString::number(w));
        p.drawLine(x, kLabelH - 2, x, barTop);
    }

    const QRect rTxt(kBarX1 + 8, 0, width() - kBarX1 - 10, height());
    f.setPixelSize(12); f.setBold(true); p.setFont(f);
    p.setPen(QColor(240, 160, 40));
    p.drawText(rTxt, Qt::AlignTop | Qt::AlignLeft,
               QString("TX %1W").arg(fwdW_, 0, 'f', 0));
    f.setPixelSize(10); p.setFont(f);
    p.setPen(swr_ > 2.5 ? QColor(240, 70, 50) : QColor(180, 200, 220));
    p.drawText(rTxt, Qt::AlignBottom | Qt::AlignLeft,
               QString("SWR %1").arg(swr_, 0, 'f', 1));
}

// ------------------------------------------------------------- Analog style
// Ivory needle meter. The pivot sits below the visible face so the needle
// sweeps a shallow, wide arc like a real panel movement.

void SMeterWidget::paintAnalog(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);

    // Bezel + backlit ivory face.
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kIvoryHi);
    g.setColorAt(1.0, kIvoryLo);
    p.setBrush(g);
    p.drawRoundedRect(face, 7, 7);

    const QPointF pivot(width() / 2.0, height() + 46.0);
    const double R = pivot.y() - 14.0;               // scale arc radius
    const double aL = 122.0, aR = 58.0;              // sweep, math degrees
    const auto ptAt = [&](double frac, double r) {
        const double a = (aL - (aL - aR) * frac) * M_PI / 180.0;
        return QPointF(pivot.x() + r * std::cos(a), pivot.y() - r * std::sin(a));
    };

    const bool rx = !tx_;
    // Scale arc: black up to the redline, red beyond (S9 on RX, 100 W on TX).
    const double redFrac = rx ? scaleFrac(0.0) : 100.0 / kMaxW;
    const QRectF arcRect(pivot.x() - R, pivot.y() - R, 2 * R, 2 * R);
    const auto spanArc = [&](double f0, double f1, const QColor& c, double wid) {
        p.setPen(QPen(c, wid));
        const double a0 = aL - (aL - aR) * f0, a1 = aL - (aL - aR) * f1;
        p.drawArc(arcRect, static_cast<int>(a1 * 16),
                  static_cast<int>((a0 - a1) * 16));
    };
    spanArc(0.0, redFrac, kInk, 1.6);
    spanArc(redFrac, 1.0, kRed, 3.0);

    // Ticks + labels.
    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    const auto tick = [&](double frac, bool major, const QColor& c) {
        p.setPen(QPen(c, major ? 1.6 : 1.0));
        p.drawLine(ptAt(frac, R - (major ? 7 : 4)), ptAt(frac, R));
    };
    const auto label = [&](double frac, const QString& t, const QColor& c) {
        p.setPen(c);
        const QPointF pt = ptAt(frac, R - 15);
        p.drawText(QRectF(pt.x() - 12, pt.y() - 6, 24, 12), Qt::AlignCenter, t);
    };
    if (rx) {
        for (int s = 1; s <= 9; ++s) {
            const double fr = scaleFrac((s - 9) * 6.0);
            tick(fr, s & 1, kInk);
            if (s & 1) label(fr, QString::number(s), kInk);
        }
        for (int over = 20; over <= 60; over += 20) {
            const double fr = scaleFrac(over);
            tick(fr, true, kRed);
            label(fr, QString("+%1").arg(over), kRed);
        }
    } else {
        for (int w = 0; w <= 120; w += 10) {
            const double fr = w / kMaxW;
            const bool major = w % 25 == 0 || w == 120;
            tick(fr, major, w > 100 ? kRed : kInk);
            if (w % 50 == 0 || w == 100)
                label(fr, QString::number(w), w > 100 ? kRed : kInk);
        }
    }

    // Peak ghost needle (RX), then the live needle over everything.
    const double needleFrac = rx
        ? scaleFrac(needleDb_)
        : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    if (rx && haveReading_) {
        p.setPen(QPen(kGhost, 1.2));
        p.drawLine(ptAt(scaleFrac(peakDb_), R * 0.42), ptAt(scaleFrac(peakDb_), R - 8));
    }
    p.setPen(QPen(kInk, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(ptAt(needleFrac, R * 0.38), ptAt(needleFrac, R - 6));

    // Readouts on the face bottom.
    f.setPixelSize(12); p.setFont(f);
    p.setPen(kInk);
    if (rx) {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
    } else {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 18, 90, 15),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("%1 W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? kRed : kInk);
        p.drawText(QRectF(face.right() - 98, face.bottom() - 18, 90, 15),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
    if (rx && mode_ != Sig) {                        // reading-mode chip
        f.setPixelSize(9); p.setFont(f);
        p.setPen(kRed);
        p.drawText(QRectF(face.right() - 40, face.bottom() - 16, 32, 12),
                   Qt::AlignRight, mode_ == Avg ? "AVG" : "PK");
    }
}

// --------------------------------------------------------------- Edge style
// Edgewise meter: linear ivory window with a vertical needle riding a
// horizontal scale.

void SMeterWidget::paintEdge(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face(1.5, 1.5, width() - 3.0, height() - 3.0);
    p.setPen(QPen(kBezel, 3));
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0.0, kIvoryHi);
    g.setColorAt(1.0, kIvoryLo);
    p.setBrush(g);
    p.drawRoundedRect(face, 6, 6);

    const double x0 = face.left() + 12, x1 = face.right() - 12;
    const double yScale = face.top() + 22;
    const auto fx = [&](double frac) { return x0 + (x1 - x0) * frac; };

    const bool rx = !tx_;
    const double redFrac = rx ? scaleFrac(0.0) : 100.0 / kMaxW;
    p.setPen(QPen(kInk, 1.6));
    p.drawLine(QPointF(x0, yScale), QPointF(fx(redFrac), yScale));
    p.setPen(QPen(kRed, 3.0));
    p.drawLine(QPointF(fx(redFrac), yScale), QPointF(x1, yScale));

    QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
    const auto tick = [&](double frac, bool major, const QColor& c) {
        p.setPen(QPen(c, major ? 1.6 : 1.0));
        p.drawLine(QPointF(fx(frac), yScale),
                   QPointF(fx(frac), yScale + (major ? 7 : 4)));
    };
    const auto label = [&](double frac, const QString& t, const QColor& c) {
        p.setPen(c);
        p.drawText(QRectF(fx(frac) - 13, face.top() + 3, 26, 11), Qt::AlignCenter, t);
    };
    if (rx) {
        for (int s = 1; s <= 9; ++s) {
            const double fr = scaleFrac((s - 9) * 6.0);
            tick(fr, s & 1, kInk);
            if (s & 1) label(fr, QString::number(s), kInk);
        }
        for (int over = 20; over <= 60; over += 20) {
            const double fr = scaleFrac(over);
            tick(fr, true, kRed);
            label(fr, QString("+%1").arg(over), kRed);
        }
    } else {
        for (int w = 0; w <= 120; w += 10) {
            const double fr = w / kMaxW;
            const bool major = w % 25 == 0 || w == 120;
            tick(fr, major, w > 100 ? kRed : kInk);
            if (w % 50 == 0 || w == 100)
                label(fr, QString::number(w), w > 100 ? kRed : kInk);
        }
    }

    const double needleFrac = rx
        ? scaleFrac(needleDb_)
        : std::clamp(needleW_ / kMaxW, 0.0, 1.0);
    if (rx && haveReading_) {
        p.setPen(QPen(kGhost, 1.2));
        p.drawLine(QPointF(fx(scaleFrac(peakDb_)), yScale - 4),
                   QPointF(fx(scaleFrac(peakDb_)), face.bottom() - 14));
    }
    p.setPen(QPen(kInk, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(fx(needleFrac), yScale - 5),
               QPointF(fx(needleFrac), face.bottom() - 12));

    f.setPixelSize(11); p.setFont(f);
    p.setPen(kInk);
    if (rx) {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 15, 90, 13),
                   Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(displayDb()));
        if (mode_ != Sig) {
            f.setPixelSize(9); p.setFont(f);
            p.setPen(kRed);
            p.drawText(QRectF(face.right() - 40, face.bottom() - 14, 32, 11),
                       Qt::AlignRight, mode_ == Avg ? "AVG" : "PK");
        }
    } else {
        p.drawText(QRectF(face.left() + 8, face.bottom() - 15, 110, 13),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("TX %1 W").arg(fwdW_, 0, 'f', 0));
        p.setPen(swr_ > 2.5 ? kRed : kInk);
        p.drawText(QRectF(face.right() - 98, face.bottom() - 15, 90, 13),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
    }
}

// ------------------------------------------------------------ Digital style
// Thetis-style triple readout: dBm big and amber, S-units white, microvolts
// cyan. All three views of the same calibrated dB-rel-S9 value (S9 = -73 dBm
// = 50 uV at the antenna input).

void SMeterWidget::paintDigital(QPainter& p) {
    QFont f = p.font();
    const QRect top(8, 2, width() - 16, height() / 2 - 2);
    const QRect bot(8, height() / 2, width() - 16, height() / 2 - 4);
    if (!tx_) {
        const double db  = displayDb();
        const double dbm = db - 73.0;
        const double uv  = haveReading_ ? 50.0 * std::pow(10.0, db / 20.0) : 0.0;
        f.setPixelSize(17); f.setBold(true); p.setFont(f);
        p.setPen(QColor(255, 178, 74));
        p.drawText(top, Qt::AlignLeft | Qt::AlignVCenter,
                   haveReading_ ? QString("%1 dBm").arg(dbm, 0, 'f', 1) : "-- dBm");
        if (mode_ != Sig) {
            f.setPixelSize(9); p.setFont(f);
            p.setPen(QColor(230, 90, 70));
            p.drawText(top, Qt::AlignRight | Qt::AlignTop,
                       mode_ == Avg ? "AVG" : "PEAK");
        }
        f.setPixelSize(15); f.setBold(true); p.setFont(f);
        p.setPen(QColor(230, 238, 246));
        p.drawText(bot, Qt::AlignLeft | Qt::AlignVCenter, sUnitsText(db));
        f.setPixelSize(12); f.setBold(false); p.setFont(f);
        p.setPen(QColor(110, 205, 255));
        QString uvTxt = "--";
        if (haveReading_) {
            if (uv >= 1000.0)     uvTxt = QString("%1 mV").arg(uv / 1000.0, 0, 'f', 2);
            else if (uv >= 100.0) uvTxt = QString("%1 µV").arg(uv, 0, 'f', 0);
            else if (uv >= 10.0)  uvTxt = QString("%1 µV").arg(uv, 0, 'f', 1);
            else                  uvTxt = QString("%1 µV").arg(uv, 0, 'f', 2);
        }
        p.drawText(bot, Qt::AlignRight | Qt::AlignVCenter, uvTxt);
    } else {
        f.setPixelSize(17); f.setBold(true); p.setFont(f);
        p.setPen(QColor(240, 160, 40));
        p.drawText(top, Qt::AlignLeft | Qt::AlignVCenter,
                   QString("TX %1 W").arg(fwdW_, 0, 'f', 0));
        f.setPixelSize(13); p.setFont(f);
        p.setPen(swr_ > 2.5 ? QColor(240, 70, 50) : QColor(230, 238, 246));
        p.drawText(bot, Qt::AlignLeft | Qt::AlignVCenter,
                   QString("SWR %1").arg(swr_, 0, 'f', 1));
        f.setPixelSize(12); f.setBold(false); p.setFont(f);
        p.setPen(QColor(150, 162, 178));
        p.drawText(bot, Qt::AlignRight | Qt::AlignVCenter,
                   QString("REF %1 W").arg(refW_, 0, 'f', 0));
    }
}

} // namespace ttc
