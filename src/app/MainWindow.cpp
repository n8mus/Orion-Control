// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"

#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    connect(pan_, &PanadapterWidget::passbandEditBegan, this, [this](int lo, int hi) {
        anchorLoHz_ = lo;  anchorHiHz_ = hi;       // where the overlay was grabbed
        anchorBwHz_ = rigBwHz_;                    // radio's real state, from polling
        anchorPbtHz_ = rigPbtHz_;
        sinceFilterEdit_.restart();
    });
    connect(pan_, &PanadapterWidget::passbandChanged,  this, &MainWindow::onPassbandChanged);
    connect(pan_, &PanadapterWidget::viewSpanChanged,  this, [this](int spanHz) {
        statusBar()->showMessage(QString("zoom -> span %1 kHz").arg(spanHz / 1000.0, 0, 'f', 1));
    });
    // Wheel tuning: 100 Hz per notch, 10 Hz with Shift (Ctrl+wheel zooms).
    connect(pan_, &PanadapterWidget::tuneStepRequested, this, [this](int steps, bool fine) {
        onTuneRequested(steps * (fine ? 10 : 100));
    });

    // Coalesce drag-to-filter writes: the Orion's UART services on a ~100 ms cycle,
    // so raw mouse-move rates (100+ cmd pairs/sec) would flood it. Send only the
    // latest edges, at most ~25x/sec, trailing edge included.
    filterTx_ = new QTimer(this);
    filterTx_->setSingleShot(true);
    filterTx_->setInterval(40);
    connect(filterTx_, &QTimer::timeout, this, &MainWindow::sendPendingFilter);

    // Radio state -> mode-sided passband overlay (LSB hangs below the carrier).
    connect(&radio_, &TenTecOrion::modeReported, this, [this](Rx rx, Mode m) {
        if (rx != Rx::Main) return;
        if (m != rigMode_) { rigMode_ = m; refreshPassbandOverlay(); }
    });
    connect(&radio_, &TenTecOrion::bandwidthReported, this, [this](Rx rx, int bw) {
        if (rx != Rx::Main) return;
        rigctld_.cacheBandwidth(bw);
        if (bw != rigBwHz_) { rigBwHz_ = bw; refreshPassbandOverlay(); }
    });
    connect(&radio_, &TenTecOrion::pbtReported, this, [this](Rx rx, int pbt) {
        if (rx != Rx::Main) return;
        if (pbt != rigPbtHz_) { rigPbtHz_ = pbt; refreshPassbandOverlay(); }
    });

    // The radio reported its frequency (startup sync or dial-follow poll): keep the
    // rigctld cache and the panadapter center locked to the physical VFO.
    connect(&radio_, &TenTecOrion::frequencyReported, this,
            [this](Rx rx, uint64_t hz) {
                if (rx != Rx::Main) return;
                awaitingFreq_ = false;               // poll answered; next one may go
                if (std::getenv("TTC_SELFTEST"))
                    std::fprintf(stderr, "[radio] VFO-A reports %.4f MHz\n", hz / 1e6);
                if (hz != centerHz_) {
                    // Right after a click-to-tune the radio takes a few hundred ms to
                    // settle; stale reads must not "confirm" the old frequency and
                    // yank the tune back. Ignore dial-follow during that window.
                    if (sinceTune_.isValid() && sinceTune_.elapsed() < 1500) return;
                    // The Orion occasionally emits a mangled frame that still parses
                    // as a plausible frequency. Require two consecutive identical
                    // reads before following a dial change (~200 ms, imperceptible).
                    if (hz != pendingHz_) { pendingHz_ = hz; return; }
                    centerHz_ = hz;
                    rigctld_.cacheFrequency(hz);     // cache only debounce-confirmed values
#ifdef HAVE_SDRPLAY
                    sdr_.setCenterFrequency(static_cast<double>(hz));
#endif
                    statusBar()->showMessage(
                        QString("radio %1 MHz  |  panadapter following").arg(hz / 1e6, 0, 'f', 4));
                }
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

    // Open the Orion so click-to-tune / drag-to-filter actually reach the radio,
    // and poll its VFO so the panadapter follows the physical dial. Device path
    // overridable via TTC_RADIO_DEV (default /dev/orion).
    const char* devEnv = std::getenv("TTC_RADIO_DEV");
    const std::string radioDev = devEnv ? devEnv : "/dev/orion";
    if (radio_.open(radioDev)) {
        radio_.queryFrequency(Rx::Main);            // one-shot sync at startup
        radio_.queryMode(Rx::Main);
        radio_.queryFilter(Rx::Main);
        awaitingFreq_ = true;
        freqQueryAge_.start();
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            if (!radio_.connected()) return;
            ++pollTick_;
            // Interleave slower mode/filter polls between the ~3 Hz frequency polls
            // so the overlay tracks front-panel mode/BW/PBT changes too.
            if (pollTick_ % 5 == 2) { radio_.queryMode(Rx::Main);   return; }
            if (pollTick_ % 5 == 4) { radio_.queryFilter(Rx::Main); return; }
            // Only one ?AF in flight: the Orion's response tail runs ~110 ms. Re-arm
            // after 600 ms anyway in case a response was dropped.
            if (awaitingFreq_ && freqQueryAge_.elapsed() < 600) return;
            radio_.queryFrequency(Rx::Main);
            awaitingFreq_ = true;
            freqQueryAge_.restart();
        });
        poll->start(200);
    } else {
        statusBar()->showMessage("radio: could not open " + QString::fromStdString(radioDev));
    }
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
    pendingHz_ = f;
    sinceTune_.restart();                       // hold off dial-follow while it settles
