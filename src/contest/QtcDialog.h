// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QList>
#include <functional>

#include "contest/ContestDb.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;

namespace ttc {

// WAE QTC sending. The flow that survived a real contest weekend:
// work the station → open this → Load → Send all → Confirm & log when
// he acknowledges (Cancel if it falls apart — nothing is spent). The
// To box follows the deck's call box until the operator types over it.
// Confirm is all-or-nothing; confirmed QSOs freeze forever (enforced
// by ContestDb, not discipline). Esc stops the keyer — never closes.
class QtcDialog : public QDialog {
    Q_OBJECT
public:
    QtcDialog(ContestDb* db, std::function<void(const QString&)> keyFn,
              std::function<void()> stopFn, QWidget* parent = nullptr);

    void openFor(qint64 contestId);      // reset + show
    void setRigFreq(qint64 hz) { rigHz_ = hz; }
    // WAE RTTY (ContestDef::qtcBothWays): a Receive page beside Send, the
    // 10-per-station limit counts sent + received, the mode logged, and
    // the whole block goes out as one over with the header sent twice.
    void setBothWays(bool on, const QString& mode);
    // Why this station may not exchange QTCs with us (same continent...),
    // empty = fine. The deck supplies it; the dialog knows no geography.
    void setCallCheck(std::function<QString(const QString&)> refuse) {
        refuse_ = std::move(refuse);
    }
    // The RTTY print rides in here; on the Receive page it fills the grid.
    void appendRx(const QString& text);

public slots:
    void followCall(const QString& call);  // deck call box rides in

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void load();
    void sendHeader();
    void sendRow(int i);
    void sendAll();
    void confirmBlock();
    void cancelBlock();
    void refreshCounters();
    QString rowText(int i) const;          // "0030 DL1ABC 456"
    int usedWith(const QString& call) const;   // sent (+ received, RTTY)
    void showPage(bool receive);
    void fillFromRx();                     // parse rxBuf_ into the grid
    void clearReceive();
    void saveReceived();

    ContestDb* db_;
    std::function<void(const QString&)> keyFn_;
    std::function<void()> stopFn_;
    qint64 contestId_ = -1;
    qint64 rigHz_ = 0;
    int block_ = 0;
    bool manualTo_ = false;                // operator typed; stop following
    QList<ContestQso> alloc_;

    QLineEdit* to_ = nullptr;
    QLabel* sentTo_ = nullptr;
    QLabel* blockLbl_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* confirmBtn_ = nullptr;

    bool bothWays_ = false;
    QString mode_ = QStringLiteral("CW");
    std::function<QString(const QString&)> refuse_;
    QLabel* toLbl_ = nullptr;              // "To:" / "From:"
    QWidget* modeRow_ = nullptr;           // Send | Receive
    QPushButton* sendTab_ = nullptr;
    QPushButton* rcvTab_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLineEdit* series_ = nullptr;          // his "12/5"
    QTableWidget* rcvTable_ = nullptr;     // 10 x (time, call, nr), editable
    QLabel* rcvCount_ = nullptr;
    QString rxBuf_;                        // print since the page was cleared
    bool rcvTouched_ = false;              // operator edited: stop auto-fill
    bool filling_ = false;
};

} // namespace ttc
