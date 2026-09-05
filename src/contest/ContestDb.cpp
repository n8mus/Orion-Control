// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestDb.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimeZone>
#include <QVariant>

namespace ttc {

namespace {
const char* kSchemaContest =
    "CREATE TABLE IF NOT EXISTS contest ("
    " id INTEGER PRIMARY KEY,"
    " def_id TEXT NOT NULL,"
    " title TEXT DEFAULT '',"
    " start_utc TEXT DEFAULT '',"          // "yyyy-MM-dd HH:mm:ss"
    " sent_exch TEXT DEFAULT '',"
    " next_serial INTEGER DEFAULT 1,"
    " cat_op TEXT DEFAULT 'SINGLE-OP', cat_assisted TEXT DEFAULT 'ASSISTED',"
    " cat_band TEXT DEFAULT 'ALL', cat_power TEXT DEFAULT 'LOW',"
    " cat_mode TEXT DEFAULT 'CW', cat_tx TEXT DEFAULT 'ONE',"
    " cat_station TEXT DEFAULT 'FIXED', cat_overlay TEXT DEFAULT '',"
    " club TEXT DEFAULT '', soapbox TEXT DEFAULT '')";

const char* kSchemaHistory =
    "CREATE TABLE IF NOT EXISTS callhistory ("
    " call TEXT PRIMARY KEY,"
    " name TEXT DEFAULT '', exch1 TEXT DEFAULT '', sect TEXT DEFAULT '',"
    " state TEXT DEFAULT '', grid TEXT DEFAULT '', ck TEXT DEFAULT '',"
    " power TEXT DEFAULT '', usertext TEXT DEFAULT '')";

const char* kSchemaQso =
    "CREATE TABLE IF NOT EXISTS cqso ("
    " id INTEGER PRIMARY KEY,"
    " contest_id INTEGER NOT NULL REFERENCES contest(id),"
    " ts_utc TEXT NOT NULL,"
    " call TEXT NOT NULL,"
    " freq_hz INTEGER NOT NULL DEFAULT 0,"
    " band TEXT NOT NULL,"
    " mode TEXT NOT NULL,"
    " rst_s TEXT DEFAULT '', rst_r TEXT DEFAULT '',"
    " serial_s INTEGER DEFAULT 0, serial_r TEXT DEFAULT '',"
    " exch1 TEXT DEFAULT '', exch2 TEXT DEFAULT '',"
    " points INTEGER DEFAULT 0,"
    " run_sp TEXT DEFAULT 'R')";

QDateTime utcFrom(const QString& s) {
    QDateTime t = QDateTime::fromString(s, "yyyy-MM-dd HH:mm:ss");
    t.setTimeZone(QTimeZone::utc());
    return t;
}

ContestRow contestFromQuery(const QSqlQuery& q) {
    ContestRow c;
    c.id         = q.value("id").toLongLong();
    c.defId      = q.value("def_id").toString();
    c.title      = q.value("title").toString();
    c.startUtc   = utcFrom(q.value("start_utc").toString());
    c.sentExch   = q.value("sent_exch").toString();
    c.nextSerial = q.value("next_serial").toInt();
    c.catOp       = q.value("cat_op").toString();
    c.catAssisted = q.value("cat_assisted").toString();
    c.catBand     = q.value("cat_band").toString();
    c.catPower    = q.value("cat_power").toString();
    c.catMode     = q.value("cat_mode").toString();
    c.catTx       = q.value("cat_tx").toString();
    c.catStation  = q.value("cat_station").toString();
    c.catOverlay  = q.value("cat_overlay").toString();
    c.club        = q.value("club").toString();
    c.soapbox     = q.value("soapbox").toString();
    return c;
}

ContestQso qsoFromQuery(const QSqlQuery& q) {
    ContestQso o;
    o.id        = q.value("id").toLongLong();
    o.contestId = q.value("contest_id").toLongLong();
    o.tsUtc     = utcFrom(q.value("ts_utc").toString());
    o.freqHz    = q.value("freq_hz").toLongLong();
    o.v.call    = q.value("call").toString();
    o.v.band    = q.value("band").toString();
    o.v.mode    = q.value("mode").toString();
    o.v.rstS    = q.value("rst_s").toString();
    o.v.rstR    = q.value("rst_r").toString();
    o.v.serialS = q.value("serial_s").toInt();
    o.v.serialR = q.value("serial_r").toString();
    o.v.exch1   = q.value("exch1").toString();
    o.v.exch2   = q.value("exch2").toString();
    o.points    = q.value("points").toInt();
    o.runSp     = q.value("run_sp").toString();
    return o;
}

void bindQso(QSqlQuery& q, const ContestQso& o) {
    q.bindValue(":cid", o.contestId);
    q.bindValue(":ts", o.tsUtc.toUTC().toString("yyyy-MM-dd HH:mm:ss"));
    q.bindValue(":call", o.v.call.trimmed().toUpper());
    q.bindValue(":freq", o.freqHz);
    q.bindValue(":band", o.v.band.trimmed().toUpper());
    q.bindValue(":mode", o.v.mode.trimmed().toUpper());
    q.bindValue(":rsts", o.v.rstS.trimmed());
    q.bindValue(":rstr", o.v.rstR.trimmed());
    q.bindValue(":sers", o.v.serialS);
    q.bindValue(":serr", o.v.serialR.trimmed().toUpper());
    q.bindValue(":ex1", o.v.exch1.trimmed().toUpper());
    q.bindValue(":ex2", o.v.exch2.trimmed().toUpper());
    q.bindValue(":pts", o.points);
    q.bindValue(":rsp", o.runSp);
}
} // namespace

ContestDb::ContestDb(QObject* parent) : QObject(parent) {
    conn_ = QStringLiteral("ttc-contestdb-%1").arg(quintptr(this));
}

ContestDb::~ContestDb() {
    if (QSqlDatabase::contains(conn_)) {
        QSqlDatabase::database(conn_, false).close();
        QSqlDatabase::removeDatabase(conn_);
    }
}

QString ContestDb::defaultPath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + "/contest.sqlite";
}

