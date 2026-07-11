// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/PanadapterWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ttc {

namespace {
constexpr int   kWfHistRows   = 1024;    // raw dB rows kept for re-render on zoom
constexpr int   kMinViewSpanHz = 4000;   // deep enough to edit a CW filter
constexpr int   kScaleBandH    = 16;     // freq-scale strip between spectrum and wf
constexpr int   kDbAxisW       = 40;     // grab width of the left dB scale
constexpr float kPeakDecayDb   = 0.15f;  // peak-hold droop per frame (~2-3 dB/s)
constexpr float kFillGamma     = 0.6f;   // compresses the ramp: warm colors arrive
                                         // ~60% up the scale (KE9NS look)
constexpr float kNoRow         = -300.0f;

// Color palettes. All stops interpolate linearly; t=0 is the scale bottom
// (noise floor), t=1 the top. "Enhanced" is the vivid KE9NS-PowerSDR-style
// ramp — signals climb through blue/cyan/green into yellow/orange/red so
// strength reads as color, not just height.
struct Stop { float t; int r, g, b; };
const Stop kEnhanced[] = {
    {0.00f,   0,   0,   0}, {0.12f,  25,   0,  60}, {0.25f,   0,  30, 160},
    {0.40f,   0, 120, 230}, {0.52f,   0, 200, 180}, {0.62f,  40, 210,  50},
    {0.72f, 230, 230,  40}, {0.82f, 255, 140,  20}, {0.92f, 255,  40,  30},
    {1.00f, 255, 255, 255},
};
const Stop kClassic[] = {
    {0.00f,   0,   0,   0}, {0.20f,   0,   0, 120}, {0.40f,   0, 160, 200},
    {0.60f,   0, 200,  80}, {0.75f, 230, 220,  40}, {0.90f, 240,  60,  30},
    {1.00f, 255, 255, 255},
};
const Stop kThermal[] = {
    {0.00f,   0,   0,   0}, {0.30f,  90,   0,  10}, {0.55f, 200,  40,   0},
    {0.75f, 255, 150,   0}, {0.90f, 255, 230,  60}, {1.00f, 255, 255, 255},
};
const Stop kGray[] = {
    {0.00f,   0,   0,   0}, {1.00f, 255, 255, 255},
};
struct Palette { const Stop* stops; size_t n; };
const Palette kPalettes[] = {
    {kEnhanced, std::size(kEnhanced)},
    {kClassic,  std::size(kClassic)},
    {kThermal,  std::size(kThermal)},
    {kGray,     std::size(kGray)},
};

// A "nice" grid step (1/2/5 x 10^k) giving ~8 divisions across the view.
double niceStep(double span) {
    const double raw = span / 8.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10.0 * mag;
}
} // namespace

QStringList PanadapterWidget::paletteNames() {
    return {"Enhanced (KE9NS)", "Classic", "Thermal", "Grayscale"};
}

PanadapterWidget::PanadapterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 320);
    setMouseTracking(true);
    dbMax_ = ds_.refDb;
    dbMin_ = ds_.refDb - ds_.rangeDb;
}

void PanadapterWidget::setSpanHz(int spanHz) {
    fullSpanHz_ = std::max(1000, spanHz);
    viewSpanHz_ = std::clamp(viewSpanHz_, kMinViewSpanHz, fullSpanHz_);
    update();
}

int PanadapterWidget::minViewSpanHz() const { return kMinViewSpanHz; }

void PanadapterWidget::setViewSpanHz(int spanHz) {
    // Slider-driven zoom: no viewSpanChanged emit, or the slider would loop.
    viewSpanHz_ = std::clamp(spanHz, kMinViewSpanHz, fullSpanHz_);
    update();
}

void PanadapterWidget::setCenterHz(uint64_t hz) {
    if (hz == centerHz_) return;
    centerHz_ = hz;
    update();
}

