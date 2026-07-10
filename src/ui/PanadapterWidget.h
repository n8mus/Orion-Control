// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QImage>
#include <vector>

namespace ttc {

// Spectrum + waterfall panadapter display. The flagship interactions live here:
//   * click a signal        -> tuneRequested(offsetHz)
//   * drag a passband edge   -> passbandChanged(loHz, hiHz)  (=> radio filter)
//   * mouse wheel            -> zoom the displayed span (drag-to-filter needs this)
// Data model: setSpectrum receives the FULL captured span (fftshifted dB bins);
// the view renders a zoomable sub-window centered on the dial. All mouse math
// goes through hzToX/xToHz, so zoom applies to every interaction automatically.
class PanadapterWidget : public QWidget {
    Q_OBJECT
public:
    explicit PanadapterWidget(QWidget* parent = nullptr);

    void setSpanHz(int spanHz);                    // full captured span
    void setViewSpanHz(int spanHz);                // zoom (slider); no signal re-emit
    int  viewSpanHz() const { return viewSpanHz_; }
    int  fullSpanHz() const { return fullSpanHz_; }
    int  minViewSpanHz() const;
    void setPassband(int loHz, int hiHz);          // offsets from center, in Hz
    // Manual-notch marker, in display (RF-offset) space; caller maps the
    // radio's audio-Hz notch through the mode sideband. Ignored mid-drag.
    void setNotch(bool on, int rfOffsetHz, int widthHz);
    void setSpectrum(const std::vector<float>& magsDb);

signals:
    void tuneRequested(int offsetHz);              // click-to-tune (offset from center)
    void tuneStepRequested(int steps, bool fine);  // wheel tune; fine = Shift held
    void passbandEditBegan(int loHz, int hiHz);    // edge grabbed: anchor radio state
    void passbandChanged(int loHz, int hiHz);      // drag-to-filter
    void notchDragged(int rfOffsetHz);             // notch marker slid (streamed)
    void notchWidthAdjustRequested(int steps);     // wheel over the notch marker
    void viewSpanChanged(int spanHz);              // Ctrl+wheel zoom

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int    hzToX(int hz) const;
    int    xToHz(int x) const;
    int    spectrumHeight() const;                 // top area; waterfall below
    void   appendWaterfallRow(const std::vector<float>& db);
    static QRgb colormap(float t);                 // t in [0,1]

    bool   overNotch(int x) const;                 // cursor within the grab zone

    // Drag-gesture bookkeeping (symmetric-BW and body-drag PBT).
    int dragStartX_  = 0;
    int dragStartLo_ = 0, dragStartHi_ = 0;

    int fullSpanHz_ = 250000;                      // what the SDR captures
    int viewSpanHz_ = 250000;                      // what we display (zoom)
    int pbLoHz_ = -1200;
    int pbHiHz_ = 1200;
    bool notchOn_     = false;
    int  notchRfHz_   = 0;                         // marker center, RF offset
    int  notchWidthHz_ = 0;
    float dbMin_ = -125.0f, dbMax_ = -55.0f;       // display dB range
    std::vector<float> spectrum_;                  // full span, fftshifted
    QImage waterfall_;                             // width = bins, scrolls down

    enum class Drag {
        None,
        LoEdge, HiEdge,     // one-edge drag: hi/lo-cut semantics (bw + pbt)
        SymEdge,            // Shift+edge: symmetric width, pure bw, pbt kept
        BodyPending, Body,  // press inside passband; becomes a pure-pbt slide
        Notch,
    } drag_ = Drag::None;
};

} // namespace ttc
