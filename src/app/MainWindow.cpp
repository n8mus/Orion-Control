// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"
#include "app/Bands.h"

#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QWidget>
#include <QTimer>
#include <QToolButton>
#include <QMenu>
#include <QActionGroup>
#include <QWidgetAction>
#include "ui/DisplayPanel.h"
#include "net/SpotClient.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ttc {

static int pbtRfSign(Mode m);   // defined below with the passband math

// Offset-LO tuning (PowerSDR if_freq): the SDR always captures this far above
// the dial so its zero-IF DC artifact can never sit on the tuned frequency.
static constexpr int kLoOffsetHz = 60000;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Ten-Tec SDR Console");

    pan_ = new PanadapterWidget(this);
    pan_->setPassband(-1200, 1200);
    pan_->setCenterHz(centerHz_);                  // grid labels need the dial freq
    smeter_ = new SMeterWidget(this);
    panel_  = new ControlPanel(this);
    // Readout colors follow the TX role (matching the routing buttons and
    // panadapter flags): red = the transmitting VFO, green = the other one.
    const QColor vfoTxRed(230, 95, 95), vfoRxGreen(70, 214, 125);
    freqDisp_ = new FrequencyDisplay("VFO A", vfoTxRed, this);
    freqDisp_->setFrequency(centerHz_);            // TX starts on A
    freqDispB_ = new FrequencyDisplay("VFO B", vfoRxGreen, this);
    freqDispB_->setFrequency(7000000);             // real value polled at startup
    auto applyVfoColors = [this, vfoTxRed, vfoRxGreen] {
        freqDisp_->setAccent(txVfo_ == 'B' ? vfoRxGreen : vfoTxRed);
        freqDispB_->setAccent(txVfo_ == 'B' ? vfoTxRed : vfoRxGreen);
    };
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
    // KE9NS arrangement: meter + zoom on the left, then VFO A, the routing
    // columns (antennas, A/B transfers, VFO assignment — Orion front-panel
    // style, vertical) and VFO B. Taller than the readouts need so more
    // controls can join later.
    topStrip->setMinimumHeight(104);
    auto* topLay = new QHBoxLayout(topStrip);
    topLay->setContentsMargins(10, 4, 10, 4);
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
    topLay->addSpacing(16);
    // Each VFO is a column: readout on top, its volume + mute right below
    // (KE9NS-style rounded buttons, matching the sidebar controls).
    auto makeVolRow = [&](int i) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        volSl_[i] = new QSlider(Qt::Horizontal, topStrip);
        volSl_[i]->setRange(0, 100);
        volSl_[i]->setFixedWidth(128);
        volSl_[i]->setFocusPolicy(Qt::NoFocus);
        volSl_[i]->setStyleSheet(
            "QSlider::groove:horizontal { height: 5px; background: #2a3644; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px;"
            " background: #6aa5d8; }");
        volLbl_[i] = new QLabel("--", topStrip);
        volLbl_[i]->setFixedWidth(22);
        volLbl_[i]->setStyleSheet("color: #8fa3b8; font-size: 11px; font-weight: bold;");
        muteBtn_[i] = new QToolButton(topStrip);
        muteBtn_[i]->setText("MUTE");
        muteBtn_[i]->setCheckable(true);
        muteBtn_[i]->setFocusPolicy(Qt::NoFocus);
        muteBtn_[i]->setFixedHeight(19);
        muteBtn_[i]->setStyleSheet(
            "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
            " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
            " padding: 0 8px; }"
            "QToolButton:hover { border-color: #4a5a6e; }"
            "QToolButton:checked { background: #8a2727; border-color: #e05d5d;"
            " color: #ffecec; }");
        row->addSpacing(4);
        row->addWidget(volSl_[i]);
        row->addWidget(volLbl_[i]);
        row->addWidget(muteBtn_[i]);
        row->addStretch(1);
        return row;
    };
    auto* colA = new QVBoxLayout;
    colA->setSpacing(3);
    colA->addWidget(freqDisp_);
    colA->addLayout(makeVolRow(0));
    topLay->addLayout(colA);
    topLay->addStretch(1);
    routing_ = new RoutingPanel(topStrip);
    topLay->addWidget(routing_);
    topLay->addStretch(1);
    auto* colB = new QVBoxLayout;
    colB->setSpacing(3);
    colB->addWidget(freqDispB_);
    colB->addLayout(makeVolRow(1));
    topLay->addLayout(colB);
    topLay->addStretch(1);

    auto spanFromSlider = [this](int v) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->maxViewSpanHz();
        return static_cast<int>(std::lround(pan_->maxViewSpanHz() * std::pow(ratio, v / 100.0)));
    };
    auto sliderFromSpan = [this](int spanHz) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->maxViewSpanHz();
        const double f = std::log(double(spanHz) / pan_->maxViewSpanHz()) / std::log(ratio);
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

    // All toolbar dropdowns share the console's dark look — without this the
    // QMenu frame and plain actions render in the system (light) theme.
    const auto styleMenu = [](QMenu* m) {
        m->setAttribute(Qt::WA_TranslucentBackground);  // real rounded corners
        m->setStyleSheet(
            "QMenu { background-color: #141b24; color: #c8d4e0;"
            " border: 1px solid #2a3644; border-radius: 6px; padding: 6px;"
            " font-size: 13px; }"
            "QMenu::item { background: transparent; padding: 6px 16px;"
            " border-radius: 4px; }"
            "QMenu::item:selected { background: #2a3644; }"
            "QMenu::item:disabled { color: #5a6b7d; }"
            "QMenu::separator { height: 1px; background: #2a3644; margin: 6px 8px; }"
            "QMenu::indicator { width: 14px; height: 14px; }"
            "QMenu::indicator:checked { background: #3f7cb4;"
            " border: 1px solid #5db2f0; border-radius: 3px; }"
            "QMenu::indicator:unchecked { background: #1c2430;"
            " border: 1px solid #2a3644; border-radius: 3px; }");
    };

    // "DISPLAY" dropdown: Flex/KE9NS-style viewer settings (ref level, range,
    // averaging, waterfall speed, palette, fill, peak hold), applied live and
    // persisted. Sits in a QWidgetAction so the popup stays open while dragging.
    auto* dispBtn = new QToolButton(topStrip);
    dispBtn->setText("DISPLAY ▾");
    dispBtn->setPopupMode(QToolButton::InstantPopup);
    dispBtn->setFocusPolicy(Qt::NoFocus);
    dispBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* dispMenu = new QMenu(dispBtn);
    styleMenu(dispMenu);
    auto* dispPanel = new DisplayPanel(dispMenu);
    auto* dispAction = new QWidgetAction(dispMenu);
    dispAction->setDefaultWidget(dispPanel);
    dispMenu->addAction(dispAction);
    dispBtn->setMenu(dispMenu);
    topLay->addSpacing(8);
    topLay->addWidget(dispBtn);
    auto saveDisplay = [](const DisplaySettings& d) {
        QSettings s;
        s.setValue("display/refDb",   d.refDb);
        s.setValue("display/rangeDb", d.rangeDb);
        s.setValue("display/palette", d.palette);
        s.setValue("display/avg",     d.avgFrames);
        s.setValue("display/wfSpeed", d.wfSpeed);
        s.setValue("display/fill",    d.fillTrace);
        s.setValue("display/peak",    d.peakHold);
        s.setValue("display/split",   d.split);
        s.setValue("display/background", d.background);
        s.setValue("display/grid",       d.showGrid);
        s.setValue("display/callsign",   d.showCall);
    };
    // Station callsign: drives the watermark and defaults the cluster login.
    // Editable in the DISPLAY panel (CALL field), persisted immediately.
    QString stationCall;
    {
        QSettings s;
        stationCall = s.value("station/callsign", "N8EM").toString();
    }
    dispPanel->setCallsign(stationCall);
    pan_->setCallsign(stationCall);
    connect(dispPanel, &DisplayPanel::callsignChanged, this,
            [this](const QString& call) {
                QSettings().setValue("station/callsign", call);
                pan_->setCallsign(call);
            });
    {   // restore persisted display settings before first paint
        QSettings s;
        DisplaySettings d;
        d.refDb     = s.value("display/refDb",   d.refDb).toFloat();
        d.rangeDb   = s.value("display/rangeDb", d.rangeDb).toFloat();
        d.palette   = s.value("display/palette", d.palette).toInt();
        d.avgFrames = s.value("display/avg",     d.avgFrames).toInt();
        d.wfSpeed   = s.value("display/wfSpeed", d.wfSpeed).toInt();
        d.fillTrace = s.value("display/fill",    d.fillTrace).toBool();
        d.peakHold  = s.value("display/peak",    d.peakHold).toBool();
        d.split     = s.value("display/split",   d.split).toFloat();
        d.background = s.value("display/background", d.background).toInt();
        d.showGrid   = s.value("display/grid",       d.showGrid).toBool();
        d.showCall   = s.value("display/callsign",   d.showCall).toBool();
        dispPanel->setSettings(d);
        pan_->setDisplaySettings(d);
    }
    connect(dispPanel, &DisplayPanel::settingsChanged, this,
            [this, saveDisplay](const DisplaySettings& d) {
                pan_->setDisplaySettings(d);
                saveDisplay(d);
            });
    // In-widget edits (dB-axis drag/wheel, divider drag) flow back the other
    // way: persist them and keep the DISPLAY panel's sliders in sync.
    connect(pan_, &PanadapterWidget::displaySettingsEdited, this,
            [this, dispPanel, saveDisplay](const DisplaySettings& d) {
                dispPanel->setSettings(d);
                saveDisplay(d);
                // In-widget edits are easy to make without noticing — say so.
                statusBar()->showMessage(
                    QString("display: REF %1 dB  RANGE %2 dB")
                        .arg(d.refDb, 0, 'f', 0).arg(d.rangeDb, 0, 'f', 0));
            });

    // "SPOTS" dropdown: DX-cluster callsign labels on the panadapter. The feed
    // is a plain cluster telnet login (default VE7CC); host/port/login live in
    // QSettings (spots/host, spots/port, spots/login) for other nodes or RBN.
    auto* spotsBtn = new QToolButton(topStrip);
    spotsBtn->setText("SPOTS ▾");
    spotsBtn->setPopupMode(QToolButton::InstantPopup);
    spotsBtn->setFocusPolicy(Qt::NoFocus);
    spotsBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* spotsMenu = new QMenu(spotsBtn);
    styleMenu(spotsMenu);
    auto* spotsOn   = spotsMenu->addAction("Show spots");
    spotsOn->setCheckable(true);
    auto* spotsClear = spotsMenu->addAction("Clear spots");
    spotsMenu->addSeparator();
    {
        QSettings s;
        const QString host  = s.value("spots/host", "dxc.ve7cc.net").toString();
        const quint16 port  = static_cast<quint16>(s.value("spots/port", 23).toUInt());
        const QString login = s.value("spots/login", stationCall).toString();
        auto* info = spotsMenu->addAction(QString("source: %1:%2 as %3")
                                              .arg(host).arg(port).arg(login));
        info->setEnabled(false);
        spotClient_.configure(host, port, login);
        spotsOn->setChecked(s.value("spots/enabled", true).toBool());
    }
    spotsBtn->setMenu(spotsMenu);
    topLay->addSpacing(8);
    topLay->addWidget(spotsBtn);

    // "AUDIO" dropdown: per-receiver volume + mute and the Orion's output
    // routing (*UC) — including one-VFO-per-ear for split pileups.
    auto* audioBtn = new QToolButton(topStrip);
    audioBtn->setText("AUDIO ▾");
    audioBtn->setPopupMode(QToolButton::InstantPopup);
    audioBtn->setFocusPolicy(Qt::NoFocus);
    audioBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* audioMenu = new QMenu(audioBtn);
    styleMenu(audioMenu);
    audioPanel_ = new AudioPanel(audioMenu);
    auto* audioAction = new QWidgetAction(audioMenu);
    audioAction->setDefaultWidget(audioPanel_);
    audioMenu->addAction(audioAction);
    audioBtn->setMenu(audioMenu);
    topLay->addSpacing(8);
    topLay->addWidget(audioBtn);

    // Per-VFO volume + mute (the rows under the readouts). Sliding a muted
    // receiver's volume implicitly unmutes it; mute remembers the level.
    for (int i = 0; i < 2; ++i) {
        const Rx rx = i == 0 ? Rx::Main : Rx::Sub;
        volTx_[i] = new QTimer(this);
        volTx_[i]->setSingleShot(true);
        volTx_[i]->setInterval(60);
        connect(volTx_[i], &QTimer::timeout, this,
                [this, i, rx] { radio_.setAfVolume(rx, pendVol_[i]); });
        connect(volSl_[i], &QSlider::valueChanged, this, [this, i, rx](int v) {
            volLbl_[i]->setText(QString::number(v));
            vol_[i] = v;
            if (muted_[i]) {                        // touch = unmute
                muted_[i] = false;
                const QSignalBlocker b(muteBtn_[i]);
                muteBtn_[i]->setChecked(false);
            }
            pendVol_[i] = v;
            volTx_[i]->start();
            if (i == 0 && txBar_) txBar_->showAfVolume(v);
            statusBar()->showMessage(QString("volume %1 -> %2")
                                         .arg(i == 0 ? "A" : "B").arg(v));
        });
        connect(muteBtn_[i], &QToolButton::toggled, this, [this, i, rx](bool on) {
            muted_[i] = on;
            if (on) {
                preMute_[i] = vol_[i];
                radio_.setAfVolume(rx, 0);
            } else {
                radio_.setAfVolume(rx, preMute_[i]);
                vol_[i] = preMute_[i];
                const QSignalBlocker b(volSl_[i]);
                volSl_[i]->setValue(preMute_[i]);
                volLbl_[i]->setText(QString::number(preMute_[i]));
                if (i == 0 && txBar_) txBar_->showAfVolume(preMute_[i]);
            }
            statusBar()->showMessage(QString("VFO %1 audio %2")
                                         .arg(i == 0 ? "A" : "B")
                                         .arg(on ? "MUTED" : "unmuted"));
        });
    }
    connect(audioPanel_, &AudioPanel::routingEdited, this,
            [this](char l, char r, char s) {
                radio_.setAudioRouting(l, r, s);
                statusBar()->showMessage(
                    QString("audio: phones L=%1 R=%2  speaker=%3").arg(l).arg(r).arg(s));
            });
    connect(&radio_, &TenTecOrion::audioRoutingReported, this,
            [this](char l, char r, char s) { audioPanel_->showRouting(l, r, s); });

    auto pushSpots = [this, spotsOn] {
        QVector<SpotLabel> labels;
        if (spotsOn->isChecked())
            for (const Spot& s : spotClient_.spots())
                labels.push_back({s.call, s.hz});
        pan_->setSpots(labels);
    };
    connect(&spotClient_, &SpotClient::spotsChanged, this, pushSpots);
    connect(&spotClient_, &SpotClient::statusChanged, this,
            [this](const QString& s) { statusBar()->showMessage(s, 4000); });
    connect(spotsOn, &QAction::toggled, this, [this, pushSpots](bool on) {
        QSettings().setValue("spots/enabled", on);
        spotClient_.setEnabled(on);
        pushSpots();
    });
    connect(spotsClear, &QAction::triggered, this, [this] { spotClient_.clear(); });
    spotClient_.setEnabled(spotsOn->isChecked());