void PanadapterWidget::setDisplaySettings(const DisplaySettings& s) {
    const bool scaleChanged = s.refDb != ds_.refDb || s.rangeDb != ds_.rangeDb
                              || s.palette != ds_.palette;
    if (s.peakHold && !ds_.peakHold) peaks_ = avg_;   // fresh start, no stale peaks
    ds_ = s;
    ds_.avgFrames = std::max(1, ds_.avgFrames);
    ds_.wfSpeed   = std::max(1, ds_.wfSpeed);
    ds_.split     = std::clamp(ds_.split, 0.15f, 0.85f);
    dbMax_ = ds_.refDb;
    dbMin_ = ds_.refDb - std::max(10.0f, ds_.rangeDb);
    if (scaleChanged)
        wfDirty_ = true;       // history re-renders in the new scale/palette
    update();
}

void PanadapterWidget::setPassband(int loHz, int hiHz) {
    if (drag_ != Drag::None) return;               // never fight an active edge drag
    pbLoHz_ = std::min(loHz, hiHz);
    pbHiHz_ = std::max(loHz, hiHz);
    update();
}

void PanadapterWidget::setNotch(bool on, int rfOffsetHz, int widthHz) {
    if (drag_ == Drag::Notch) return;              // never fight an active drag
    notchOn_ = on;
    notchRfHz_ = rfOffsetHz;
    notchWidthHz_ = widthHz;
    update();
}

bool PanadapterWidget::overNotch(int x) const {
    if (!notchOn_) return false;
    const int halfPx = (hzToX(notchRfHz_ + notchWidthHz_ / 2)
                        - hzToX(notchRfHz_ - notchWidthHz_ / 2)) / 2;
    return std::abs(x - hzToX(notchRfHz_)) <= std::max(6, halfPx + 3);
}

int PanadapterWidget::spectrumHeight() const {
    return static_cast<int>((height() - kScaleBandH) * ds_.split);
}

bool PanadapterWidget::inScaleBand(int y) const {
    const int hSpec = spectrumHeight();
    return y >= hSpec - 2 && y < hSpec + kScaleBandH + 2;
}

bool PanadapterWidget::inDbAxis(int x, int y) const {
    return x < kDbAxisW && y < spectrumHeight() - 2;
}

QRgb PanadapterWidget::mapColor(float t) const {
    const Palette& pal =
        kPalettes[std::clamp(ds_.palette, 0, int(std::size(kPalettes)) - 1)];
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 1; i < pal.n; ++i) {
        if (t <= pal.stops[i].t) {
            const Stop& a = pal.stops[i - 1];
            const Stop& b = pal.stops[i];
            const float f = (t - a.t) / (b.t - a.t);
            return qRgb(int(a.r + f * (b.r - a.r)),
                        int(a.g + f * (b.g - a.g)),
                        int(a.b + f * (b.b - a.b)));
        }
    }
    return qRgb(255, 255, 255);
}

void PanadapterWidget::pushWaterfallRow(const std::vector<float>& db) {
    const int n = static_cast<int>(db.size());
    if (n < 2) return;
    if (wfBins_ != n) {                            // bin-count change: reset history
        wfBins_ = n;
        wfHist_.assign(static_cast<size_t>(kWfHistRows) * n, kNoRow);
        wfHead_ = wfCount_ = wfPending_ = 0;
        wfDirty_ = true;
    }
    std::memcpy(&wfHist_[static_cast<size_t>(wfHead_) * n], db.data(),
                sizeof(float) * n);
    wfHead_ = (wfHead_ + 1) % kWfHistRows;
    wfCount_ = std::min(wfCount_ + 1, kWfHistRows);
    ++wfPending_;
}

// Crisp column mapping: each pixel column takes the MAX of its bin range, so
// narrow signals never vanish on wide spans and nothing is smoothed away.
void PanadapterWidget::renderWfLine(const float* src, QRgb* dst, int w,
                                    int binLo, int binHi) const {
    const int nv = binHi - binLo;
    const float invRange = 1.0f / (dbMax_ - dbMin_);
    for (int j = 0; j < w; ++j) {
        int b0 = binLo + static_cast<int>(static_cast<int64_t>(j) * nv / w);
        int b1 = binLo + static_cast<int>(static_cast<int64_t>(j + 1) * nv / w);
        if (b1 <= b0) b1 = b0 + 1;
        float m = src[b0];
        for (int b = b0 + 1; b < b1; ++b) m = std::max(m, src[b]);
        dst[j] = mapColor((m - dbMin_) * invRange);
    }
}

