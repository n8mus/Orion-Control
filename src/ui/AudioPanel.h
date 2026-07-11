// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include "radio/RadioController.h"   // Rx

class QSlider;
class QLabel;
class QToolButton;
class QTimer;

namespace ttc {

// Audio popup for the AUDIO toolbar dropdown: per-receiver volume + mute,
// and the Orion's output routing matrix —
//
//   VOL A  ────────●──  43   [MUTE]
//   VOL B  ──●──────── 100   [MUTE]
//            A    B   A+B
//   PH L    [x]  [ ]  [ ]      <- one VFO per ear for split work
//   PH R    [ ]  [x]  [ ]
//   SPKR    [ ]  [ ]  [x]
//
// Volume changes are coalesced (~16/sec) before emitting. Mute is console
// logic (owner remembers the level and sends volume 0). show*() never emit.
class AudioPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioPanel(QWidget* parent = nullptr);

    void showVolume(Rx rx, int pct);
    void showMute(Rx rx, bool on);
    void showRouting(char left, char right, char speaker);
    bool isMuted(Rx rx) const;

signals:
    void volumeEdited(Rx rx, int pct);
    void muteToggled(Rx rx, bool on);
    void routingEdited(char left, char right, char speaker);

private:
    void emitRouting();
    QToolButton* cell(const QString& tip);

    QSlider*     vol_[2]    = {};   // [0]=A/main, [1]=B/sub
    QLabel*      volVal_[2] = {};
    QToolButton* mute_[2]   = {};
    QToolButton* route_[3][3] = {}; // [row L/R/SPKR][col A/B/A+B]
    QTimer*      volTx_[2]  = {};   // coalesce slider streams
    int          pendVol_[2] = {0, 0};
    bool         updating_ = false;
};

} // namespace ttc
