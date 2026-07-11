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
// TX monitor and AF volume. Slider drags are coalesced (~25/s trailing edge)
// like every other streamed control, so they can't flood the Orion's UART.
class TxBar : public QWidget {
    Q_OBJECT
public:
    explicit TxBar(QWidget* parent = nullptr);

    bool ampMode() const;
    int  ampLimit() const;

signals:
    void txPowerChanged(int pct);
    void ampModeChanged(bool on, int limitPct);   // toggle or limit spin
    void tuneRequested();
    void tunerEnableToggled(bool on);
    void micGainChanged(int pct);
    void monitorChanged(int pct);
    void afVolumeChanged(int pct);

public slots:
    void setAmpMode(bool on, int limitPct);       // restore from settings
    void showTxPower(int pct);
    void showMicGain(int pct);
    void showMonitor(int pct);
    void showAfVolume(int pct);
    void showTuner(bool on);

private:
    QSlider* makeSlider(const QString& name, QLabel*& value,
                        QTimer*& tx, int& pend, void (TxBar::*sig)(int));
    void applyPowerCap();                          // slider max follows amp limit

    QSlider* pwr_ = nullptr;   QLabel* pwrVal_ = nullptr;
    QSlider* mic_ = nullptr;   QLabel* micVal_ = nullptr;
    QSlider* mon_ = nullptr;   QLabel* monVal_ = nullptr;
    QSlider* af_  = nullptr;   QLabel* afVal_  = nullptr;
    QPushButton* ampBtn_   = nullptr;
    QSpinBox*    ampLimit_ = nullptr;
    QPushButton* tuneBtn_  = nullptr;
    QPushButton* tunerBtn_ = nullptr;
    QTimer* pwrTx_ = nullptr;  int pendPwr_ = -1;
    QTimer* micTx_ = nullptr;  int pendMic_ = -1;
    QTimer* monTx_ = nullptr;  int pendMon_ = -1;
    QTimer* afTx_  = nullptr;  int pendAf_  = -1;
};

} // namespace ttc
