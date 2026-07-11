// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>

class QToolButton;

namespace ttc {

// Audio popup for the AUDIO toolbar dropdown: the Orion's output routing
// matrix (volume + mute live under the VFO readouts in the top strip) —
//
//            A    B   A+B
//   PH L    [x]  [ ]  [ ]      <- one VFO per ear for split work
//   PH R    [ ]  [x]  [ ]
//   SPKR    [ ]  [ ]  [x]
//
// showRouting() never emits.
class AudioPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioPanel(QWidget* parent = nullptr);

    void showRouting(char left, char right, char speaker);

signals:
    void routingEdited(char left, char right, char speaker);

private:
    void emitRouting();
    QToolButton* cell(const QString& tip);

    QToolButton* route_[3][3] = {}; // [row L/R/SPKR][col A/B/A+B]
    bool         updating_ = false;
};

} // namespace ttc
