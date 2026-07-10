// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
class QTimer;
#include "radio/TenTecOrion.h"
#include "net/RigctldServer.h"
#include "ui/PanadapterWidget.h"
#ifdef HAVE_SDRPLAY
#include "sdr/SdrPlaySource.h"
#include "dsp/SpectrumComputer.h"
#endif

namespace ttc {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onTuneRequested(int offsetHz);
    void onPassbandChanged(int loHz, int hiHz);

private:
    void refreshPassbandOverlay();     // radio state -> mode-sided on-screen passband
    void sendPendingFilter();          // coalesced drag-to-filter serial writes
    TenTecOrion      radio_;
    RigctldServer    rigctld_{&radio_};
    PanadapterWidget* pan_ = nullptr;
    uint64_t centerHz_ = 7150000;              // open on 40 m where the Orion lives
    bool awaitingFreq_ = false;                // one ?AF in flight at a time
    QElapsedTimer freqQueryAge_;
    uint64_t pendingHz_ = 0;                   // change must be seen twice (glitch filter)
    int pollTick_ = 0;                         // schedules mode/filter queries between ?AF
    QElapsedTimer sinceTune_;                  // suppress dial-follow right after a click
    QElapsedTimer sinceFilterEdit_;            // suppress overlay refresh right after a drag

    // Radio filter state (from polling) and mode-aware passband mapping.
    Mode rigMode_  = Mode::USB;
    int  rigBwHz_  = 2400;
    int  rigPbtHz_ = 0;

    // Delta-anchored drag: on edge grab, snapshot the radio's real polled state;
    // drags send anchor + delta, so no assumed nominal placement can cause jumps.
    int anchorLoHz_ = 0, anchorHiHz_ = 0;      // overlay edges at drag start
    int anchorBwHz_ = 2400, anchorPbtHz_ = 0;  // radio state at drag start

    // Coalesced drag-to-filter: keep only the latest values, send ~25x/sec.
    QTimer* filterTx_ = nullptr;
    int  pendBwHz_ = 0, pendPbtHz_ = 0;
    bool filterDirty_ = false;
#ifdef HAVE_SDRPLAY
    SdrPlaySource    sdr_;
    SpectrumComputer spectrum_{4096};   // 61 Hz/bin at 250 kHz span — survives deep zoom
#endif
};

} // namespace ttc
