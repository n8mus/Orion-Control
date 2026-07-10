// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"

#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace ttc {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Ten-Tec SDR Console");

    pan_ = new PanadapterWidget(this);
    pan_->setPassband(-1200, 1200);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(pan_);
    setCentralWidget(central);

    connect(pan_, &PanadapterWidget::tuneRequested,    this, &MainWindow::onTuneRequested);
    connect(pan_, &PanadapterWidget::passbandChanged,  this, &MainWindow::onPassbandChanged);

    // Keep the rigctld server's cached frequency in step with the radio.
    connect(&radio_, &TenTecOrion::frequencyReported, this,
            [this](Rx rx, uint64_t hz) {
                if (rx == Rx::Main) { centerHz_ = hz; rigctld_.cacheFrequency(hz); }
            });

    // Start the interop seam. Clients (cqrlog/WSJT-X/fldigi/GridTracker) connect here.
    const bool listening = rigctld_.listen(4532);
    statusBar()->showMessage(
        listening ? "rigctld emulation on :4532  |  radio: not connected (Phase 0: wire /dev/ttyS0)"
                  : "FAILED to bind :4532 (already in use? stop flrig/rigctld)");
}

void MainWindow::onTuneRequested(int offsetHz) {
    const uint64_t f = centerHz_ + offsetHz;
    radio_.setFrequencyHz(Rx::Main, f);
    rigctld_.cacheFrequency(f);
    statusBar()->showMessage(QString("tune -> %1 Hz").arg(f));
}

void MainWindow::onPassbandChanged(int loHz, int hiHz) {
    radio_.setPassband(Rx::Main, loHz, hiHz);       // -> *RMF width + *RMP center
    rigctld_.cacheBandwidth(hiHz - loHz);
    statusBar()->showMessage(QString("filter -> lo %1  hi %2  (bw %3 Hz)")
                                 .arg(loHz).arg(hiHz).arg(hiHz - loHz));
}

} // namespace ttc
