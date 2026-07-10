// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/PanadapterWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace ttc {

PanadapterWidget::PanadapterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 240);
    setMouseTracking(true);
}

void PanadapterWidget::setSpanHz(int spanHz) { spanHz_ = std::max(1000, spanHz); update(); }

void PanadapterWidget::setPassband(int loHz, int hiHz) {
    pbLoHz_ = std::min(loHz, hiHz);
    pbHiHz_ = std::max(loHz, hiHz);
    update();
}

void PanadapterWidget::setSpectrum(const std::vector<float>& magsDb) {
    spectrum_ = magsDb;
    update();
}

int PanadapterWidget::hzToX(int hz) const {
    const double frac = (hz + spanHz_ / 2.0) / spanHz_;
    return static_cast<int>(std::lround(frac * width()));
}

int PanadapterWidget::xToHz(int x) const {
    const double frac = static_cast<double>(x) / std::max(1, width());
    return static_cast<int>(std::lround(frac * spanHz_ - spanHz_ / 2.0));
}

void PanadapterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));

    // Passband highlight (the on-screen filter that maps to the radio's DSP).
    const int xLo = hzToX(pbLoHz_), xHi = hzToX(pbHiHz_);
    p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())), QColor(60, 120, 200, 60));
    p.setPen(QColor(120, 180, 255));
    p.drawLine(xLo, 0, xLo, height());
    p.drawLine(xHi, 0, xHi, height());

    // Center (tuned) marker.
    p.setPen(QColor(200, 80, 80));
    p.drawLine(width() / 2, 0, width() / 2, height());

    // Spectrum trace (placeholder flat noise floor until the DSP path is wired).
    p.setPen(QColor(120, 220, 140));
    const int n = static_cast<int>(spectrum_.size());
    if (n >= 2) {
        QPointF prev;
        for (int i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) / (n - 1) * width();
            const double y = height() - (spectrum_[i] + 120.0) / 120.0 * height();
            QPointF pt(x, std::clamp(y, 0.0, static_cast<double>(height())));
            if (i) p.drawLine(prev, pt);
            prev = pt;
        }
    } else {
        p.drawText(rect(), Qt::AlignCenter,
                   "panadapter — no IQ source (build with -DBUILD_SDRPLAY=ON)");
    }
}

void PanadapterWidget::mousePressEvent(QMouseEvent* e) {
    const int x = e->pos().x();
    const int edgeTol = 6;
    if (std::abs(x - hzToX(pbLoHz_)) <= edgeTol)      drag_ = Drag::LoEdge;
    else if (std::abs(x - hzToX(pbHiHz_)) <= edgeTol) drag_ = Drag::HiEdge;
    else {
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
