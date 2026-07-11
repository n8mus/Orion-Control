// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/TxBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <algorithm>

namespace ttc {

TxBar::TxBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(40);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x10, 0x15, 0x1c));
    setPalette(pal);
    setStyleSheet(R"(
        QLabel { color: #8fa3b8; font-size: 10px; font-weight: bold; }
        QLabel[valueLabel="true"] { color: #c8d4e0; font-weight: normal; font-size: 11px; }
        QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                      border-radius: 3px; font-size: 11px; min-height: 24px;
                      padding: 0 10px; }
        QPushButton:hover { background: #26303e; }
        QPushButton:checked { background: #2b5c8a; color: #ffffff; border-color: #3f7cb4; }
        QPushButton#amp:checked { background: #8a5c2b; border-color: #b47c3f; }
        QPushButton#prof { padding: 0 7px; font-size: 10px; border-radius: 8px; }
        QPushButton#prof:checked { background: #1f7a45; border-color: #3ecf7a; }
        QSpinBox { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                   font-size: 11px; }
        QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;
                                     background: #6aa5d8; }
    )");

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 2, 8, 2);
    lay->setSpacing(6);

    lay->addWidget(new QLabel("PWR"));
    pwr_ = makeSlider("PWR", pwrVal_, pwrTx_, pendPwr_, &TxBar::txPowerChanged);
    pwr_->setFixedWidth(120);
    lay->addWidget(pwr_);
    lay->addWidget(pwrVal_);

    ampBtn_ = new QPushButton("AMP");
    ampBtn_->setObjectName("amp");
    ampBtn_->setCheckable(true);
    ampBtn_->setFocusPolicy(Qt::NoFocus);
    ampBtn_->setToolTip("Amplifier mode: cap drive at the limit and enforce it");
    ampLimit_ = new QSpinBox;
    ampLimit_->setRange(5, 100);
    ampLimit_->setValue(40);
    ampLimit_->setSuffix("W");
    ampLimit_->setEnabled(false);
    ampLimit_->setFocusPolicy(Qt::ClickFocus);
    lay->addWidget(ampBtn_);
    lay->addWidget(ampLimit_);
    connect(ampBtn_, &QPushButton::toggled, this, [this](bool on) {
        ampLimit_->setEnabled(on);
        applyPowerCap();
        emit ampModeChanged(on, ampLimit_->value());
    });
    connect(ampLimit_, &QSpinBox::valueChanged, this, [this](int lim) {
        applyPowerCap();
        emit ampModeChanged(ampBtn_->isChecked(), lim);
    });

    lay->addSpacing(8);
    tuneBtn_ = new QPushButton("TUNE");
    tuneBtn_->setCheckable(true);                  // latches while the carrier is up
    tuneBtn_->setFocusPolicy(Qt::NoFocus);
    tuneBtn_->setToolTip("Steady carrier at the set watts for tuning the amp or an "
                         "external tuner (runs the internal tuner cycle if TUNER is on)");
    tuneLvl_ = new QSpinBox;
    tuneLvl_->setRange(5, 100);
    tuneLvl_->setValue(20);
    tuneLvl_->setSuffix("W");
    tuneLvl_->setFocusPolicy(Qt::ClickFocus);
    tuneLvl_->setToolTip("Manual tune carrier power");
    tunerBtn_ = new QPushButton("TUNER");
    tunerBtn_->setCheckable(true);
    tunerBtn_->setFocusPolicy(Qt::NoFocus);
    tunerBtn_->setToolTip("Enable the internal tuner (leave off with an external tuner)");
    lay->addWidget(tuneBtn_);
    lay->addWidget(tuneLvl_);
    lay->addWidget(tunerBtn_);
    connect(tuneBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit tuneToggled(on); });
    connect(tuneLvl_, &QSpinBox::valueChanged, this,
            [this](int w) { emit tuneLevelChanged(w); });
    connect(tunerBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit tunerEnableToggled(on); });

    lay->addSpacing(8);
    lay->addWidget(new QLabel("MIC"));
    mic_ = makeSlider("MIC", micVal_, micTx_, pendMic_, &TxBar::micGainChanged);
    lay->addWidget(mic_);
    lay->addWidget(micVal_);

    // TX audio shaping: the Orion's whole CAT-visible surface is the TX
    // filter bandwidth (900-3900 Hz continuous, hi-cut in effect — the low
    // corner is fixed in firmware, no EQ/rolloff commands exist) and the
    // speech processor level.
    lay->addSpacing(8);
    lay->addWidget(new QLabel("TX BW"));
    txbw_ = makeSlider("TXBW", txbwVal_, txbwTx_, pendTxbw_, &TxBar::txFilterChanged);
    {
        const QSignalBlocker b(txbw_);             // range change must not command
        txbw_->setRange(900, 3900);                // the radio before its state is in
        txbw_->setSingleStep(50);
    }
    txbw_->setToolTip("SSB transmit filter bandwidth (Orion range 900-3900 Hz)");
    txbwVal_->setFixedWidth(32);
    lay->addWidget(txbw_);
    lay->addWidget(txbwVal_);

    lay->addWidget(new QLabel("PROC"));
    proc_ = makeSlider("PROC", procVal_, procTx_, pendProc_, &TxBar::speechProcChanged);
    {
        const QSignalBlocker b(proc_);
        proc_->setRange(0, 9);
    }
    proc_->setFixedWidth(56);
    proc_->setToolTip("Speech processor level (0 = off)");
    connect(proc_, &QSlider::valueChanged, this,   // after makeSlider's handler:
            [this](int v) { if (v == 0) procVal_->setText("OFF"); });
    lay->addWidget(proc_);
    lay->addWidget(procVal_);

    // TX profiles: one click mid-pileup, no popup. The lit button shows the
    // active profile; touching any TX slider clears it (settings have drifted).
    lay->addSpacing(10);
    static const char* kProfNames[4] = {"RAG", "DX", "CONT", "ESSB"};
    static const char* kProfTips[4]  = {"normal ragchew", "DX / pileup",
                                        "contest", "wide clean audio"};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QPushButton(kProfNames[i]);
        b->setObjectName("prof");
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(QString("TX profile: %1\nclick = recall   right-click = "
                              "save current TX BW/PROC/MIC/PWR here").arg(kProfTips[i]));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        profBtn_[i] = b;
        lay->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] {
            markProfile(i);
            emit profileRecalled(i);
        });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, i] {
            markProfile(i);                        // saved = current, so it's active
            emit profileSaveRequested(i);
        });
    }
    // Manual edits to anything a profile bundles un-light the profile button.
    for (QSlider* s : {pwr_, mic_, txbw_, proc_})
        connect(s, &QSlider::valueChanged, this, [this] { markProfile(-1); });
    lay->addStretch(1);
}