#ifdef HAVE_SDRPLAY
    // Compact "SDR" dropdown for RSP2 hardware controls (hidden until clicked).
    // Antenna A = Orion spare jack, B = Omni VII spare jack — switch it when you
    // switch radios; it retunes the panadapter input live.
    auto* sdrBtn = new QToolButton(topStrip);
    sdrBtn->setText("SDR ▾");
    sdrBtn->setPopupMode(QToolButton::InstantPopup);
    sdrBtn->setFocusPolicy(Qt::NoFocus);
    sdrBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* sdrMenu = new QMenu(sdrBtn);
    styleMenu(sdrMenu);
    auto* antGroup = new QActionGroup(this);
    antGroup->setExclusive(true);
    auto* antA = antGroup->addAction(sdrMenu->addAction("Antenna A  (Orion)"));
    auto* antB = antGroup->addAction(sdrMenu->addAction("Antenna B  (Omni VII)"));
    antA->setCheckable(true);
    antB->setCheckable(true);
    sdrMenu->addSeparator();
    auto* bcNotch = sdrMenu->addAction("Broadcast (MW) notch");
    bcNotch->setCheckable(true);
    sdrMenu->addSeparator();

    // Front-end gain, live-adjustable: too much gain = ADC overload warnings
    // and a taller DC spike; too little buries weak signals. IF slider shows
    // gain (right = hotter, mapped to the API's 59..20 dB reduction); LNA is
    // the coarse attenuator (0 = max gain .. 8 = max attenuation).
    auto* gainW = new QWidget(sdrMenu);
    gainW->setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLabel[cap=\"1\"] { color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 3px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }");
    auto* gg = new QGridLayout(gainW);
    gg->setContentsMargins(14, 10, 14, 10);
    auto* ifCap = new QLabel("IF GAIN", gainW);
    ifCap->setProperty("cap", "1");
    gg->addWidget(ifCap, 0, 0);
    auto* ifGain = new QSlider(Qt::Horizontal, gainW);
    ifGain->setRange(20, 59);                       // stored as reduction dB
    ifGain->setInvertedAppearance(true);            // right = more gain
    ifGain->setFixedWidth(170);
    auto* ifVal = new QLabel(gainW);
    ifVal->setFixedWidth(48);
    gg->addWidget(ifGain, 0, 1);
    gg->addWidget(ifVal, 0, 2);
    auto* lnaCap = new QLabel("LNA", gainW);
    lnaCap->setProperty("cap", "1");
    gg->addWidget(lnaCap, 1, 0);
    auto* lna = new QComboBox(gainW);
    for (int i = 0; i <= 8; ++i)
        lna->addItem(QString("%1%2").arg(i).arg(i == 0 ? "  (max gain)"
                                              : i == 8 ? "  (max attn)" : ""));
    gg->addWidget(lna, 1, 1, 1, 2);
    auto* gainAction = new QWidgetAction(sdrMenu);
    gainAction->setDefaultWidget(gainW);
    sdrMenu->addAction(gainAction);

    sdrBtn->setMenu(sdrMenu);
    topLay->addSpacing(8);
    topLay->addWidget(sdrBtn);

    {   // restore persisted RSP2 state before start() applies it
        QSettings s;
        const bool useB = s.value("sdr/antennaB", false).toBool();
        const bool bcn  = s.value("sdr/broadcastNotch", false).toBool();
        const int  gr   = std::clamp(s.value("sdr/gRdB", 40).toInt(), 20, 59);
        const int  ls   = std::clamp(s.value("sdr/lna", 6).toInt(), 0, 8);
        (useB ? antB : antA)->setChecked(true);
        bcNotch->setChecked(bcn);
        sdr_.setAntennaB(useB);
        sdr_.setBroadcastNotch(bcn);
        sdr_.setGain(gr, ls);
        const QSignalBlocker b1(ifGain), b2(lna);
        ifGain->setValue(gr);
        lna->setCurrentIndex(ls);
        ifVal->setText(QString("-%1 dB").arg(gr));
    }
    auto applySdrGain = [this, ifGain, ifVal, lna] {
        const int gr = ifGain->value(), ls = lna->currentIndex();
        ifVal->setText(QString("-%1 dB").arg(gr));
        sdr_.setGainLive(gr, ls);
        QSettings s;
        s.setValue("sdr/gRdB", gr);
        s.setValue("sdr/lna", ls);
        statusBar()->showMessage(QString("SDR gain: IF -%1 dB  LNA state %2").arg(gr).arg(ls));
    };
    connect(ifGain, &QSlider::valueChanged, this, applySdrGain);
    connect(lna, &QComboBox::currentIndexChanged, this, applySdrGain);
    connect(antA, &QAction::triggered, this, [this] {
        sdr_.setAntennaB(false);
        QSettings().setValue("sdr/antennaB", false);
        statusBar()->showMessage("RSP2 antenna A (Orion)");
    });
    connect(antB, &QAction::triggered, this, [this] {
        sdr_.setAntennaB(true);
        QSettings().setValue("sdr/antennaB", true);
        statusBar()->showMessage("RSP2 antenna B (Omni VII)");
    });
    connect(bcNotch, &QAction::toggled, this, [this](bool on) {
        sdr_.setBroadcastNotch(on);
        QSettings().setValue("sdr/broadcastNotch", on);
    });
