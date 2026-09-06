// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestWindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "contest/Cabrillo.h"
#include "contest/ContestEngine.h"
#include "util/CtyLookup.h"

namespace ttc {

ContestWindow::ContestWindow(ContestDb* db, const CtyLookup* cty,
                             QWidget* parent)
    : QDialog(parent), db_(db), cty_(cty) {
    setWindowTitle("Contest manager");
    buildUi();
    connect(db_, &ContestDb::changed, this, [this] {
        if (contestId_ >= 0) refreshAll();
        refreshResumeList();
    });
    const QByteArray geom =
        QSettings().value("contest/mgrGeom").toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);
    refreshResumeList();
    // Come up on the live contest so the log grid is one click away.
    const qint64 last =
        QSettings().value("contest/currentId", -1).toLongLong();
    if (last > 0) openContest(last);
}

void ContestWindow::buildUi() {
    auto* lay = new QVBoxLayout(this);

    // ---- new / resume ---------------------------------------------------
    {
        auto* h = new QHBoxLayout;
        h->addWidget(new QLabel("New:", this));
        defPick_ = new QComboBox(this);
        defPick_->setObjectName("defPick");
        for (const ContestDef* d : contestDefs())
            defPick_->addItem(d->title, d->id);
        h->addWidget(defPick_);
        h->addWidget(new QLabel("sent exch:", this));
        sentExchEdit_ = new QLineEdit(this);
        sentExchEdit_->setObjectName("sentExch");
        sentExchEdit_->setFixedWidth(110);
        sentExchEdit_->setToolTip(
            "What you send besides RST/serial — CWT \"Jon MI\", CQ WW "
            "your zone. Serial contests can leave it empty.");
        h->addWidget(sentExchEdit_);
        h->addWidget(new QLabel("location:", this));
        locationEdit_ = new QLineEdit(
            QSettings().value("contest/location", "MI").toString(), this);
        locationEdit_->setFixedWidth(50);
        locationEdit_->setToolTip(
            "Cabrillo LOCATION: (ARRL section / state)");
        h->addWidget(locationEdit_);
        auto* startBtn = new QPushButton("Start", this);
        connect(startBtn, &QPushButton::clicked, this,
                [this] { newContest(); });
        h->addWidget(startBtn);
        h->addSpacing(18);
        h->addWidget(new QLabel("Resume:", this));
        resumePick_ = new QComboBox(this);
        resumePick_->setObjectName("resumePick");
        resumePick_->setMinimumWidth(220);
        h->addWidget(resumePick_);
        auto* openBtn = new QPushButton("Open", this);
        connect(openBtn, &QPushButton::clicked, this, [this] {
            const qint64 id = resumePick_->currentData().toLongLong();
            if (id > 0) openContest(id);
        });
        h->addWidget(openBtn);
        h->addStretch(1);
        connect(defPick_, &QComboBox::currentIndexChanged, this, [this] {
            if (const ContestDef* d =
                    contestDef(defPick_->currentData().toString()))
                sentExchEdit_->setText(d->sentExchDefault);
        });
        if (const ContestDef* d0 =
                contestDef(defPick_->currentData().toString()))
            sentExchEdit_->setText(d0->sentExchDefault);
        lay->addLayout(h);
    }

    // ---- header ---------------------------------------------------------
    {
        auto* h = new QHBoxLayout;
        title_ = new QLabel("no contest open", this);
        QFont bf = title_->font();
        bf.setBold(true);
        title_->setFont(bf);
        h->addWidget(title_);
        score_ = new QLabel(this);
        h->addWidget(score_);
        h->addStretch(1);
        lay->addLayout(h);
    }

    // ---- the log --------------------------------------------------------
    table_ = new QTableWidget(this);
    table_->setColumnCount(12);
    table_->setHorizontalHeaderLabels({"UTC", "Call", "kHz", "Snt", "SNr",
                                       "Rcv", "RNr", "Ex1", "Ex2", "Ex3",
                                       "Pt", "QTC"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editSelected(); });
    lay->addWidget(table_, 1);

    {
        auto* h = new QHBoxLayout;
        auto* editBtn = new QPushButton("Edit…", this);
        connect(editBtn, &QPushButton::clicked, this,
                [this] { editSelected(); });
        h->addWidget(editBtn);
        auto* delBtn = new QPushButton("Delete", this);
        connect(delBtn, &QPushButton::clicked, this,
                [this] { deleteSelected(); });
        h->addWidget(delBtn);
        h->addStretch(1);
        auto* cabBtn = new QPushButton("Cabrillo…", this);
        connect(cabBtn, &QPushButton::clicked, this,
                [this] { exportCabrillo(); });
        h->addWidget(cabBtn);
        lay->addLayout(h);
    }

    status_ = new QLabel(this);
    lay->addWidget(status_);
    resize(880, 560);
}

