// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/PanadapterWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ttc {

namespace {
constexpr int   kWaterfallRows = 220;
constexpr int   kMinViewSpanHz = 4000;   // deep enough to edit a CW filter
constexpr float kSpectrumFrac  = 0.42f;  // top 42% spectrum, rest waterfall
} // namespace

PanadapterWidget::PanadapterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 320);
    setMouseTracking(true);
}

void PanadapterWidget::setSpanHz(int spanHz) {
    fullSpanHz_ = std::max(1000, spanHz);
    viewSpanHz_ = std::clamp(viewSpanHz_, kMinViewSpanHz, fullSpanHz_);
    update();
}

void PanadapterWidget::setPassband(int loHz, int hiHz) {
    if (drag_ != Drag::None) return;               // never fight an active edge drag
    pbLoHz_ = std::min(loHz, hiHz);
    pbHiHz_ = std::max(loHz, hiHz);
    update();
}

int PanadapterWidget::spectrumHeight() const {
    return static_cast<int>(height() * kSpectrumFrac);
}

// Simple SDR palette: black -> deep blue -> cyan -> green -> yellow -> red -> white.
QRgb PanadapterWidget::colormap(float t) {
    struct Stop { float t; int r, g, b; };
    static const Stop stops[] = {
        {0.00f,   0,   0,   0}, {0.20f,   0,   0, 120}, {0.40f,   0, 160, 200},
        {0.60f,   0, 200,  80}, {0.75f, 230, 220,  40}, {0.90f, 240,  60,  30},
        {1.00f, 255, 255, 255},
    };
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 1; i < std::size(stops); ++i) {
        if (t <= stops[i].t) {
            const auto& a = stops[i - 1];
            const auto& b = stops[i];
            const float f = (t - a.t) / (b.t - a.t);
            return qRgb(int(a.r + f * (b.r - a.r)),
                        int(a.g + f * (b.g - a.g)),
                        int(a.b + f * (b.b - a.b)));
        }
    }
    return qRgb(255, 255, 255);
}

void PanadapterWidget::appendWaterfallRow(const std::vector<float>& db) {
    const int n = static_cast<int>(db.size());
    if (n < 2) return;
    if (waterfall_.width() != n) {                 // (re)init on bin-count change
        waterfall_ = QImage(n, kWaterfallRows, QImage::Format_RGB32);
        waterfall_.fill(Qt::black);
    }
    // Scroll one row down, then paint the newest row across the top.
    const int bpl = waterfall_.bytesPerLine();
    std::memmove(waterfall_.bits() + bpl, waterfall_.bits(),
                 static_cast<size_t>(bpl) * (kWaterfallRows - 1));
    QRgb* row = reinterpret_cast<QRgb*>(waterfall_.scanLine(0));
    const float range = dbMax_ - dbMin_;
    for (int i = 0; i < n; ++i)
        row[i] = colormap((db[i] - dbMin_) / range);
}

void PanadapterWidget::setSpectrum(const std::vector<float>& magsDb) {
    spectrum_ = magsDb;
    appendWaterfallRow(magsDb);
    update();
}

int PanadapterWidget::hzToX(int hz) const {
    const double frac = (hz + viewSpanHz_ / 2.0) / viewSpanHz_;
    return static_cast<int>(std::lround(frac * width()));
}

int PanadapterWidget::xToHz(int x) const {
    const double frac = static_cast<double>(x) / std::max(1, width());
    return static_cast<int>(std::lround(frac * viewSpanHz_ - viewSpanHz_ / 2.0));
}

void PanadapterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int hSpec = spectrumHeight();
    p.fillRect(rect(), QColor(12, 16, 22));

    const int n = static_cast<int>(spectrum_.size());
    // Visible bin window: view span centered inside the full captured span.
    int binLo = 0, binHi = n;
    if (n >= 2) {
        const double frac = static_cast<double>(viewSpanHz_) / fullSpanHz_;
        binLo = static_cast<int>(n * (0.5 - frac / 2.0));
        binHi = static_cast<int>(n * (0.5 + frac / 2.0));
        binLo = std::clamp(binLo, 0, n - 2);
        binHi = std::clamp(binHi, binLo + 2, n);
    }

    // Waterfall (lower area): draw the visible bin window scaled to the widget.
    if (!waterfall_.isNull() && n >= 2) {
        const QRect src(binLo, 0, binHi - binLo, waterfall_.height());
        const QRect dst(0, hSpec, width(), height() - hSpec);
        p.drawImage(dst, waterfall_, src);
    }

    // Frequency gridlines every viewSpan/10, spectrum area only.
    p.setPen(QColor(255, 255, 255, 22));
    for (int i = 1; i < 10; ++i) {
        const int x = width() * i / 10;
        p.drawLine(x, 0, x, hSpec);
    }

    // Passband highlight across spectrum AND waterfall (SmartSDR-style).
    const int xLo = hzToX(pbLoHz_), xHi = hzToX(pbHiHz_);
    p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())), QColor(60, 120, 200, 55));
    p.setPen(QPen(QColor(120, 180, 255), 1));
    p.drawLine(xLo, 0, xLo, height());
    p.drawLine(xHi, 0, xHi, height());

    // Center (tuned) marker, full height.
    p.setPen(QColor(200, 80, 80));
    p.drawLine(width() / 2, 0, width() / 2, height());

    // Spectrum trace over the visible bin window.
    if (n >= 2) {
        p.setPen(QColor(120, 220, 140));
        const float range = dbMax_ - dbMin_;
        QPointF prev;
        const int nv = binHi - binLo;
        for (int i = 0; i < nv; ++i) {
            const double x = static_cast<double>(i) / (nv - 1) * width();
            const float t = (spectrum_[binLo + i] - dbMin_) / range;
            const double y = hSpec * (1.0 - std::clamp(t, 0.0f, 1.0f));
            QPointF pt(x, y);
            if (i) p.drawLine(prev, pt);
            prev = pt;
        }
    } else {
        p.setPen(QColor(120, 220, 140));
        p.drawText(QRect(0, 0, width(), hSpec), Qt::AlignCenter,
                   "panadapter — no IQ source (build with -DBUILD_SDRPLAY=ON)");
    }

    // Span readout + separator line.
    p.setPen(QColor(200, 200, 200, 160));
    p.drawText(6, 14, QString("span %1 kHz   wheel: tune  shift: fine  ctrl: zoom")
                          .arg(viewSpanHz_ / 1000.0, 0, 'f', viewSpanHz_ < 20000 ? 1 : 0));
    p.setPen(QColor(255, 255, 255, 40));
    p.drawLine(0, hSpec, width(), hSpec);
}

void PanadapterWidget::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    if (e->modifiers() & Qt::ControlModifier) {     // Ctrl+wheel = zoom
        const double factor = std::pow(1.25, -steps);
        viewSpanHz_ = std::clamp(static_cast<int>(std::lround(viewSpanHz_ * factor)),
                                 kMinViewSpanHz, fullSpanHz_);
        emit viewSpanChanged(viewSpanHz_);
        update();
    } else {                                        // plain wheel = tune (Shift = fine)
        const int n = static_cast<int>(steps > 0 ? std::ceil(steps) : std::floor(steps));
        emit tuneStepRequested(n, e->modifiers() & Qt::ShiftModifier);
    }
    e->accept();
}

void PanadapterWidget::mousePressEvent(QMouseEvent* e) {
    const int x = e->pos().x();
    const int edgeTol = 6;
    if (std::abs(x - hzToX(pbLoHz_)) <= edgeTol)      drag_ = Drag::LoEdge;
    else if (std::abs(x - hzToX(pbHiHz_)) <= edgeTol) drag_ = Drag::HiEdge;
    if (drag_ != Drag::None) {
        emit passbandEditBegan(pbLoHz_, pbHiHz_);   // consumer anchors radio state
        return;
    }
    {
        drag_ = Drag::None;
        emit tuneRequested(xToHz(x));               // click-to-tune
    }
}

void PanadapterWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::None) return;
    const int hz = xToHz(e->pos().x());
    if (drag_ == Drag::LoEdge) pbLoHz_ = std::min(hz, pbHiHz_ - 50);
    else                       pbHiHz_ = std::max(hz, pbLoHz_ + 50);
    update();
    emit passbandChanged(pbLoHz_, pbHiHz_);         // drag-to-filter
}

void PanadapterWidget::mouseReleaseEvent(QMouseEvent*) { drag_ = Drag::None; }

} // namespace ttc
