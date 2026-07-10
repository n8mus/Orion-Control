// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include "app/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Ten-Tec SDR Console");
    ttc::MainWindow w;
    w.resize(900, 360);
    w.show();
    return app.exec();
}