void ContestWindow::refreshResumeList() {
    const qint64 keep = resumePick_->currentData().toLongLong();
    resumePick_->clear();
    for (const ContestRow& c : db_->contests())
        resumePick_->addItem(
            QString("%1  (%2 QSOs)")
                .arg(c.title)
                .arg(db_->qsos(c.id).size()),
            c.id);
    if (keep > 0) {
        const int i = resumePick_->findData(keep);
        if (i >= 0) resumePick_->setCurrentIndex(i);
    }
}

bool ContestWindow::openContestId(qint64 id) {
    openContest(id);
    return contestId_ == id;
}

void ContestWindow::newContest() {
    const ContestDef* d = contestDef(defPick_->currentData().toString());
    if (!d) return;
    ContestRow c;
    c.defId = d->id;
    c.title = d->title + " — "
        + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd");
    c.startUtc = QDateTime::currentDateTimeUtc();
    c.sentExch = sentExchEdit_->text().trimmed();
    c.catMode = d->modeCategory;
    const qint64 id = db_->createContest(c);
    if (id < 0) {
        status_->setText("could not create the contest (database error)");
        return;
    }
    QSettings().setValue("contest/location",
                         locationEdit_->text().trimmed());
    trace(QString("NEW %1 \"%2\"").arg(d->id, c.title));
    openContest(id);
}

void ContestWindow::openContest(qint64 id) {
    row_ = db_->contest(id);
    def_ = contestDef(row_.defId);
    if (row_.id < 0 || !def_) {
        status_->setText("contest not found or its definition is missing");
        return;
    }
    contestId_ = id;
    QSettings().setValue("contest/currentId", id);
    ctx_ = ContestContext();
    ctx_.myCall =
        QSettings().value("station/callsign", "N8EM").toString().toUpper();
    CtyInfo me;
    if (cty_ && cty_->info(normalizeForCty(ctx_.myCall), me)) {
        ctx_.myCont = me.cont;
        ctx_.myCountry = me.country;
        ctx_.myCq = me.cq;
        ctx_.myItu = me.itu;
    }
    title_->setText(row_.title);
    setWindowTitle("Contest manager — " + row_.title);
    refreshAll();
    trace(QString("MGR OPEN %1 id=%2").arg(row_.defId).arg(id));
    emit contestOpened(id);
}

