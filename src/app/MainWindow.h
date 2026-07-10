// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
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
    TenTecOrion      radio_;
    RigctldServer    rigctld_{&radio_};
    PanadapterWidget* pan_ = nullptr;
    uint64_t centerHz_ = 7150000;              // open on 40 m where the Orion lives
    bool awaitingFreq_ = false;                // one ?AF in flight at a time
    QElapsedTimer freqQueryAge_;
    uint64_t pendingHz_ = 0;                   // change must be seen twice (glitch filter)
#ifdef HAVE_SDRPLAY
    SdrPlaySource    sdr_;
    SpectrumComputer spectrum_{4096};   // 61 Hz/bin at 250 kHz span — survives deep zoom
#endif
};

} // namespace ttc
