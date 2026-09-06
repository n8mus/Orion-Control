// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QList>

#include "contest/ContestDb.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QWidget;

namespace ttc {

class CtyLookup;

// The contest MANAGER — the between-runs screen. Per-QSO operating
// lives on the ContestDeck; this window starts/resumes contests, shows
// the full log with edit/delete (both refused for QSOs already reported
// in a QTC — the receiving station holds a copy of what we logged), and
// writes the verified Cabrillo.
class ContestWindow : public QDialog {
    Q_OBJECT
public:
    ContestWindow(ContestDb* db, const CtyLookup* cty,
                  QWidget* parent = nullptr);

    bool openContestId(qint64 id);

signals:
    // The deck follows whichever contest this manager opens or creates.
    void contestOpened(qint64 id);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    void refreshResumeList();
    void newContest();
    void openContest(qint64 id);
    void refreshAll();
    void editSelected();
    void deleteSelected();
    void exportCabrillo();
    qint64 selectedQsoId() const;
    void trace(const QString& line);

    ContestDb* db_;
    const CtyLookup* cty_;
    qint64 contestId_ = -1;
    const ContestDef* def_ = nullptr;
    ContestRow row_;
    ContestContext ctx_;
    QList<ContestQso> qsos_;

    QComboBox* defPick_ = nullptr;
    QComboBox* resumePick_ = nullptr;
    QLineEdit* sentExchEdit_ = nullptr;
    QLineEdit* locationEdit_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* score_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace ttc