void ContestWindow::refreshAll() {
    qsos_ = db_->qsos(contestId_);
    QList<CQsoValues> vals;
    for (const ContestQso& q : qsos_) vals << q.v;
    const int qtcN = def_->hasQtc ? db_->qtcCount(contestId_) : 0;
    const ScoreBreakdown sb =
        computeScore(*def_, vals, cty_, ctx_, qtcN);
    QString s = QString("· %1 QSOs · %2 pts · %3 wt-mults · score %4")
                    .arg(sb.qsos)
                    .arg(sb.points)
                    .arg(sb.weightedMults)
                    .arg(QLocale::c().toString(qlonglong(sb.total)));
    if (def_->hasQtc) s += QString(" · QTC %1").arg(qtcN);
    score_->setText(s);

    const QSet<qint64> reported =
        def_->hasQtc ? db_->qtcReportedQsoIds(contestId_)
                     : QSet<qint64>();
    table_->setRowCount(int(qsos_.size()));
    for (int i = 0; i < qsos_.size(); ++i) {
        const ContestQso& q = qsos_[qsos_.size() - 1 - i];  // newest first
        const auto put = [&](int col, const QString& t) {
            auto* it = new QTableWidgetItem(t);
            if (col == 0) it->setData(Qt::UserRole, q.id);
            table_->setItem(i, col, it);
        };
        put(0, q.tsUtc.toString("HHmm"));
        put(1, q.v.call);
        put(2, QString::number(q.freqHz / 1000.0, 'f', 1));
        put(3, q.v.rstS);
        put(4, q.v.serialS > 0 ? QString::number(q.v.serialS) : QString());
        put(5, q.v.rstR);
        put(6, q.v.serialR);
        put(7, q.v.exch1);
        put(8, q.v.exch2);
        put(9, q.v.exch3);
        put(10, QString::number(q.points));
        put(11, reported.contains(q.id) ? QStringLiteral("✓ sent")
                                        : QString());
    }
}

qint64 ContestWindow::selectedQsoId() const {
    const int r = table_->currentRow();
    if (r < 0) return -1;
    const QTableWidgetItem* it = table_->item(r, 0);
    return it ? it->data(Qt::UserRole).toLongLong() : -1;
}

void ContestWindow::editSelected() {
    const qint64 id = selectedQsoId();
    if (id < 0 || !def_) return;
    if (db_->qsoReported(id)) {
        QMessageBox::warning(
            this, "Frozen QSO",
            "This QSO has gone out in a QTC block — the receiving "
            "station holds a copy of exactly what was logged, so it can "
            "no longer be edited.");
        return;
    }
    ContestQso q;
    for (const ContestQso& c : qsos_)
        if (c.id == id) { q = c; break; }
    if (q.id < 0) return;

    QDialog d(this);
    d.setWindowTitle(QString("Edit %1").arg(q.v.call));
    auto* form = new QFormLayout(&d);
    auto* call = new QLineEdit(q.v.call, &d);
    form->addRow("Call", call);
    QLineEdit *rstS = nullptr, *rstR = nullptr, *serR = nullptr,
              *ex1 = nullptr, *ex2 = nullptr, *ex3 = nullptr;
    if (def_->hasRst) {
        rstS = new QLineEdit(q.v.rstS, &d);
        rstR = new QLineEdit(q.v.rstR, &d);
        form->addRow("RST sent", rstS);
        form->addRow("RST rcvd", rstR);
    }
    for (const ExchFieldDef& fd : def_->fields) {
        switch (fd.col) {
            case ExchCol::RstR: break;             // covered above
            case ExchCol::SerialR:
                serR = new QLineEdit(q.v.serialR, &d);
                form->addRow(fd.label, serR);
                break;
            case ExchCol::Exch1:
                ex1 = new QLineEdit(q.v.exch1, &d);
                form->addRow(fd.label, ex1);
                break;
            case ExchCol::Exch2:
                ex2 = new QLineEdit(q.v.exch2, &d);
                form->addRow(fd.label, ex2);
                break;
            case ExchCol::Exch3:
                ex3 = new QLineEdit(q.v.exch3, &d);
                form->addRow(fd.label, ex3);
                break;
        }
    }
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;

    if (!loggableCall(call->text())) {
        status_->setText("edit refused: that call is not loggable");
        return;
    }
    q.v.call = call->text().trimmed().toUpper();
    if (rstS) q.v.rstS = rstS->text().trimmed();
    if (rstR) q.v.rstR = rstR->text().trimmed();
    if (serR) q.v.serialR = serR->text().trimmed();
    if (ex1) q.v.exch1 = ex1->text().trimmed();
    if (ex2) q.v.exch2 = ex2->text().trimmed();
    if (ex3) q.v.exch3 = ex3->text().trimmed();
    // Points follow the edit (a corrected call can change the country).
    CtyInfo ci;
    const bool ok = cty_ && cty_->info(normalizeForCty(q.v.call), ci);
    q.points = def_->points ? def_->points(q.v, ci, ok, ctx_) : 0;
    if (!db_->updateQso(q)) {
        status_->setText("edit refused (reported in a QTC, or database "
                         "error)");
        return;
    }
    trace(QString("MGR EDIT id=%1 -> %2").arg(id).arg(q.v.call));
    status_->setText(QString("edited %1").arg(q.v.call));
}

