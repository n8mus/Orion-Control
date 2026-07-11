// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QColor>
#include <cstdint>

class QLineEdit;

namespace ttc {

// KE9NS-style frequency readout: big accent-colored MHz/kHz digits, the Hz
// group smaller and white, leading zeros dropped ("14.237.000", "3.831.500").
//   * wheel over a digit  -> step that decade (the whole dial is a tune knob)
//   * click top/bottom half of a digit -> +/- that decade
//   * double-click        -> inline type-in entry (MHz, kHz or Hz accepted)
// setFrequency() feeds it from the radio; user changes emit frequencyEdited().
// An optional caption ("VFO A"/"VFO B") draws above the digits; accent is the
// digit color (KE9NS: amber for VFO A, green for VFO B).
class FrequencyDisplay : public QWidget {
    Q_OBJECT
public:
    explicit FrequencyDisplay(const QString& caption = QString(),
                              const QColor& accent = QColor(255, 210, 60),
                              QWidget* parent = nullptr);

    void setFrequency(uint64_t hz);
    void setAccent(const QColor& c);        // digit color (red = TX VFO)

signals:
    void frequencyEdited(uint64_t hz);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int  digitAt(int x) const;              // 0 = 10 MHz digit ... 7 = 1 Hz; -1 = none
    void bump(int digit, int direction);
    void beginEdit();
    void finishEdit();

    uint64_t hz_ = 14200000;
    QString  caption_;
    QColor   accent_;                       // MHz/kHz digit color
    int      digitTop_ = 0;                 // y offset of the digit row
    QLineEdit* edit_ = nullptr;
};

} // namespace ttc
