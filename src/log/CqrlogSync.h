// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QProcess;

namespace ttc {

class CtyLookup;
class LogDb;

// cqrlog -> console log mirror, so the two logs stay in sync for ALL
// modes (operator's requirement). The console->cqrlog direction already
// exists (the LOG window and contest pushes send ADIF to the bridge);
// this is the missing half: QSOs that land in cqrlog first — WSJT-X
// digi (cqrlog wins the unicast 2237), hand entries in cqrlog itself —
// are pulled into the console's own log, which feeds the spot table's
// country/band/mode dots.
//
// Same access pattern as LogbookIndex: the mysql CLI over cqrlog's
// embedded-server socket, only while cqrlog is running, one query at a
// time. Incremental on id_cqrlog_main (watermark in QSettings
// log/cqrlogSyncId); rows convert to ADIF and go through
// LogDb::importAdif, whose near-duplicate check makes re-pulls and
// bridge-echoes harmless and whose upload stamps keep mirrored history
// out of every online-log push.
class CqrlogSync : public QObject {
    Q_OBJECT
public:
    CqrlogSync(LogDb* db, const CtyLookup* cty, QObject* parent = nullptr);

    void start();                    // first pull soon + periodic retries
    void pullSoon(int delayMs = 3000);

    // mysql -N -B rows -> ADIF records (pure, for synctest). Column
    // order matches kColumns below; maxId receives the highest
    // id_cqrlog_main seen (untouched when there are no rows).
    static QByteArray rowsToAdif(const QString& tsv, qint64* maxId);
    static int columnCount();

signals:
    void pulled(int added);          // added > 0: new QSOs mirrored in

private:
    void pull();

    LogDb* db_;
    const CtyLookup* cty_;
    QProcess* proc_ = nullptr;
    QTimer timer_;
    qint64 lastId_ = 0;
};

} // namespace ttc