void PanadapterWidget::ensureWaterfallImage(int w, int h, int binLo, int binHi) {
    if (w < 1 || h < 1 || wfBins_ < 2) return;
    const bool geomChanged = wfImg_.width() != w || wfImg_.height() != h
                             || binLo != lastBinLo_ || binHi != lastBinHi_;
    auto histRow = [this](int back) {
        return &wfHist_[static_cast<size_t>(
            ((wfHead_ - 1 - back) % kWfHistRows + kWfHistRows) % kWfHistRows) * wfBins_];
    };
    if (geomChanged || wfDirty_) {                 // full re-render from history
        wfImg_ = QImage(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(wfImg_.scanLine(y));
            if (y < wfCount_) renderWfLine(histRow(y), line, w, binLo, binHi);
            else              std::fill(line, line + w, qRgb(0, 0, 0));
        }
        lastBinLo_ = binLo;
        lastBinHi_ = binHi;
        wfDirty_ = false;
    } else if (wfPending_ > 0) {                   // scroll + render only new rows
        const int rows = std::min(wfPending_, h);
        const int bpl = wfImg_.bytesPerLine();
        std::memmove(wfImg_.bits() + static_cast<size_t>(bpl) * rows, wfImg_.bits(),
                     static_cast<size_t>(bpl) * (h - rows));
        for (int y = 0; y < rows; ++y)
            renderWfLine(histRow(y), reinterpret_cast<QRgb*>(wfImg_.scanLine(y)),
                         w, binLo, binHi);
    }
    wfPending_ = 0;
}

