// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QSet>
#include <QString>

class QProcess;
class QTimer;

namespace ttc {

// Read-only view of the cqrlog logbook for worked-before spot coloring.
// Loads three summaries (worked call+band, confirmed call+band, hunted
// POTA parks) through the mysql CLI over cqrlog's own embedded server
// socket (~/.config/cqrlog/database/sock) — no Qt SQL plugin, no schema
// coupling beyond cqrlog_main's stable columns. That server only runs
// while cqrlog is open, so every good load is cached to disk and the
// cache restored at startup; a periodic retry keeps the live view fresh
// whenever cqrlog is up. Confirmation = any of card (qsl_r 'Q'),
// LoTW (lotw_qslr 'L') or eQSL (eqsl_qsl_rcvd 'E'), cqrlog's own codes.
class LogbookIndex : public QObject {
    Q_OBJECT
public:
    explicit LogbookIndex(QObject* parent = nullptr);

    void start();                          // restore cache + begin refresh
    void refreshSoon(int delayMs = 2500);  // e.g. right after LOG sends a QSO

    // Spot status: 'N' never worked, 'B' worked but not this band,
    // 'W' worked this band, 'C' confirmed this band, '?' no data yet.
    QChar status(const QString& call, const QString& band) const;
    bool  parkHunted(const QString& park) const;
    bool  ready() const { return haveData_; }

    static QString bandForHz(qint64 hz);   // cqrlog band text ("20M")

signals:
    void updated();                        // sets rebuilt (live or cache)

private:
    void refresh();                        // spawn the mysql query
    void parseRows(const QString& tsv);    // "T<TAB>call<TAB>band" lines
    QString cachePath() const;

    bool haveData_ = false;
    QSet<QString> workedCall_;             // "N8EM"
    QSet<QString> workedCallBand_;         // "N8EM|20M"
    QSet<QString> confCallBand_;
    QSet<QString> parks_;                  // "US-1234"
    QProcess* proc_ = nullptr;
    QTimer*   timer_ = nullptr;
};

} // namespace ttc
