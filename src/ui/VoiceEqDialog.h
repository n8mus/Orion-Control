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

namespace ttc {

// The EQ response, drawn over the voice range with the Orion's own TX
// passband edge marked: what the PC shapes inside, what the radio cuts.
class VoiceEqCurve : public QWidget {
public:
    explicit VoiceEqCurve(QWidget* parent = nullptr);
    void setSettings(const VoiceEqSettings& s) { s_ = s; update(); }
    QSize sizeHint() const override { return {560, 200}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    VoiceEqSettings s_;
};

// Voice keyer EQ editor (AUDIO ▾ → Voice keyer EQ…). Non-modal: edit,
// Apply (re-renders every VK message from its untouched take), listen on
// the speakers, adjust, repeat — then transmit with the VK buttons as
// usual. Owns no files; MainWindow does the rendering on applyRequested.
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
    void refreshEnables();

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
    VoiceEqCurve* curve_  = nullptr;
    QLabel*      status_  = nullptr;
    bool         loading_ = false;
};

} // namespace ttc