void PanadapterWidget::setSpectrum(const std::vector<float>& magsDb) {
    const size_t n = magsDb.size();
    spectrum_ = magsDb;
    if (avg_.size() != n) {                        // bin count changed: hard reset
        avg_ = magsDb;
        peaks_ = magsDb;
    } else {
        const float a = 1.0f / ds_.avgFrames;      // EMA; avgFrames==1 -> raw
        for (size_t i = 0; i < n; ++i) {
            avg_[i] += (magsDb[i] - avg_[i]) * a;
            if (ds_.peakHold)
                peaks_[i] = std::max(peaks_[i] - kPeakDecayDb, avg_[i]);
        }
    }
    // Waterfall speed: max-merge wfSpeed frames into one committed row, so slow
    // scroll keeps every blip instead of dropping frames.
    if (wfAccum_.size() != n) {
        wfAccum_ = magsDb;
        wfAccumN_ = 1;
    } else {
        for (size_t i = 0; i < n; ++i) wfAccum_[i] = std::max(wfAccum_[i], magsDb[i]);
        ++wfAccumN_;
    }
    if (wfAccumN_ >= ds_.wfSpeed) {
        pushWaterfallRow(wfAccum_);
        std::fill(wfAccum_.begin(), wfAccum_.end(), kNoRow);
        wfAccumN_ = 0;
    }
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

void PanadapterWidget::drawFreqGrid(QPainter& p, int hSpec) {
    if (centerHz_ == 0) {                          // no dial info: plain grid only
        p.setPen(QColor(255, 255, 255, 22));
        for (int i = 1; i < 10; ++i)
            p.drawLine(width() * i / 10, 0, width() * i / 10, hSpec);
        return;
    }
    // Gridlines on absolute "nice" frequencies (labels live on the scale band).
    const double step = niceStep(viewSpanHz_);
    const double f0 = static_cast<double>(centerHz_) - viewSpanHz_ / 2.0;
    const double f1 = static_cast<double>(centerHz_) + viewSpanHz_ / 2.0;
    p.setPen(QColor(255, 255, 255, 26));
    for (double fq = std::ceil(f0 / step) * step; fq <= f1; fq += step) {
        const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
        p.drawLine(x, 0, x, hSpec);
    }
}

// KE9NS/PowerSDR-style frequency scale: a dedicated strip between the spectrum
// and the waterfall with ticks + MHz labels. The strip is also the split
// handle — drag it up/down to resize spectrum vs waterfall.
void PanadapterWidget::drawScaleBand(QPainter& p, int hSpec) {
    p.fillRect(QRect(0, hSpec, width(), kScaleBandH), QColor(20, 27, 36));
    p.setPen(QColor(255, 255, 255, 50));
    p.drawLine(0, hSpec, width(), hSpec);
    p.drawLine(0, hSpec + kScaleBandH - 1, width(), hSpec + kScaleBandH - 1);
    if (centerHz_ == 0) return;
    const double step = niceStep(viewSpanHz_);
    const int decimals = std::clamp(
        static_cast<int>(std::ceil(std::log10(1e6 / step))), 1, 6);
    const double f0 = static_cast<double>(centerHz_) - viewSpanHz_ / 2.0;
    const double f1 = static_cast<double>(centerHz_) + viewSpanHz_ / 2.0;
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    // Minor ticks every step/5 when there's room for them.
    const double minor = step / 5.0;
    if (hzToX(static_cast<int>(minor)) - hzToX(0) > 8) {
        p.setPen(QColor(160, 175, 190, 110));
        for (double fq = std::ceil(f0 / minor) * minor; fq <= f1; fq += minor) {
            const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
            p.drawLine(x, hSpec, x, hSpec + 3);
        }
    }
    for (double fq = std::ceil(f0 / step) * step; fq <= f1; fq += step) {
        const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
        p.setPen(QColor(200, 215, 230, 200));
        p.drawLine(x, hSpec, x, hSpec + 5);
        p.drawText(x + 3, hSpec + kScaleBandH - 4,
                   QString::number(fq / 1e6, 'f', decimals));
    }
}

void PanadapterWidget::drawDbScale(QPainter& p, int hSpec) {
    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    const float range = dbMax_ - dbMin_;
    const int step = range > 80.0f ? 20 : 10;      // KE9NS-density when zoomed in
    for (int db = static_cast<int>(std::ceil(dbMin_ / step)) * step;
         db < static_cast<int>(dbMax_); db += step) {
        const int y = static_cast<int>(hSpec * (1.0f - (db - dbMin_) / range));
        if (y < 20 || y > hSpec - 6) continue;     // keep clear of the text row
        p.setPen(QColor(255, 255, 255, 18));
        p.drawLine(0, y, width(), y);
        p.setPen(QColor(165, 180, 195, 160));
        p.drawText(4, y - 2, QString::number(db));
    }
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

    // Waterfall (below the scale band): raw history, 1 row = 1 pixel line.
    const int wfTop = hSpec + kScaleBandH;
    if (wfBins_ >= 2 && n >= 2 && height() > wfTop) {
        ensureWaterfallImage(width(), height() - wfTop, binLo, binHi);
        if (!wfImg_.isNull()) p.drawImage(0, wfTop, wfImg_);
    }

    // Per-pixel-column signal level (0..1 of the scale): max of the column's
    // bins when zoomed out (narrow signals never vanish), linear interpolation
    // between bins when zoomed deep (no staircase). Drives both fill and trace,
    // and gives a clean one-vertex-per-pixel line instead of sub-pixel jitter.
    const float range = dbMax_ - dbMin_;
    const int w = width();
    std::vector<float> colT;
    if (n >= 2 && w > 1) {
        colT.resize(w);
        const int nv = binHi - binLo;
        for (int x = 0; x < w; ++x) {
            float v;
            if (nv >= w) {
                int b0 = binLo + static_cast<int>(static_cast<int64_t>(x) * nv / w);
                int b1 = binLo + static_cast<int>(static_cast<int64_t>(x + 1) * nv / w);
                if (b1 <= b0) b1 = b0 + 1;
                v = avg_[b0];
                for (int b = b0 + 1; b < b1; ++b) v = std::max(v, avg_[b]);
            } else {
                const double pos = binLo + static_cast<double>(x) * (nv - 1) / (w - 1);
                const int b = static_cast<int>(pos);
                const float f = static_cast<float>(pos - b);
                v = avg_[b] * (1.0f - f) + avg_[std::min(b + 1, n - 1)] * f;
            }
            colT[x] = std::clamp((v - dbMin_) / range, 0.0f, 1.0f);
        }
    }

    // KE9NS-style fill: color keyed to the dB AXIS, so the bands run
    // horizontally and a signal climbs through blue -> green -> yellow -> red
    // as it gets stronger (per-column coloring made vertical streaks instead).
    // Gamma-compressed so the warm colors arrive partway up the scale, not
    // only at the very top.
    if (!colT.empty() && ds_.fillTrace && hSpec > 1) {
        if (fillImg_.width() != w || fillImg_.height() != hSpec)
            fillImg_ = QImage(w, hSpec, QImage::Format_RGB32);
        std::vector<int> colY(w);
        for (int x = 0; x < w; ++x)
            colY[x] = static_cast<int>(hSpec * (1.0f - colT[x]));
        const QRgb bg = qRgb(12, 16, 22);
        for (int y = 0; y < hSpec; ++y) {
            const float t = 1.0f - static_cast<float>(y) / (hSpec - 1);
            const QRgb c = mapColor(std::pow(t, kFillGamma));
            QRgb* line = reinterpret_cast<QRgb*>(fillImg_.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = (y >= colY[x]) ? c : bg;
        }
        p.drawImage(0, 0, fillImg_);
    }

    // Frequency gridlines and the (draggable) dB scale, over the fill.
    drawFreqGrid(p, hSpec);
    drawDbScale(p, hSpec);

    QPainterPath tracePath;
    if (!colT.empty()) {
        for (int x = 0; x < w; ++x) {
            const double y = hSpec * (1.0 - colT[x]);
            if (x == 0) tracePath.moveTo(x, y);
            else        tracePath.lineTo(x, y);
        }
    }

    // Passband highlight across spectrum AND waterfall (SmartSDR-style).
    const int xLo = hzToX(pbLoHz_), xHi = hzToX(pbHiHz_);
    p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())), QColor(60, 120, 200, 55));
    p.setPen(QPen(QColor(120, 180, 255), 1));
    p.drawLine(xLo, 0, xLo, height());
    p.drawLine(xHi, 0, xHi, height());

    // Manual-notch marker: orange band, full height, over the passband tint.
    if (notchOn_) {
        const int xnLo = hzToX(notchRfHz_ - notchWidthHz_ / 2);
        const int xnHi = hzToX(notchRfHz_ + notchWidthHz_ / 2);
        p.fillRect(QRect(QPoint(xnLo, 0), QPoint(xnHi, height())), QColor(255, 140, 0, 70));
        p.setPen(QPen(QColor(255, 160, 40), 1));
        const int xnC = hzToX(notchRfHz_);
        p.drawLine(xnC, 0, xnC, height());
        p.drawText(xnC + 3, hSpec - 6, "N");
    }

    // Frequency scale band on the divider (after the tints, so it stays clean),
    // then the center (tuned) marker crossing everything.
    drawScaleBand(p, hSpec);
    p.setPen(QColor(200, 80, 80));
    p.drawLine(width() / 2, 0, width() / 2, height());

    // Peak-hold trace (under the live trace), then the spectrum trace itself.
    if (n >= 2) {
        if (ds_.peakHold && peaks_.size() == spectrum_.size()) {
            p.setPen(QColor(235, 235, 245, 150));
            const int nv = binHi - binLo;
            QPointF prev;
            for (int i = 0; i < nv; ++i) {
                const double x = static_cast<double>(i) / (nv - 1) * width();
                const float t = (peaks_[binLo + i] - dbMin_) / range;
                const double y = hSpec * (1.0 - std::clamp(t, 0.0f, 1.0f));
                QPointF pt(x, y);
                if (i) p.drawLine(prev, pt);
                prev = pt;
            }
        }
        p.setPen(QColor(210, 245, 215));
        p.drawPath(tracePath);
    } else {
        p.setPen(QColor(120, 220, 140));
        p.drawText(QRect(0, 0, width(), hSpec), Qt::AlignCenter,
                   "panadapter — no IQ source (build with -DBUILD_SDRPLAY=ON)");
    }

    // Span readout (the scale band draws its own separators).
    p.setPen(QColor(200, 200, 200, 160));
    p.drawText(6, 14, QString("span %1 kHz   wheel: tune (shift fine)  edge: cut  "
                              "shift+edge: bw  body: pbt")
                          .arg(viewSpanHz_ / 1000.0, 0, 'f', viewSpanHz_ < 20000 ? 1 : 0));
}

