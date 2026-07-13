// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/LogbookIndex.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

namespace ttc {

namespace {
// TTC_CQRLOG_HOME overrides for tests (points at a sandboxed copy).
QString cqrlogHome() {
    const QByteArray env = qgetenv("TTC_CQRLOG_HOME");
    return env.isEmpty() ? QDir::homePath() : QString::fromUtf8(env);
}
// cqrlog names its databases cqrlog001, cqrlog002… after the log number in
// cqrlog_login.cfg. LastOpenedLog is what the auto-open uses; follow it.
QString cqrlogDbName() {
    QSettings login(cqrlogHome() + "/.config/cqrlog/cqrlog_login.cfg",
                    QSettings::IniFormat);
    const int n = login.value("cqrlog/LastOpenedLog",
                              login.value("LastOpenedLog", 1)).toInt();
    return QString("cqrlog%1").arg(n, 3, 10, QChar('0'));
}
QString cqrlogSocket() {
    return cqrlogHome() + "/.config/cqrlog/database/sock";
}
} // namespace

LogbookIndex::LogbookIndex(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(5 * 60 * 1000);    // retry/refresh while console runs
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
}

void LogbookIndex::start() {
    QFile cache(cachePath());
    if (cache.open(QIODevice::ReadOnly | QIODevice::Text)) {
        parseRows(QString::fromUtf8(cache.readAll()));
        if (haveData_) emit updated();
    }
    timer_->start();
    refresh();
}

void LogbookIndex::refreshSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { refresh(); });
}

void LogbookIndex::refresh() {
    if (proc_) return;                     // one at a time
    if (!QFile::exists(cqrlogSocket())) return;   // cqrlog not running
    proc_ = new QProcess(this);
    const QString sql =
        "SELECT 'W', callsign, band FROM cqrlog_main "
        "UNION SELECT 'C', callsign, band FROM cqrlog_main "
        " WHERE qsl_r='Q' OR lotw_qslr='L' OR eqsl_qsl_rcvd='E' "
        "UNION SELECT 'P', pota_hunted_ref, '' FROM cqrlog_main "
        " WHERE pota_hunted_ref IS NOT NULL AND pota_hunted_ref<>'';";
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        const QString out = QString::fromUtf8(proc_->readAllStandardOutput());
        proc_->deleteLater();
        proc_ = nullptr;
        if (st != QProcess::NormalExit || code != 0 || out.isEmpty()) return;
        parseRows(out);
        QFile cache(cachePath());
        if (cache.open(QIODevice::WriteOnly | QIODevice::Text))
            cache.write(out.toUtf8());
        emit updated();
    });
    proc_->start("mysql",
                 {"--socket=" + cqrlogSocket(), "-u", "root", cqrlogDbName(),
                  "-N", "-B", "--connect-timeout=3", "-e", sql});
    QTimer::singleShot(15000, proc_, [p = proc_] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void LogbookIndex::parseRows(const QString& tsv) {
    QSet<QString> wc, wcb, ccb, pk;
    for (const QString& line : tsv.split('\n', Qt::SkipEmptyParts)) {
        const QStringList f = line.split('\t');
        if (f.size() < 2) continue;
        const QString call = f[1].trimmed().toUpper();
        const QString band = f.size() > 2 ? f[2].trimmed().toUpper() : QString();
        if (call.isEmpty() || call == "NULL") continue;
        if (f[0] == "W") {
            wc.insert(call);
            wcb.insert(call + '|' + band);
        } else if (f[0] == "C") {
            ccb.insert(call + '|' + band);
        } else if (f[0] == "P") {
            // pota_hunted_ref can hold a comma list (N-fer park-to-park)
            for (const QString& p : call.split(',', Qt::SkipEmptyParts))
                pk.insert(p.trimmed());
        }
    }
    if (wc.isEmpty() && pk.isEmpty()) return;      // don't wipe on bad read
    workedCall_ = std::move(wc);
    workedCallBand_ = std::move(wcb);
    confCallBand_ = std::move(ccb);
    parks_ = std::move(pk);
    haveData_ = true;
}

QChar LogbookIndex::status(const QString& call, const QString& band) const {
    if (!haveData_) return QChar('?');
    const QString c = call.trimmed().toUpper();
    if (!workedCall_.contains(c)) return QChar('N');
    const QString key = c + '|' + band;
    if (confCallBand_.contains(key)) return QChar('C');
    if (workedCallBand_.contains(key)) return QChar('W');
    return QChar('B');
}

bool LogbookIndex::parkHunted(const QString& park) const {
    return haveData_ && parks_.contains(park.trimmed().toUpper());
}

QString LogbookIndex::bandForHz(qint64 hz) {
    struct B { qint64 lo, hi; const char* name; };
    static const B bands[] = {
        {1800000, 2000000, "160M"}, {3500000, 4000000, "80M"},
        {5250000, 5450000, "60M"},  {7000000, 7300000, "40M"},
        {10100000, 10150000, "30M"}, {14000000, 14350000, "20M"},
        {18068000, 18168000, "17M"}, {21000000, 21450000, "15M"},
        {24890000, 24990000, "12M"}, {28000000, 29700000, "10M"},
        {50000000, 54000000, "6M"},  {144000000, 148000000, "2M"},
        {420000000, 450000000, "70CM"}, {902000000, 928000000, "33CM"},
    };
    for (const B& b : bands)
        if (hz >= b.lo && hz <= b.hi) return b.name;
    return QString();
}

QString LogbookIndex::cachePath() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/logbook-index.tsv";
}

} // namespace ttc