bool ContestDb::open(const QString& path) {
    path_ = path.isEmpty()
        ? QSettings().value("contest/dbPath", defaultPath()).toString()
        : path;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSqlDatabase db = QSqlDatabase::contains(conn_)
        ? QSqlDatabase::database(conn_, false)
        : QSqlDatabase::addDatabase("QSQLITE", conn_);
    db.setDatabaseName(path_);
    if (!db.open()) return false;
    QSqlQuery q(db);
    if (!q.exec(QString::fromLatin1(kSchemaContest))) return false;
    if (!q.exec(QString::fromLatin1(kSchemaQso))) return false;
    if (!q.exec(QString::fromLatin1(kSchemaHistory))) return false;
    q.exec("CREATE INDEX IF NOT EXISTS idx_cqso_contest"
           " ON cqso(contest_id)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_cqso_call"
           " ON cqso(contest_id, call)");
    return true;
}

bool ContestDb::isOpen() const {
    return QSqlDatabase::contains(conn_)
        && QSqlDatabase::database(conn_, false).isOpen();
}

qint64 ContestDb::createContest(const ContestRow& c) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "INSERT INTO contest (def_id, title, start_utc, sent_exch,"
        " next_serial, cat_op, cat_assisted, cat_band, cat_power, cat_mode,"
        " cat_tx, cat_station, cat_overlay, club, soapbox) VALUES"
        " (:def, :title, :start, :exch, :ser, :op, :as, :bd, :pw, :md,"
        "  :tx, :st, :ov, :club, :soap)");
    q.bindValue(":def", c.defId);
    q.bindValue(":title", c.title.trimmed());
    q.bindValue(":start",
                c.startUtc.toUTC().toString("yyyy-MM-dd HH:mm:ss"));
    q.bindValue(":exch", c.sentExch.trimmed());
    q.bindValue(":ser", c.nextSerial);
    q.bindValue(":op", c.catOp);
    q.bindValue(":as", c.catAssisted);
    q.bindValue(":bd", c.catBand);
    q.bindValue(":pw", c.catPower);
    q.bindValue(":md", c.catMode);
    q.bindValue(":tx", c.catTx);
    q.bindValue(":st", c.catStation);
    q.bindValue(":ov", c.catOverlay);
    q.bindValue(":club", c.club);
    q.bindValue(":soap", c.soapbox);
    if (!q.exec()) return -1;
    emit changed();
    return q.lastInsertId().toLongLong();
}

bool ContestDb::updateContest(const ContestRow& c) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "UPDATE contest SET title=:title, sent_exch=:exch,"
        " next_serial=:ser, cat_op=:op, cat_assisted=:as, cat_band=:bd,"
        " cat_power=:pw, cat_mode=:md, cat_tx=:tx, cat_station=:st,"
        " cat_overlay=:ov, club=:club, soapbox=:soap WHERE id=:id");
    q.bindValue(":title", c.title.trimmed());
    q.bindValue(":exch", c.sentExch.trimmed());
    q.bindValue(":ser", c.nextSerial);
    q.bindValue(":op", c.catOp);
    q.bindValue(":as", c.catAssisted);
    q.bindValue(":bd", c.catBand);
    q.bindValue(":pw", c.catPower);
    q.bindValue(":md", c.catMode);
    q.bindValue(":tx", c.catTx);
    q.bindValue(":st", c.catStation);
    q.bindValue(":ov", c.catOverlay);
    q.bindValue(":club", c.club);
    q.bindValue(":soap", c.soapbox);
    q.bindValue(":id", c.id);
    const bool ok = q.exec();
    if (ok) emit changed();
    return ok;
}

