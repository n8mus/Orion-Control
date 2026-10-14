// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VoiceEqDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
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
    constexpr int kPts = 240;
    const auto& targets = voiceEqTargets();
    if (target_ > 0 && target_ < int(targets.size())) {
        const VoiceEqTarget& t = targets[size_t(target_)];
        QPainterPath tp;
        for (int i = 0; i <= kPts; ++i) {
            const float hz = kLoHz * std::pow(kHiHz / kLoHz, float(i) / kPts);
            const QPointF pt(xOf(hz), yOf(t.dbAt(hz)));
            if (i == 0) tp.moveTo(pt); else tp.lineTo(pt);
        }
        p.setPen(QPen(QColor(232, 176, 74, 200), 1.5, Qt::DashLine));
        p.drawPath(tp);
        p.drawText(QRectF(r.left() + 4, r.top() + 1, 200, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, "target: " + t.name);
    }
    QPainterPath path;
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
    setWindowTitle("Voice keyer audio — EQ & dynamics");
    setStyleSheet(kStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // Profiles: the built-in starting points, then the operator's own.
    // Save as… stores the curve on screen under a name; Delete removes one
    // of his (built-ins stay). The curve on screen is ALWAYS saved anyway
    // (live apply) — profiles are for keeping and switching between curves.
    auto* prof = new QHBoxLayout;
    prof->addWidget(new QLabel("Profile", this));
    preset_ = new QComboBox(this);
    preset_->setMinimumWidth(220);
    preset_->setToolTip("Built-in starting points, then your saved profiles.\n"
                        "Tune by ear: listen here, then on the Orion's MON\n"
                        "into a dummy load or at low power.");
    prof->addWidget(preset_, 1);
    auto* saveAs = new QPushButton("Save as…", this);
    saveAs->setToolTip("Save the curve on screen as one of your profiles");
    auto* del = new QPushButton("Delete", this);
    del->setToolTip("Delete the selected profile (your own only)");
    prof->addWidget(saveAs);
    prof->addWidget(del);
    lay->addLayout(prof);
    fillProfiles(QSettings().value("vk/eqProfile").toString());
    const auto syncDelete = [this, del] {
        del->setEnabled(preset_->currentData().toString().startsWith("u:"));
    };
    syncDelete();
    connect(preset_, &QComboBox::currentIndexChanged, this, syncDelete);

    // EQ tab: on/off + target, the curve, the eight bands.
    auto* tabs = new QTabWidget(this);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #2a3644; }"
        "QTabBar::tab { background: #1c2430; color: #8fa3b8; padding: 4px 14px;"
        " border: 1px solid #2a3644; }"
        "QTabBar::tab:selected { background: #26303e; color: #e8b04a; }");
    auto* eqPage = new QWidget(tabs);
    auto* eqLay = new QVBoxLayout(eqPage);
    eqLay->setContentsMargins(8, 8, 8, 8);
    auto* top = new QHBoxLayout;
    enabled_ = new QCheckBox("EQ on", eqPage);
    top->addWidget(enabled_);
    top->addStretch(1);
    top->addWidget(new QLabel("Target", eqPage));
    target_ = new QComboBox(eqPage);
    for (const VoiceEqTarget& t : voiceEqTargets()) target_->addItem(t.name);
    target_->setToolTip("A reference curve drawn behind yours (dashed) to\n"
                        "shape toward — display only. From AetherSDR.");
    top->addWidget(target_);
    eqLay->addLayout(top);
    curve_ = new VoiceEqCurve(eqPage);
    eqLay->addWidget(curve_);

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
    eqLay->addLayout(g);
    tabs->addTab(eqPage, "EQ");
    tabs->addTab(buildDynamicsPage(), "Dynamics");
    lay->addWidget(tabs);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    lay->addWidget(status_);

    auto* bRow = new QHBoxLayout;
    bRow->addWidget(new QLabel("Listen to", this));
    slot_ = new QComboBox(this);
    for (int i = 1; i <= 4; ++i) slot_->addItem(QString("VK%1").arg(i), i - 1);
    bRow->addWidget(slot_);
    auto* listen = new QPushButton("Listen", this);
    listen->setToolTip("Play the chosen message on the PC speakers with the\n"
                       "current curve (nothing keyed). Changes apply by\n"
                       "themselves — the VK buttons always send the curve\n"
                       "shown here.");
    listen->setDefault(true);
    bRow->addWidget(listen);
    bRow->addStretch(1);
    auto* close = new QPushButton("Close", this);
    bRow->addWidget(close);
    lay->addLayout(bRow);

    target_->setCurrentIndex(std::clamp(
        QSettings().value("vk/eqTarget", 0).toInt(), 0, target_->count() - 1));
    curve_->setTarget(target_->currentIndex());
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int i) {
        curve_->setTarget(i);
        QSettings().setValue("vk/eqTarget", i);
    });

    applyTimer_ = new QTimer(this);
    applyTimer_->setSingleShot(true);
    applyTimer_->setInterval(400);                 // spin-box scrubbing settles
    connect(applyTimer_, &QTimer::timeout, this,
            [this] { emit applyRequested(collect()); });

    load(s);

    // Any edit redraws the curve at once, drops the preset label to "—"
    // (the curve is now the operator's own), and re-renders shortly after.
    const auto edited = [this] {
        if (loading_) return;
        refreshEnables();
        curve_->setSettings(collect());
        const QSignalBlocker b(preset_);
        preset_->setCurrentIndex(0);
        setActiveProfile(QString());               // now his own, unsaved
        applyTimer_->start();
    };
    connect(enabled_, &QCheckBox::toggled, this, edited);
    for (QCheckBox* c : {dyn_.gateOn, dyn_.deessOn, dyn_.compOn, dyn_.limOn, dyn_.limDc})
        connect(c, &QCheckBox::toggled, this, edited);
    for (QDoubleSpinBox* d : {dyn_.gateThr, dyn_.gateRatio, dyn_.gateFloor,
                              dyn_.gateHold, dyn_.gateRel, dyn_.deHz, dyn_.deThr,
                              dyn_.deAmt, dyn_.cThr, dyn_.cRatio, dyn_.cAtt,
                              dyn_.cRel, dyn_.cKnee, dyn_.cDrive, dyn_.limCeil})
        connect(d, &QDoubleSpinBox::valueChanged, this, edited);
    connect(dyn_.cPhase, &QSpinBox::valueChanged, this, edited);
    for (Row& row : rows_) {
        connect(row.on, &QCheckBox::toggled, this, edited);
        connect(row.hz, &QSpinBox::valueChanged, this, edited);
        if (row.db) connect(row.db, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.q) connect(row.q, &QDoubleSpinBox::valueChanged, this, edited);
        if (row.slope)
            connect(row.slope, &QComboBox::currentIndexChanged, this, edited);
    }
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        const QString key = preset_->itemData(idx).toString();
        VoiceEqSettings s;
        if (key.startsWith("b:"))
            s = VoiceEqSettings::preset(VoiceEqSettings::Preset(key.mid(2).toInt()));
        else if (!key.startsWith("u:")
                 || !VoiceEqSettings::loadUserProfile(key.mid(2), s))
            return;                                // "—" or vanished
        load(s);
        const QSignalBlocker b(preset_);
        preset_->setCurrentIndex(idx);
        setActiveProfile(key);
        applyTimer_->start();
    });
    connect(saveAs, &QPushButton::clicked, this, [this] {
        const QString cur = preset_->currentData().toString();
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "Save EQ profile", "Profile name:", QLineEdit::Normal,
            cur.startsWith("u:") ? cur.mid(2) : QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        if (VoiceEqSettings::userProfileNames().contains(name)
            && QMessageBox::question(this, "Save EQ profile",
                   QString("Replace your profile \"%1\"?").arg(name))
                   != QMessageBox::Yes)
            return;
        VoiceEqSettings::saveUserProfile(name, collect());
        fillProfiles("u:" + name);
        setActiveProfile("u:" + name);
        setStatus(QString("Saved profile \"%1\".").arg(name));
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const QString key = preset_->currentData().toString();
        if (!key.startsWith("u:")) return;
        if (QMessageBox::question(this, "Delete EQ profile",
                QString("Delete your profile \"%1\"? (The curve on screen "
                        "stays.)").arg(key.mid(2))) != QMessageBox::Yes)
            return;
        VoiceEqSettings::deleteUserProfile(key.mid(2));
        fillProfiles(QString());
        setActiveProfile(QString());
        setStatus(QString("Deleted profile \"%1\".").arg(key.mid(2)));
    });
    connect(listen, &QPushButton::clicked, this, [this] {
        applyTimer_->stop();                       // listen applies it now
        emit listenRequested(slot_->currentData().toInt(), collect());
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void VoiceEqDialog::setStatus(const QString& text) { status_->setText(text); }

// Item data: "" = none/edited, "b:<n>" = built-in preset n, "u:<name>" =
// the operator's profile.
void VoiceEqDialog::fillProfiles(const QString& select) {
    const QSignalBlocker b(preset_);
    preset_->clear();
    preset_->addItem("—  (your current curve)", QString());
    const QStringList builtins = VoiceEqSettings::presetNames();
    for (int i = 0; i < builtins.size(); ++i)
        preset_->addItem(builtins[i], QString("b:%1").arg(i));
    const QStringList mine = VoiceEqSettings::userProfileNames();
    if (!mine.isEmpty()) preset_->insertSeparator(preset_->count());
    for (const QString& n : mine) preset_->addItem("★ " + n, "u:" + n);
    preset_->setCurrentIndex(std::max(0, preset_->findData(select)));
}

void VoiceEqDialog::setActiveProfile(const QString& key) {
    QSettings().setValue("vk/eqProfile", key);
}

namespace {
QDoubleSpinBox* dspin(QWidget* p, double lo, double hi, double step, int dec,
                      const QString& suffix, const QString& tip) {
    auto* d = new QDoubleSpinBox(p);
    d->setRange(lo, hi);
    d->setSingleStep(step);
    d->setDecimals(dec);
    d->setSuffix(suffix);
    d->setFixedWidth(100);
    d->setToolTip(tip);
    return d;
}
QGroupBox* stage(QWidget* p, const QString& title, QCheckBox*& on,
                 const QString& tip, QGridLayout*& g) {
    auto* box = new QGroupBox(title, p);
    g = new QGridLayout(box);
    g->setHorizontalSpacing(10);
    on = new QCheckBox("On", box);
    on->setToolTip(tip);
    g->addWidget(on, 0, 0);
    return box;
}
void field(QGridLayout* g, int row, int col, const QString& label, QWidget* w) {
    g->addWidget(new QLabel(label, w->parentWidget()), row, col * 2);
    g->addWidget(w, row, col * 2 + 1);
}
}  // namespace

// Dynamics tab, in chain order. Controls are the ported AetherSDR stages'
// own parameters; the rarely-touched ones (gate hysteresis/look-ahead,
// de-esser Q/slope, the compressor's internal limiter) keep their values
// from the loaded profile and are not shown.
QWidget* VoiceEqDialog::buildDynamicsPage() {
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 8, 8, 8);
    QGridLayout* g = nullptr;

    v->addWidget(stage(page, "NOISE GATE  (first)", dyn_.gateOn,
        "Turns the mic down between words — fan hum and room noise\n"
        "stay off the air. Ratio 2 = gentle expander, 10 = hard gate.", g));
    dyn_.gateThr   = dspin(page, -80, 0, 1, 0, " dB", "Opens above this level");
    dyn_.gateRatio = dspin(page, 1, 10, 0.5, 1, " :1", "2 = expander, 10 = gate");
    dyn_.gateFloor = dspin(page, -80, 0, 1, 0, " dB", "Most it ever turns down");
    dyn_.gateHold  = dspin(page, 0, 500, 10, 0, " ms", "Stays open this long after you stop");
    dyn_.gateRel   = dspin(page, 5, 2000, 10, 0, " ms", "How gently it closes");
    field(g, 1, 0, "Threshold", dyn_.gateThr);
    field(g, 1, 1, "Ratio", dyn_.gateRatio);
    field(g, 1, 2, "Floor", dyn_.gateFloor);
    field(g, 2, 0, "Hold", dyn_.gateHold);
    field(g, 2, 1, "Release", dyn_.gateRel);

    v->addWidget(stage(page, "DE-ESSER  (after the EQ)", dyn_.deessOn,
        "Cuts harsh 's' and 'sh' in their own band only — useful when\n"
        "the EQ boosts presence.", g));
    dyn_.deHz  = dspin(page, 1000, 12000, 100, 0, " Hz", "Sibilance band centre; SSB's is just under 3.9 kHz");
    dyn_.deThr = dspin(page, -60, 0, 1, 0, " dB", "Acts above this level in the band");
    dyn_.deAmt = dspin(page, -24, 0, 0.5, 1, " dB", "Most it cuts");
    field(g, 1, 0, "Frequency", dyn_.deHz);
    field(g, 1, 1, "Threshold", dyn_.deThr);
    field(g, 1, 2, "Max cut", dyn_.deAmt);

    v->addWidget(stage(page, "COMPRESSOR  (talk power)", dyn_.compOn,
        "Brings quiet syllables up toward loud ones — more average power\n"
        "at the same peak. Make-up gain is automatic for VK messages.", g));
    dyn_.cThr   = dspin(page, -60, 0, 1, 0, " dB", "Compresses above this level");
    dyn_.cRatio = dspin(page, 1, 20, 0.5, 1, " :1", "How hard (20 = limiter)");
    dyn_.cAtt   = dspin(page, 0.1, 200, 1, 1, " ms", "How fast it grabs (2-10 ms for voice)");
    dyn_.cRel   = dspin(page, 5, 2000, 10, 0, " ms", "How fast it lets go");
    dyn_.cKnee  = dspin(page, 0, 24, 1, 0, " dB", "Soft knee width (gentler onset)");
    dyn_.cDrive = dspin(page, 0, 18, 0.5, 1, " dB", "Pushes more voice into the compressor");
    dyn_.cPhase = new QSpinBox(page);
    dyn_.cPhase->setRange(0, 6);
    dyn_.cPhase->setFixedWidth(100);
    dyn_.cPhase->setToolTip("All-pass phase rotator stages (Orban/Optimod trick):\n"
                            "evens out lopsided voice peaks for 2-3 dB more\n"
                            "loudness, no tone change. 0 = off, 4 = broadcast");
    field(g, 1, 0, "Threshold", dyn_.cThr);
    field(g, 1, 1, "Ratio", dyn_.cRatio);
    field(g, 1, 2, "Knee", dyn_.cKnee);
    field(g, 2, 0, "Attack", dyn_.cAtt);
    field(g, 2, 1, "Release", dyn_.cRel);
    field(g, 2, 2, "Drive", dyn_.cDrive);
    field(g, 3, 0, "Phase rot.", dyn_.cPhase);

    v->addWidget(stage(page, "FINAL LIMITER  (last)", dyn_.limOn,
        "The safety net: nothing leaves above this ceiling.", g));
    dyn_.limCeil = dspin(page, -12, 0, 0.5, 1, " dB", "Absolute ceiling");
    dyn_.limDc = new QCheckBox("Remove DC offset (25 Hz)", page);
    field(g, 1, 0, "Ceiling", dyn_.limCeil);
    g->addWidget(dyn_.limDc, 1, 2, 1, 3);
    v->addStretch(1);
    return page;
}

void VoiceEqDialog::load(const VoiceEqSettings& s) {
    loading_ = true;
    base_ = s;
    enabled_->setChecked(s.enabled);
    dyn_.gateOn->setChecked(s.gate.on);
    dyn_.gateThr->setValue(s.gate.thresholdDb);
    dyn_.gateRatio->setValue(s.gate.ratio);
    dyn_.gateFloor->setValue(s.gate.floorDb);
    dyn_.gateHold->setValue(s.gate.holdMs);
    dyn_.gateRel->setValue(s.gate.releaseMs);
    dyn_.deessOn->setChecked(s.deess.on);
    dyn_.deHz->setValue(s.deess.freqHz);
    dyn_.deThr->setValue(s.deess.thresholdDb);
    dyn_.deAmt->setValue(s.deess.amountDb);
    dyn_.compOn->setChecked(s.comp.on);
    dyn_.cThr->setValue(s.comp.thresholdDb);
    dyn_.cRatio->setValue(s.comp.ratio);
    dyn_.cAtt->setValue(s.comp.attackMs);
    dyn_.cRel->setValue(s.comp.releaseMs);
    dyn_.cKnee->setValue(s.comp.kneeDb);
    dyn_.cDrive->setValue(s.comp.driveDb);
    dyn_.cPhase->setValue(s.comp.phaseStages);
    dyn_.limOn->setChecked(s.limit.on);
    dyn_.limCeil->setValue(s.limit.ceilingDb);
    dyn_.limDc->setChecked(s.limit.dcBlock);
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
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.gateThr, dyn_.gateRatio,
             dyn_.gateFloor, dyn_.gateHold, dyn_.gateRel})
        w->setEnabled(dyn_.gateOn->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.deHz, dyn_.deThr, dyn_.deAmt})
        w->setEnabled(dyn_.deessOn->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{dyn_.cThr, dyn_.cRatio, dyn_.cAtt,
             dyn_.cRel, dyn_.cKnee, dyn_.cDrive, dyn_.cPhase})
        w->setEnabled(dyn_.compOn->isChecked());
    dyn_.limCeil->setEnabled(dyn_.limOn->isChecked());
    dyn_.limDc->setEnabled(dyn_.limOn->isChecked());
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
    VoiceEqSettings s = base_;                     // hidden fields ride along
    s.enabled = enabled_->isChecked();
    s.gate.on = dyn_.gateOn->isChecked();
    s.gate.thresholdDb = float(dyn_.gateThr->value());
    s.gate.ratio = float(dyn_.gateRatio->value());
    s.gate.floorDb = float(dyn_.gateFloor->value());
    s.gate.holdMs = float(dyn_.gateHold->value());
    s.gate.releaseMs = float(dyn_.gateRel->value());
    s.deess.on = dyn_.deessOn->isChecked();
    s.deess.freqHz = float(dyn_.deHz->value());
    s.deess.thresholdDb = float(dyn_.deThr->value());
    s.deess.amountDb = float(dyn_.deAmt->value());
    s.comp.on = dyn_.compOn->isChecked();
    s.comp.thresholdDb = float(dyn_.cThr->value());
    s.comp.ratio = float(dyn_.cRatio->value());
    s.comp.attackMs = float(dyn_.cAtt->value());
    s.comp.releaseMs = float(dyn_.cRel->value());
    s.comp.kneeDb = float(dyn_.cKnee->value());
    s.comp.driveDb = float(dyn_.cDrive->value());
    s.comp.phaseStages = dyn_.cPhase->value();
    s.limit.on = dyn_.limOn->isChecked();
    s.limit.ceilingDb = float(dyn_.limCeil->value());
    s.limit.dcBlock = dyn_.limDc->isChecked();
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
