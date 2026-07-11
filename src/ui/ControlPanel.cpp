// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ControlPanel.h"
#include "app/Bands.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace ttc {

namespace {

const struct { const char* label; Mode mode; } kModes[] = {
    {"USB", Mode::USB}, {"LSB", Mode::LSB},
    {"CWU", Mode::CWU}, {"CWL", Mode::CWL},
    {"AM",  Mode::AM},  {"FM",  Mode::FM},
};

const struct { const char* label; char agc; } kAgcs[] = {
    {"F", 'F'}, {"M", 'M'}, {"S", 'S'}, {"P", 'P'}, {"OFF", 'O'},
};

const char* kAttenLabels[] = {"0", "6", "12", "18"};

QPushButton* makeButton(const QString& text) {
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setMinimumHeight(26);
    return b;
}

QGroupBox* makeGroup(const QString& title) {
    auto* g = new QGroupBox(title);
    g->setFlat(true);
    return g;
}

} // namespace

ControlPanel::ControlPanel(QWidget* parent) : QWidget(parent) {
    setFixedWidth(200);
    // Flex-ish dark theme; keep it on the panel so the app-wide palette is untouched.
    setStyleSheet(R"(
        ControlPanel { background: #10151c; }
        QGroupBox { color: #8fa3b8; border: none; margin-top: 14px;
                    font-size: 10px; font-weight: bold; }
        QGroupBox::title { subcontrol-origin: margin; left: 4px; }
        QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                      border-radius: 3px; font-size: 11px; }
        QPushButton:hover { background: #26303e; }
        QPushButton:checked { background: #2b5c8a; color: #ffffff;
                              border-color: #3f7cb4; }
        QLabel { color: #c8d4e0; font-size: 11px; }
        QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;
                                     background: #6aa5d8; }
    )");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x10, 0x15, 0x1c));
    setPalette(pal);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 8);
    lay->setSpacing(4);

    // --- Band ------------------------------------------------------------
    auto* bandBox = makeGroup("BAND");
    auto* bandGrid = new QGridLayout(bandBox);
    bandGrid->setContentsMargins(0, 4, 0, 0);
    bandGrid->setSpacing(3);
    bandGroup_ = new QButtonGroup(this);
    bandGroup_->setExclusive(true);
    for (int i = 0; i < kBandCount; ++i) {
        auto* b = makeButton(kBands[i].label);
        b->setMinimumHeight(22);
        bandGroup_->addButton(b, i);
        bandGrid->addWidget(b, i / 4, i % 4);
    }
    connect(bandGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit bandSelected(id); });
    // Band-stack indicator: which of the 4 registers (A-D) is active.
    // Pressing the active band's button again cycles the stack.
    stackLbl_ = new QLabel("STACK --");
    stackLbl_->setStyleSheet("color: #6f87a0; font-size: 10px;");
    bandGrid->addWidget(stackLbl_, (kBandCount + 3) / 4, 0, 1, 4,
                        Qt::AlignRight | Qt::AlignVCenter);
    lay->addWidget(bandBox);

    // --- Mode ------------------------------------------------------------
    auto* modeBox = makeGroup("MODE");
    auto* modeGrid = new QGridLayout(modeBox);
    modeGrid->setContentsMargins(0, 4, 0, 0);
    modeGrid->setSpacing(4);
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->setExclusive(true);
    for (int i = 0; i < 6; ++i) {
        auto* b = makeButton(kModes[i].label);
        modeGroup_->addButton(b, i);
        modeGrid->addWidget(b, i / 2, i % 2);
    }
    connect(modeGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit modeSelected(kModes[id].mode); });
    lay->addWidget(modeBox);

    // --- AGC --------------------------------------------------------------
    auto* agcBox = makeGroup("AGC");
    auto* agcRow = new QHBoxLayout(agcBox);
    agcRow->setContentsMargins(0, 4, 0, 0);
    agcRow->setSpacing(3);
    agcGroup_ = new QButtonGroup(this);
    agcGroup_->setExclusive(true);
    for (int i = 0; i < 5; ++i) {
        auto* b = makeButton(kAgcs[i].label);
        agcGroup_->addButton(b, i);
        agcRow->addWidget(b);
    }
    connect(agcGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit agcSelected(kAgcs[id].agc); });
    lay->addWidget(agcBox);

    // --- Attenuator ---------------------------------------------------------
    auto* attBox = makeGroup("ATTENUATOR (dB)");
    auto* attRow = new QHBoxLayout(attBox);
    attRow->setContentsMargins(0, 4, 0, 0);
    attRow->setSpacing(3);
    attenGroup_ = new QButtonGroup(this);
    attenGroup_->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
        auto* b = makeButton(kAttenLabels[i]);
        attenGroup_->addButton(b, i);
        attRow->addWidget(b);
    }
    connect(attenGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { emit attenSelected(id); });
    lay->addWidget(attBox);

    // --- RF gain ------------------------------------------------------------
    auto* rfBox = makeGroup("RF GAIN");
    auto* rfRow = new QHBoxLayout(rfBox);
    rfRow->setContentsMargins(0, 4, 0, 0);
    rfGain_ = new QSlider(Qt::Horizontal);
    rfGain_->setRange(0, 100);
    rfGain_->setValue(100);
    rfGainVal_ = new QLabel("100");
    rfGainVal_->setFixedWidth(26);
    rfGainVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rfRow->addWidget(rfGain_);
    rfRow->addWidget(rfGainVal_);
    lay->addWidget(rfBox);

    gainTx_ = new QTimer(this);
    gainTx_->setSingleShot(true);
    gainTx_->setInterval(40);
    connect(gainTx_, &QTimer::timeout, this, [this] {
        if (pendGain_ >= 0) { emit rfGainChanged(pendGain_); pendGain_ = -1; }
    });
    connect(rfGain_, &QSlider::valueChanged, this, [this](int v) {
        rfGainVal_->setText(QString::number(v));
        pendGain_ = v;
        if (!gainTx_->isActive()) gainTx_->start();
    });

    // --- PBT re-center --------------------------------------------------------
    // Normal edge drags are hi/lo-cut and move PBT; this snaps it back to the
    // detent (bandwidth kept). Also on double-click of either passband edge.
    auto* pbtBox = makeGroup("PBT");
    auto* pbtRow = new QHBoxLayout(pbtBox);
    pbtRow->setContentsMargins(0, 4, 0, 0);
    auto* pbtBtn = new QPushButton("PBT 0");
    pbtBtn->setFocusPolicy(Qt::NoFocus);
    pbtBtn->setMinimumHeight(26);
    pbtVal_ = new QLabel("0 Hz");
    pbtVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pbtRow->addWidget(pbtBtn);
    pbtRow->addWidget(pbtVal_, 1);
    connect(pbtBtn, &QPushButton::clicked, this,
            [this] { emit pbtZeroRequested(); });
    lay->addWidget(pbtBox);

    // --- DSP: NR / NB / auto-notch -------------------------------------------
    // Queryable on v3 despite the docs (undocumented ?R.NR/NB/NA) — synced
    // from the radio at startup.
    lay->addWidget(makeDspRow("NR", nr_, nrVal_));
    lay->addWidget(makeDspRow("NB", nb_, nbVal_));

    // Hardware noise blanker (undocumented *RMNH, strict on/off).
    auto* hwBox = makeGroup("HARDWARE NB");
    auto* hwRow = new QHBoxLayout(hwBox);
    hwRow->setContentsMargins(0, 4, 0, 0);
    hwNbBtn_ = makeButton("ON");
    hwRow->addWidget(hwNbBtn_);
    connect(hwNbBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit hwNbToggled(on); });
    lay->addWidget(hwBox);

    lay->addWidget(makeDspRow("AUTO NOTCH", an_, anVal_));

    // Manual notch: engage button + live center x width readout. Placement is
    // on the panadapter itself (drag the orange marker; wheel over it = width).
    auto* notchBox = makeGroup("MANUAL NOTCH");
    auto* notchRow = new QHBoxLayout(notchBox);
    notchRow->setContentsMargins(0, 4, 0, 0);
    notchBtn_ = makeButton("NOTCH");
    notchVal_ = new QLabel("--");
    notchVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    notchRow->addWidget(notchBtn_);
    notchRow->addWidget(notchVal_, 1);
    connect(notchBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit notchToggled(on); });
    lay->addWidget(notchBox);
    connect(nr_, &QSlider::valueChanged, this, [this](int v) {
        nrVal_->setText(v ? QString::number(v) : "off");
        emit nrChanged(v);
    });
    connect(nb_, &QSlider::valueChanged, this, [this](int v) {
        nbVal_->setText(v ? QString::number(v) : "off");
        emit nbChanged(v);
    });
    connect(an_, &QSlider::valueChanged, this, [this](int v) {
        anVal_->setText(v ? QString::number(v) : "off");
        emit autoNotchChanged(v);
    });

    lay->addStretch(1);
}