QList<ContestRow> ContestDb::contests() const {
    QList<ContestRow> out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec("SELECT * FROM contest ORDER BY start_utc DESC, id DESC"))
        return out;
    while (q.next()) out << contestFromQuery(q);
    return out;
}

ContestRow ContestDb::contest(qint64 id) const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT * FROM contest WHERE id=:id");
    q.bindValue(":id", id);
    if (q.exec() && q.next()) return contestFromQuery(q);
    return {};
}

bool ContestDb::setNextSerial(qint64 contestId, int serial) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("UPDATE contest SET next_serial=:s WHERE id=:id");
    q.bindValue(":s", serial < 1 ? 1 : serial);
    q.bindValue(":id", contestId);
    return q.exec();
}

qint64 ContestDb::addQso(const ContestQso& o) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "INSERT INTO cqso (contest_id, ts_utc, call, freq_hz, band, mode,"
        " rst_s, rst_r, serial_s, serial_r, exch1, exch2, points, run_sp)"
        " VALUES (:cid, :ts, :call, :freq, :band, :mode, :rsts, :rstr,"
        " :sers, :serr, :ex1, :ex2, :pts, :rsp)");
    bindQso(q, o);
    if (!q.exec()) return -1;
    emit changed();
    return q.lastInsertId().toLongLong();
}

bool ContestDb::updateQso(const ContestQso& o) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "UPDATE cqso SET contest_id=:cid, ts_utc=:ts, call=:call,"
        " freq_hz=:freq, band=:band, mode=:mode, rst_s=:rsts, rst_r=:rstr,"
        " serial_s=:sers, serial_r=:serr, exch1=:ex1, exch2=:ex2,"
        " points=:pts, run_sp=:rsp WHERE id=:id");
    bindQso(q, o);
    q.bindValue(":id", o.id);
    const bool ok = q.exec();
    if (ok) emit changed();
    return ok;
}

bool ContestDb::deleteQso(qint64 id) {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("DELETE FROM cqso WHERE id=:id");
    q.bindValue(":id", id);
    const bool ok = q.exec();
    if (ok) emit changed();
    return ok;
}

QList<ContestQso> ContestDb::qsos(qint64 contestId) const {
    QList<ContestQso> out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT * FROM cqso WHERE contest_id=:cid"
              " ORDER BY ts_utc, id");
    q.bindValue(":cid", contestId);
    if (!q.exec()) return out;
    while (q.next()) out << qsoFromQuery(q);
    return out;
}

QList<CQsoValues> ContestDb::qsoValues(qint64 contestId) const {
    QList<CQsoValues> out;
    for (const ContestQso& o : qsos(contestId)) out << o.v;
    return out;
}

int ContestDb::importCallHistory(const QList<HistoryRow>& rows) {
    QSqlDatabase db = QSqlDatabase::database(conn_);
    if (!db.transaction()) return -1;
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO callhistory (call, name, exch1, sect, state, grid,"
        " ck, power, usertext) VALUES (:call, :name, :ex1, :sect, :state,"
        " :grid, :ck, :pwr, :ut)"
        " ON CONFLICT(call) DO UPDATE SET name=:name, exch1=:ex1,"
        " sect=:sect, state=:state, grid=:grid, ck=:ck, power=:pwr,"
        " usertext=:ut");
    int n = 0;
    for (const HistoryRow& r : rows) {
        q.bindValue(":call", r.call.trimmed().toUpper());
        q.bindValue(":name", r.name);
        q.bindValue(":ex1", r.exch1);
        q.bindValue(":sect", r.sect);
        q.bindValue(":state", r.state);
        q.bindValue(":grid", r.grid);
        q.bindValue(":ck", r.ck);
        q.bindValue(":pwr", r.power);
        q.bindValue(":ut", r.userText);
        if (!q.exec()) {              // one bad row must not eat the rest
            db.rollback();
            return -1;
        }
        ++n;
    }
    if (!db.commit()) {
        db.rollback();
        return -1;
    }
    return n;
}

HistoryRow ContestDb::historyFor(const QString& call) const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT * FROM callhistory WHERE call=:c");
    q.bindValue(":c", call.trimmed().toUpper());
    HistoryRow r;
    if (q.exec() && q.next()) {
        r.call = q.value("call").toString();
        r.name = q.value("name").toString();
        r.exch1 = q.value("exch1").toString();
        r.sect = q.value("sect").toString();
        r.state = q.value("state").toString();
        r.grid = q.value("grid").toString();
        r.ck = q.value("ck").toString();
        r.power = q.value("power").toString();
        r.userText = q.value("usertext").toString();
    }
    return r;
}

int ContestDb::historyCount() const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec("SELECT COUNT(*) FROM callhistory") && q.next())
        return q.value(0).toInt();
    return 0;
}

} // namespace ttc
