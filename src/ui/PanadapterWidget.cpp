// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/PanadapterWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QDateTime>
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

QStringList PanadapterWidget::backgroundNames() {
    return {"Dark", "Blue Rays", "World Map"};
}

namespace {
// "Blue with lighting from above" (KE9NS): deep blue vertical gradient plus a
// few soft light shafts slanting down from the top, rendered once per size.
QImage renderBlueRays(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient base(0, 0, 0, h);
    base.setColorAt(0.0, QColor(16, 42, 78));
    base.setColorAt(0.55, QColor(8, 20, 40));
    base.setColorAt(1.0, QColor(3, 8, 16));
    p.fillRect(QRect(0, 0, w, h), base);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Plus);   // additive glow
    // Three rounded pools of light from above (KE9NS look): radial gradients
    // centered just above the top edge, so each lobe reads as a soft dome
    // rather than a hard-edged beam. Fixed table = never flickers on rebuild.
    struct Lamp { float x, r, bright; };
    static const Lamp lamps[] = {
        {0.18f, 0.70f, 1.00f}, {0.50f, 0.85f, 0.85f}, {0.82f, 0.65f, 0.95f},
    };
    for (const Lamp& L : lamps) {
        QRadialGradient g(QPointF(L.x * w, -0.10 * h), L.r * h);
        g.setColorAt(0.00, QColor(130, 190, 255, int(150 * L.bright)));
        g.setColorAt(0.40, QColor(90, 150, 230, int(60 * L.bright)));
        g.setColorAt(1.00, QColor(0, 0, 0, 0));
        p.fillRect(QRect(0, 0, w, h), g);
    }
    return img;
}

// World map with live grayline: the bundled equirectangular Blue Marble,
// darkened for use as a backdrop, with the night side shaded from the
// current UTC subsolar point (soft twilight band = the grayline).
QImage renderWorldMap(int w, int h) {
    static QImage earth;                            // decoded once
    if (earth.isNull())
        earth = QImage(":/earth.jpg").convertToFormat(QImage::Format_RGB32);
    if (earth.isNull()) return renderBlueRays(w, h);  // resource missing
    QImage img = earth.scaled(w, h, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);
    // Subsolar point from UTC (declination approx; equation of time ignored —
    // a display grayline, not an almanac).
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const int doy = utc.date().dayOfYear();
    const double hours = utc.time().hour() + utc.time().minute() / 60.0;
    const double decl = 23.44 * std::sin(2.0 * M_PI * (doy - 81) / 365.25)
                        * M_PI / 180.0;
    const double subLon = (12.0 - hours) * 15.0 * M_PI / 180.0;
    std::vector<double> sinLatSinD(h), cosLatCosD(h), cosDLon(w);
    for (int y = 0; y < h; ++y) {
        const double lat = (90.0 - 180.0 * (y + 0.5) / h) * M_PI / 180.0;
        sinLatSinD[y] = std::sin(lat) * std::sin(decl);
        cosLatCosD[y] = std::cos(lat) * std::cos(decl);
    }
    for (int x = 0; x < w; ++x) {
        const double lon = (360.0 * (x + 0.5) / w - 180.0) * M_PI / 180.0;
        cosDLon[x] = std::cos(lon - subLon);
    }
    // KE9NS grayline zones (spot.cs SUNANGLE/Darken): flat brightness steps,
    // not a glow — day above the horizon, a wide dusk/dawn band while the sun
    // sits 0..6 degrees below (SZA 90..96), dark night core beyond that. The
    // narrow smoothstep on each boundary just kills aliasing on the edges.
    const double duskLo = std::sin(-6.0 * M_PI / 180.0);   // night below this
    auto smooth = [](double lo, double hi, double v) {
        const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double sinElev = sinLatSinD[y] + cosLatCosD[y] * cosDLon[x];
            const double tDusk = smooth(duskLo - 0.02, duskLo + 0.02, sinElev);
            const double tDay  = smooth(-0.015, 0.015, sinElev);
            const double bright = 0.18 + 0.25 * tDusk + 0.35 * tDay;
            const int f = static_cast<int>(bright * 256.0); // 18%/43%/78%
            const QRgb c = line[x];
            line[x] = qRgb((qRed(c)   * f) >> 8,
                           (qGreen(c) * f) >> 8,
                           (qBlue(c)  * f) >> 8);
        }
    }
    return img;
}
} // namespace