QWidget* ControlPanel::makeDspRow(const QString& name, QSlider*& slider, QLabel*& value) {
    auto* box = makeGroup(name);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 4, 0, 0);
    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 9);
    slider->setValue(0);
    value = new QLabel("off");
    value->setFixedWidth(26);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(slider);
    row->addWidget(value);
    return box;
}

void ControlPanel::showBand(int bandIdx) {
    QSignalBlocker block(bandGroup_);
    if (bandIdx < 0 || bandIdx >= kBandCount) {
        // Out of every ham band: uncheck whatever is lit.
        if (auto* b = bandGroup_->checkedButton()) {
            bandGroup_->setExclusive(false);
            b->setChecked(false);
            bandGroup_->setExclusive(true);
        }
        return;
    }
    if (auto* b = bandGroup_->button(bandIdx)) b->setChecked(true);
}

void ControlPanel::showBandStack(int regIdx) {
    if (regIdx < 0 || regIdx >= kStackCount) stackLbl_->setText("STACK --");
    else stackLbl_->setText(QString("STACK %1").arg(kStackNames[regIdx]));
}

void ControlPanel::showMode(Mode m) {
    for (int i = 0; i < 6; ++i)
        if (kModes[i].mode == m) {
            QSignalBlocker block(modeGroup_);
            if (auto* b = modeGroup_->button(i)) b->setChecked(true);
            return;
        }
}

