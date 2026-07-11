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
    void samToggled(bool on);             // pseudo-sync-AM (ECSS) pseudo-mode
    void digitalToggled(bool on);         // line-in vs mic audio switch
    void agcSelected(char agc);           // 'F','M','S','P','O'
    void agcThresholdChanged(int uv);     // programmable-AGC threshold slider
    void agcHangChanged(int tenths);      // hang time, tenths of a second (0 = off)
    void agcDecayChanged(int rate);       // decay rate (radio min 5)
    void attenSelected(int step);         // 0..3 = off/6/12/18 dB
    void preampToggled(bool on);          // front-end preamp
    void rfGainChanged(int gain);         // 0..100, coalesced while sliding
    void nrChanged(int level);            // 0..9, 0 = off
    void nbChanged(int level);
    void autoNotchChanged(int level);
    void notchToggled(bool on);           // manual notch engage
    void safToggled(bool on);             // SAF peak filter (shares the notch engine)
    void hwNbToggled(bool on);            // hardware noise blanker
    void pbtZeroRequested();              // "PBT 0" button
    void pbtChanged(int hz);              // slider, coalesced while sliding

public slots:
    void showBand(int bandIdx);           // -1 = out of every band
    void showBandStack(int regIdx);       // 0..3 = A..D, -1 = none
    void showBandStackText(const QString& text);  // channelized bands (60 m)
    void showMode(Mode m);
    void showSam(bool on);                // no emit
    void setSamLabel(const QString& t);   // "SAM" / "SAM-U" / "SAM-L"
    void showDigital(bool on);            // no emit
    void clearModeSelection();            // SAM presents as its own mode
    void showAgc(char agc);
    void showAgcThreshold(double uv);     // radio's actual (quantized) values
    void showAgcHang(double sec);
    void showAgcDecay(int rate);
    void showAtten(int step);
    void showPreamp(bool on);
    void showRfGain(int gain);
    // These only ever fire if the radio answers the speculative ?R.NN/NB/NA
    // queries — the sliders then reflect true radio state instead of "off".
    void showNr(int level);
    void showNb(int level);
    void showAutoNotch(int level);
    void showNotch(bool on, int centerHz, int widthHz);
    void showSaf(bool on);
    void showHwNb(bool on);
    void showPbt(int pbtHz);              // live off-center readout

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
    QPushButton* safBtn_   = nullptr;
    QPushButton* hwNbBtn_  = nullptr;
    QPushButton* preBtn_   = nullptr;
    QPushButton* samBtn_   = nullptr;
    QPushButton* digBtn_   = nullptr;
    QLabel* pbtVal_ = nullptr;
    QSlider* pbt_   = nullptr;
    // Trailing-edge coalescer for slider drags (the Orion's UART services
    // commands on a ~100 ms cycle; raw slider-move rates would flood it).
    QTimer* gainTx_ = nullptr;
    int pendGain_ = -1;
    QTimer* pbtTx_ = nullptr;
    int pendPbt_ = -9999;
    QSlider* agcThr_   = nullptr;  QLabel* agcThrVal_   = nullptr;
    QSlider* agcHang_  = nullptr;  QLabel* agcHangVal_  = nullptr;
    QSlider* agcDecay_ = nullptr;  QLabel* agcDecayVal_ = nullptr;
    QTimer* agcThrTx_ = nullptr;   QTimer* agcHangTx_ = nullptr;
    QTimer* agcDecayTx_ = nullptr;
    int pendAgcThr_ = -1, pendAgcHang_ = -1, pendAgcDecay_ = -1;
};

} // namespace ttc
