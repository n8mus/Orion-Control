// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QTimer>
#include <cstdlib>
#include "app/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Ten-Tec SDR Console");
    ttc::MainWindow w;
    w.resize(900, 360);
    w.show();

    // Headless self-test: TTC_SELFTEST=<seconds> runs the full pipeline then quits
    // cleanly (so the SDR device is released). Used for CI / no-display verification.
    if (const char* s = std::getenv("TTC_SELFTEST"))
        QTimer::singleShot(atoi(s) * 1000, &app, &QApplication::quit);

    return app.exec();
}
