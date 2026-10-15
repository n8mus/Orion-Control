// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SdrRxDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace ttc {

namespace {
// The console's dialog scheme; group headings carry their own colour
// (unstyled they take the desktop theme's — unreadable on Cinnamon).
const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QGroupBox { color: #e8b04a; font-weight: bold; border: 1px solid #2a3644;"
    " margin-top: 12px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QComboBox, QPushButton { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #2a3644; padding: 3px 8px; }"
    "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
    " selection-background-color: #2b5c8a; }";
}  // namespace

SdrRxDialog::SdrRxDialog(bool on, const SdrRx::Settings& s, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("SDR receive audio");
    setStyleSheet(kStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    on_ = new QCheckBox("SDR receive audio on (PC speakers / headset)", this);
    on_->setStyleSheet("QCheckBox { color: #e8b04a; font-weight: bold; }");
    lay->addWidget(on_);
    auto* note = new QLabel("The RSP2's IQ through WDSP, Thetis's receiver. Follows the\n"
                            "radio's mode, filter and CW pitch; silent while transmitting.\n"
                            "It runs ~50-100 ms behind the Orion — turn the radio down.",
                            this);
    lay->addWidget(note);

    auto* audio = new QGroupBox("AUDIO", this);
    auto* ag = new QGridLayout(audio);
    ag->addWidget(new QLabel("Volume", audio), 0, 0);
    vol_ = new QSlider(Qt::Horizontal, audio);
    vol_->setRange(-40, 20);
    vol_->setMinimumWidth(200);
    ag->addWidget(vol_, 0, 1);
    volVal_ = new QLabel(audio);
    volVal_->setFixedWidth(48);
    ag->addWidget(volVal_, 0, 2);
    ag->addWidget(new QLabel("AGC", audio), 1, 0);
    agc_ = new QComboBox(audio);
    agc_->addItems({"Fixed", "Long", "Slow", "Medium", "Fast"});
    agc_->setToolTip("WDSP AGC (Thetis's): attack/hang/decay presets");
    ag->addWidget(agc_, 1, 1);
    lay->addWidget(audio);

    auto* noise = new QGroupBox("NOISE", this);
    auto* ng = new QGridLayout(noise);
    ng->addWidget(new QLabel("Noise blanker", noise), 0, 0);
    nb_ = new QComboBox(noise);
    nb_->addItems({"Off", "NB — impulse blanker", "NB2 — blank and fill"});
    nb_->setToolTip("On the IQ, before any filter (as Thetis runs them). NB gates\n"
                    "an impulse to zero; NB2 fills the gap from either side —\n"
                    "better when the pop lands on the signal. Never both.");
    ng->addWidget(nb_, 0, 1);
    ng->addWidget(new QLabel("Noise reduction", noise), 1, 0);
    nr_ = new QComboBox(noise);
    nr_->addItems({"Off", "NR — LMS (Thetis NR)", "NR2 — spectral (Thetis NR2)",
                   "NR3 — neural (WDSP 2.10)"});
    nr_->setToolTip("One at a time, as Thetis offers them. NR2 is the usual\n"
                    "choice on SSB; NR3 is the neural reducer (adds ~51 ms).");
    ng->addWidget(nr_, 1, 1);
    snb_ = new QCheckBox("SNB — spectral noise blanker", noise);
    snb_->setToolTip("Thetis SNB: removes impulse noise in the frequency domain");
    anf_ = new QCheckBox("ANF — automatic notch", noise);
    anf_->setToolTip("Removes steady carriers / heterodynes (not for CW!)");
    rnn_ = new QCheckBox("RNNoise (on the audio)", noise);
    rnn_->setToolTip("Xiph RNNoise after the demod, as AetherSDR offers it — voice only");
    ng->addWidget(snb_, 2, 0, 1, 2);
    ng->addWidget(anf_, 3, 0, 1, 2);
    ng->addWidget(rnn_, 4, 0, 1, 2);
    lay->addWidget(noise);

    info_ = new QLabel(this);
    info_->setStyleSheet("QLabel { color: #c8d4e0; font-family: monospace; }");
    lay->addWidget(info_);
    auto* bRow = new QHBoxLayout;
    bRow->addStretch(1);
    auto* close = new QPushButton("Close", this);
    bRow->addWidget(close);
    lay->addLayout(bRow);

    loading_ = true;
    on_->setChecked(on);
    vol_->setValue(int(std::lround(s.volumeDb)));
    volVal_->setText(QString("%1 dB").arg(vol_->value()));
    agc_->setCurrentIndex(int(s.agc));
    nb_->setCurrentIndex(int(s.nb));
    nr_->setCurrentIndex(int(s.nr));
    snb_->setChecked(s.snb);
    anf_->setChecked(s.anf);
    rnn_->setChecked(s.rnnoise);
    loading_ = false;

    connect(on_, &QCheckBox::toggled, this, [this] { emitChanged(); });
    connect(vol_, &QSlider::valueChanged, this, [this](int v) {
        volVal_->setText(QString("%1 dB").arg(v));
        emitChanged();
    });
    for (QComboBox* c : {agc_, nb_, nr_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { emitChanged(); });
    for (QCheckBox* c : {snb_, anf_, rnn_})
        connect(c, &QCheckBox::toggled, this, [this] { emitChanged(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    auto* tick = new QTimer(this);
    tick->setInterval(250);
    connect(tick, &QTimer::timeout, this, [this] {
        if (status_) info_->setText(status_());
    });
    tick->start();
}

void SdrRxDialog::setOn(bool on) {
    const QSignalBlocker b(on_);
    on_->setChecked(on);
}

SdrRx::Settings SdrRxDialog::collect() const {
    SdrRx::Settings s;
    s.volumeDb = float(vol_->value());
    s.agc = SdrRx::Agc(agc_->currentIndex());
    s.nb = SdrRx::NoiseBlanker(nb_->currentIndex());
    s.nr = SdrRx::NoiseReduction(nr_->currentIndex());
    s.snb = snb_->isChecked();
    s.anf = anf_->isChecked();
    s.rnnoise = rnn_->isChecked();
    return s;
}

void SdrRxDialog::emitChanged() {
    if (loading_) return;
    emit changed(on_->isChecked(), collect());
}

} // namespace ttc
