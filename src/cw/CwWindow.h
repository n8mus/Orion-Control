// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QLabel;
class QSpinBox;
class QPushButton;
class QCheckBox;
class QUdpSocket;

namespace ttc {

class WinKeyer;

// CW sending window (the CWX idea): type-ahead line, four macro memories,
// speed control synced both ways with the WinKeyer pot, TUNE and STOP.
// The paddle always wins — the keyer halts buffered sending in hardware
// the instant it's touched, and we just clear the display.
//
// Macros support %mc (my call, DISPLAY panel) and %c (his call, LOG panel).
// Defaults mirror the operator's cqrlog CW F-keys. Right-click a memory
// button to edit it; texts persist in QSettings (cw/mem1..4).
//
// While the keyer is open the window also serves the cwdaemon UDP protocol
// (default 127.0.0.1:6789): plain text = send, ESC-4 = abort, ESC-2<n> =
// speed. Point cqrlog's CW interface at cwdaemon and both programs can key
// through the one WinKeyer, console arbitrating — same single-master
// pattern as rig control on :4532.
class CwWindow : public QDialog {
    Q_OBJECT
public:
    explicit CwWindow(QWidget* parent = nullptr);

    void setMyCall(const QString& call)  { myCall_ = call; }
    void setHisCall(const QString& call) { hisCall_ = call; }
    void openKeyer();                    // connect + handshake (idempotent)

protected:
    void showEvent(QShowEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QString substitute(QString t) const; // %mc / %c
    void sendText(const QString& t);
    void editMemory(int i);
    void updateStatus(const QString& s = QString());

    WinKeyer* wk_ = nullptr;
    QUdpSocket* daemon_ = nullptr;       // cwdaemon-protocol server
    QLineEdit* line_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* sentView_ = nullptr;         // what's queued/sent this over
    QSpinBox* wpm_ = nullptr;
    QPushButton* mem_[4] = {};
    QPushButton* tuneBtn_ = nullptr;
    QCheckBox* live_ = nullptr;          // stream keystrokes as typed
    QString myCall_, hisCall_;
    int prevLen_ = 0;                    // live-mode: chars already streamed
};

} // namespace ttc
