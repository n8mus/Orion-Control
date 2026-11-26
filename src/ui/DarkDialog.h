// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace ttc {

// The console's dark dialog look (the tuner dialogs' and the QTC window's),
// one sheet for every plain QDialog / QMessageBox in the contest panels.
inline QString darkDialogCss() {
    return QStringLiteral(
        "QDialog, QMessageBox { background: #11161d; }"
        "QLabel, QCheckBox, QRadioButton, QGroupBox { color: #c8d4e0; }"
        "QGroupBox { border: 1px solid #2a3644; margin-top: 8px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #8fa3b8; }"
        "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTextEdit,"
        " QPlainTextEdit, QListWidget { background: #1c2430; color: #e8eef5;"
        " border: 1px solid #2a3644; padding: 3px 6px;"
        " selection-background-color: #3a4a5c; selection-color: #ffffff; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #e8eef5;"
        " selection-background-color: #2b5c8a; }"
        "QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid"
        " #2a3644; padding: 4px 10px; }"
        "QPushButton:hover { background: #243040; }"
        "QPushButton:default { border-color: #2b5c8a; }"
        "QPushButton:disabled { color: #4a5868; }"
        "QTableWidget { background: #0b1016; color: #c8d4e0; border: 1px"
        " solid #2a3644; gridline-color: #1c2430;"
        " selection-background-color: #2b5c8a; selection-color: #ffffff; }"
        "QHeaderView::section { background: #1c2430; color: #8fa3b8;"
        " border: none; padding: 3px 8px; font-weight: bold; }"
        "QTableCornerButton::section { background: #1c2430; border: none; }");
}

} // namespace ttc
