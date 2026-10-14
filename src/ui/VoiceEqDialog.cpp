// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VoiceEqDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

namespace ttc {

namespace {
constexpr float kLoHz = 40.f, kHiHz = 6000.f;
constexpr float kDbSpan = 15.f;                    // plot is ±15 dB
constexpr float kOrionTxEdgeHz = 3900.f;           // widest Orion TX filter

const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QComboBox, QPushButton { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #2a3644; padding: 3px 8px; }"
    "QPushButton:default { border-color: #3ecf7a; }"
    // Colors only on the spin boxes: a border rule drops the arrows.
    "QSpinBox, QDoubleSpinBox { background: #1c2430; color: #c8d4e0; }"
    "QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled"
    " { color: #4a5868; }";
}  // namespace

VoiceEqCurve::VoiceEqCurve(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(170);
}

void VoiceEqCurve::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(34, 8, -8, -20);
    p.fillRect(rect(), QColor(11, 16, 22));
    const auto xOf = [&](float hz) {
        return r.left() + r.width() * std::log(hz / kLoHz) / std::log(kHiHz / kLoHz);
    };
    const auto yOf = [&](float db) {
        return r.center().y() - std::clamp(db, -kDbSpan, kDbSpan) / kDbSpan * r.height() / 2;
    };
    // The radio's side of the story: above 3.9 kHz the Orion's TX filter
    // removes it whatever the EQ does.
    p.fillRect(QRectF(QPointF(xOf(kOrionTxEdgeHz), r.top()), r.bottomRight()),
               QColor(60, 30, 30, 90));
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(QColor(150, 90, 90));
    p.drawText(QRectF(xOf(kOrionTxEdgeHz) - 103, r.top() + 1, 100, 14),
               Qt::AlignRight | Qt::AlignVCenter, "Orion TX edge →");
    for (float hz : {50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f}) {
        const double x = xOf(hz);
        p.setPen(QColor(32, 42, 54));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QColor(110, 125, 140));
        p.drawText(QRectF(x - 20, r.bottom() + 3, 40, 14), Qt::AlignCenter,
                   hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz));
    }
    for (int db = -12; db <= 12; db += 6) {
        const double y = yOf(float(db));
        p.setPen(db == 0 ? QColor(70, 84, 100) : QColor(32, 42, 54));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QColor(110, 125, 140));
        p.drawText(QRectF(0, y - 7, 30, 14), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(db));
    }
    QPainterPath path;
    constexpr int kPts = 240;
    for (int i = 0; i <= kPts; ++i) {
        const float hz = kLoHz * std::pow(kHiHz / kLoHz, float(i) / kPts);
        const QPointF pt(xOf(hz), yOf(s_.responseDb(hz)));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    p.setPen(QPen(s_.enabled ? QColor(80, 230, 120) : QColor(90, 100, 110), 2));
    p.drawPath(path);
}

