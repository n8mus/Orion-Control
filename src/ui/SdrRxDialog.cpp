// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SdrRxDialog.h"
#include "ui/VoiceEqDialog.h"   // VoiceEqCurve

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
#include <vector>

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

// Listening shapes, tune-by-ear starting points (slots: 0 HP, 1 LS,
// 2-5 peaks, 6 HS, 7 LP). Each starts from flat, so a slot not named
// here is off.
struct RxEqPreset {
    const char* name;
    struct B { int slot; float hz, db, q; int slope; };
    std::vector<B> bands;
};
const std::vector<RxEqPreset>& rxEqPresets() {
    static const std::vector<RxEqPreset> p = {
        {"Flat", {}},
        // Thin the mud, lift the consonants: SSB in a pile of QRM.
        {"Voice clarity", {{0, 150, 0, 0.707f, 24}, {2, 300, -3, 1.0f, 0},
                           {4, 2200, 4, 1.0f, 0}}},
        // Long sessions: warm lows, the top hiss taken down.
        {"Mellow (less hiss)", {{1, 200, 2, 0.707f, 0}, {6, 2500, -6, 0.707f, 0}}},
        // Band noise above the voice cut hard.
        {"Hiss cut 2.6 kHz", {{0, 120, 0, 0.707f, 24}, {7, 2600, 0, 0.707f, 48}}},
        // CW: a soft audio window around the usual pitches.
        {"CW window", {{0, 300, 0, 0.707f, 24}, {7, 1000, 0, 0.707f, 24}}},
    };
    return p;
}
}  // namespace

void SdrRxDialog::loadEqPreset(int idx) {
    const auto& all = rxEqPresets();
    if (idx < 0 || idx >= int(all.size())) return;
    eq_ = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    for (auto& b : eq_.bands) b.enabled = false;
    for (const auto& b : all[size_t(idx)].bands) {
        auto& d = eq_.bands[size_t(b.slot)];
        d.freqHz = b.hz;
        d.gainDb = b.db;
        d.q = b.q;
        if (b.slope) d.slopeDbPerOct = b.slope;
        d.enabled = true;
    }
    eq_.enabled = idx > 0;
    {
        const QSignalBlocker blk(eqOn_);
        eqOn_->setChecked(eq_.enabled);
    }
    curve_->setSettings(eq_);
    emitChanged();
}

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

    // RX EQ: the voice keyer's editor on the receive audio — drag a
    // handle (left/right = frequency, up/down = gain, Shift-drag or the
    // wheel = Q); grabbing a band that is off turns it on.
    auto* eqBox = new QGroupBox("EQ", this);
    auto* eqLay = new QVBoxLayout(eqBox);
    auto* eqRow = new QHBoxLayout;
    eqOn_ = new QCheckBox("EQ on", eqBox);
    eqRow->addWidget(eqOn_);
    eqRow->addStretch(1);
    eqRow->addWidget(new QLabel("Preset", eqBox));
    eqPreset_ = new QComboBox(eqBox);
    eqPreset_->addItem("Load…");
    for (const auto& p : rxEqPresets()) eqPreset_->addItem(p.name);
    eqPreset_->setToolTip("Starting points — then drag the curve to taste");
    eqRow->addWidget(eqPreset_);
    eqLay->addLayout(eqRow);
    curve_ = new VoiceEqCurve(eqBox);
    curve_->setEdge(0.f, QString());
    curve_->setToolTip("Drag a dot: left/right = frequency, up/down = gain;\n"
                       "Shift-drag or the wheel = Q (width). HP/LP move in frequency.");
    eqLay->addWidget(curve_);
    lay->addWidget(eqBox);

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
    eq_ = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    eq_.enabled = s.eqOn;
    for (size_t i = 0; i < s.eq.size(); ++i) eq_.bands[i] = s.eq[i];
    eqOn_->setChecked(s.eqOn);
    curve_->setSettings(eq_);
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
    connect(eqOn_, &QCheckBox::toggled, this, [this](bool v) {
        eq_.enabled = v;
        curve_->setSettings(eq_);
        emitChanged();
    });
    connect(eqPreset_, &QComboBox::activated, this, [this](int i) {
        if (i > 0) loadEqPreset(i - 1);
        const QSignalBlocker blk(eqPreset_);
        eqPreset_->setCurrentIndex(0);
    });
    curve_->onBandEdited = [this](int band, const ClientEq::BandParams& p) {
        eq_.bands[size_t(band)] = p;
        if (p.enabled && !eq_.enabled) {           // the curve turned it on
            eq_.enabled = true;
            const QSignalBlocker blk(eqOn_);
            eqOn_->setChecked(true);
        }
        emitChanged();
    };
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
    s.eqOn = eq_.enabled;
    for (size_t i = 0; i < s.eq.size(); ++i) s.eq[i] = eq_.bands[i];
    return s;
}

void SdrRxDialog::emitChanged() {
    if (loading_) return;
    emit changed(on_->isChecked(), collect());
}

} // namespace ttc