#endif
    left->addWidget(topStrip);
    left->addWidget(pan_, 1);
    txBar_ = new TxBar(this);
    {
        QSettings s;
        txBar_->setAmpMode(s.value("amp/enabled", false).toBool(),
                           s.value("amp/limit", 40).toInt());
    }
    left->addWidget(txBar_);
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
    // Wheel tuning: 100 Hz per notch, 10 Hz with Shift (Ctrl+wheel zooms);
    // in SAM the steps drop to 10/1 Hz for the carrier zero-beat. The wheel
    // follows whichever VFO was tuned last (right-click/drag on B hands it
    // to B; any A tune takes it back).
    connect(pan_, &PanadapterWidget::tuneStepRequested, this, [this](int steps, bool fine) {
        const int unit = samActive_ ? (fine ? 1 : 10) : (fine ? 10 : 100);
        onTuneRequested(steps * unit);
    });
    connect(pan_, &PanadapterWidget::vfoBStepRequested, this, [this](int steps, bool fine) {
        vfoBHz_ = static_cast<uint64_t>(
            static_cast<qint64>(vfoBHz_) + steps * (fine ? 10 : 100));
        freqDispB_->setFrequency(vfoBHz_);
        sinceVfoBEdit_.restart();
        pushVfoB();
        pendBHz_ = vfoBHz_;                        // coalesce like B drags
        bfDirty_ = true;
        if (!bfTx_->isActive()) bfTx_->start();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(vfoBHz_ / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
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
        if (rx == Rx::Sub) { subMode_ = m; return; }
        rigctld_.cacheMode(m);                      // clients always see true mode
        if (samActive_ && m == samEngine_)          // SAM's engine reporting in:
            panel_->clearModeSelection();           // keep SAM as the lit mode
        else
            panel_->showMode(m);                    // sidebar mirrors the front panel
        if (m != rigMode_) {
            rigMode_ = m;
            refreshPassbandOverlay();
            refreshNotchOverlay();                  // marker side flips with sideband
            if (bandStamp_) bandStamp_->start();    // keep the stack register current
            if (samActive_ && m != samEngine_) {    // front panel left SAM's mode
                samActive_ = false;
                panel_->showSam(false);
                panel_->setSamLabel("SAM");
            }
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
        rigctld_.cachePtt(false);                   // radio answered in receive
    });
    connect(&radio_, &TenTecOrion::txMeterReported, this,
            [this](double fwd, double ref, double swr) {
                smeter_->setTxLevel(fwd, ref, swr);
                rigctld_.cachePtt(true);            // radio is provably keyed
                // Amp protection, fast path: actual forward power well above
                // the drive limit while keyed -> command the power back down.
                if (txBar_->ampMode() && fwd > txBar_->ampLimit() * 1.15
                    && (!sinceEnforce_.isValid() || sinceEnforce_.elapsed() > 1000)) {
                    sinceEnforce_.restart();
                    radio_.setTxPower(txBar_->ampLimit());
                    txBar_->showTxPower(txBar_->ampLimit());
                    statusBar()->showMessage(
                        QString("AMP LIMIT: %1 W measured, drive forced to %2")
                            .arg(fwd, 0, 'f', 0).arg(txBar_->ampLimit()));
                }
            });

    // TX bar <-> radio.
    connect(txBar_, &TxBar::txPowerChanged, this,
            [this](int p) { radio_.setTxPower(p); });
    connect(txBar_, &TxBar::micGainChanged, this,
            [this](int p) { radio_.setMicGain(p); });
    connect(txBar_, &TxBar::monitorChanged, this,
            [this](int p) { radio_.setMonitor(p); });
    connect(txBar_, &TxBar::afVolumeChanged, this, [this](int p) {
        volSl_[0]->setValue(p);                    // TX-bar AF = main volume; the
    });                                            // slider handler does the rest
    connect(txBar_, &TxBar::tunerEnableToggled, this, [this](bool on) {
        tunerOn_ = on;
        radio_.setTunerEnabled(on);
    });
    tuneTimeout_ = new QTimer(this);
    tuneTimeout_->setSingleShot(true);
    tuneTimeout_->setInterval(15000);              // carrier never outlives 15 s
    connect(tuneTimeout_, &QTimer::timeout, this, &MainWindow::stopManualTune);
    connect(txBar_, &TxBar::tuneToggled, this, [this](bool on) {
        if (!on) { stopManualTune(); return; }
        if (tunerOn_) {
            // Internal tuner enabled: the radio runs its own cycle (*TTT
            // generates its own carrier and drops it when matched).
            radio_.startTune();
            statusBar()->showMessage("internal tuner: tune cycle started");
            txBar_->showTuneActive(false);         // momentary in this mode
            return;
        }
        startManualTune();
    });
    connect(txBar_, &TxBar::tuneLevelChanged, this, [](int w) {
        QSettings().setValue("tune/power", w);
    });
    txBar_->setTuneLevel(QSettings().value("tune/power", 20).toInt());

    // Digital/voice audio switch ([vendor]-style): line-in for digital, mic for voice.
    {
        QSettings s;
        lastMicGain_    = s.value("audio/voiceMic", 51).toInt();
        lastSpeechProc_ = s.value("audio/voiceSpeech", 2).toInt();
    }
    connect(panel_, &ControlPanel::digitalToggled, this,
            [this](bool on) { setDigitalMode(on); });
    connect(txBar_, &TxBar::ampModeChanged, this, [this](bool on, int limit) {
        QSettings s;
        s.setValue("amp/enabled", on);
        s.setValue("amp/limit", limit);
        if (on) radio_.queryTxPower();              // check the current drive now
        statusBar()->showMessage(on ? QString("amp mode: drive capped at %1 W").arg(limit)
                                    : "amp mode off");
    });
    connect(&radio_, &TenTecOrion::txPowerReported, this, [this](int p) {
        lastTxPwr_ = p;                             // for restore after manual tune
        if (tuning_) return;                        // don't fight the tune carrier
        // Amp protection, slow path: the periodic ?TP poll catches a front-
        // panel PWR change above the limit even when not transmitting.
        if (txBar_->ampMode() && p > txBar_->ampLimit()) {
            if (!sinceEnforce_.isValid() || sinceEnforce_.elapsed() > 1000) {
                sinceEnforce_.restart();
                radio_.setTxPower(txBar_->ampLimit());
                statusBar()->showMessage(
                    QString("AMP LIMIT: drive %1 forced back to %2 W")
                        .arg(p).arg(txBar_->ampLimit()));
            }
            txBar_->showTxPower(txBar_->ampLimit());
            return;
        }
        txBar_->showTxPower(p);
    });
    connect(&radio_, &TenTecOrion::micGainReported, this, [this](int p) {
        txBar_->showMicGain(p);
        if (!digital_) lastMicGain_ = p;            // learn the voice mic setting
    });
    connect(&radio_, &TenTecOrion::speechProcReported, this, [this](int l) {
        if (!digital_) lastSpeechProc_ = l;         // learn the voice speech-proc
    });
    connect(&radio_, &TenTecOrion::monitorReported, this,
            [this](int p) { txBar_->showMonitor(p); });
    connect(&radio_, &TenTecOrion::afVolumeReported, this, [this](Rx rx, int p) {
        const int i = rx == Rx::Main ? 0 : 1;
        if (muted_[i]) return;                     // muted: 0 expected, keep slider
        vol_[i] = p;
        const QSignalBlocker b(volSl_[i]);
        volSl_[i]->setValue(p);
        volLbl_[i]->setText(QString::number(p));
        if (i == 0) txBar_->showAfVolume(p);
    });
    connect(&radio_, &TenTecOrion::tunerReported, this, [this](bool on) {
        tunerOn_ = on;
        txBar_->showTuner(on);
    });

    // rigctld PTT (WSJT-X / fldigi keying through :4532). The 't' reply
    // tracks the radio's true T/R state via the metering replies.
    connect(&rigctld_, &RigctldServer::pttRequested, this,
            [this](bool on) { radio_.setPtt(on); });
    connect(&radio_, &TenTecOrion::agcReported, this, [this](Rx rx, char a) {
        if (rx == Rx::Main) panel_->showAgc(a);
    });
    connect(&radio_, &TenTecOrion::rfGainReported, this, [this](Rx rx, int g) {
        if (rx == Rx::Main) panel_->showRfGain(g);
    });
    connect(&radio_, &TenTecOrion::attenReported, this, [this](Rx rx, int s) {
        if (rx == Rx::Main) panel_->showAtten(s);
    });
    connect(&radio_, &TenTecOrion::preampReported, this, [this](Rx rx, bool on) {
        if (rx == Rx::Main) panel_->showPreamp(on);
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
    connect(panel_, &ControlPanel::modeSelected, this, [this](Mode m) {
        if (samActive_) {                          // picking a real mode ends SAM
            samActive_ = false;
            panel_->showSam(false);
            panel_->setSamLabel("SAM");
        }
        applyMode(m);
        panel_->showMode(m);
    });

    // SAM ([vendor]-style ECSS): the Orion has no true sync-AM, so receive the
    // AM signal in USB with the carrier zero-beaten instead. Engaging swaps
    // to USB + a wide filter; the panadapter wheel drops to 10 Hz (1 Hz with
    // Shift) for the careful zero-beat. Off restores the previous mode/filter.
    // Click cycle: off -> SAM-U -> SAM-L -> off. The sideband flip is the
    // real sync-AM listener's move — pick whichever side dodges the QRM.
    const auto samFilter = [this] {
        QTimer::singleShot(450, this, [this] {     // after the mode switch settles
            radio_.setBandwidthHz(Rx::Main, 4000); // AM-width audio
            radio_.setPbtHz(Rx::Main, 0);
        });
    };
    connect(panel_, &ControlPanel::samToggled, this, [this, samFilter](bool on) {
        if (on) {                                  // engage, upper sideband first
            samActive_ = true;
            samEngine_ = Mode::USB;
            preSamMode_ = rigMode_;
            preSamBw_   = rigBwHz_;
            applyMode(Mode::USB);                  // the ECSS engine, not the face
            panel_->clearModeSelection();          // SAM alone stays lit
            panel_->setSamLabel("SAM-U");
            samFilter();
            statusBar()->showMessage(
                "SAM (upper): zero-beat the carrier — wheel 10 Hz, Shift 1 Hz");
        } else if (samActive_ && samEngine_ == Mode::USB) {
            samEngine_ = Mode::LSB;                // second click: flip sideband
            panel_->showSam(true);                 // stay lit
            panel_->setSamLabel("SAM-L");
            applyMode(Mode::LSB);
            panel_->clearModeSelection();
            samFilter();                           // mode change recalls per-mode filter
            statusBar()->showMessage("SAM (lower sideband)");
        } else {                                   // third click: off
            samActive_ = false;
            panel_->setSamLabel("SAM");
            applyMode(preSamMode_);
            panel_->showMode(preSamMode_);
            QTimer::singleShot(450, this, [this] {
                radio_.setBandwidthHz(Rx::Main, preSamBw_);
            });
            statusBar()->showMessage("SAM off — previous mode restored");
        }
    });

    // Band buttons with Orion-style stack registers: a fresh band recalls its
    // last-used register; clicking the active band again cycles A->B->C->D.
    connect(panel_, &ControlPanel::bandSelected, this, [this](int idx) {
        if (idx < 0 || idx >= kBandCount) return;
        QSettings s;
        int reg;
        // Cycle A->B->C->D on repeated presses of the SAME button. Keyed off the
        // last button pressed (syncBandRegister clears it when the dial moves to
        // another band under us, so a stale press can't trigger a bogus cycle).
        if (idx == lastBandPress_) {
            reg = (curReg_ + 1) % kStackCount;      // same button again: next reg
        } else {
            reg = s.value(QString("band/%1/reg").arg(kBands[idx].label), 0).toInt();
            reg = std::clamp(reg, 0, kStackCount - 1);
        }
        saveBandMemory();                           // stash the outgoing register
        recallStack(idx, reg);
        lastBandPress_ = idx;                       // after recall (sync resets it)
    });

    // The Orion's CAT set has no way to read its internal band-stack registers,
    // so the console mirrors them instead: shortly after ANY dial/mode/filter
    // change settles (front-panel knob or band button, WSJT-X, click-to-tune),
    // stamp the state into the active register for that band. Our band buttons
    // then return to the same spot the radio's own band button would.
    bandStamp_ = new QTimer(this);
    bandStamp_->setSingleShot(true);
    bandStamp_->setInterval(1500);
    connect(bandStamp_, &QTimer::timeout, this, &MainWindow::saveBandMemory);

    // Frequency readout edits (digit wheel/click or type-in) tune the radio.
    connect(freqDisp_, &FrequencyDisplay::frequencyEdited, this,
            [this](uint64_t hz) { tuneAbsolute(hz); });
    // VFO B goes straight to the sub receiver's dial (*BF); the panadapter
    // stays on VFO A, whose IQ tap feeds it.
    connect(freqDispB_, &FrequencyDisplay::frequencyEdited, this,
            [this](uint64_t hz) {
                radio_.setFrequencyHz(Rx::Sub, hz);
                vfoBHz_ = hz;
                pushVfoB();
                sinceVfoBEdit_.restart();           // poll echo may still be stale
                statusBar()->showMessage(
                    QString("VFO B -> %1 MHz").arg(hz / 1e6, 0, 'f', 6));
            });
    pushVfoB();                                     // marker live from the start

    // Routing matrices + A/B dial transfers (the Orion front panel's VFO and
    // antenna button groups, driven over *KV / *KA).
    connect(routing_, &RoutingPanel::vfoAssignmentEdited, this,
            [this, applyVfoColors](char m, char s, char t) {
                radio_.setVfoAssignment(m, s, t);
                // [vendor] split setup (no separate split button): the moment a
                // click puts VFO B on TX or RX, park B above the dial — 5 kHz
                // in voice, 1 kHz in CW. The radio's own menu copies the mode
                // to the sub, so only the dial needs setting.
                const bool bEngaged = (t == 'B' && txVfo_ != 'B')
                                   || (m == 'B' && rxVfo_ != 'B');
                txVfo_ = t;
                rxVfo_ = m;
                if (bEngaged) {
                    const bool cw = rigMode_ == Mode::CWU || rigMode_ == Mode::CWL;
                    const uint64_t b = centerHz_ + (cw ? 1000 : 5000);
                    radio_.setFrequencyHz(Rx::Sub, b);
                    vfoBHz_ = b;
                    freqDispB_->setFrequency(b);
                    sinceVfoBEdit_.restart();
                }
                applyVfoColors();
                pushVfoB();
                statusBar()->showMessage(
                    QString("VFO routing: RX %1  SUB %2  TX %3%4")
                        .arg(m).arg(s).arg(t)
                        .arg(m != t ? "  (SPLIT)" : ""));
            });
    connect(routing_, &RoutingPanel::antennaRoutingEdited, this,
            [this](char a1, char a2, char rxa) {
                radio_.setAntennaRouting(a1, a2, rxa);
                statusBar()->showMessage(QString("antenna routing: ANT1=%1 ANT2=%2 RX=%3")
                                             .arg(a1).arg(a2).arg(rxa));
            });
    // Dial transfers carry MODE with the frequency (PowerSDR semantics: the
    // VFO is freq+mode). Mode commands trail the freq by 120+ ms — the Orion
    // drops commands that arrive while it's busy with a mode switch.
    connect(routing_, &RoutingPanel::copyABRequested, this, [this] {
        radio_.setFrequencyHz(Rx::Sub, centerHz_);
        vfoBHz_ = centerHz_;
        pushVfoB();
        freqDispB_->setFrequency(centerHz_);
        sinceVfoBEdit_.restart();
        // Sub mode: handled by the radio itself (menu set to copy the mode;
        // *RSM is dead on v3 firmware anyway).
        statusBar()->showMessage(QString("A>B: VFO B -> %1 MHz")
                                     .arg(centerHz_ / 1e6, 0, 'f', 6));
    });
    connect(routing_, &RoutingPanel::copyBARequested, this, [this] {
        tuneAbsolute(vfoBHz_);
        QTimer::singleShot(120, this, [this, m = subMode_] {
            applyMode(m);
            panel_->showMode(m);
        });
        statusBar()->showMessage(QString("B>A: VFO A -> %1 MHz")
                                     .arg(vfoBHz_ / 1e6, 0, 'f', 6));
    });
    connect(routing_, &RoutingPanel::swapABRequested, this, [this] {
        const uint64_t a = centerHz_, b = vfoBHz_;
        const Mode ma = rigMode_, mb = subMode_;
        radio_.setFrequencyHz(Rx::Sub, a);
        vfoBHz_ = a;
        pushVfoB();
        freqDispB_->setFrequency(a);
        sinceVfoBEdit_.restart();
        tuneAbsolute(b);
        if (mb != ma) {                              // bring B's mode to the main RX
            QTimer::singleShot(120, this, [this, mb] {
                applyMode(mb);
                panel_->showMode(mb);
            });
        }
        statusBar()->showMessage(QString("A<>B: %1 <-> %2 MHz")
                                     .arg(b / 1e6, 0, 'f', 6).arg(a / 1e6, 0, 'f', 6));
    });
    connect(&radio_, &TenTecOrion::vfoAssignmentReported, this,
            [this, applyVfoColors](char m, char s, char t) {
                routing_->showVfoAssignment(m, s, t);
                txVfo_ = t;
                rxVfo_ = m;
                applyVfoColors();
                pushVfoB();
            });
    connect(&radio_, &TenTecOrion::antennaRoutingReported, this,
            [this](char a1, char a2, char rxa) { routing_->showAntennaRouting(a1, a2, rxa); });
    if (std::getenv("TTC_TEST_XFER"))               // headless transfer exercise
        QTimer::singleShot(3000, routing_, &RoutingPanel::swapABRequested);
    connect(panel_, &ControlPanel::agcSelected, this,
            [this](char a) { radio_.setAgc(Rx::Main, a); });
    connect(panel_, &ControlPanel::agcThresholdChanged, this, [this](int uv) {
        radio_.setAgcThreshold(Rx::Main, uv);
        statusBar()->showMessage(QString("AGC threshold -> %1 µV").arg(uv));
        // Read back shortly: the radio quantizes, show its real value.
        QTimer::singleShot(300, this, [this] { radio_.queryAgcThreshold(Rx::Main); });
    });
    connect(&radio_, &TenTecOrion::agcThresholdReported, this,
            [this](Rx rx, double uv) {
                if (rx == Rx::Main) panel_->showAgcThreshold(uv);
            });
    connect(panel_, &ControlPanel::agcHangChanged, this, [this](int tenths) {
        radio_.setAgcHang(Rx::Main, tenths / 10.0);
        statusBar()->showMessage(tenths == 0
                                     ? QString("AGC hang -> off")
                                     : QString("AGC hang -> %1 s").arg(tenths / 10.0, 0, 'f', 1));
        QTimer::singleShot(300, this, [this] { radio_.queryAgcHang(Rx::Main); });
    });
    connect(&radio_, &TenTecOrion::agcHangReported, this,
            [this](Rx rx, double sec) {
                if (rx == Rx::Main) panel_->showAgcHang(sec);
            });
    connect(panel_, &ControlPanel::agcDecayChanged, this, [this](int rate) {
        radio_.setAgcDecay(Rx::Main, rate);
        statusBar()->showMessage(QString("AGC decay -> %1").arg(rate));
        QTimer::singleShot(300, this, [this] { radio_.queryAgcDecay(Rx::Main); });
    });
    connect(&radio_, &TenTecOrion::agcDecayReported, this,
            [this](Rx rx, int rate) {
                if (rx == Rx::Main) panel_->showAgcDecay(rate);
            });
    connect(panel_, &ControlPanel::attenSelected, this,
            [this](int s) { radio_.setAttenuator(Rx::Main, s); });
    connect(panel_, &ControlPanel::preampToggled, this,
            [this](bool on) { radio_.setPreamp(Rx::Main, on); });
    connect(panel_, &ControlPanel::rfGainChanged, this,
            [this](int g) { radio_.setRfGain(Rx::Main, g); });
    connect(panel_, &ControlPanel::nrChanged, this,
            [this](int v) { radio_.setNoiseReduction(Rx::Main, v); });
    connect(panel_, &ControlPanel::nbChanged, this,
            [this](int v) { radio_.setNoiseBlanker(Rx::Main, v); });
    connect(panel_, &ControlPanel::autoNotchChanged, this,
            [this](int v) { radio_.setAutoNotch(Rx::Main, v); });
    connect(&radio_, &TenTecOrion::bandwidthReported, this, [this](Rx rx, int bw) {
        if (rx == Rx::Sub) {
            if (bw != subBwHz_) { subBwHz_ = bw; pushVfoB(); }
            return;
        }
        rigctld_.cacheBandwidth(bw);
        if (bw != rigBwHz_) { rigBwHz_ = bw; refreshPassbandOverlay(); bandStamp_->start(); }
    });
    connect(&radio_, &TenTecOrion::pbtReported, this, [this](Rx rx, int pbt) {
        if (rx == Rx::Sub) {
            if (pbt != subPbtHz_) { subPbtHz_ = pbt; pushVfoB(); }
            return;
        }
        panel_->showPbt(pbt);
        if (pbt != rigPbtHz_) { rigPbtHz_ = pbt; refreshPassbandOverlay(); bandStamp_->start(); }
    });

    // Slide VFO B on the panadapter (grab its tint/line, or Shift+click to
    // drop it on a station) — the pileup "get my TX dead on the spot" moves.
    // Drags stream *BF coalesced to ~25 writes/sec like the filter drags.
    bfTx_ = new QTimer(this);
    bfTx_->setInterval(40);
    connect(bfTx_, &QTimer::timeout, this, [this] {
        if (!bfDirty_) { bfTx_->stop(); return; }
        bfDirty_ = false;
        radio_.setFrequencyHz(Rx::Sub, pendBHz_);
    });
    connect(pan_, &PanadapterWidget::vfoBDragged, this, [this](int off) {
        const uint64_t hz =
            static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        vfoBHz_ = hz;
        freqDispB_->setFrequency(hz);
        sinceVfoBEdit_.restart();
        pendBHz_ = hz;
        bfDirty_ = true;
        if (!bfTx_->isActive()) bfTx_->start();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(hz / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
    });
    // Drag the A dial line: stream full retunes (radio + SDR + view recenter),
    // coalesced a bit slower than B since each one moves the whole display.
    afTx_ = new QTimer(this);
    afTx_->setInterval(60);
    connect(afTx_, &QTimer::timeout, this, [this] {
        if (!afDirty_) { afTx_->stop(); return; }
        afDirty_ = false;
        tuneAbsolute(pendAHz_);
    });
    connect(pan_, &PanadapterWidget::vfoADragged, this, [this](uint64_t hz) {
        pendAHz_ = hz;
        afDirty_ = true;
        if (!afTx_->isActive()) afTx_->start();
        statusBar()->showMessage(QString("VFO A -> %1 MHz").arg(hz / 1e6, 0, 'f', 6));
    });
    connect(pan_, &PanadapterWidget::vfoBTuneRequested, this, [this](int off) {
        const uint64_t hz =
            static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        radio_.setFrequencyHz(Rx::Sub, hz);
        vfoBHz_ = hz;
        freqDispB_->setFrequency(hz);
        sinceVfoBEdit_.restart();
        pushVfoB();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(hz / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
    });

    // PBT back to the detent: sidebar button or double-click a passband edge.
    // Bandwidth and dial untouched — the filter just slides back to center.
    const auto zeroPbt = [this] {
        filterTx_->stop();                          // drop any stale pending PBT
        filterDirty_ = false;
        radio_.setPbtHz(Rx::Main, 0);
        rigPbtHz_ = 0;
        panel_->showPbt(0);
        sinceFilterEdit_.invalidate();              // explicit action: refresh now
        refreshPassbandOverlay();
        statusBar()->showMessage("PBT -> 0");
    };
    connect(panel_, &ControlPanel::pbtZeroRequested, this, zeroPbt);
    connect(pan_, &PanadapterWidget::pbtZeroRequested, this, zeroPbt);
    // Sidebar PBT slider: deliberate shifts (already coalesced in the panel).
    connect(panel_, &ControlPanel::pbtChanged, this, [this](int hz) {
        radio_.setPbtHz(Rx::Main, hz);
        rigPbtHz_ = hz;
        sinceFilterEdit_.invalidate();              // explicit action: redraw now
        refreshPassbandOverlay();
        sinceFilterEdit_.restart();                 // ...then guard the poll echo
        statusBar()->showMessage(QString("PBT -> %1%2 Hz")
                                     .arg(hz > 0 ? "+" : "").arg(hz));
    });

    // The radio reported its frequency (startup sync or dial-follow poll): keep the
    // rigctld cache and the panadapter center locked to the physical VFO.
    connect(&radio_, &TenTecOrion::frequencyReported, this,
            [this](Rx rx, uint64_t hz) {
                if (rx == Rx::Sub) {                 // VFO B: display-follow only
                    if (!sinceVfoBEdit_.isValid() || sinceVfoBEdit_.elapsed() > 1500) {
                        vfoBHz_ = hz;
                        freqDispB_->setFrequency(hz);
                        pushVfoB();
                    }
                    return;
                }
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
                    pan_->setCenterHz(hz);           // keep grid labels on the dial
                    syncBandRegister();              // mirror the move into the stack
#ifdef HAVE_SDRPLAY
                    sdr_.setCenterFrequency(static_cast<double>(hz + kLoOffsetHz));
#endif
                    statusBar()->showMessage(
                        QString("radio %1 MHz  |  panadapter following").arg(hz / 1e6, 0, 'f', 4));
                }
            });

    // Start the interop seam. Clients (cqrlog/WSJT-X/fldigi/GridTracker) connect
    // here. TTC_RIGCTLD_PORT overrides for side-by-side protocol testing.
    const char* rcpEnv = std::getenv("TTC_RIGCTLD_PORT");
    const bool listening = rigctld_.listen(rcpEnv ? quint16(atoi(rcpEnv)) : 4532);
    statusBar()->showMessage(
        listening ? "rigctld emulation on :4532  |  radio: not connected (wire /dev/orion)"
                  : "FAILED to bind :4532 (already in use? stop flrig/rigctld)");

#ifdef HAVE_SDRPLAY
    // IQ -> FFT -> panadapter. The spectrum callback runs on the SDR thread, so
    // marshal the result onto the GUI thread before touching the widget.
    // PowerSDR-style offset LO (their if_freq / spur reduction): the RSP2
    // tunes kLoOffsetHz ABOVE the dial, so the zero-IF DC artifact — line,
    // phase-noise hump and recalibration transients — sits at a fixed
    // +60 kHz from the tuned frequency, never on it and never in a passband.
    // The capture is widened to 500 kHz so the dial-centered view still
    // reaches large spans (max symmetric span = 500k - 2*60k = 380 kHz).
    constexpr double kSampleRate = 2000000.0;
    constexpr int    kDecim      = 4;                 // 500 kHz capture
    const int spanHz = static_cast<int>(kSampleRate / kDecim);
    pan_->setSpanHz(spanHz);
    pan_->setDialOffsetHz(kLoOffsetHz);
    spectrum_.setOutput([this](const std::vector<float>& db) {
        if (std::getenv("TTC_SELFTEST")) {          // headless evidence the FFT is live
            static int frames = 0;
            if (++frames % 20 == 1) {
                float mn = 1e9f, mx = -1e9f;
                for (float v : db) { mn = std::min(mn, v); mx = std::max(mx, v); }
                std::fprintf(stderr, "[spectrum] frame %d  bins=%zu  min %.1f dB  peak %.1f dB\n",
                             frames, db.size(), mn, mx);
                if (std::getenv("TTC_DCDUMP")) {    // profile the center hump
                    const int c = static_cast<int>(db.size()) / 2;
                    for (int o = -20; o <= 20; o += 2)
                        std::fprintf(stderr, "[dc] %+5d Hz  %.1f dB\n", o * 61, db[c + o]);
                }
            }
        }
        QMetaObject::invokeMethod(pan_, [this, copy = db]() mutable {
            pan_->setSpectrum(copy);
        }, Qt::QueuedConnection);
    });
    sdr_.setIqCallback([this](const IqBlock& iq) { spectrum_.addSamples(iq); });
    sdr_.setDecimation(kDecim);
    if (sdr_.apiOk()
        && sdr_.start(static_cast<double>(centerHz_ + kLoOffsetHz), kSampleRate)) {
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
        radio_.queryFrequency(Rx::Sub);             // VFO B readout
        radio_.queryVfoAssignment();                // routing matrices
        radio_.queryAntennaRouting();
        radio_.queryMode(Rx::Main);
        radio_.queryMode(Rx::Sub);                  // transfers carry the mode
        radio_.queryFilter(Rx::Sub);                // B's on-screen filter width
        radio_.queryFilter(Rx::Main);
        radio_.queryAgc(Rx::Main);                  // sync the control sidebar
        radio_.queryAgcThreshold(Rx::Main);
        radio_.queryAgcHang(Rx::Main);
        radio_.queryAgcDecay(Rx::Main);
        radio_.queryRfGain(Rx::Main);
        radio_.queryAttenuator(Rx::Main);
        radio_.queryPreamp(Rx::Main);
        radio_.queryDspLevels(Rx::Main);            // syncs NR/NB/AN/hw-NB sliders
        radio_.queryNotch(Rx::Main);                // syncs notch center/width/engage
        radio_.queryTxPower();                      // syncs the TX bar
        radio_.queryTxAudio();
        radio_.queryAfVolume(Rx::Sub);              // AUDIO panel (?UM in TxAudio)
        radio_.queryAudioRouting();
        // The startup burst can flood the Orion's UART servicing and drop a
        // few query responses — re-ask for the audio state once it settles.
        QTimer::singleShot(2500, this, [this] {
            radio_.queryAfVolume(Rx::Main);
            radio_.queryAfVolume(Rx::Sub);
            radio_.queryAudioRouting();
        });
        radio_.queryAuxInputGain();
        radio_.queryTuner();
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
                    switch ((pollTick_ / 25) % 11) {
                        case 0:  radio_.queryAgc(Rx::Main);
                                 radio_.queryAgcThreshold(Rx::Main); break;
                        case 9:  radio_.queryAgcHang(Rx::Main);
                                 radio_.queryAgcDecay(Rx::Main);     break;
                        case 1:  radio_.queryRfGain(Rx::Main);     break;
                        case 2:  radio_.queryAttenuator(Rx::Main); break;
                        case 3:  radio_.queryPreamp(Rx::Main);     break;
                        case 4:  radio_.queryTxPower();            break;
                        case 5:  radio_.queryVfoAssignment();      break;
                        case 6:  radio_.queryAntennaRouting();     break;
                        case 7:  radio_.queryAfVolume(Rx::Main);   // front-panel knobs
                                 radio_.queryAfVolume(Rx::Sub);    break;
                        case 8:  radio_.queryAudioRouting();       break;
                        default: radio_.queryNotch(Rx::Main);      break;
                    }
                } else {
                    radio_.querySMeter();
                }
                return;
            }
            if (phase == 2) {
                switch ((pollTick_ / 5) % 5) {       // main mode/filter, VFO B side
                    case 0:  radio_.queryMode(Rx::Main);      break;
                    case 1:  radio_.queryFilter(Rx::Main);    break;
                    case 2:  radio_.queryFrequency(Rx::Sub);  break;
                    case 3:  radio_.queryMode(Rx::Sub);       break;
                    default: radio_.queryFilter(Rx::Sub);     break;
                }
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
    stopManualTune();                            // never leave a carrier up
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
    pan_->setCenterHz(f);                       // keep grid labels on the dial
    syncBandRegister();                         // mirror the move into the stack
#ifdef HAVE_SDRPLAY
    sdr_.setCenterFrequency(static_cast<double>(f + kLoOffsetHz));
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

// Manual tune: the CAT set has no "tune button" command (*TTT needs the
// internal tuner enabled), so reproduce it: steady carrier at the set watts
// via FM mode + key, with the previous power/mode restored afterwards.
void MainWindow::startManualTune() {
    if (tuning_) return;
    tuning_ = true;
    preTunePwr_  = lastTxPwr_;
    preTuneMode_ = rigMode_;
    int level = txBar_->tuneLevel();
    if (txBar_->ampMode()) level = std::min(level, txBar_->ampLimit());
    radio_.setTxPower(level);
    if (rigMode_ != Mode::FM) radio_.setMode(Rx::Main, Mode::FM);
    radio_.setPtt(true);
    tuneTimeout_->start();
    statusBar()->showMessage(
        QString("TUNE: %1 W carrier — click again to stop (auto-stop 15 s)").arg(level));
}

void MainWindow::stopManualTune() {
    if (!tuning_) return;
    tuning_ = false;
    tuneTimeout_->stop();
    radio_.setPtt(false);                          // drop the carrier FIRST
    if (preTuneMode_ != Mode::FM) applyMode(preTuneMode_);
    radio_.setTxPower(preTunePwr_);
    txBar_->showTxPower(preTunePwr_);
    txBar_->showTuneActive(false);
    statusBar()->showMessage("TUNE: carrier off, power and mode restored");
}

// [vendor]-style digital/voice audio switch. The Orion has no MIC/LINE/BOTH CAT
// command, so the radio's input source is set to BOTH once (front panel) and
// we swap between front-mic and rear line-input purely by their gains:
//   digital: mic 0, speech proc off, aux/line 100
//   voice:   aux/line 0, mic + speech proc restored to the learned values
// Voice settings are snapshotted (and persisted) the moment we go digital, so
// whatever you actually run for SSB is what comes back.
void MainWindow::setDigitalMode(bool on) {
    if (on == digital_) return;
    if (on) {
        QSettings s;                                // remember the voice setup
        s.setValue("audio/voiceMic", lastMicGain_);
        s.setValue("audio/voiceSpeech", lastSpeechProc_);
        digital_ = true;                            // (gates the "learn" slots)
        radio_.setMicGain(0);
        radio_.setSpeechProc(0);
        radio_.setAuxInputGain(100);
        statusBar()->showMessage("DIGITAL: line-in 100, mic/speech off");
    } else {
        digital_ = false;
        radio_.setAuxInputGain(0);
        radio_.setMicGain(lastMicGain_);
        radio_.setSpeechProc(lastSpeechProc_);
        txBar_->showMicGain(lastMicGain_);
        statusBar()->showMessage(QString("VOICE: mic %1, speech %2, line-in off")
                                     .arg(lastMicGain_).arg(lastSpeechProc_));
    }
    panel_->showDigital(on);
}

void MainWindow::saveBandMemory() {
    if (curBand_ < 0 || curBand_ >= kBandCount) return;
    // Only stamp the register if the current frequency actually belongs to this
    // band — otherwise a client (WSJT-X/cqrlog) that moved the VFO elsewhere
    // would corrupt the outgoing register with an unrelated frequency.
    if (centerHz_ < kBands[curBand_].loHz || centerHz_ > kBands[curBand_].hiHz) return;
    QSettings s;
    const QString key = QString("band/%1/%2/")
                            .arg(kBands[curBand_].label).arg(kStackNames[curReg_]);
    s.setValue(key + "freq", QVariant::fromValue<qulonglong>(centerHz_));
    s.setValue(key + "mode", static_cast<int>(rigMode_));
    s.setValue(key + "bw",   rigBwHz_);
    s.setValue(key + "pbt",  rigPbtHz_);
    s.setValue(QString("band/%1/reg").arg(kBands[curBand_].label), curReg_);
}

// Every frequency change funnels here (dial-follow and tuneAbsolute). Keeps
// curBand_/curReg_ honest and schedules a debounced stamp so the active stack
// register always holds "where I last was on this band" — the same semantics
// as the Orion's own (unreadable-over-CAT) band stack, kept in lockstep.
void MainWindow::syncBandRegister() {
    const int idx = bandIndexOf(centerHz_);
    const bool crossed = (idx != curBand_);
    if (crossed) {
        curBand_ = idx;
        panel_->showBand(idx);
        lastBandPress_ = -1;                    // band moved under the buttons:
        if (idx >= 0) {                         // next press recalls, not cycles
            QSettings s;
            curReg_ = std::clamp(
                s.value(QString("band/%1/reg").arg(kBands[idx].label), 0).toInt(),
                0, kStackCount - 1);
            // If we landed exactly on a stored register (the radio's band
            // button recalls the same spot we stamped), adopt its letter.
            for (int r = 0; r < kStackCount; ++r) {
                const QString key = QString("band/%1/%2/")
                                        .arg(kBands[idx].label).arg(kStackNames[r]);
                const uint64_t f = s.value(key + "freq",
                    QVariant::fromValue<qulonglong>(kBands[idx].stack[r].hz))
                    .toULongLong();
                if (f == centerHz_) { curReg_ = r; break; }
            }
        }
    }
    panel_->showBandStack(curBand_ >= 0 ? curReg_ : -1);
    if (curBand_ >= 0 && bandStamp_) bandStamp_->start();
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

    // The Orion silently drops CAT commands that arrive while it is busy with
    // a mode switch (it reconfigures the DSP and recalls the per-mode filter —
    // set commands have no ACK, so nothing notices). Bursting *RMM then *AF
    // meant the radio never retuned on a recall while the console's stack
    // letter cycled anyway. Sequence it instead: frequency first while the
    // radio is idle, mode once the tune has landed, this register's filter
    // once the mode switch has settled (overriding the Orion's own per-mode
    // filter recall; applyMode's 400 ms re-query then confirms it).
    tuneAbsolute(f);                            // syncs curBand_ (may guess a register)
    curReg_ = reg;                              // explicit recall wins over the guess
    QTimer::singleShot(120, this, [this, m] {
        applyMode(m);
        panel_->showMode(m);
    });
    QTimer::singleShot(450, this, [this, bw, pbt] {
        radio_.setBandwidthHz(Rx::Main, bw);
        radio_.setPbtHz(Rx::Main, pbt);
    });
    rigBwHz_  = bw;                             // optimistic UI; poll confirms
    rigPbtHz_ = pbt;
    rigctld_.cacheBandwidth(bw);
    panel_->showPbt(pbt);
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

// VFO B's on-screen representation: dial + sub-RX filter edges + TX state.
// Sideband placement uses the main mode — the radio is set to copy the mode
// to the sub, and v3 firmware can't set sub mode over CAT anyway.
void MainWindow::pushVfoB() {
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, subBwHz_, subPbtHz_, lo, hi);
    const char role = txVfo_ == 'B' ? 'T' : (rxVfo_ == 'B' ? 'R' : 'N');
    pan_->setVfoB(vfoBHz_, role, lo, hi);
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
    panel_->showPbt(pendPbtHz_);
}

} // namespace ttc
