// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QElapsedTimer>

namespace ttc {

// SmartSDR-style horizontal S-meter bar. Fed raw Orion ?S readings; converts
// to dB-relative-to-S9 via the hamlib TT565 V2 calibration table (firmware
// 2.x/3.x scale), which we can re-fit against the real radio if it reads off.
class SMeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit SMeterWidget(QWidget* parent = nullptr);

public slots:
    void setRawLevel(int raw);            // raw @SRM units from the radio

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static double rawToDbS9(int raw);     // piecewise-linear cal table
    double dbS9_    = -120.0;             // current level, dB relative to S9
    double peakDb_  = -120.0;             // peak-hold marker
    QElapsedTimer sincePeak_;
    bool haveReading_ = false;
};

} // namespace ttc
