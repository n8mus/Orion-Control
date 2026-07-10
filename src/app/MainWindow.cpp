// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"

#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
        listening ? "rigctld emulation on :4532  |  radio: not connected (wire /dev/orion)"
                  : "FAILED to bind :4532 (already in use? stop flrig/rigctld)");

#ifdef HAVE_SDRPLAY
    // IQ -> FFT -> panadapter. The spectrum callback runs on the SDR thread, so
    // marshal the result onto the GUI thread before touching the widget.
    constexpr double kSampleRate = 2000000.0;
    constexpr int    kDecim      = 8;                 // 250 kHz displayed span
    const int spanHz = static_cast<int>(kSampleRate / kDecim);
    pan_->setSpanHz(spanHz);
    spectrum_.setOutput([this](const std::vector<float>& db) {
        if (std::getenv("TTC_SELFTEST")) {          // headless evidence the FFT is live
            static int frames = 0;
            if (++frames % 20 == 1) {
                float mn = 1e9f, mx = -1e9f;
                for (float v : db) { mn = std::min(mn, v); mx = std::max(mx, v); }
                std::fprintf(stderr, "[spectrum] frame %d  bins=%zu  min %.1f dB  peak %.1f dB\n",
                             frames, db.size(), mn, mx);
            }
        }
        QMetaObject::invokeMethod(pan_, [this, copy = db]() mutable {
            pan_->setSpectrum(copy);
        }, Qt::QueuedConnection);
    });
    sdr_.setIqCallback([this](const IqBlock& iq) { spectrum_.addSamples(iq); });
    sdr_.setDecimation(kDecim);
    if (sdr_.apiOk() && sdr_.start(static_cast<double>(centerHz_), kSampleRate)) {
        statusBar()->showMessage(QString("RSP2 panadapter %1 MHz, span %2 kHz  |  rigctld :4532")
                                     .arg(centerHz_ / 1e6, 0, 'f', 4).arg(spanHz / 1000));
    } else {
        statusBar()->showMessage("SDR unavailable: " + QString::fromStdString(sdr_.lastError()));
    }
#endif
}

MainWindow::~MainWindow() {
#ifdef HAVE_SDRPLAY
    // Stop the SDRplay streaming thread (Uninit drains callbacks) BEFORE the
    // spectrum_/dsp members it references are destroyed. Without this, member
    // teardown order frees the FFT under a still-running callback (use-after-free).
    sdr_.stop();
#endif
}

void MainWindow::onTuneRequested(int offsetHz) {
    const uint64_t f = centerHz_ + offsetHz;
    radio_.setFrequencyHz(Rx::Main, f);
    rigctld_.cacheFrequency(f);
    centerHz_ = f;                              // recenter view on the clicked signal
#ifdef HAVE_SDRPLAY
    sdr_.setCenterFrequency(static_cast<double>(f));
#endif
    statusBar()->showMessage(QString("tune -> %1 Hz").arg(f));
}

void MainWindow::onPassbandChanged(int loHz, int hiHz) {
    radio_.setPassband(Rx::Main, loHz, hiHz);       // -> *RMF width + *RMP center
    rigctld_.cacheBandwidth(hiHz - loHz);
    statusBar()->showMessage(QString("filter -> lo %1  hi %2  (bw %3 Hz)")
                                 .arg(loHz).arg(hiHz).arg(hiHz - loHz));
}

} // namespace ttc