const QImage& PanadapterWidget::backgroundImage(int w, int h) {
    const qint64 minute = ds_.background == 2
        ? QDateTime::currentSecsSinceEpoch() / 60 : 0;      // grayline advances
    if (bgMode_ != ds_.background || bgW_ != w || bgH_ != h
        || bgMinute_ != minute) {
        bgCache_ = ds_.background == 2 ? renderWorldMap(w, h)
                                       : renderBlueRays(w, h);
        bgMode_ = ds_.background;
        bgW_ = w;
        bgH_ = h;
        bgMinute_ = minute;
    }
    return bgCache_;
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

void PanadapterWidget::setSpots(const QVector<SpotLabel>& s) {
    spots_ = s;
    update();
}

void PanadapterWidget::setCallsign(const QString& call) {
    callsign_ = call;
    update();
}

void PanadapterWidget::setVfoB(uint64_t hz, char role, int loHz, int hiHz) {
    if (drag_ == Drag::VfoB) return;               // don't snap it mid-drag
    if (hz == vfoBHz_ && role == vfoBRole_ && loHz == vfoBLo_ && hiHz == vfoBHi_)
        return;
    vfoBHz_ = hz;
    vfoBRole_ = role;
    vfoBLo_ = loHz;
    vfoBHi_ = hiHz;
    update();
}

bool PanadapterWidget::overVfoB(int x) const {
    if (!vfoBHz_ || !centerHz_) return false;
    const qint64 off = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
    if (std::llabs(off) > viewSpanHz_) return false;
    const int xb  = hzToX(static_cast<int>(off));
    const int xLo = hzToX(static_cast<int>(off) + vfoBLo_);
    const int xHi = hzToX(static_cast<int>(off) + vfoBHi_);
    return x >= std::min(xLo, xb) - 4 && x <= std::max(xHi, xb) + 4;
}

// KE9NS-style spot markers: a thin vertical line at the spotted frequency
// with the callsign running down it. Labels are click-to-tune (hit rects
// collected here, tested in mousePressEvent).
void PanadapterWidget::drawSpots(QPainter& p, int hSpec) {
    spotHits_.clear();
    if (spots_.isEmpty() || centerHz_ == 0 || hSpec < 60) return;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(false);
    p.setFont(f);
    const QFontMetrics fm(f);
    const qint64 half = viewSpanHz_ / 2;
    for (const SpotLabel& s : spots_) {
        const qint64 off = s.hz - static_cast<qint64>(centerHz_);
        if (off < -half || off > half) continue;
        const int x = hzToX(static_cast<int>(off));
        p.setPen(QColor(150, 120, 255, 110));           // KE9NS violet marker
        p.drawLine(x, 20, x, hSpec);
        const int textLen = fm.horizontalAdvance(s.call);
        p.save();
        p.translate(x - 2, 22);
        p.rotate(90);                                    // callsign runs downward
        p.setPen(QColor(255, 216, 50));
        p.drawText(0, 0, s.call);
        p.restore();
        spotHits_.push_back({QRect(x - 8, 20, 14, std::min(textLen + 6, hSpec - 24)),
                             s.hz});
    }
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
    if (!ds_.showGrid) return;
    if (centerHz_ == 0) {                          // no dial info: plain grid only
        p.setPen(QColor(255, 255, 255, 22));
        for (int i = 1; i < 10; ++i)
            p.drawLine(width() * i / 10, 0, width() * i / 10, hSpec);
        return;
    }
    // Gridlines on absolute "nice" frequencies (labels live on the scale band).
    // KE9NS density: fainter minor lines at the scale band's minor-tick step
    // between the major lines, so the grid reads every 1/5 of a label step.
    const double step = niceStep(viewSpanHz_);
    const double f0 = static_cast<double>(centerHz_) - viewSpanHz_ / 2.0;
    const double f1 = static_cast<double>(centerHz_) + viewSpanHz_ / 2.0;
    const double minor = step / 5.0;
    if (hzToX(static_cast<int>(minor)) - hzToX(0) > 14) {
        p.setPen(QColor(255, 255, 255, 14));
        for (double fq = std::ceil(f0 / minor) * minor; fq <= f1; fq += minor) {
            const qint64 q = static_cast<qint64>(std::llround(fq));
            if (q % static_cast<qint64>(std::llround(step)) == 0) continue;
            const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
            p.drawLine(x, 0, x, hSpec);
        }
    }
    p.setPen(QColor(255, 255, 255, 34));
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
    const double f0 = static_cast<double>(centerHz_) - viewSpanHz_ / 2.0;
    const double f1 = static_cast<double>(centerHz_) + viewSpanHz_ / 2.0;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
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
    // KE9NS-style labels: yellow, MHz.kkk.h format (e.g. 7.280.0 — the last
    // digit is hundreds of Hz).
    for (double fq = std::ceil(f0 / step) * step; fq <= f1; fq += step) {
        const int x = hzToX(static_cast<int>(std::lround(fq - double(centerHz_))));
        p.setPen(QColor(200, 215, 230, 200));
        p.drawLine(x, hSpec, x, hSpec + 5);
        const qint64 hz  = static_cast<qint64>(std::llround(fq));
        const qint64 mhz = hz / 1000000;
        const int    khz = static_cast<int>((hz / 1000) % 1000);
        const int    hun = static_cast<int>((hz % 1000) / 100);
        p.setPen(QColor(255, 216, 50));
        p.drawText(x + 3, hSpec + kScaleBandH - 4,
                   QString("%1.%2.%3").arg(mhz)
                       .arg(khz, 3, 10, QLatin1Char('0')).arg(hun));
    }
}

void PanadapterWidget::drawDbScale(QPainter& p, int hSpec) {
    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    const float range = dbMax_ - dbMin_;
    const int step = range > 80.0f ? 20 : 10;      // KE9NS-density when zoomed in
    // Minor lines at half the label step (KE9NS grid density), unlabeled.
    if (ds_.showGrid) {
        p.setPen(QColor(255, 255, 255, 12));
        const int half = step / 2;
        for (int db = static_cast<int>(std::ceil(dbMin_ / half)) * half;
             db < static_cast<int>(dbMax_); db += half) {
            if (db % step == 0) continue;          // majors drawn below
            const int y = static_cast<int>(hSpec * (1.0f - (db - dbMin_) / range));
            if (y < 20 || y > hSpec - 6) continue;
            p.drawLine(0, y, width(), y);
        }
    }
    for (int db = static_cast<int>(std::ceil(dbMin_ / step)) * step;
         db < static_cast<int>(dbMax_); db += step) {
        const int y = static_cast<int>(hSpec * (1.0f - (db - dbMin_) / range));
        if (y < 20 || y > hSpec - 6) continue;     // keep clear of the text row
        if (ds_.showGrid) {                        // labels stay (axis is a control)
            p.setPen(QColor(255, 255, 255, 26));
            p.drawLine(0, y, width(), y);
        }
        p.setPen(QColor(255, 216, 50, 200));
        p.drawText(4, y - 2, QString::number(db));
    }
}

void PanadapterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int hSpec = spectrumHeight();
    p.fillRect(rect(), QColor(12, 16, 22));
    if (ds_.background != 0 && hSpec > 1)          // KE9NS-style backdrop
        p.drawImage(0, 0, backgroundImage(width(), hSpec));

    // Station-callsign watermark: faint gray in the upper left (clear of the
    // span readout), under everything the display draws on top.
    if (ds_.showCall && !callsign_.isEmpty() && hSpec > 40) {
        const QFont saved = p.font();
        QFont f = saved;
        f.setPixelSize(std::min(hSpec / 6, width() / 14));
        f.setBold(true);
        f.setLetterSpacing(QFont::PercentageSpacing, 112);
        p.setFont(f);
        p.setPen(QColor(200, 215, 230, 26));
        p.drawText(QRect(kDbAxisW + 10, 20, width() - kDbAxisW - 20, hSpec - 24),
                   Qt::AlignLeft | Qt::AlignTop, callsign_);
        p.setFont(saved);
    }

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
        // Transparent above the trace so the backdrop (map/rays) shows through.
        if (fillImg_.width() != w || fillImg_.height() != hSpec
            || fillImg_.format() != QImage::Format_ARGB32_Premultiplied)
            fillImg_ = QImage(w, hSpec, QImage::Format_ARGB32_Premultiplied);
        std::vector<int> colY(w);
        for (int x = 0; x < w; ++x)
            colY[x] = static_cast<int>(hSpec * (1.0f - colT[x]));
        for (int y = 0; y < hSpec; ++y) {
            const float t = 1.0f - static_cast<float>(y) / (hSpec - 1);
            const QRgb c = mapColor(std::pow(t, kFillGamma));
            QRgb* line = reinterpret_cast<QRgb*>(fillImg_.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = (y >= colY[x]) ? c : qRgba(0, 0, 0, 0);
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
    // then the VFO markers crossing everything. Colors follow the routing
    // panel: red = the transmitting VFO, green = the receiving one. Normal
    // (TX+RX both on A) keeps the plain center marker and shows B, when it's
    // in view, as a quiet blue sub-RX line.
    drawScaleBand(p, hSpec);
    {
        const QFont savedFont = p.font();
        auto flag = [&](int x, const QString& text, const QColor& c) {
            QFont ff = savedFont;
            ff.setPixelSize(9);
            ff.setBold(true);
            p.setFont(ff);
            QRect r(x + 4, 18, p.fontMetrics().horizontalAdvance(text) + 8, 13);
            if (r.right() > width() - 2) r.moveRight(x - 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 200));
            p.drawRoundedRect(r, 2, 2);
            p.setPen(QColor(250, 252, 255));
            p.drawText(r, Qt::AlignCenter, text);
        };
        // Role colors match the routing buttons: red = transmitting VFO,
        // green = the main receiver's VFO, blue = a parked sub dial.
        const QColor txRed(224, 93, 93), rxGreen(62, 207, 122), subBlue(93, 178, 240);
        QColor aCol(200, 80, 80);
        QString aText = "A";
        if (vfoBRole_ == 'T')      { aCol = rxGreen; aText = "A RX"; }
        else if (vfoBRole_ == 'R') { aCol = txRed;   aText = "A TX"; }
        p.setPen(aCol);
        p.drawLine(width() / 2, 0, width() / 2, height());
        flag(width() / 2, aText, aCol);
        if (vfoBHz_ && centerHz_) {                // VFO B, if inside the view
            const qint64 off = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
            const int xb = hzToX(static_cast<int>(off));
            if (std::llabs(off) < viewSpanHz_ / 2 && xb >= 0 && xb <= width()) {
                // Full VFO look, same as the main passband: filter tint across
                // spectrum + waterfall, edge lines, dial line, flag.
                const QColor& c = vfoBRole_ == 'T' ? txRed
                                : vfoBRole_ == 'R' ? rxGreen : subBlue;
                const QString bText = vfoBRole_ == 'T' ? "B TX"
                                    : vfoBRole_ == 'R' ? "B RX" : "B";
                const int xLo = hzToX(static_cast<int>(off) + vfoBLo_);
                const int xHi = hzToX(static_cast<int>(off) + vfoBHi_);
                p.setBrush(Qt::NoBrush);
                p.fillRect(QRect(QPoint(xLo, 0), QPoint(xHi, height())),
                           QColor(c.red(), c.green(), c.blue(), 42));
                p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 170), 1));
                p.drawLine(xLo, 0, xLo, height());
                p.drawLine(xHi, 0, xHi, height());
                p.setPen(QPen(c, 1));
                p.drawLine(xb, 0, xb, height());
                flag(xb, bText, c);
            }
        }
        p.setFont(savedFont);
        // The flags set a fill brush; clear it or the trace path below is
        // rendered as a giant filled polygon in the flag's color.
        p.setBrush(Qt::NoBrush);
    }

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
        drawSpots(p, hSpec);                       // cluster spots, on top
    } else {
        p.setPen(QColor(120, 220, 140));
        p.drawText(QRect(0, 0, width(), hSpec), Qt::AlignCenter,
                   "panadapter — no IQ source (build with -DBUILD_SDRPLAY=ON)");
    }

    // Span readout (the scale band draws its own separators).
    p.setPen(QColor(200, 200, 200, 160));
    p.drawText(6, 14, QString("span %1 kHz   wheel: tune (shift fine)  drag: tune  "
                              "edge: bw  shift+edge: cut  ctrl+body: pbt  rclick: VFO B")
                          .arg(viewSpanHz_ / 1000.0, 0, 'f', viewSpanHz_ < 20000 ? 1 : 0));
}

