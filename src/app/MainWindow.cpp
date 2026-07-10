// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"
#include "app/Bands.h"

#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ttc {

static int pbtRfSign(Mode m);   // defined below with the passband math

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Ten-Tec SDR Console");

    pan_ = new PanadapterWidget(this);
    pan_->setPassband(-1200, 1200);
    smeter_ = new SMeterWidget(this);
    panel_  = new ControlPanel(this);
    freqDisp_ = new FrequencyDisplay(this);
    freqDisp_->setFrequency(centerHz_);
    curBand_ = bandIndexOf(centerHz_);
    if (curBand_ >= 0) {
        QSettings s;
        curReg_ = std::clamp(
            s.value(QString("band/%1/reg").arg(kBands[curBand_].label), 0).toInt(),
            0, kStackCount - 1);
    }
    panel_->showBand(curBand_);
    panel_->showBandStack(curBand_ >= 0 ? curReg_ : -1);

    // S-meter above the panadapter, control sidebar on the right.
    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(0);
    // The meter is compact and fixed-size; park it left in a dark strip so the
    // rest of the row doesn't show the default window color.
    auto* topStrip = new QWidget(this);
    topStrip->setAutoFillBackground(true);
    QPalette strip = topStrip->palette();
    strip.setColor(QPalette::Window, QColor(12, 16, 22));
    topStrip->setPalette(strip);
    auto* topLay = new QHBoxLayout(topStrip);
    topLay->setContentsMargins(0, 0, 10, 0);
    topLay->addWidget(smeter_);
    // Zoom slider (log scale, 0 = full span .. 100 = deepest zoom); Ctrl+wheel
    // on the panadapter still works and keeps the slider in sync.
    auto* zoomLbl = new QLabel("ZOOM", topStrip);
    zoomLbl->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    auto* zoom = new QSlider(Qt::Horizontal, topStrip);
    zoom->setRange(0, 100);
    zoom->setFixedWidth(140);
    zoom->setStyleSheet(
        "QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;"
        " background: #6aa5d8; }");
    topLay->addSpacing(16);
    topLay->addWidget(zoomLbl);
    topLay->addWidget(zoom);
    topLay->addStretch(1);
    topLay->addWidget(freqDisp_);

    auto spanFromSlider = [this](int v) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->fullSpanHz();
        return static_cast<int>(std::lround(pan_->fullSpanHz() * std::pow(ratio, v / 100.0)));
    };
    auto sliderFromSpan = [this](int spanHz) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->fullSpanHz();
        const double f = std::log(double(spanHz) / pan_->fullSpanHz()) / std::log(ratio);
        return std::clamp(static_cast<int>(std::lround(f * 100.0)), 0, 100);
    };
    connect(zoom, &QSlider::valueChanged, this, [this, spanFromSlider](int v) {
        pan_->setViewSpanHz(spanFromSlider(v));
        statusBar()->showMessage(
            QString("zoom -> span %1 kHz").arg(pan_->viewSpanHz() / 1000.0, 0, 'f', 1));
    });
    connect(pan_, &PanadapterWidget::viewSpanChanged, this,
            [zoom, sliderFromSpan](int spanHz) {
                QSignalBlocker block(zoom);
                zoom->setValue(sliderFromSpan(spanHz));
            });
    left->addWidget(topStrip);
    left->addWidget(pan_, 1);
    lay->addLayout(left, 1);
    lay->addWidget(panel_);
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
    notchTx_ = new QTimer(this);
    notchTx_->setSingleShot(true);
    notchTx_->setInterval(40);
    connect(notchTx_, &QTimer::timeout, this, &MainWindow::sendPendingNotch);

    // Radio state -> mode-sided passband overlay (LSB hangs below the carrier).
    connect(&radio_, &TenTecOrion::modeReported, this, [this](Rx rx, Mode m) {
        if (rx != Rx::Main) return;
        rigctld_.cacheMode(m);                      // clients always see true mode
        panel_->showMode(m);                        // sidebar mirrors the front panel
        if (m != rigMode_) {
            rigMode_ = m;
            refreshPassbandOverlay();
            refreshNotchOverlay();                  // marker side flips with sideband
        }
    });

    // Manual notch: drag the orange marker to move it, wheel over it for width,
    // sidebar button to engage. All audio<->RF mapping happens here.
    connect(pan_, &PanadapterWidget::notchDragged, this, [this](int rfOffsetHz) {
        const int audio = std::clamp(pbtRfSign(rigMode_) * rfOffsetHz, 20, 4000);
        notchCenter_ = audio;
        panel_->showNotch(notchOn_, notchCenter_, notchWidth_);
        sinceNotchEdit_.restart();
        notchDirty_ = true;
        pendNotchHz_ = audio;
        if (!notchTx_->isActive()) notchTx_->start();  // coalesce like the filter drag
    });
    connect(pan_, &PanadapterWidget::notchWidthAdjustRequested, this, [this](int steps) {
        notchWidth_ = std::clamp(notchWidth_ + steps * 10, 10, 300);
        radio_.setNotchWidth(Rx::Main, notchWidth_);
        sinceNotchEdit_.restart();
        refreshNotchOverlay();
        panel_->showNotch(notchOn_, notchCenter_, notchWidth_);
        statusBar()->showMessage(QString("notch width -> %1 Hz").arg(notchWidth_));
    });
    connect(panel_, &ControlPanel::notchToggled, this, [this](bool on) {
        radio_.setNotchEngaged(Rx::Main, on);
        notchOn_ = on;
        refreshNotchOverlay();
        panel_->showNotch(on, notchCenter_, notchWidth_);
    });
    connect(panel_, &ControlPanel::hwNbToggled, this,
            [this](bool on) { radio_.setHardwareNb(Rx::Main, on); });
    connect(&radio_, &TenTecOrion::notchCenterReported, this, [this](Rx rx, int hz) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchCenter_ = hz;
        refreshNotchOverlay();
        panel_->showNotch(notchOn_, notchCenter_, notchWidth_);
    });
    connect(&radio_, &TenTecOrion::notchWidthReported, this, [this](Rx rx, int hz) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchWidth_ = hz;
        refreshNotchOverlay();
        panel_->showNotch(notchOn_, notchCenter_, notchWidth_);
    });
    connect(&radio_, &TenTecOrion::notchEngagedReported, this, [this](Rx rx, bool on) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchOn_ = on;
        refreshNotchOverlay();
        panel_->showNotch(on, notchCenter_, notchWidth_);
    });
    connect(&radio_, &TenTecOrion::hardwareNbReported, this, [this](Rx rx, bool on) {
        if (rx == Rx::Main) panel_->showHwNb(on);
    });

    // Radio state -> control surface (front-panel changes reflect on screen).
    connect(&radio_, &TenTecOrion::sMeterReported, this, [this](int mainRaw, int) {
        if (std::getenv("TTC_SELFTEST")) {
            static int n = 0;
            if (++n % 4 == 1) std::fprintf(stderr, "[radio] S-meter main raw %d\n", mainRaw);
        }
        smeter_->setRawLevel(mainRaw);
    });
    connect(&radio_, &TenTecOrion::agcReported, this, [this](Rx rx, char a) {
        if (rx == Rx::Main) panel_->showAgc(a);
    });
    connect(&radio_, &TenTecOrion::rfGainReported, this, [this](Rx rx, int g) {
        if (rx == Rx::Main) panel_->showRfGain(g);
    });
    connect(&radio_, &TenTecOrion::attenReported, this, [this](Rx rx, int s) {
        if (rx == Rx::Main) panel_->showAtten(s);
    });
    connect(&radio_, &TenTecOrion::nrReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showNr(l);
    });
    connect(&radio_, &TenTecOrion::nbReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showNb(l);
    });
    connect(&radio_, &TenTecOrion::autoNotchReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showAutoNotch(l);
    });

    // Control surface -> radio. Sets are fire-and-forget; polling confirms.
    connect(panel_, &ControlPanel::modeSelected, this,
            [this](Mode m) { applyMode(m); });

    // Band buttons with Orion-style stack registers: a fresh band recalls its
    // last-used register; clicking the active band again cycles A->B->C->D.
    connect(panel_, &ControlPanel::bandSelected, this, [this](int idx) {
        if (idx < 0 || idx >= kBandCount) return;
        QSettings s;
        int reg;
        if (idx == curBand_) {
            reg = (curReg_ + 1) % kStackCount;      // same band: next register
        } else {
            reg = s.value(QString("band/%1/reg").arg(kBands[idx].label), 0).toInt();
            reg = std::clamp(reg, 0, kStackCount - 1);
        }
        saveBandMemory();                           // stash the outgoing register
        recallStack(idx, reg);
    });

    // Frequency readout edits (digit wheel/click or type-in) tune the radio.
    connect(freqDisp_, &FrequencyDisplay::frequencyEdited, this,
            [this](uint64_t hz) { tuneAbsolute(hz); });
    connect(panel_, &ControlPanel::agcSelected, this,
            [this](char a) { radio_.setAgc(Rx::Main, a); });
    connect(panel_, &ControlPanel::attenSelected, this,
            [this](int s) { radio_.setAttenuator(Rx::Main, s); });
    connect(panel_, &ControlPanel::rfGainChanged, this,
            [this](int g) { radio_.setRfGain(Rx::Main, g); });
    connect(panel_, &ControlPanel::nrChanged, this,
            [this](int v) { radio_.setNoiseReduction(Rx::Main, v); });
    connect(panel_, &ControlPanel::nbChanged, this,
            [this](int v) { radio_.setNoiseBlanker(Rx::Main, v); });
    connect(panel_, &ControlPanel::autoNotchChanged, this,
            [this](int v) { radio_.setAutoNotch(Rx::Main, v); });
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
                    freqDisp_->setFrequency(hz);
                    curBand_ = bandIndexOf(hz);      // dial crossed a band edge?
                    panel_->showBand(curBand_);
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
        radio_.queryAgc(Rx::Main);                  // sync the control sidebar
        radio_.queryRfGain(Rx::Main);
        radio_.queryAttenuator(Rx::Main);
        radio_.queryDspLevels(Rx::Main);            // syncs NR/NB/AN/hw-NB sliders
        radio_.queryNotch(Rx::Main);                // syncs notch center/width/engage
        awaitingFreq_ = true;
        freqQueryAge_.start();
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            if (!radio_.connected()) return;
            ++pollTick_;
            // 1 s rotation at 200 ms/tick, sized to the Orion's ~100 ms UART
            // servicing cycle: 2 Hz S-meter, 2 Hz frequency, mode and filter
            // alternating at 0.5 Hz each. Every 5 s one S-meter slot is lent
            // to the AGC/RF-gain/attenuator round-robin so front-panel knob
            // changes eventually reflect in the sidebar.
            const int phase = pollTick_ % 5;
            if (phase == 1 || phase == 3) {
                if (pollTick_ % 25 == 3) {
                    switch ((pollTick_ / 25) % 4) {
                        case 0:  radio_.queryAgc(Rx::Main);        break;
                        case 1:  radio_.queryRfGain(Rx::Main);     break;
                        case 2:  radio_.queryAttenuator(Rx::Main); break;
                        default: radio_.queryNotch(Rx::Main);      break;
                    }
                } else {
                    radio_.querySMeter();
                }
                return;
            }
            if (phase == 2) {
                if ((pollTick_ / 5) % 2) radio_.queryMode(Rx::Main);
                else                     radio_.queryFilter(Rx::Main);
                return;
            }
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
    saveBandMemory();                            // remember this band across runs
