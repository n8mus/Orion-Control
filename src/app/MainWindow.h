// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include "radio/TenTecOrion.h"
#include "net/RigctldServer.h"
#include "ui/PanadapterWidget.h"

namespace ttc {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onTuneRequested(int offsetHz);
    void onPassbandChanged(int loHz, int hiHz);

private:
    TenTecOrion      radio_;
    RigctldServer    rigctld_{&radio_};
    PanadapterWidget* pan_ = nullptr;
    uint64_t centerHz_ = 14200000;
};

} // namespace ttc