void PanadapterWidget::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    // NOTE: no wheel gesture on the dB axis — a plain wheel there used to
    // adjust RANGE, which silently rescaled the colors whenever a wheel-tune
    // happened near the left edge. Range lives in the DISPLAY panel; the axis
    // drag still shifts REF.
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
        else if (wheelVfo_ == 'B')                  // wheel follows last-tuned VFO
            emit vfoBStepRequested(n, e->modifiers() & Qt::ShiftModifier);
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
    // Right-click = drop VFO B on that frequency (get the TX dead on the
    // station the DX just worked, one click).
    if (e->button() == Qt::RightButton) {
        drag_ = Drag::None;
        if (!inScaleBand(y) && !inDbAxis(x, y)) {
            wheelVfo_ = 'B';                        // wheel now nudges B
            emit vfoBTuneRequested(xToHz(x));
        }
        return;
    }
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
    // A click on a spot callsign tunes to the spotted frequency.
    for (const auto& [rect, hz] : spotHits_) {
        if (rect.contains(e->pos())) {
            drag_ = Drag::None;
            wheelVfo_ = 'A';
            emit tuneRequested(static_cast<int>(hz - static_cast<qint64>(centerHz_)));
            return;
        }
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
        // Plain edge = symmetric width change (pure bandwidth, PBT untouched —
        // matches the front-panel BW knob and the sub filter's behavior);
        // Shift+edge = hi/lo-cut (width + center, moves PBT).
        if (e->modifiers() & Qt::ShiftModifier) drag_ = onLo ? Drag::LoEdge : Drag::HiEdge;
        else                                    drag_ = Drag::SymEdge;
        emit passbandEditBegan(pbLoHz_, pbHiHz_);   // consumer anchors radio state
        return;
    }
    // Grab VFO B (line or its passband tint) to slide it — split TX placement.
    if (overVfoB(x)) {
        drag_ = Drag::VfoB;
        wheelVfo_ = 'B';
        dragStartBOff_ = static_cast<qint64>(vfoBHz_) - static_cast<qint64>(centerHz_);
        return;
    }
    // Grab the A dial line to drag-tune the main VFO.
    if (std::abs(x - width() / 2) <= 4 && centerHz_) {
        drag_ = Drag::VfoA;
        wheelVfo_ = 'A';
        dragStartCenter_ = centerHz_;
        return;
    }
    if (x > hzToX(pbLoHz_) && x < hzToX(pbHiHz_)) {
        // Inside the passband: dragging moves the VFO itself (drag-to-tune);
        // Ctrl+drag slides the PBT instead. Released without moving it's a
        // click-to-tune. Decided on first move.
        drag_ = Drag::BodyPending;
        bodyPbt_ = e->modifiers() & Qt::ControlModifier;
        dragStartCenter_ = centerHz_;
        return;
    }
    drag_ = Drag::None;
    wheelVfo_ = 'A';
    emit tuneRequested(xToHz(x));                   // click-to-tune
}

void PanadapterWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::None) {
        // Hover feedback: vertical-drag targets, and VFO B's horizontal grab.
        const int hx = e->pos().x(), hy = e->pos().y();
        if (inScaleBand(hy) || inDbAxis(hx, hy)) setCursor(Qt::SizeVerCursor);
        else if (overVfoB(hx)
                 || std::abs(hx - width() / 2) <= 4) setCursor(Qt::SizeHorCursor);
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
    if (drag_ == Drag::VfoB) {
        const qint64 off = dragStartBOff_ + (hz - xToHz(dragStartX_));
        vfoBHz_ = static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        update();
        emit vfoBDragged(static_cast<int>(off));    // consumer streams *BF
        return;
    }
    if (drag_ == Drag::VfoA) {
        // Anchored to the grab-time dial: cursor movement maps 1:1 to Hz, so
        // the view recentering underneath doesn't compound.
        const qint64 delta = hz - xToHz(dragStartX_);
        emit vfoADragged(static_cast<uint64_t>(
            static_cast<qint64>(dragStartCenter_) + delta));
        return;
    }
    if (drag_ == Drag::BodyPending) {
        if (std::abs(x - dragStartX_) < 4) return;  // still just a click
        if (!bodyPbt_) {
            drag_ = Drag::VfoA;                     // drag the VFO, not the filter
            wheelVfo_ = 'A';
            const qint64 delta = hz - xToHz(dragStartX_);
            emit vfoADragged(static_cast<uint64_t>(
                static_cast<qint64>(dragStartCenter_) + delta));
            return;                                 // MUST NOT fall into edge math
        }
        drag_ = Drag::Body;                         // Ctrl: classic PBT slide
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
    if (drag_ == Drag::BodyPending) {
        wheelVfo_ = 'A';
        emit tuneRequested(xToHz(e->pos().x()));
    }
    drag_ = Drag::None;
}

} // namespace ttc
