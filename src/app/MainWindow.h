// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
class QTimer;
#include "radio/TenTecOrion.h"
#include "net/RigctldServer.h"
#include "ui/PanadapterWidget.h"
#include "ui/SMeterWidget.h"
#include "ui/ControlPanel.h"
#include "ui/FrequencyDisplay.h"
#include "ui/TxBar.h"
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
    void refreshNotchOverlay();        // radio notch (audio Hz) -> RF-offset marker
    void sendPendingFilter();          // coalesced drag-to-filter serial writes
    void sendPendingNotch();           // coalesced drag-to-notch serial writes
    void tuneAbsolute(uint64_t hz);    // every tune path funnels through here
    void applyMode(Mode m);            // user mode change (button or band memory)
    void saveBandMemory();             // stash freq/mode/filter in curBand_/curReg_
    void recallStack(int band, int reg); // recall a band-stack register
    TenTecOrion      radio_;
    RigctldServer    rigctld_{&radio_};
    PanadapterWidget* pan_ = nullptr;
    SMeterWidget*     smeter_ = nullptr;
    ControlPanel*     panel_  = nullptr;
    FrequencyDisplay* freqDisp_ = nullptr;
    TxBar*            txBar_ = nullptr;
    int curBand_ = -1;                         // index into kBands, -1 = none
    int curReg_  = 0;                          // active stack register (0..3 = A..D)
    QElapsedTimer sinceEnforce_;               // rate-limit amp drive-limit corrections
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

    // Manual notch state (audio Hz, from polling; see refreshNotchOverlay).
    bool notchOn_      = false;
    int  notchCenter_  = 550;
    int  notchWidth_   = 30;
    QElapsedTimer sinceNotchEdit_;             // suppress poll snap-back after a drag

    // Coalesced drag-to-filter: keep only the latest values, send ~25x/sec.
    QTimer* filterTx_ = nullptr;
    int  pendBwHz_ = 0, pendPbtHz_ = 0;
    bool filterDirty_ = false;
    QTimer* notchTx_ = nullptr;                // same pattern for notch drags
    int  pendNotchHz_ = 0;
    bool notchDirty_ = false;
#ifdef HAVE_SDRPLAY
    SdrPlaySource    sdr_;
    SpectrumComputer spectrum_{4096};   // 61 Hz/bin at 250 kHz span — survives deep zoom
#endif
};

} // namespace ttc
