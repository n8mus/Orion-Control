// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QIcon>
#include <QTimer>
#include <cstdlib>
#include "app/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("n8mus");     // QSettings (band memories etc.)
    QApplication::setApplicationName("tentec-console");
    QApplication::setWindowIcon(QIcon(":/icon.png"));
    ttc::MainWindow w;
    w.resize(1360, 600);                   // room for the dual-VFO/routing strip
    w.show();

    // Headless self-test: TTC_SELFTEST=<seconds> runs the full pipeline then quits
    // cleanly (so the SDR device is released). Used for CI / no-display verification.
    // TTC_SCREENSHOT=<path.png> additionally grabs the window just before quitting
    // (works on the offscreen platform) for layout review without a display.
    if (const char* s = std::getenv("TTC_SELFTEST")) {
        QTimer::singleShot(atoi(s) * 1000, &app, [&app, &w] {
            if (const char* shot = std::getenv("TTC_SCREENSHOT"))
                w.grab().save(QString::fromLocal8Bit(shot));
            app.quit();
        });
    }

    return app.exec();
}