void ControlPanel::showAgc(char agc) {
    for (int i = 0; i < 5; ++i)
        if (kAgcs[i].agc == agc) {
            QSignalBlocker block(agcGroup_);
            if (auto* b = agcGroup_->button(i)) b->setChecked(true);
            return;
        }
}

void ControlPanel::showAtten(int step) {
    if (step < 0 || step > 3) return;
    QSignalBlocker block(attenGroup_);
    if (auto* b = attenGroup_->button(step)) b->setChecked(true);
}

void ControlPanel::showRfGain(int gain) {
    QSignalBlocker block(rfGain_);
    rfGain_->setValue(gain);
    rfGainVal_->setText(QString::number(gain));
}

static void showDspLevel(QSlider* slider, QLabel* value, int level) {
    QSignalBlocker block(slider);
    slider->setValue(level);
    value->setText(level ? QString::number(level) : "off");
}

void ControlPanel::showNr(int level)        { showDspLevel(nr_, nrVal_, level); }
void ControlPanel::showNb(int level)        { showDspLevel(nb_, nbVal_, level); }
void ControlPanel::showAutoNotch(int level) { showDspLevel(an_, anVal_, level); }

void ControlPanel::showNotch(bool on, int centerHz, int widthHz) {
    QSignalBlocker block(notchBtn_);
    notchBtn_->setChecked(on);
    notchVal_->setText(QString("%1 Hz x %2").arg(centerHz).arg(widthHz));
}

void ControlPanel::showHwNb(bool on) {
    QSignalBlocker block(hwNbBtn_);
    hwNbBtn_->setChecked(on);
}

void ControlPanel::showPbt(int pbtHz) {
    pbtVal_->setText(QString("%1%2 Hz").arg(pbtHz > 0 ? "+" : "").arg(pbtHz));
    // Make an off-center passband visible at a glance.
    pbtVal_->setStyleSheet(pbtHz == 0 ? "color: #c8d4e0;" : "color: #f0b040;");
}

} // namespace ttc