#ifdef HAVE_SDRPLAY
    // Stop the SDRplay streaming thread (Uninit drains callbacks) BEFORE the
    // spectrum_/dsp members it references are destroyed. Without this, member
    // teardown order frees the FFT under a still-running callback (use-after-free).
    sdr_.stop();
#endif
}

void MainWindow::onTuneRequested(int offsetHz) {
    tuneAbsolute(centerHz_ + offsetHz);
}

void MainWindow::tuneAbsolute(uint64_t f) {
    f = std::clamp<uint64_t>(f, 100000, 60000000);
    radio_.setFrequencyHz(Rx::Main, f);
    rigctld_.cacheFrequency(f);
    centerHz_ = f;                              // recenter view on the new frequency
    pendingHz_ = f;
    sinceTune_.restart();                       // hold off dial-follow while it settles
    freqDisp_->setFrequency(f);
    curBand_ = bandIndexOf(f);
    panel_->showBand(curBand_);
#ifdef HAVE_SDRPLAY
    sdr_.setCenterFrequency(static_cast<double>(f));
#endif
    statusBar()->showMessage(QString("tune -> %1 MHz").arg(f / 1e6, 0, 'f', 6));
}

void MainWindow::applyMode(Mode m) {
    radio_.setMode(Rx::Main, m);
    rigMode_ = m;
    rigctld_.cacheMode(m);
    refreshPassbandOverlay();
    refreshNotchOverlay();
    // The Orion recalls its per-mode stored filter on a mode change — fetch it
    // as soon as the firmware has settled instead of waiting for the slow poll.
    QTimer::singleShot(400, this, [this] { radio_.queryFilter(Rx::Main); });
}

