// SPDX-License-Identifier: GPL-2.0-or-later
// Contest UI smoke harness, offscreen: the MANAGER creates a contest
// through its widgets, the DECK logs QSOs through the ESM flow, the QTC
// dialog loads/sends/confirms a block against a seeded WAE log, and
// screenshots come out for eyeballing. No keyer, no radio, no network.
//   QT_QPA_PLATFORM=offscreen ./contestuitest [out.png]
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>

#include "contest/ContestDb.h"
#include "contest/ContestDeck.h"
#include "contest/ContestWindow.h"
#include "contest/QtcDialog.h"
#include "util/CtyLookup.h"

using namespace ttc;

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("ok    %s\n", what);                               \
        } else {                                                           \
            std::printf("FAIL  %s\n", what);                               \
            ++fails;                                                       \
        }                                                                  \
    } while (0)

static QPushButton* buttonWithText(QWidget* w, const QString& t) {
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == t) return b;
    return nullptr;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/cfg").toUtf8());

    CtyLookup cty;
    bool ctyOk = false;
    for (const char* p : {"resources/cty.dat", "../resources/cty.dat"})
        if (cty.load(QString::fromLatin1(p))) { ctyOk = true; break; }
    CHECK(ctyOk, "cty.dat loads");

    ContestDb db;
    CHECK(db.open(tmp.path() + "/contest.sqlite"), "contest.db opens");

    // ---- MANAGER: create a CWT through the widgets ----------------------
    ContestWindow w(&db, &cty);
    qint64 openedId = -1;
    QObject::connect(&w, &ContestWindow::contestOpened,
                     [&openedId](qint64 id) { openedId = id; });
    auto* defPick = w.findChild<QComboBox*>("defPick");
    CHECK(defPick, "mgr: contest picker exists");
    if (!defPick) return 1;
    defPick->setCurrentIndex(defPick->findData("CW-OPS"));
    auto* sentExch = w.findChild<QLineEdit*>("sentExch");
    CHECK(sentExch && sentExch->text() == "Jon MI",
          "mgr: sent-exchange default follows the picker");
    QPushButton* start = buttonWithText(&w, "Start");
    CHECK(start, "mgr: Start button exists");
    start->click();
    CHECK(openedId > 0 && db.contests().size() == 1,
          "mgr: Start creates and opens the contest");
    const qint64 cid = openedId;

    // ---- DECK: the ESM flow logs QSOs -----------------------------------
    ContestDeck deck(&db, &cty, nullptr, nullptr);
    CHECK(deck.openContestId(cid), "deck opens the contest");
    deck.setRig(14032000, "CW");
    deck.setMasterScp({"N3JT", "N3JTX", "K6RB"});
    deck.show();

    auto* dCall = deck.findChild<QLineEdit*>("entryCall");
    auto* dEx0 = deck.findChild<QLineEdit*>("exchEdit0");
    auto* dEx1 = deck.findChild<QLineEdit*>("exchEdit1");
    CHECK(dCall && dEx0 && dEx1, "deck: entry fields exist");
    if (!dCall || !dEx0 || !dEx1) return 1;

    deck.prefillCall("N3JT");
    dEx0->setText("JIM");
    dEx1->setText("1");
    QMetaObject::invokeMethod(dCall, "returnPressed");  // answer beat
    QMetaObject::invokeMethod(dCall, "returnPressed");  // TU + log
    deck.prefillCall("K6RB");
    dEx0->setText("ROB");
    dEx1->setText("3");
    QMetaObject::invokeMethod(dCall, "returnPressed");
    QMetaObject::invokeMethod(dCall, "returnPressed");
    CHECK(db.qsos(cid).size() == 2, "deck: ESM logs two QSOs");
    CHECK(deck.classifySpot("N3JT") == 'W' &&
              deck.classifySpot("W9ZZZ") == 'M',
          "deck: classification tracks the log");

    // Manager's grid follows the deck through the changed() signal.
    auto* grid = w.findChild<QTableWidget*>();
    CHECK(grid && grid->rowCount() == 2, "mgr: log grid mirrors the deck");

    // ---- QTC: seeded WAE contest, full load/send/confirm ----------------
    ContestRow wae;
    wae.defId = "DARC-WAEDC-CW";
    wae.title = "WAE QTC ui";
    wae.startUtc = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const qint64 wid = db.createContest(wae);
    QList<qint64> ids;
    for (int i = 0; i < 12; ++i) {
        ContestQso q;
        q.contestId = wid;
        q.tsUtc = wae.startUtc.addSecs(300 * (i + 1));
        q.freqHz = 7024000;
        q.v.call = i == 2 ? "DL8WPX" : QString("DL%1AA").arg(i);
        q.v.band = "40M";
        q.v.mode = "CW";
        q.v.rstS = "599";
        q.v.rstR = "599";
        q.v.serialR = QString::number(100 + i);
        q.v.serialS = i + 1;
        ids << db.addQso(q);
    }
    QStringList keyed;
    QtcDialog qtc(&db, [&keyed](const QString& t) { keyed << t; },
                  [] {});
    qtc.openFor(wid);
    qtc.followCall("DL8WPX");
    QPushButton* load = buttonWithText(&qtc, "Load");
    CHECK(load, "qtcui: Load button exists");
    load->click();
    auto* qtable = qtc.findChild<QTableWidget*>();
    CHECK(qtable && qtable->rowCount() == 10,
          "qtcui: ten rows loaded, receiver's own QSO excluded");
    buttonWithText(&qtc, "Send all")->click();
    CHECK(keyed.size() == 11 && keyed.first() == "QTC 1/10",
          "qtcui: Send all keys the header + ten lines");
    CHECK(keyed[1].contains("DL0AA") && keyed[1].contains("100"),
          "qtcui: line format time-call-serial");
    buttonWithText(&qtc, "Confirm && log block")->click();
    CHECK(db.qtcCount(wid) == 10, "qtcui: confirm lands the block");
    CHECK(qtable->rowCount() == 0, "qtcui: table clears after confirm");
    load->click();
    CHECK(qtable->rowCount() == 0,
          "qtcui: station at the 10-QTC cap loads nothing");

    // ---- the arrow walk: empty box, spot landing, typing ----------------
    {
        int walked = 0;
        QObject::connect(&deck, &ContestDeck::walkSpots,
                         [&walked](int) { ++walked; });
        auto* wCall = deck.findChild<QLineEdit*>("entryCall");
        wCall->clear();
        wCall->setFocus();
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &right);
        CHECK(walked == 1, "walk: empty call box, → walks");
        // A landing fills the box — the arrows must KEEP walking.
        deck.prefillCall("DL8WPX");
        QKeyEvent right2(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &right2);
        CHECK(walked == 2, "walk: rolls onward from an untouched landing");
        // The first typed character hands the arrows back to the cursor.
        QKeyEvent typeD(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, "D");
        QCoreApplication::sendEvent(wCall, &typeD);
        QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &left);
        CHECK(walked == 2, "walk: typing reclaims the arrows for editing");
        wCall->clear();
    }

    // ---- phone contest: F-keys play voice slots -------------------------
    {
        ContestRow ssb;
        ssb.defId = "CQ-WW-SSB";
        ssb.title = "CQWW SSB ui";
        ssb.startUtc = QDateTime::currentDateTimeUtc();
        ssb.sentExch = "4";
        const qint64 sid = db.createContest(ssb);
        ContestDeck vdeck(&db, &cty, nullptr, nullptr);
        CHECK(vdeck.openContestId(sid), "voiceui: SSB contest opens");
        vdeck.setRig(14250000, "SSB");
        vdeck.show();
        QList<int> played;
        vdeck.setVoiceKeyer([&played](int s) { played << s; }, [] {});
        QPushButton* f1 = nullptr;
        for (QPushButton* b : vdeck.findChildren<QPushButton*>())
            if (b->text().startsWith("F1\n")) f1 = b;
        CHECK(f1 && f1->isEnabled(), "voiceui: F1 is the CQ voice key");
        f1->click();
        CHECK(played == QList<int>{0},
              "voiceui: F1 plays VK1 (slot 0) instead of keying CW");
        // ESM on an empty call box: Enter = CQ = the same voice slot.
        auto* vCall = vdeck.findChild<QLineEdit*>("entryCall");
        QMetaObject::invokeMethod(vCall, "returnPressed");
        CHECK(played.size() == 2 && played.last() == 0,
              "voiceui: ESM Enter plays the CQ message");
        QPushButton* f2 = nullptr;
        for (QPushButton* b : vdeck.findChildren<QPushButton*>())
            if (b->text().startsWith("F2\n")) f2 = b;
        CHECK(f2 && !f2->isEnabled(),
              "voiceui: F2 (his call) is blank on phone — you speak it");
    }

    // ---- screenshots ----------------------------------------------------
    deck.prefillCall("N3J");
    deck.resize(1900, 270);
    QCoreApplication::processEvents();
    QPushButton* scpHit = nullptr;
    for (QPushButton* b : deck.findChildren<QPushButton*>())
        if (b->text() == "N3JT") scpHit = b;
    CHECK(scpHit && scpHit->isVisible(),
          "deck: super check offers N3JT for partial N3J");
    const QString png = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : tmp.path() + "/contestui.png";
    CHECK(deck.grab().save(png), "deck screenshot saved");
    CHECK(w.grab().save(png + ".mgr.png"), "manager screenshot saved");
    std::printf("      -> %s\n", qPrintable(png));

    std::printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
