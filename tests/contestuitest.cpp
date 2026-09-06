// SPDX-License-Identifier: GPL-2.0-or-later
// Contest-window smoke harness: builds the real ContestWindow offscreen,
// opens a CWT instance, logs QSOs through the same widgets the operator
// uses, checks the database and score line, and grabs a PNG for
// eyeballing. No keyer, no radio, no network.
//   QT_QPA_PLATFORM=offscreen ./contestuitest [out.png]
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <cstdio>

#include "contest/ContestDb.h"
#include "contest/ContestDeck.h"
#include "contest/ContestWindow.h"
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    // Settings must not touch the operator's live config.
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/cfg").toUtf8());

    CtyLookup cty;
    bool ctyOk = false;
    for (const char* p : {"resources/cty.dat", "../resources/cty.dat"})
        if (cty.load(QString::fromLatin1(p))) { ctyOk = true; break; }
    CHECK(ctyOk, "cty.dat loads");

    ContestDb db;
    CHECK(db.open(tmp.path() + "/contest.sqlite"), "contest.db opens");

    ContestRow c;
    c.defId = "CW-OPS";
    c.title = "CWT smoke";
    c.startUtc = QDateTime::currentDateTimeUtc();
    c.sentExch = "Jon MI";
    const qint64 cid = db.createContest(c);
    CHECK(cid > 0, "contest created");

    ContestWindow w(&db, &cty, nullptr);
    CHECK(w.openContestId(cid), "window opens the contest");
    w.setRig(14032000, "CW");
    w.show();

    auto* call = w.findChild<QLineEdit*>("entryCall");
    auto* ex0 = w.findChild<QLineEdit*>("exchEdit0");   // NAME
    auto* ex1 = w.findChild<QLineEdit*>("exchEdit1");   // NR/STATE
    CHECK(call && ex0 && ex1, "entry fields exist (CWT: no RST anywhere)");

    QPushButton* logBtn = nullptr;
    for (QPushButton* b : w.findChildren<QPushButton*>())
        if (b->text() == "LOG") logBtn = b;
    CHECK(logBtn, "LOG button exists");
    if (!call || !ex0 || !ex1 || !logBtn) return 1;

    // A short call must be refused, silently staying unlogged.
    call->setText("W1");
    ex0->setText("ART");
    ex1->setText("CT");
    logBtn->click();
    CHECK(db.qsos(cid).isEmpty(), "unloggable call is refused");

    // A missing required field must be refused too.
    call->setText("N3JT");
    ex0->clear();
    logBtn->click();
    CHECK(db.qsos(cid).isEmpty(), "missing exchange is refused");

    // The real thing.
    ex0->setText("JIM");
    ex1->setText("1");
    logBtn->click();
    const auto rows1 = db.qsos(cid);
    CHECK(rows1.size() == 1 && rows1[0].v.call == "N3JT"
              && rows1[0].v.exch1 == "JIM" && rows1[0].v.exch2 == "1"
              && rows1[0].v.band == "20M" && rows1[0].points == 1,
          "QSO logs with band from the rig feed");
    CHECK(call->text().isEmpty() && ex0->text().isEmpty(),
          "fields wipe after the log (the silent 'it logged' signal)");

    // Second station: mult count must move (unique calls).
    call->setText("K6RB");
    ex0->setText("ROB");
    ex1->setText("3");
    logBtn->click();
    CHECK(db.qsos(cid).size() == 2, "second QSO logs");

    bool scoreShown = false;
    for (QLabel* l : w.findChildren<QLabel*>())
        if (l->text().contains("QSOs 2") && l->text().contains("Mults 2"))
            scoreShown = true;
    CHECK(scoreShown, "score line shows 2 QSOs, 2 mults");

    const QString png = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : tmp.path() + "/contestwin.png";
    w.resize(1100, 640);
    CHECK(w.grab().save(png), "window screenshot saved");
    std::printf("      -> %s\n", qPrintable(png));

    // ---- the deck: the in-console primary surface -----------------------
    ContestDeck deck(&db, &cty, nullptr, nullptr);
    CHECK(deck.openContestId(cid), "deck opens the contest");
    deck.setRig(14032000, "CW");
    deck.setMasterScp({"N3JT", "N3JTX", "K6RB"});
    deck.show();

    // Spot classification drives the panadapter colors and the walk.
    CHECK(deck.classifySpot("N3JT") == 'W',
          "deck: worked call classifies gray (dupe)");
    CHECK(deck.classifySpot("W9ZZZ") == 'M',
          "deck: unworked call is a fresh mult (CWT: unique calls)");

    auto* dCall = deck.findChild<QLineEdit*>("entryCall");
    auto* dEx0 = deck.findChild<QLineEdit*>("exchEdit0");
    auto* dEx1 = deck.findChild<QLineEdit*>("exchEdit1");
    CHECK(dCall && dEx0 && dEx1, "deck: entry fields exist");
    if (!dCall || !dEx0 || !dEx1) return 1;

    // History prefill data, then the deck's log path end-to-end.
    QList<HistoryRow> hr;
    HistoryRow h;
    h.call = "W9ZZZ";
    h.name = "Ann";
    h.exch1 = "77";
    hr << h;
    CHECK(db.importCallHistory(hr) == 1, "deck: history import");
    deck.prefillCall("W9ZZZ");
    QPushButton* dLog = nullptr;
    for (QPushButton* b : deck.findChildren<QPushButton*>())
        if (b->text().startsWith("F12")) dLog = b;   // F12 = WIPE exists
    CHECK(dLog, "deck: F12 WIPE button present");
    dEx0->setText("ANN");
    dEx1->setText("77");
    // ESM off-path: drive logNow via Enter handling is ESM's job; the
    // deck logs through the same engine — use the widgets directly.
    QMetaObject::invokeMethod(dCall, "returnPressed");
    // Run-mode ESM answers first (keys nothing here — null keyer), so
    // the QSO is not yet logged; a second Enter closes TU+log.
    QMetaObject::invokeMethod(dCall, "returnPressed");
    CHECK(db.qsos(cid).size() == 3
              && db.qsos(cid).last().v.call == "W9ZZZ",
          "deck: ESM run two-beat logs the QSO");
    CHECK(dCall->text().isEmpty(), "deck: silent wipe after the log");
    CHECK(deck.classifySpot("W9ZZZ") == 'W',
          "deck: freshly logged call reclassifies as worked");

    // Screenshot with a partial typed so the SUPER CHECK line shows.
    deck.prefillCall("N3J");
    deck.resize(1900, 270);
    QCoreApplication::processEvents();   // let the layout place the row
    QPushButton* scpHit = nullptr;
    for (QPushButton* b : deck.findChildren<QPushButton*>())
        if (b->text() == "N3JT") scpHit = b;
    CHECK(scpHit && scpHit->isVisible(),
          "deck: super check offers N3JT for partial N3J");
    const QString png2 = png + ".deck.png";
    CHECK(deck.grab().save(png2), "deck screenshot saved");
    std::printf("      -> %s\n", qPrintable(png2));

    std::printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
