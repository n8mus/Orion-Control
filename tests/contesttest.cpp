// SPDX-License-Identifier: GPL-2.0-or-later
// Contest-engine test: callsign normalization, serial formatting, dupe
// scopes, per-contest scoring (CQ WW zero-point-own-country, WAE EU-only
// with weighted mults), macro expansion, ContestDb round-trip, and the
// Cabrillo build + parse-back self-check — including the reversed-
// exchange corruption the CW Open robot caught in the old system.
// No hardware, no network, no GUI.
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>

#include "contest/Cabrillo.h"
#include "contest/ContestDb.h"
#include "contest/ContestDef.h"
#include "contest/ContestEngine.h"
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

static CQsoValues mkq(const QString& call, const QString& band,
                      const QString& exch1 = QString(),
                      const QString& serialR = QString()) {
    CQsoValues v;
    v.call = call;
    v.band = band;
    v.mode = "CW";
    v.rstS = "599";
    v.rstR = "599";
    v.exch1 = exch1;
    v.serialR = serialR;
    return v;
}

static void testNormalize() {
    CHECK(normalizeForCty("N8EM") == "N8EM", "normalize: plain call");
    CHECK(normalizeForCty("DL/N8EM") == "DL", "normalize: DL/N8EM -> DL");
    CHECK(normalizeForCty("N8EM/DL") == "DL", "normalize: N8EM/DL -> DL");
    CHECK(normalizeForCty("W1AW/4") == "W1AW",
          "normalize: bare area digit keeps the call");
    CHECK(normalizeForCty("N8EM/QRP") == "N8EM", "normalize: /QRP tail");
    CHECK(normalizeForCty("F/ON4UN/P") == "F",
          "normalize: portable tail then shortest");
    CHECK(normalizeForCty("VP2M/K9ZZ") == "VP2M",
          "normalize: shortest segment wins");
}

static void testLoggable() {
    CHECK(!loggableCall("TEST"), "loggable: no digit refused");
    CHECK(!loggableCall("W1"), "loggable: too short refused");
    CHECK(!loggableCall("123"), "loggable: no letter refused");
    CHECK(loggableCall("DL1TEST"), "loggable: DL1TEST accepted");
    CHECK(loggableCall("K9A"), "loggable: K9A accepted");
}

static void testSerials() {
    CHECK(formatSerial(1, true, 3) == "TT1", "serial: 1 -> TT1");
    CHECK(formatSerial(190, true, 3) == "1NT", "serial: 190 -> 1NT");
    CHECK(formatSerial(45, true, 3) == "T45", "serial: 45 -> T45");
    CHECK(formatSerial(9, false, 1) == "9", "serial: MST plain digits");
    CHECK(formatSerial(1024, true, 3) == "1T24", "serial: no truncation");
}

static void testDupes() {
    const ContestDef* cwt = contestDef("CW-OPS");
    QList<CQsoValues> log{mkq("N3JT", "20M")};
    CHECK(isDupe(*cwt, log, "N3JT", "20M", "CW"),
          "dupe: same band is a dupe (CWT)");
    CHECK(!isDupe(*cwt, log, "N3JT", "40M", "CW"),
          "dupe: other band is workable (CWT)");
    CHECK(!isDupe(*cwt, log, "N3JTX", "20M", "CW"),
          "dupe: different call is not a dupe");

    ContestDef whole = *cwt;
    whole.dupe = DupeScope::Contest;
    CHECK(isDupe(whole, log, "n3jt", "40M", "CW"),
          "dupe: whole-contest scope, case-insensitive");
}

static void testCqWw(const CtyLookup& cty) {
    const ContestDef* d = contestDef("CQ-WW-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    CHECK(cty.info("N8EM", me), "cty: resolves N8EM");
    ctx.myCountry = me.country;
    ctx.myCq = me.cq;
    CHECK(me.cont == "NA", "cty: continent parsed (N8EM -> NA)");

    // Own country 0, VE (same continent, NA) 2, DL (other continent) 3 —
    // and every one of them still claims zone+country mults.
    QList<CQsoValues> log;
    log << mkq("W1AW", "20M", "5") << mkq("VE3AT", "20M", "4")
        << mkq("DL2CC", "20M", "14");
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 0 + 2 + 3, "cqww: 0/2/3 point ladder");
    CHECK(sb.mults == 6, "cqww: 3 zones + 3 countries on one band");
    CHECK(sb.total == qint64(5) * 6, "cqww: score = points x mults");

    // Same countries again on another band are fresh mults.
    log << mkq("DL8WPX", "40M", "14");
    const ScoreBreakdown sb2 = computeScore(*d, log, &cty, ctx);
    CHECK(sb2.mults == 8, "cqww: mults are per band");
}

static void testWae(const CtyLookup& cty) {
    const ContestDef* d = contestDef("DARC-WAEDC-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;

    QList<CQsoValues> log;
    log << mkq("DL1ABC", "40M", "", "456")   // EU, 40 m: 1 pt, mult x3
        << mkq("G4XYZ", "80M", "", "112")    // EU, 80 m: 1 pt, mult x4
        << mkq("W1AW", "20M", "", "7")       // NA-NA: zero
        << mkq("DL2CC", "160M", "", "9")     // 160 m is not a WAE band
        << mkq("DL8WPX", "40M", "", "77");   // Germany 40 m again: no new mult
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 3, "wae: EU-only scoring (W1AW, 160 m zero)");
    CHECK(sb.mults == 2, "wae: two country-band mults");
    CHECK(sb.weightedMults == 3 + 4, "wae: band weights 40x3 + 80x4");
    CHECK(sb.total == 3 * 7, "wae: total = pts x weighted mults");
}