void TxBar::markProfile(int slot) {
    for (int j = 0; j < 4; ++j) {
        const QSignalBlocker b(profBtn_[j]);
        profBtn_[j]->setChecked(j == slot);
    }
}

QSlider* TxBar::makeSlider(const QString&, QLabel*& value, QTimer*& tx, int& pend,
                           void (TxBar::*sig)(int)) {
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange(0, 100);
    s->setFixedWidth(90);
    value = new QLabel("--");
    value->setProperty("valueLabel", true);
    value->setFixedWidth(26);
    tx = new QTimer(this);
    tx->setSingleShot(true);
    tx->setInterval(40);
    connect(tx, &QTimer::timeout, this, [this, &pend, sig] {
        if (pend >= 0) { emit (this->*sig)(pend); pend = -1; }
    });
    connect(s, &QSlider::valueChanged, this, [this, value, &pend, tPtr = tx](int v) {
        value->setText(QString::number(v));
        pend = v;
        if (!tPtr->isActive()) tPtr->start();
    });
    return s;
}

bool TxBar::ampMode() const { return ampBtn_->isChecked(); }
int  TxBar::ampLimit() const { return ampLimit_->value(); }
int  TxBar::tuneLevel() const { return tuneLvl_->value(); }

void TxBar::setTuneLevel(int watts) {
    QSignalBlocker block(tuneLvl_);
    tuneLvl_->setValue(std::clamp(watts, 5, 100));
}

void TxBar::showTuneActive(bool on) {
    QSignalBlocker block(tuneBtn_);
    tuneBtn_->setChecked(on);
}

void TxBar::applyPowerCap() {
    const int cap = ampBtn_->isChecked() ? ampLimit_->value() : 100;
    pwr_->setMaximum(cap);                // also drags an over-cap value down,
}                                         // which emits and corrects the radio

void TxBar::setAmpMode(bool on, int limitPct) {
    QSignalBlocker b1(ampBtn_), b2(ampLimit_);
    ampBtn_->setChecked(on);
    ampLimit_->setValue(std::clamp(limitPct, 5, 100));
    ampLimit_->setEnabled(on);
    applyPowerCap();
}

static void showVal(QSlider* s, QLabel* l, int v) {
    QSignalBlocker block(s);
    s->setValue(v);
    l->setText(QString::number(v));
}

void TxBar::showTxPower(int pct)  { showVal(pwr_, pwrVal_, pct); }
void TxBar::showMicGain(int pct)  { showVal(mic_, micVal_, pct); }
void TxBar::showTxFilter(int hz)  { showVal(txbw_, txbwVal_, hz); }

void TxBar::showSpeechProc(int level) {
    showVal(proc_, procVal_, level);
    if (level == 0) procVal_->setText("OFF");
}

void TxBar::showTuner(bool on) {
    QSignalBlocker block(tunerBtn_);
    tunerBtn_->setChecked(on);
}

} // namespace ttc