void PanadapterWidget::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    // Wheel over the dB axis adjusts the range (contrast): up = tighter/punchier.
    if (inDbAxis(static_cast<int>(e->position().x()),
                 static_cast<int>(e->position().y()))) {
        ds_.rangeDb = std::clamp(ds_.rangeDb - static_cast<float>(steps) * 5.0f,
                                 30.0f, 120.0f);
        dbMax_ = ds_.refDb;
        dbMin_ = ds_.refDb - ds_.rangeDb;
        wfDirty_ = true;
        update();
        emit displaySettingsEdited(ds_);
        e->accept();
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {     // Ctrl+wheel = zoom
        const double factor = std::pow(1.25, -steps);
        viewSpanHz_ = std::clamp(static_cast<int>(std::lround(viewSpanHz_ * factor)),
                                 kMinViewSpanHz, fullSpanHz_);
        emit viewSpanChanged(viewSpanHz_);
        update();
    } else {
        const int n = static_cast<int>(steps > 0 ? std::ceil(steps) : std::floor(steps));
        // Wheel over the notch marker widens/narrows it; elsewhere it tunes
        // (Shift = fine).
        if (overNotch(static_cast<int>(e->position().x())))
            emit notchWidthAdjustRequested(n);
        else
            emit tuneStepRequested(n, e->modifiers() & Qt::ShiftModifier);
    }
    e->accept();
}

