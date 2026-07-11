// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/DisplayPanel.h"

#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QSignalBlocker>
#include <algorithm>

namespace ttc {

namespace {
QLabel* makeCaption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    return l;
}
} // namespace

DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 11px; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 2px 6px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }"
        "QCheckBox { spacing: 6px; }"
        "QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;"
        " background: #6aa5d8; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(12, 10, 12, 10);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(8);

    // Reference level: the dB value at the TOP of the spectrum scale. Lower it
    // to pull weak signals up out of the floor, raise it for strong-signal work.
    g->addWidget(makeCaption("REF LVL", this), 0, 0);
    ref_ = new QSlider(Qt::Horizontal, this);
    ref_->setRange(-100, -20);
    ref_->setFixedWidth(150);
    refVal_ = new QLabel(this);
    refVal_->setFixedWidth(52);
    g->addWidget(ref_, 0, 1);
    g->addWidget(refVal_, 0, 2);

    // Range = scale height in dB (contrast): small range = punchy colors.
    g->addWidget(makeCaption("RANGE", this), 1, 0);
    range_ = new QSlider(Qt::Horizontal, this);
    range_->setRange(30, 120);
    range_->setFixedWidth(150);
    rangeVal_ = new QLabel(this);
    rangeVal_->setFixedWidth(52);
    g->addWidget(range_, 1, 1);
    g->addWidget(rangeVal_, 1, 2);

    g->addWidget(makeCaption("AVG", this), 2, 0);
    avg_ = new QComboBox(this);
    avg_->addItem("Off", 1);
    avg_->addItem("2", 2);
    avg_->addItem("4", 4);
    avg_->addItem("8", 8);
    avg_->addItem("16", 16);
    g->addWidget(avg_, 2, 1, 1, 2);

    g->addWidget(makeCaption("WF SPEED", this), 3, 0);
    speed_ = new QComboBox(this);
    speed_->addItem("Fast", 1);
    speed_->addItem("Normal", 2);
    speed_->addItem("Slow", 4);
    speed_->addItem("Slowest", 8);
    g->addWidget(speed_, 3, 1, 1, 2);

    g->addWidget(makeCaption("PALETTE", this), 4, 0);
    pal_ = new QComboBox(this);
    pal_->addItems(PanadapterWidget::paletteNames());
    g->addWidget(pal_, 4, 1, 1, 2);

    fill_ = new QCheckBox("Fill spectrum", this);
    peak_ = new QCheckBox("Peak hold", this);
    g->addWidget(fill_, 5, 0, 1, 3);
    g->addWidget(peak_, 6, 0, 1, 3);

    auto updateLabels = [this] {
        refVal_->setText(QString("%1 dB").arg(ref_->value()));
        rangeVal_->setText(QString("%1 dB").arg(range_->value()));
    };
    connect(ref_,   &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(range_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(avg_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(speed_, &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(pal_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(fill_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(peak_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);

    setSettings(DisplaySettings{});                // defaults until owner restores
}

DisplaySettings DisplayPanel::settings() const {
    DisplaySettings s;
    s.refDb     = static_cast<float>(ref_->value());
    s.rangeDb   = static_cast<float>(range_->value());
    s.avgFrames = avg_->currentData().toInt();
    s.wfSpeed   = speed_->currentData().toInt();
    s.palette   = pal_->currentIndex();
    s.fillTrace = fill_->isChecked();
    s.peakHold  = peak_->isChecked();
    s.split     = split_;
    return s;
}

void DisplayPanel::setSettings(const DisplaySettings& s) {
    split_ = s.split;
    const QSignalBlocker b1(ref_), b2(range_), b3(avg_), b4(speed_), b5(pal_),
        b6(fill_), b7(peak_);
    ref_->setValue(static_cast<int>(s.refDb));
    range_->setValue(static_cast<int>(s.rangeDb));
    const int ai = avg_->findData(s.avgFrames);
    avg_->setCurrentIndex(ai >= 0 ? ai : 2);
    const int si = speed_->findData(s.wfSpeed);
    speed_->setCurrentIndex(si >= 0 ? si : 1);
    pal_->setCurrentIndex(std::clamp(s.palette, 0, pal_->count() - 1));
    fill_->setChecked(s.fillTrace);
    peak_->setChecked(s.peakHold);
    refVal_->setText(QString("%1 dB").arg(ref_->value()));
    rangeVal_->setText(QString("%1 dB").arg(range_->value()));
}

void DisplayPanel::emitChanged() {
    emit settingsChanged(settings());
}

} // namespace ttc
