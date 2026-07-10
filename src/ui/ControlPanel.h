// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include "radio/RadioController.h"

class QButtonGroup;
class QPushButton;
class QSlider;
class QLabel;
class QTimer;

namespace ttc {

// Right-hand control sidebar: mode, AGC, attenuator, RF gain, NR/NB/auto-notch.
// Emits user intent; MainWindow forwards to the radio. show*() slots sync the
// widgets FROM radio polls without re-emitting (blockSignals), so front-panel
// changes reflect here and there is no feedback loop.
class ControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlPanel(QWidget* parent = nullptr);

signals:
    void bandSelected(int bandIdx);       // index into kBands (app/Bands.h)
    void modeSelected(Mode m);
    void agcSelected(char agc);           // 'F','M','S','P','O'
    void attenSelected(int step);         // 0..3 = off/6/12/18 dB
    void rfGainChanged(int gain);         // 0..100, coalesced while sliding
    void nrChanged(int level);            // 0..9, 0 = off
    void nbChanged(int level);
    void autoNotchChanged(int level);
    void notchToggled(bool on);           // manual notch engage
    void hwNbToggled(bool on);            // hardware noise blanker

public slots:
    void showBand(int bandIdx);           // -1 = out of every band
    void showBandStack(int regIdx);       // 0..3 = A..D, -1 = none
    void showMode(Mode m);
    void showAgc(char agc);
    void showAtten(int step);
    void showRfGain(int gain);
    // These only ever fire if the radio answers the speculative ?R.NN/NB/NA
    // queries — the sliders then reflect true radio state instead of "off".
    void showNr(int level);
    void showNb(int level);
    void showAutoNotch(int level);
    void showNotch(bool on, int centerHz, int widthHz);
    void showHwNb(bool on);

private:
    QWidget* makeDspRow(const QString& name, QSlider*& slider, QLabel*& value);

    QButtonGroup* bandGroup_  = nullptr;
    QLabel*       stackLbl_   = nullptr;
    QButtonGroup* modeGroup_  = nullptr;
    QButtonGroup* agcGroup_   = nullptr;
    QButtonGroup* attenGroup_ = nullptr;
    QSlider* rfGain_ = nullptr;   QLabel* rfGainVal_ = nullptr;
    QSlider* nr_     = nullptr;   QLabel* nrVal_     = nullptr;
    QSlider* nb_     = nullptr;   QLabel* nbVal_     = nullptr;
    QSlider* an_     = nullptr;   QLabel* anVal_     = nullptr;
    QPushButton* notchBtn_ = nullptr;  QLabel* notchVal_ = nullptr;
    QPushButton* hwNbBtn_  = nullptr;
    // Trailing-edge coalescer for slider drags (the Orion's UART services
    // commands on a ~100 ms cycle; raw slider-move rates would flood it).
    QTimer* gainTx_ = nullptr;
    int pendGain_ = -1;
};

} // namespace ttc
