// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace ttc {

// Bottom transmit strip: power (with amplifier drive limit), tuner, mic gain,
// TX filter bandwidth and speech processor, plus four one-click
// TX profile buttons (KE9NS TXProfile idea: bundle the whole transmit-audio
// setup — left-click recalls, right-click saves the current settings into
// that slot). Slider drags are coalesced (~25/s trailing edge) like every
// other streamed control, so they can't flood the Orion's UART. (AF volume
// lives under each VFO readout in the top strip.)
class TxBar : public QWidget {
    Q_OBJECT
public:
    explicit TxBar(QWidget* parent = nullptr);

    bool ampMode() const;
    int  ampLimit() const;
    int  tuneLevel() const;                       // carrier watts for manual tune

signals:
    void txPowerChanged(int pct);
    void ampModeChanged(bool on, int limitPct);   // toggle or limit spin
    void tuneToggled(bool on);                    // manual tune carrier on/off
    void tuneLevelChanged(int watts);
    void tunerEnableToggled(bool on);
    void micGainChanged(int pct);
    void txFilterChanged(int hz);                 // TX BW slider (900-3900)
    void speechProcChanged(int level);            // PROC slider (0 = off)
    void profileRecalled(int slot);               // left-click a profile button
    void profileSaveRequested(int slot);          // right-click: store current

public slots:
    void setAmpMode(bool on, int limitPct);       // restore from settings
    void setTuneLevel(int watts);                 // restore from settings
    void showTuneActive(bool on);                 // reflect/force the TUNE latch
    void showTxPower(int pct);
    void showMicGain(int pct);
    void showTxFilter(int hz);
    void showSpeechProc(int level);
    void showTuner(bool on);

private:
    QSlider* makeSlider(const QString& name, QLabel*& value,
                        QTimer*& tx, int& pend, void (TxBar::*sig)(int));
    void applyPowerCap();                          // slider max follows amp limit
    void markProfile(int slot);                    // check one button (-1 = none)

    QSlider* pwr_ = nullptr;   QLabel* pwrVal_ = nullptr;
    QSlider* mic_ = nullptr;   QLabel* micVal_ = nullptr;
    QSlider* txbw_ = nullptr;  QLabel* txbwVal_ = nullptr;
    QSlider* proc_ = nullptr;  QLabel* procVal_ = nullptr;
    QPushButton* profBtn_[4] = {};
    QPushButton* ampBtn_   = nullptr;
    QSpinBox*    ampLimit_ = nullptr;
    QPushButton* tuneBtn_  = nullptr;
    QSpinBox*    tuneLvl_  = nullptr;
    QPushButton* tunerBtn_ = nullptr;
    QTimer* pwrTx_ = nullptr;  int pendPwr_ = -1;
    QTimer* micTx_ = nullptr;  int pendMic_ = -1;
    QTimer* txbwTx_ = nullptr; int pendTxbw_ = -1;
    QTimer* procTx_ = nullptr; int pendProc_ = -1;
};

} // namespace ttc