void MainWindow::saveBandMemory() {
    if (curBand_ < 0 || curBand_ >= kBandCount) return;
    QSettings s;
    const QString key = QString("band/%1/%2/")
                            .arg(kBands[curBand_].label).arg(kStackNames[curReg_]);
    s.setValue(key + "freq", QVariant::fromValue<qulonglong>(centerHz_));
    s.setValue(key + "mode", static_cast<int>(rigMode_));
    s.setValue(key + "bw",   rigBwHz_);
    s.setValue(key + "pbt",  rigPbtHz_);
    s.setValue(QString("band/%1/reg").arg(kBands[curBand_].label), curReg_);
}

void MainWindow::recallStack(int band, int reg) {
    const StackDef& seed = kBands[band].stack[reg];
    QSettings s;
    const QString key = QString("band/%1/%2/")
                            .arg(kBands[band].label).arg(kStackNames[reg]);
    const uint64_t f =
        s.value(key + "freq", QVariant::fromValue<qulonglong>(seed.hz)).toULongLong();
    const Mode m = static_cast<Mode>(
        s.value(key + "mode", static_cast<int>(seed.mode)).toInt());
    const int bw  = std::clamp(s.value(key + "bw",  seed.bwHz).toInt(), 100, 6000);
    const int pbt = std::clamp(s.value(key + "pbt", seed.pbtHz).toInt(), -8000, 8000);

    curReg_ = reg;                              // before tuneAbsolute sets curBand_
    applyMode(m);
    panel_->showMode(m);
    tuneAbsolute(f);
    // Override the Orion's own per-mode filter recall with this register's
    // stored filter. Serial commands land in order, so the *RMF/*RMP sent
    // after *RMM win; the 400 ms filter re-query then confirms them.
    radio_.setBandwidthHz(Rx::Main, bw);
    radio_.setPbtHz(Rx::Main, pbt);
    rigBwHz_  = bw;
    rigPbtHz_ = pbt;
    rigctld_.cacheBandwidth(bw);
    refreshPassbandOverlay();
    panel_->showBandStack(reg);
    s.setValue(QString("band/%1/reg").arg(kBands[band].label), reg);
    statusBar()->showMessage(QString("band %1m stack %2  ->  %3 MHz")
                                 .arg(kBands[band].label).arg(kStackNames[reg])
                                 .arg(f / 1e6, 0, 'f', 4));
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

// The notch is an audio-domain DSP filter: an RF signal at offset d from the
// carrier demodulates to audio |d| regardless of PBT (PBT moves the passband
// filter, not the BFO). So marker RF offset = sideband sign x audio center.
void MainWindow::refreshNotchOverlay() {
    pan_->setNotch(notchOn_, pbtRfSign(rigMode_) * notchCenter_, notchWidth_);
}

void MainWindow::sendPendingNotch() {
    if (!notchDirty_) return;
    notchDirty_ = false;
    radio_.setNotchCenter(Rx::Main, pendNotchHz_);  // -> *RMNC (clamped in driver)
    statusBar()->showMessage(QString("notch -> %1 Hz x %2 Hz")
                                 .arg(pendNotchHz_).arg(notchWidth_));
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
