// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QImage>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace ttc {

// User-adjustable display parameters (KE9NS/Flex-style), persisted by the
// owner and applied via setDisplaySettings. refDb is the top of the spectrum
// scale; refDb - rangeDb is the bottom (contrast). One shared scale drives
// the trace, the gradient fill and the waterfall so colors always agree.
struct DisplaySettings {
    float refDb     = -55.0f;   // top of scale, dB
    float rangeDb   =  70.0f;   // scale height, dB (contrast)
    int   palette   = 0;        // index into paletteNames()
    int   avgFrames = 4;        // spectrum EMA length; 1 = off
    int   wfSpeed   = 2;        // FFT frames per waterfall row; 1 = fastest
    bool  fillTrace = true;     // KE9NS-style gradient fill under the trace
    bool  peakHold  = false;    // slow-decay peak trace
    float split     = 0.42f;    // spectrum/waterfall divider position (0..1)
};

// Spectrum + waterfall panadapter display. The flagship interactions live here:
//   * click a signal        -> tuneRequested(offsetHz)
//   * drag a passband edge   -> passbandChanged(loHz, hiHz)  (=> radio filter)
//   * mouse wheel            -> zoom the displayed span (drag-to-filter needs this)
// Data model: setSpectrum receives the FULL captured span (fftshifted dB bins);
// the view renders a zoomable sub-window centered on the dial. All mouse math
// goes through hzToX/xToHz, so zoom applies to every interaction automatically.
// The waterfall keeps raw dB rows (not pixels) so zoom/resize/palette changes
// re-render history crisply — each display row maps pixel columns to bins with
// a max-pick, never smooth image scaling.
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
    void setCenterHz(uint64_t hz);                 // dial freq, for grid labels

    void setDisplaySettings(const DisplaySettings& s);
    const DisplaySettings& displaySettings() const { return ds_; }
    static QStringList paletteNames();

signals:
    void tuneRequested(int offsetHz);              // click-to-tune (offset from center)
    void tuneStepRequested(int steps, bool fine);  // wheel tune; fine = Shift held
    void passbandEditBegan(int loHz, int hiHz);    // edge grabbed: anchor radio state
    void passbandChanged(int loHz, int hiHz);      // drag-to-filter
    void notchDragged(int rfOffsetHz);             // notch marker slid (streamed)
    void notchWidthAdjustRequested(int steps);     // wheel over the notch marker
    void pbtZeroRequested();                       // double-click a passband edge
    void viewSpanChanged(int spanHz);              // Ctrl+wheel zoom
    // In-widget edits (dB-axis drag, range wheel, divider drag) changed ds_;
    // owner persists and syncs the DISPLAY panel.
    void displaySettingsEdited(const DisplaySettings& s);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int    hzToX(int hz) const;
    int    xToHz(int x) const;
    int    spectrumHeight() const;                 // top area; waterfall below
    QRgb   mapColor(float t) const;                // t in [0,1], current palette
    void   pushWaterfallRow(const std::vector<float>& db);
    void   renderWfLine(const float* src, QRgb* dst, int w, int binLo, int binHi) const;
    void   ensureWaterfallImage(int w, int h, int binLo, int binHi);
    void   drawFreqGrid(QPainter& p, int hSpec);   // gridlines, spectrum area only
    void   drawScaleBand(QPainter& p, int hSpec);  // freq scale strip on the divider
    void   drawDbScale(QPainter& p, int hSpec);    // horizontal dB lines + labels

    bool   overNotch(int x) const;                 // cursor within the grab zone
    bool   inScaleBand(int y) const;               // over the draggable divider strip
    bool   inDbAxis(int x, int y) const;           // over the draggable dB scale

    // Drag-gesture bookkeeping (symmetric-BW and body-drag PBT).
    int dragStartX_  = 0;
    int dragStartY_  = 0;
    int dragStartLo_ = 0, dragStartHi_ = 0;
    float dragStartRef_   = 0.0f;                  // refDb at dB-axis grab
    float dragStartSplit_ = 0.42f;                 // split at divider grab

    int fullSpanHz_ = 250000;                      // what the SDR captures
    int viewSpanHz_ = 250000;                      // what we display (zoom)
    uint64_t centerHz_ = 0;                        // 0 = unknown, labels skipped
    int pbLoHz_ = -1200;
    int pbHiHz_ = 1200;
    bool notchOn_     = false;
    int  notchRfHz_   = 0;                         // marker center, RF offset
    int  notchWidthHz_ = 0;

    DisplaySettings ds_;
    float dbMin_ = -125.0f, dbMax_ = -55.0f;       // derived from ds_ (ref/range)

    std::vector<float> spectrum_;                  // latest raw frame, full span
    std::vector<float> avg_;                       // EMA of spectrum_ (the trace)
    std::vector<float> peaks_;                     // slow-decay peak-hold trace

    // Waterfall: ring buffer of raw dB rows + a pixel-resolution rendered image.
    // New rows render incrementally; zoom/resize/settings force a full rebuild.
    std::vector<float> wfHist_;                    // kWfHistRows x wfBins_, ring
    int  wfBins_ = 0, wfHead_ = 0, wfCount_ = 0;
    int  wfPending_ = 0;                           // rows added since last render
    std::vector<float> wfAccum_;                   // max-merge across wfSpeed frames
    int  wfAccumN_ = 0;
    QImage wfImg_;                                 // rendered, 1 row = 1 pixel line
    int  lastBinLo_ = -1, lastBinHi_ = -1;         // geometry key for wfImg_
    bool wfDirty_ = true;                          // settings changed: full re-render
    QImage gradStrip_;                             // 1 x hSpec fill gradient (cached)
    int  gradPal_ = -1;                            // palette the strip was built for

    enum class Drag {
        None,
        LoEdge, HiEdge,     // one-edge drag: hi/lo-cut semantics (bw + pbt)
        SymEdge,            // Shift+edge: symmetric width, pure bw, pbt kept
        BodyPending, Body,  // press inside passband; becomes a pure-pbt slide
        Notch,
        Divider,            // drag the freq-scale band: move the wf split
        DbAxis,             // drag the left dB scale: shift ref level
    } drag_ = Drag::None;
};

} // namespace ttc
