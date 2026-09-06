// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QList>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include "contest/ContestDb.h"
#include "contest/ContestEngine.h"

class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QSpinBox;

namespace ttc {

class CtyLookup;
class CwWindow;
class QtcDialog;
class RotorLink;

// The contest deck: the strip that replaces the WATERFALL while contest
// mode is on (the operator's sketch, 2026-09-05). The spectrum above it
// is the band map; this is everything the hands touch per QSO:
//
//   [ CW READ ] [ SUPER CHECK          ] [ CW TYPE ]
//              [ N1MM-style entry+Fkeys ]
//
// Keys are deliberately chord-free: F1–F11 macros (right-click a key to
// edit it), F12 = WIPE always, Enter = ESM, Esc = stop keying, ←/→ walk
// the panadapter spots while the call box is empty, PgUp/PgDn = speed.
// The heavier screens (log grid, setup/resume, Cabrillo, statistics)
// live in the contest manager window behind one button.
class ContestDeck : public QWidget {
    Q_OBJECT
public:
    ContestDeck(ContestDb* db, const CtyLookup* cty, CwWindow* cw,
                RotorLink* rotor, QWidget* parent = nullptr);

    void setRig(qint64 hz, const QString& adifMode);
    void setMasterScp(const QSet<QString>& scp) { scp_ = scp; }
    bool openContestId(qint64 id);
    bool contestActive() const { return contestId_ >= 0; }

    // Contest coloring for a panadapter spot: 'M' new mult, 'N' new,
    // 'W' worked/dupe, 'Z' zero points. 0 = leave the normal colors.
    char classifySpot(const QString& call) const;

public slots:
    void appendRead(const QString& text);   // decoded CW rides in here
    void prefillCall(const QString& call);  // spot click / arrow landing

signals:
    void walkSpots(int dir);                // ←/→ on an empty call box
    void openManagerRequested();            // "Contest log…"

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void openContest(qint64 id);
    void rebuildEntryFields();
    void refreshAll();
    void enterPressed();                    // ESM
    void execPlan(const QList<EsmAct>& plan, bool updateOnly);
    void updateEsmHint();
    void logNow();
    void wipe();                            // F12
    void keyFkey(int idx0);
    void editFkey(int idx0);                // right-click editor
    QString fkeySpec(int key) const;        // override -> def fallback
    void applyFkeyLabels();
    void onCallEdited();
    void historyPrefill();                  // space in the call box
    void refreshScp();
    void keyText(const QString& text);
    QString currentBand() const;
    void trace(const QString& line);

    ContestDb* db_;
    const CtyLookup* cty_;
    CwWindow* cw_;
    RotorLink* rotor_;

    qint64 contestId_ = -1;
    const ContestDef* def_ = nullptr;
    ContestRow row_;
    ContestContext ctx_;
    QList<ContestQso> qsos_;
    QList<CQsoValues> values_;
    ScoreBreakdown sb_;
    QSet<QString> scp_;
    bool runMode_ = true;
    bool esmOn_ = true;
    bool myCallSent_ = false, exchSent_ = false;
    double myLat_ = 0, myLon_ = 0;
    int hdg_ = -1;

    qint64 rigHz_ = 0;
    QString rigMode_ = "CW";

    // widgets
    QPlainTextEdit* read_ = nullptr;
    QWidget* scpRow_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* clock_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QSpinBox* wpm_ = nullptr;
    QLineEdit* call_ = nullptr;
    QLineEdit* rstS_ = nullptr;
    QLabel* sentNr_ = nullptr;
    QWidget* fieldsBox_ = nullptr;
    QList<QPair<ExchCol, QLineEdit*>> edits_;
    QLabel* dupe_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* hdgLbl_ = nullptr;
    QPushButton* fk_[12] = {};
    QLineEdit* type_ = nullptr;
    QLabel* sent_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* qtcBtn_ = nullptr;      // WAE only
    QtcDialog* qtc_ = nullptr;           // lazy
    QList<QShortcut*> shortcuts_;
    QTimer clockTimer_;
};

} // namespace ttc