void ContestWindow::deleteSelected() {
    const qint64 id = selectedQsoId();
    if (id < 0) return;
    if (db_->qsoReported(id)) {
        QMessageBox::warning(
            this, "Frozen QSO",
            "This QSO has gone out in a QTC block and cannot be deleted "
            "— the stored traffic would disagree with the log.");
        return;
    }
    QString call;
    for (const ContestQso& c : qsos_)
        if (c.id == id) { call = c.v.call; break; }
    if (QMessageBox::question(
            this, "Delete QSO",
            QString("Delete %1 from the log?").arg(call))
        != QMessageBox::Yes)
        return;
    if (!db_->deleteQso(id)) {
        status_->setText("delete refused (database)");
        return;
    }
    trace(QString("MGR DELETE id=%1 %2").arg(id).arg(call));
    status_->setText(QString("deleted %1").arg(call));
}

void ContestWindow::exportCabrillo() {
    if (contestId_ < 0 || !def_) return;
    CabrilloStation st;
    QSettings s;
    st.call = s.value("station/callsign", "N8EM").toString().toUpper();
    st.gridLocator = s.value("station/grid", "EN83al").toString();
    st.location = s.value("contest/location", "MI").toString();
    st.name = s.value("contest/opname").toString();
    st.address = s.value("contest/address").toString();
    st.club = row_.club;
    const QList<ContestDb::QtcRow> qtcs = db_->qtcRows(contestId_);
    const QString text =
        Cabrillo::build(*def_, row_, qsos_, qtcs, st, cty_, ctx_);
    QString err;
    if (!Cabrillo::selfCheck(text, *def_, row_, qsos_, qtcs, &err)) {
        // A file that fails its own parse-back must never reach a
        // sponsor's robot. Refuse loudly.
        QMessageBox::critical(
            this, "Cabrillo verification FAILED",
            "The generated file does not match the log — NOT written.\n\n"
                + err);
        trace("CABRILLO SELF-CHECK FAILED: " + err);
        return;
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
        + "/Cabrillo";
    QDir().mkpath(dir);
    const QString path = dir + "/" + st.call + "-" + def_->cabrilloName
        + ".log";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Cabrillo", "Cannot write " + path);
        return;
    }
    f.write(text.toUtf8());
    f.close();
    trace(QString("CABRILLO %1 (%2 QSOs, %3 QTC, verified)")
              .arg(path)
              .arg(qsos_.size())
              .arg(qtcs.size()));
    status_->setText(
        QString("Cabrillo written and verified — %1 QSOs, %2 QTC → %3")
            .arg(qsos_.size())
            .arg(qtcs.size())
            .arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ContestWindow::closeEvent(QCloseEvent* e) {
    QSettings().setValue("contest/mgrGeom", saveGeometry());
    QDialog::closeEvent(e);
}

void ContestWindow::trace(const QString& line) {
    static const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/contest-trace.log";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    f.write((QDateTime::currentDateTimeUtc().toString(
                 "yyyy-MM-dd HH:mm:ss ")
             + line + "\n")
                .toUtf8());
}

} // namespace ttc
