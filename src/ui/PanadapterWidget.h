// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <vector>

namespace ttc {

// Spectrum/panadapter display. The flagship interactions live here:
//   * click a signal        -> tuneRequested(offsetHz)
//   * drag a passband edge   -> passbandChanged(loHz, hiHz)  (=> radio filter)
// Rendering is a placeholder trace until the WDSP/SDRplay path lands (Phase 2).
class PanadapterWidget : public QWidget {
    Q_OBJECT
public:
    explicit PanadapterWidget(QWidget* parent = nullptr);

    void setSpanHz(int spanHz);
    void setPassband(int loHz, int hiHz);          // offsets from center, in Hz
    void setSpectrum(const std::vector<float>& magsDb);

signals:
    void tuneRequested(int offsetHz);              // click-to-tune
    void passbandChanged(int loHz, int hiHz);      // drag-to-filter

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    int   hzToX(int hz) const;
    int   xToHz(int x) const;

    int spanHz_ = 48000;                           // typical SDRplay panadapter span
    int pbLoHz_ = -1200;
    int pbHiHz_ = 1200;
    std::vector<float> spectrum_;

    enum class Drag { None, LoEdge, HiEdge } drag_ = Drag::None;
};

} // namespace ttc
