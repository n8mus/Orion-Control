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
    void setPassband(int loHz, int hiHz);          // offsets from center, in Hz
    void setSpectrum(const std::vector<float>& magsDb);

signals:
    void tuneRequested(int offsetHz);              // click-to-tune (offset from center)
    void tuneStepRequested(int steps, bool fine);  // wheel tune; fine = Shift held
    void passbandChanged(int loHz, int hiHz);      // drag-to-filter
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

    int fullSpanHz_ = 250000;                      // what the SDR captures
    int viewSpanHz_ = 250000;                      // what we display (zoom)
    int pbLoHz_ = -1200;
    int pbHiHz_ = 1200;
    float dbMin_ = -125.0f, dbMax_ = -55.0f;       // display dB range
    std::vector<float> spectrum_;                  // full span, fftshifted
    QImage waterfall_;                             // width = bins, scrolls down

    enum class Drag { None, LoEdge, HiEdge } drag_ = Drag::None;
};

} // namespace ttc
