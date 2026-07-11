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
    digBtn_ = new QPushButton("DIG");
    digBtn_->setCheckable(true);
    digBtn_->setFocusPolicy(Qt::NoFocus);
    digBtn_->setToolTip("Digital mode: line-in audio, mic + speech processor off "
                        "(restores your voice settings when off)");
    lay->addWidget(digBtn_);
    connect(digBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit digitalModeToggled(on); });

    lay->addSpacing(8);
    lay->addWidget(new QLabel("MIC"));
    mic_ = makeSlider("MIC", micVal_, micTx_, pendMic_, &TxBar::micGainChanged);
    lay->addWidget(mic_);
    lay->addWidget(micVal_);

    lay->addWidget(new QLabel("MON"));
    mon_ = makeSlider("MON", monVal_, monTx_, pendMon_, &TxBar::monitorChanged);
    lay->addWidget(mon_);
    lay->addWidget(monVal_);

    lay->addWidget(new QLabel("AF"));
    af_ = makeSlider("AF", afVal_, afTx_, pendAf_, &TxBar::afVolumeChanged);
    lay->addWidget(af_);
    lay->addWidget(afVal_);
    lay->addStretch(1);
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
void TxBar::showMonitor(int pct)  { showVal(mon_, monVal_, pct); }
void TxBar::showAfVolume(int pct) { showVal(af_, afVal_, pct); }

void TxBar::showTuner(bool on) {
    QSignalBlocker block(tunerBtn_);
    tunerBtn_->setChecked(on);
}

void TxBar::showDigitalMode(bool on) {
    QSignalBlocker block(digBtn_);
    digBtn_->setChecked(on);
}

} // namespace ttc
