// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QList>
#include <QPair>
#include <QTimer>

#include "contest/ContestDb.h"
#include "contest/ContestEngine.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QWidget;

namespace ttc {

class CtyLookup;
class CwWindow;

// The contest operating window: entry fields shaped by the loaded
// contest's definition, F1–F12 keying through the console's own keyer
// (in-process — works with the CW window closed), dupe check, live
// score, the contest log, and Cabrillo export with parse-back
// verification. Phase 1: Run/S&P sets and Enter-logs; the full ESM
// three-beat, check-partial and bandmap ride the next phases.
//
// Everything here is deliberately stand-alone: its QSOs live in
// contest.db, never touch the station logbook, and nothing uploads
// anywhere. The contest's product is the Cabrillo file.
class ContestWindow : public QDialog {
    Q_OBJECT
public:
    ContestWindow(ContestDb* db, const CtyLookup* cty, CwWindow* cw,
                  QWidget* parent = nullptr);

    // Dial and mode ride in from MainWindow's poll, same as LogWindow.
    void setRig(qint64 hz, const QString& adifMode);

    // Programmatic resume — MainWindow restore paths and the offscreen
    // UI harness (contestuitest).
    bool openContestId(qint64 id);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    void refreshResumeList();
    void newContest();
    void openContest(qint64 id);
    void rebuildEntryFields();
    void refreshAll();               // table + score + serial display
    void refreshScore();
    void tryLog();                   // Enter
    void wipe();                     // Ctrl+W
    void keyFkey(int idx0);          // F1..F12 (0-based)
    void applyFkeyLabels();
    void onCallEdited();
    void exportCabrillo();
    void setSpeed(int wpm);
    QString currentBand() const;
    void trace(const QString& line); // contest-trace.log, always-on

    ContestDb* db_;
    const CtyLookup* cty_;
    CwWindow* cw_;

    qint64 contestId_ = -1;
    const ContestDef* def_ = nullptr;
    ContestRow row_;
    ContestContext ctx_;
    QList<ContestQso> qsos_;         // cache, refreshed on changed()
    QList<CQsoValues> values_;
    bool runMode_ = true;

    qint64 rigHz_ = 0;
    QString rigMode_ = "CW";

    // setup strip (visible until a contest is open)
    QWidget* setupStrip_ = nullptr;
    QComboBox* defPick_ = nullptr;
    QComboBox* resumePick_ = nullptr;
    QLineEdit* sentExchEdit_ = nullptr;
    QLineEdit* locationEdit_ = nullptr;

    // header
    QLabel* title_ = nullptr;
    QLabel* clock_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QSpinBox* wpm_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* rate_ = nullptr;

    // entry
    QWidget* entryBox_ = nullptr;
    QLineEdit* call_ = nullptr;
    QLineEdit* rstS_ = nullptr;      // null when the contest has no RST
    QLabel* sentNr_ = nullptr;       // null when no running serial
    QWidget* fieldsBox_ = nullptr;
    QList<QPair<ExchCol, QLineEdit*>> edits_;
    QLabel* info_ = nullptr;
    QLabel* dupe_ = nullptr;

    QPushButton* fk_[12] = {};
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer clockTimer_;
};

} // namespace ttc
