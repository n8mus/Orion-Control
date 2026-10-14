// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QWidget>

#include "audio/VoiceEq.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTimer;

namespace ttc {

// The EQ response, drawn over the voice range with the Orion's own TX
// passband edge marked: what the PC shapes inside, what the radio cuts.
class VoiceEqCurve : public QWidget {
public:
    explicit VoiceEqCurve(QWidget* parent = nullptr);
    void setSettings(const VoiceEqSettings& s) { s_ = s; update(); }
    void setTarget(int idx) { target_ = idx; update(); }   // voiceEqTargets()
    QSize sizeHint() const override { return {560, 200}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    VoiceEqSettings s_;
    int target_ = 0;
};

// Voice keyer EQ editor (AUDIO ▾ → Voice keyer EQ…). Non-modal and LIVE:
// every edit (preset, checkbox, number) re-renders the VK messages from
// their untouched takes ~0.4 s after the last change — no Apply step
// (live-found: with an Apply button, preset changes that were never
// applied went out unchanged and sounded like "the EQ does nothing").
// Owns no files; MainWindow does the rendering on applyRequested.
class VoiceEqDialog : public QDialog {
    Q_OBJECT
public:
    explicit VoiceEqDialog(const VoiceEqSettings& s, QWidget* parent = nullptr);
    void setStatus(const QString& text);

signals:
    void applyRequested(const ttc::VoiceEqSettings& s);
    // Apply first, then play this slot on the PC speakers (nothing keyed).
    void listenRequested(int slot, const ttc::VoiceEqSettings& s);

private:
    VoiceEqSettings collect() const;
    void load(const VoiceEqSettings& s);
    void fillProfiles(const QString& select);      // built-ins + the operator's
    void setActiveProfile(const QString& key);     // remembered across opens
    void refreshEnables();

    QWidget* buildDynamicsPage();
    // Dynamics controls (gate -> EQ -> de-esser -> compressor -> limiter).
    struct Dyn {
        QCheckBox *gateOn = nullptr, *deessOn = nullptr, *compOn = nullptr,
                  *limOn = nullptr, *limDc = nullptr;
        QDoubleSpinBox *gateThr = nullptr, *gateRatio = nullptr,
                       *gateFloor = nullptr, *gateHold = nullptr,
                       *gateRel = nullptr;
        QDoubleSpinBox *deHz = nullptr, *deThr = nullptr, *deAmt = nullptr;
        QDoubleSpinBox *cThr = nullptr, *cRatio = nullptr, *cAtt = nullptr,
                       *cRel = nullptr, *cKnee = nullptr, *cDrive = nullptr;
        QSpinBox *cPhase = nullptr;
        QDoubleSpinBox *limCeil = nullptr;
    } dyn_;
    VoiceEqSettings base_;                         // carries fields not on screen
    struct Row {
        QCheckBox*      on    = nullptr;
        QSpinBox*       hz    = nullptr;
        QDoubleSpinBox* db    = nullptr;   // shelves + peaks
        QDoubleSpinBox* q     = nullptr;   // shelves + peaks
        QComboBox*      slope = nullptr;   // high-/low-pass
    };
    Row          rows_[VoiceEqSettings::kBands];
    QCheckBox*   enabled_ = nullptr;
    QComboBox*   preset_  = nullptr;
    QComboBox*   slot_    = nullptr;
    QComboBox*   target_  = nullptr;
    VoiceEqCurve* curve_  = nullptr;
    QLabel*      status_  = nullptr;
    QTimer*      applyTimer_ = nullptr;           // debounces live apply
    bool         loading_ = false;
};

} // namespace ttc