VoiceEqDialog::VoiceEqDialog(const VoiceEqSettings& s, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Voice keyer EQ");
    setStyleSheet(kStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* top = new QHBoxLayout;
    enabled_ = new QCheckBox("EQ on for VK1-VK4 messages", this);
    top->addWidget(enabled_);
    top->addStretch(1);
    top->addWidget(new QLabel("Preset", this));
    preset_ = new QComboBox(this);
    preset_->addItem("—");
    preset_->addItems(VoiceEqSettings::presetNames());
    preset_->setToolTip("Starting points — tune by ear (listen here, then\n"
                        "on the Orion's MON while transmitting into a\n"
                        "dummy load or at low power)");
    top->addWidget(preset_);
    lay->addLayout(top);

    curve_ = new VoiceEqCurve(this);
    lay->addWidget(curve_);

    auto* g = new QGridLayout;
    g->setHorizontalSpacing(10);
    g->addWidget(new QLabel("Band", this), 0, 0);
    g->addWidget(new QLabel("Hz", this), 0, 1);
    g->addWidget(new QLabel("Gain dB", this), 0, 2);
    g->addWidget(new QLabel("Q / slope", this), 0, 3);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        Row& row = rows_[i];
        const ClientEq::FilterType t = VoiceEqSettings::slotType(i);
        const bool slopeBand = t == ClientEq::FilterType::HighPass
                            || t == ClientEq::FilterType::LowPass;
        row.on = new QCheckBox(VoiceEqSettings::slotName(i), this);
        g->addWidget(row.on, i + 1, 0);
        row.hz = new QSpinBox(this);
        row.hz->setRange(20, 8000);
        row.hz->setSingleStep(10);
        row.hz->setFixedWidth(90);
        g->addWidget(row.hz, i + 1, 1);
        if (slopeBand) {
            row.slope = new QComboBox(this);
            for (int sl : {12, 24, 36, 48})
                row.slope->addItem(QString("%1 dB/oct").arg(sl), sl);
            g->addWidget(row.slope, i + 1, 3);
        } else {
            row.db = new QDoubleSpinBox(this);
            row.db->setRange(-15.0, 15.0);
            row.db->setDecimals(1);
            row.db->setSingleStep(0.5);
            row.db->setFixedWidth(80);
            g->addWidget(row.db, i + 1, 2);
            row.q = new QDoubleSpinBox(this);
            row.q->setRange(0.3, 8.0);
            row.q->setDecimals(2);
            row.q->setSingleStep(0.1);
            row.q->setFixedWidth(80);
            g->addWidget(row.q, i + 1, 3);
        }
    }
    g->setColumnStretch(4, 1);
    lay->addLayout(g);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    lay->addWidget(status_);

    auto* bRow = new QHBoxLayout;
    bRow->addWidget(new QLabel("Listen to", this));
    slot_ = new QComboBox(this);
    for (int i = 1; i <= 4; ++i) slot_->addItem(QString("VK%1").arg(i), i - 1);
    bRow->addWidget(slot_);
    auto* listen = new QPushButton("Apply && listen", this);
    listen->setToolTip("Re-render the messages with this curve, then play\n"
                       "the chosen one on the PC speakers (nothing keyed)");
    bRow->addWidget(listen);
    bRow->addStretch(1);
    auto* apply = new QPushButton("Apply", this);
    apply->setDefault(true);
    auto* close = new QPushButton("Close", this);
    bRow->addWidget(apply);
    bRow->addWidget(close);
    lay->addLayout(bRow);

    load(s);

    // Any edit redraws the curve and drops the preset label to "—" (the
    // curve is now the operator's own). Nothing renders until Apply.
    const auto edited = [this] {
        if (loading_) return;
        refreshEnables();
        curve_->setSettings(collect());
        const QSignalBlocker b(preset_);
        preset_->setCurrentIndex(0);
    };
    connect(enabled_, &QCheckBox::toggled, this, edited);
    for (Row& row : rows_) {
        connect(row.on, &QCheckBox::toggled, this, edited);
        connect(row.hz, &QSpinBox::valueChanged, this, edited);
        if (row.db) connect(row.db, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.q) connect(row.q, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.slope)
            connect(row.slope, &QComboBox::currentIndexChanged, this, edited);
    }
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx <= 0) return;
        load(VoiceEqSettings::preset(VoiceEqSettings::Preset(idx - 1)));
        const QSignalBlocker b(preset_);
        preset_->setCurrentIndex(idx);
    });
    connect(apply, &QPushButton::clicked, this,
            [this] { emit applyRequested(collect()); });
    connect(listen, &QPushButton::clicked, this, [this] {
        emit listenRequested(slot_->currentData().toInt(), collect());
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void VoiceEqDialog::setStatus(const QString& text) { status_->setText(text); }

void VoiceEqDialog::load(const VoiceEqSettings& s) {
    loading_ = true;
    enabled_->setChecked(s.enabled);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const auto& b = s.bands[size_t(i)];
        Row& row = rows_[i];
        row.on->setChecked(b.enabled);
        row.hz->setValue(int(std::lround(b.freqHz)));
        if (row.db) row.db->setValue(b.gainDb);
        if (row.q) row.q->setValue(b.q);
        if (row.slope)
            row.slope->setCurrentIndex(
                std::max(0, row.slope->findData(b.slopeDbPerOct)));
    }
    loading_ = false;
    refreshEnables();
    curve_->setSettings(collect());
}

void VoiceEqDialog::refreshEnables() {
    const bool eqOn = enabled_->isChecked();
    for (Row& row : rows_) {
        row.on->setEnabled(eqOn);
        const bool live = eqOn && row.on->isChecked();
        row.hz->setEnabled(live);
        if (row.db) row.db->setEnabled(live);
        if (row.q) row.q->setEnabled(live);
        if (row.slope) row.slope->setEnabled(live);
    }
}

VoiceEqSettings VoiceEqDialog::collect() const {
    VoiceEqSettings s;
    s.enabled = enabled_->isChecked();
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        const Row& row = rows_[i];
        auto& b = s.bands[size_t(i)];
        b.type = VoiceEqSettings::slotType(i);
        b.enabled = row.on->isChecked();
        b.freqHz = float(row.hz->value());
        b.gainDb = row.db ? float(row.db->value()) : 0.f;
        b.q = row.q ? float(row.q->value()) : 0.707f;
        b.slopeDbPerOct = row.slope ? row.slope->currentData().toInt() : 12;
    }
    return s;
}

} // namespace ttc
