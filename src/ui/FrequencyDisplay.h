// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <cstdint>

class QLineEdit;

namespace ttc {

// Flex-style frequency readout: big "14.200.000" digits.
//   * wheel over a digit  -> step that decade (the whole dial is a tune knob)
//   * click top/bottom half of a digit -> +/- that decade
//   * double-click        -> inline type-in entry (MHz, kHz or Hz accepted)
// setFrequency() feeds it from the radio; user changes emit frequencyEdited().
class FrequencyDisplay : public QWidget {
    Q_OBJECT
public:
    explicit FrequencyDisplay(QWidget* parent = nullptr);

    void setFrequency(uint64_t hz);

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
    QLineEdit* edit_ = nullptr;
};

} // namespace ttc