static void testMacros() {
    const ContestDef* wae = contestDef("DARC-WAEDC-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    const QString f3 =
        expandMacro("{SNT} {SENTNR}", *wae, ctx, "DL8WPX", "", 1);
    CHECK(f3 == "5NN TT1", "macro: WAE exchange 5NN TT1");
    const ContestDef* cwt = contestDef("CW-OPS");
    CHECK(expandMacro("{EXCH}", *cwt, ctx, "", "Jon MI", 0) == "Jon MI",
          "macro: CWT sent exchange");
    CHECK(expandMacro("cq wae {MYCALL} {MYCALL} wae", *wae, ctx, "", "", 1)
              == "cq wae N8EM N8EM wae",
          "macro: my call substitution");
}

static void testDb(const QString& dir) {
    ContestDb db;
    CHECK(db.open(dir + "/contest.sqlite"), "db: opens");

    ContestRow c;
    c.defId = "DARC-WAEDC-CW";
    c.title = "WAE DX CW 2026";
    c.startUtc = QDateTime::fromString("2026-08-08 00:00:00",
                                       "yyyy-MM-dd HH:mm:ss");
    c.startUtc.setTimeZone(QTimeZone::utc());
    c.sentExch = "";
    const qint64 cid = db.createContest(c);
    CHECK(cid > 0, "db: contest created");

    ContestQso q;
    q.contestId = cid;
    q.tsUtc = c.startUtc.addSecs(1800);
    q.freqHz = 7024300;
    q.v = mkq("DL1ABC", "40M", "", "456");
    q.v.serialS = 1;
    CHECK(db.addQso(q) > 0, "db: qso added");
    CHECK(db.setNextSerial(cid, 2), "db: serial persisted");
    CHECK(db.contest(cid).nextSerial == 2, "db: serial reads back");

    const QList<ContestQso> rows = db.qsos(cid);
    CHECK(rows.size() == 1 && rows[0].v.call == "DL1ABC"
              && rows[0].v.serialR == "456" && rows[0].v.serialS == 1
              && rows[0].tsUtc.toString("HHmm") == "0030",
          "db: qso round-trips");
    CHECK(db.contests().size() == 1
              && db.contests()[0].defId == "DARC-WAEDC-CW",
          "db: contest list");
}

static void testCabrillo(const CtyLookup& cty) {
    // CW Open: serial-then-name BOTH sides — the exact shape the old
    // system got wrong (received side went out name-then-serial).
    const ContestDef* d = contestDef("CW-OPEN");
    ContestRow c;
    c.defId = d->id;
    c.sentExch = "Jon";
    ContestQso q;
    q.tsUtc = QDateTime::fromString("2026-09-05 12:23:00",
                                    "yyyy-MM-dd HH:mm:ss");
    q.tsUtc.setTimeZone(QTimeZone::utc());
    q.freqHz = 7029000;
    q.v = mkq("N2IC", "40M", "STEVE", "52");
    q.v.serialS = 1;
    QList<ContestQso> qsos{q};

    CabrilloStation st;
    st.call = "N8EM";
    st.name = "Jon Greenwood";
    st.gridLocator = "EN83AL";
    st.location = "MI";
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";

    const QString text = Cabrillo::build(*d, c, qsos, st, &cty, ctx);
    CHECK(text.startsWith("START-OF-LOG: 3.0\r\n"), "cab: header + CRLF");
    CHECK(text.contains("CONTEST: CW-OPEN"), "cab: contest tag");
    CHECK(text.contains("END-OF-LOG:"), "cab: end tag");

    QString qline;
    for (const QString& l : text.split("\r\n"))
        if (l.startsWith("QSO: ")) qline = l;
    CHECK(qline.contains(" 7029 CW 2026-09-05 1223 "), "cab: freq/mode/ts");
    // serial then name, both sides, in order.
    const int sMine = qline.indexOf("001 JON");
    const int sHis = qline.indexOf("052 STEVE");
    CHECK(sMine > 0 && sHis > sMine,
          "cab: serial-name order on BOTH sides (the CW Open bug)");

    QString err;
    const bool selfOk = Cabrillo::selfCheck(text, *d, c, qsos, &err);
    if (!selfOk) std::printf("      self-check said: %s\n", qPrintable(err));
    CHECK(selfOk, "cab: self-check passes");

    // Corrupt the file the way the old bug did — swap the received side
    // to name-then-serial — and the self-check must refuse it.
    QString bad = text;
    bad.replace("052 STEVE", "STEVE 052");
    CHECK(!Cabrillo::selfCheck(bad, *d, c, qsos, &err),
          "cab: reversed exchange is caught");

    // Cut-number copy normalizes on export: TT1 in the box, 001 on file.
    ContestQso q2 = q;
    q2.v.call = "K6RB";
    q2.v.serialR = "TT7";
    QList<ContestQso> qsos2{q, q2};
    const QString t2 = Cabrillo::build(*d, c, qsos2, st, &cty, ctx);
    CHECK(t2.contains("K6RB          007"), "cab: cut numbers uncut on file");
    CHECK(Cabrillo::selfCheck(t2, *d, c, qsos2, nullptr),
          "cab: self-check with uncut serials");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;

    CtyLookup cty;
    bool ctyOk = false;
    for (const char* p : {"resources/cty.dat", "../resources/cty.dat",
                          "../../resources/cty.dat"})
        if (cty.load(QString::fromLatin1(p))) { ctyOk = true; break; }
    if (!ctyOk) {
        std::printf("FAIL  cannot find resources/cty.dat (run from repo "
                    "or build dir)\n");
        return 1;
    }

    testNormalize();
    testLoggable();
    testSerials();
    testDupes();
    testCqWw(cty);
    testWae(cty);
    testMacros();
    testDb(tmp.path());
    testCabrillo(cty);

    std::printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