#ifdef HAVE_SDRPLAY
    sdr_.setCenterFrequency(static_cast<double>(f));
#endif
    statusBar()->showMessage(QString("tune -> %1 Hz").arg(f));
}

// PBT sign in RF space: positive PBT conventionally shifts the passband toward
// higher AUDIO frequencies, which in LSB/CWL means LOWER RF. Measured semantics
// (set/read-back verified live): *RMP is signed ASCII, *RMF never disturbs PBT.
// If the ear test says LSB drags move the wrong way, flip here — one line.
static int pbtRfSign(Mode m) {
    return (m == Mode::LSB || m == Mode::CWL) ? -1 : +1;
}

// Visual placement of the passband relative to the carrier: nominal filter
// position per mode, shifted by PBT (in RF space). The nominal audio low-cut
// offset isn't CAT-readable, so this can sit a couple hundred Hz off — harmless,
// because drags are delta-anchored to the radio's real state, never absolute.
static void edgesFromRig(Mode m, int bw, int pbt, int& lo, int& hi) {
    int centerRf;
    switch (m) {
        case Mode::USB: centerRf = +bw / 2; break;
        case Mode::LSB: centerRf = -bw / 2; break;
        default:        centerRf = 0;       break;
    }
    centerRf += pbtRfSign(m) * pbt;
    lo = centerRf - bw / 2;
    hi = centerRf + bw / 2;
}

void MainWindow::refreshPassbandOverlay() {
    // Don't snap the overlay out from under the user right after they dragged it.
    if (sinceFilterEdit_.isValid() && sinceFilterEdit_.elapsed() < 2000) return;
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
    pan_->setPassband(lo, hi);
}

void MainWindow::onPassbandChanged(int loHz, int hiHz) {
    // Delta from the drag anchor, applied to the radio's real anchored state.
    const int dWidth  = (hiHz - loHz) - (anchorHiHz_ - anchorLoHz_);
    const int dCenter = ((hiHz + loHz) - (anchorHiHz_ + anchorLoHz_)) / 2;
    pendBwHz_  = std::clamp(anchorBwHz_ + dWidth, 100, 6000);
    pendPbtHz_ = std::clamp(anchorPbtHz_ + pbtRfSign(rigMode_) * dCenter, -8000, 8000);
    filterDirty_ = true;
    sinceFilterEdit_.restart();
    if (!filterTx_->isActive()) filterTx_->start();  // coalesce to ~25 writes/sec
    statusBar()->showMessage(QString("filter -> bw %1 Hz  pbt %2 Hz")
                                 .arg(pendBwHz_).arg(pendPbtHz_));
}

void MainWindow::sendPendingFilter() {
    if (!filterDirty_) return;
    filterDirty_ = false;
    radio_.setBandwidthHz(Rx::Main, pendBwHz_);     // -> *RMF
    radio_.setPbtHz(Rx::Main, pendPbtHz_);          // -> *RMP
    rigBwHz_  = pendBwHz_;                          // optimistic; poll will confirm
    rigPbtHz_ = pendPbtHz_;
    rigctld_.cacheBandwidth(pendBwHz_);
}

} // namespace ttc