void PanadapterWidget::mousePressEvent(QMouseEvent* e) {
    const int x = e->pos().x();
    const int y = e->pos().y();
    const int edgeTol = 6;
    dragStartX_  = x;
    dragStartY_  = y;
    dragStartLo_ = pbLoHz_;
    dragStartHi_ = pbHiHz_;
    // The frequency-scale band IS the split handle (KE9NS-style).
    if (inScaleBand(y)) {
        drag_ = Drag::Divider;
        dragStartSplit_ = ds_.split;
        return;
    }
    // The notch is the narrowest target — give it priority over the passband
    // edges it may sit on top of.
    if (overNotch(x)) {
        drag_ = Drag::Notch;
        return;
    }
    // Left dB scale: vertical drag shifts the reference level (grid drag in
    // PowerSDR). Checked after the notch but before the passband edges.
    if (inDbAxis(x, y)) {
        drag_ = Drag::DbAxis;
        dragStartRef_ = ds_.refDb;
        return;
    }
    const bool onLo = std::abs(x - hzToX(pbLoHz_)) <= edgeTol;
    const bool onHi = std::abs(x - hzToX(pbHiHz_)) <= edgeTol;
    if (onLo || onHi) {
        // Shift+edge = symmetric width change (pure bandwidth, like the BW
        // knob); plain edge = hi/lo-cut (width + center).
        if (e->modifiers() & Qt::ShiftModifier) drag_ = Drag::SymEdge;
        else                                    drag_ = onLo ? Drag::LoEdge : Drag::HiEdge;
        emit passbandEditBegan(pbLoHz_, pbHiHz_);   // consumer anchors radio state
        return;
    }
    if (x > hzToX(pbLoHz_) && x < hzToX(pbHiHz_)) {
        // Inside the passband: could become a body drag (pure PBT slide) or,
        // if released without moving, a click-to-tune. Decide on first move.
        drag_ = Drag::BodyPending;
        return;
    }
    drag_ = Drag::None;
    emit tuneRequested(xToHz(x));                   // click-to-tune
}

void PanadapterWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::None) {
        // Hover feedback for the two vertical-drag targets.
        const int hx = e->pos().x(), hy = e->pos().y();
        if (inScaleBand(hy) || inDbAxis(hx, hy)) setCursor(Qt::SizeVerCursor);
        else                                     unsetCursor();
        return;
    }
    const int x = e->pos().x();
    const int hz = xToHz(x);
    if (drag_ == Drag::Divider) {
        // Move the spectrum/waterfall split; the waterfall re-renders from raw
        // history at the new height, so nothing smears.
        const int usable = std::max(1, height() - kScaleBandH);
        ds_.split = std::clamp(
            dragStartSplit_ + static_cast<float>(e->pos().y() - dragStartY_) / usable,
            0.15f, 0.85f);
        update();
        emit displaySettingsEdited(ds_);
        return;
    }
    if (drag_ == Drag::DbAxis) {
        // Drag the scale down = numbers slide down = ref level rises.
        const int hSpec = std::max(1, spectrumHeight());
        const float dDb = static_cast<float>(e->pos().y() - dragStartY_)
                          * ds_.rangeDb / hSpec;
        ds_.refDb = std::clamp(dragStartRef_ + dDb, -100.0f, -20.0f);
        dbMax_ = ds_.refDb;
        dbMin_ = ds_.refDb - ds_.rangeDb;
        wfDirty_ = true;
        update();
        emit displaySettingsEdited(ds_);
        return;
    }
    if (drag_ == Drag::Notch) {
        notchRfHz_ = hz;
        update();
        emit notchDragged(notchRfHz_);              // drag-to-notch
        return;
    }
    if (drag_ == Drag::BodyPending) {
        if (std::abs(x - dragStartX_) < 4) return;  // still just a click
        drag_ = Drag::Body;
        emit passbandEditBegan(dragStartLo_, dragStartHi_);
    }
    if (drag_ == Drag::Body) {
        // Slide the whole passband: width constant -> the consumer's delta
        // decomposition sends pure PBT.
        const int delta = hz - xToHz(dragStartX_);
        pbLoHz_ = dragStartLo_ + delta;
        pbHiHz_ = dragStartHi_ + delta;
    } else if (drag_ == Drag::SymEdge) {
        // Mirror both edges about the grab-time center: center constant ->
        // pure bandwidth, PBT untouched (matches the front-panel BW knob).
        const int center = (dragStartLo_ + dragStartHi_) / 2;
        const int half = std::max(25, std::abs(hz - center));
        pbLoHz_ = center - half;
        pbHiHz_ = center + half;
    } else if (drag_ == Drag::LoEdge) {
        pbLoHz_ = std::min(hz, pbHiHz_ - 50);
    } else {
        pbHiHz_ = std::max(hz, pbLoHz_ + 50);
    }
    update();
    emit passbandChanged(pbLoHz_, pbHiHz_);         // drag-to-filter
}

void PanadapterWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    // Double-click either filter edge: snap PBT back to center (bandwidth
    // kept). The first click only armed an edge drag, so nothing else fired.
    const int x = e->pos().x();
    const int edgeTol = 6;
    if (!overNotch(x) && (std::abs(x - hzToX(pbLoHz_)) <= edgeTol
                          || std::abs(x - hzToX(pbHiHz_)) <= edgeTol)) {
        drag_ = Drag::None;                        // cancel the armed drag
        emit pbtZeroRequested();
    }
}

void PanadapterWidget::mouseReleaseEvent(QMouseEvent* e) {
    // A press inside the passband that never moved is a click-to-tune.
    if (drag_ == Drag::BodyPending)
        emit tuneRequested(xToHz(e->pos().x()));
    drag_ = Drag::None;
}

} // namespace ttc
