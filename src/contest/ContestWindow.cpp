// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestWindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLocale>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>

#include "contest/Cabrillo.h"
#include "cw/CwWindow.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

namespace {
QString fkeyLabel(const QString& spec) {   // "Label|text" -> "Label"
    const int bar = spec.indexOf('|');
    return bar < 0 ? spec : spec.left(bar);
}
QString fkeyText(const QString& spec) {
    const int bar = spec.indexOf('|');
    return bar < 0 ? QString() : spec.mid(bar + 1);
}
} // namespace

ContestWindow::ContestWindow(ContestDb* db, const CtyLookup* cty,
                             CwWindow* cw, QWidget* parent)
    : QDialog(parent), db_(db), cty_(cty), cw_(cw) {
    setWindowTitle("Contest");
    buildUi();
    connect(db_, &ContestDb::changed, this, [this] {
        if (contestId_ >= 0) refreshAll();
    });
    connect(&clockTimer_, &QTimer::timeout, this, [this] {
        clock_->setText(QDateTime::currentDateTimeUtc()
                            .toString("HH:mm:ss'z'"));
    });
    clockTimer_.start(1000);
    const QByteArray geom =
        QSettings().value("contest/geom").toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);
    refreshResumeList();
}

void ContestWindow::buildUi() {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // ---- setup strip: pick a definition or resume an instance ----------
    setupStrip_ = new QWidget(this);
    {
        auto* h = new QHBoxLayout(setupStrip_);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(new QLabel("New:", setupStrip_));
        defPick_ = new QComboBox(setupStrip_);
        for (const ContestDef* d : contestDefs())
            defPick_->addItem(d->title, d->id);
        h->addWidget(defPick_);
        h->addWidget(new QLabel("sent exch:", setupStrip_));
        sentExchEdit_ = new QLineEdit(setupStrip_);
        sentExchEdit_->setFixedWidth(110);
        sentExchEdit_->setToolTip(
            "What you send besides RST/serial — CWT \"Jon MI\", CQ WW your "
            "zone. Serial contests can leave it empty.");
        h->addWidget(sentExchEdit_);
        h->addWidget(new QLabel("location:", setupStrip_));
        locationEdit_ = new QLineEdit(
            QSettings().value("contest/location", "MI").toString(),
            setupStrip_);
        locationEdit_->setFixedWidth(50);
        locationEdit_->setToolTip("Cabrillo LOCATION: (ARRL section / state)");
        h->addWidget(locationEdit_);
        auto* newBtn = new QPushButton("Start", setupStrip_);
        connect(newBtn, &QPushButton::clicked, this,
                [this] { newContest(); });
        h->addWidget(newBtn);
        h->addSpacing(20);
        h->addWidget(new QLabel("Resume:", setupStrip_));
        resumePick_ = new QComboBox(setupStrip_);
        resumePick_->setMinimumWidth(220);
        h->addWidget(resumePick_);
        auto* resBtn = new QPushButton("Open", setupStrip_);
        connect(resBtn, &QPushButton::clicked, this, [this] {
            const qint64 id = resumePick_->currentData().toLongLong();
            if (id > 0) openContest(id);
        });
        h->addWidget(resBtn);
        h->addStretch(1);
        connect(defPick_, &QComboBox::currentIndexChanged, this, [this] {
            const ContestDef* d =
                contestDef(defPick_->currentData().toString());
            if (d) sentExchEdit_->setText(d->sentExchDefault);
        });
        if (const ContestDef* d0 =
                contestDef(defPick_->currentData().toString()))
            sentExchEdit_->setText(d0->sentExchDefault);
    }
    lay->addWidget(setupStrip_);

    // ---- header: title · clock · run/s&p · speed · score ---------------
    {
        auto* h = new QHBoxLayout;
        title_ = new QLabel("no contest open", this);
        QFont tf = title_->font();
        tf.setBold(true);
        title_->setFont(tf);
        h->addWidget(title_);
        h->addSpacing(12);
        clock_ = new QLabel("--:--:--z", this);
        QFont cf = clock_->font();
        cf.setPointSize(cf.pointSize() + 4);
        clock_->setFont(cf);
        h->addWidget(clock_);
        h->addSpacing(12);
        runBtn_ = new QPushButton("RUN", this);
        spBtn_ = new QPushButton("S&&P", this);
        for (QPushButton* b : {runBtn_, spBtn_}) {
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            h->addWidget(b);
        }
        runBtn_->setChecked(true);
        connect(runBtn_, &QPushButton::clicked, this, [this] {
            runMode_ = true;
            runBtn_->setChecked(true);
            spBtn_->setChecked(false);
            applyFkeyLabels();
        });
        connect(spBtn_, &QPushButton::clicked, this, [this] {
            runMode_ = false;
            runBtn_->setChecked(false);
            spBtn_->setChecked(true);
            applyFkeyLabels();
        });
        h->addSpacing(12);
        h->addWidget(new QLabel("wpm", this));
        wpm_ = new QSpinBox(this);
        wpm_->setRange(5, 60);
        wpm_->setValue(cw_ ? cw_->speedWpm()
                           : QSettings().value("cw/wpm", 30).toInt());
        wpm_->setFocusPolicy(Qt::ClickFocus);  // arrows stay on the entry
        wpm_->setToolTip("CW speed — PgUp/PgDn from anywhere");
        connect(wpm_, &QSpinBox::valueChanged, this, [this](int v) {
            if (cw_) cw_->setSpeedWpm(v);
        });
        h->addWidget(wpm_);
        h->addStretch(1);
        score_ = new QLabel(this);
        h->addWidget(score_);
        h->addSpacing(10);
        rate_ = new QLabel(this);
        h->addWidget(rate_);
        lay->addLayout(h);
    }

    // ---- entry row ------------------------------------------------------
    entryBox_ = new QWidget(this);
    {
        auto* h = new QHBoxLayout(entryBox_);
        h->setContentsMargins(0, 0, 0, 0);
        auto* callBox = new QVBoxLayout;
        auto* callLbl = new QLabel("CALL", entryBox_);
        call_ = new QLineEdit(entryBox_);
        call_->setObjectName("entryCall");
        QFont bf = call_->font();
        bf.setPointSize(bf.pointSize() + 4);
        call_->setFont(bf);
        call_->setMaxLength(14);
        call_->setMinimumWidth(150);
        connect(call_, &QLineEdit::textEdited, this,
                [this] { onCallEdited(); });
        connect(call_, &QLineEdit::returnPressed, this,
                [this] { tryLog(); });
        callBox->addWidget(callLbl);
        callBox->addWidget(call_);
        h->addLayout(callBox);
        fieldsBox_ = new QWidget(entryBox_);
        new QHBoxLayout(fieldsBox_);
        fieldsBox_->layout()->setContentsMargins(0, 0, 0, 0);
        h->addWidget(fieldsBox_);
        h->addStretch(1);
        auto* logBtn = new QPushButton("LOG", entryBox_);
        logBtn->setToolTip("Enter logs from any field once the exchange "
                           "is complete");
        connect(logBtn, &QPushButton::clicked, this, [this] { tryLog(); });
        h->addWidget(logBtn);
        auto* wipeBtn = new QPushButton("Wipe ^W", entryBox_);
        connect(wipeBtn, &QPushButton::clicked, this, [this] { wipe(); });
        h->addWidget(wipeBtn);
        auto* cabBtn = new QPushButton("Cabrillo…", entryBox_);
        connect(cabBtn, &QPushButton::clicked, this,
                [this] { exportCabrillo(); });
        h->addWidget(cabBtn);
        for (QPushButton* b : {logBtn, wipeBtn, cabBtn})
            b->setFocusPolicy(Qt::NoFocus);
    }
    lay->addWidget(entryBox_);

    {
        auto* h = new QHBoxLayout;
        dupe_ = new QLabel(this);
        dupe_->setStyleSheet("color:#e0b050; font-weight:bold;");
        h->addWidget(dupe_);
        info_ = new QLabel(this);
        h->addWidget(info_);
        h->addStretch(1);
        lay->addLayout(h);
    }

    // ---- F keys ---------------------------------------------------------
    {
        auto* h = new QHBoxLayout;
        h->setSpacing(3);
        for (int i = 0; i < 12; ++i) {
            fk_[i] = new QPushButton(this);
            fk_[i]->setFocusPolicy(Qt::NoFocus);
            fk_[i]->setMinimumWidth(52);
            connect(fk_[i], &QPushButton::clicked, this,
                    [this, i] { keyFkey(i); });
            h->addWidget(fk_[i], 1);
            auto* sc = new QShortcut(
                QKeySequence(Qt::Key_F1 + i), this);
            connect(sc, &QShortcut::activated, this,
                    [this, i] { keyFkey(i); });
        }
        auto* stop = new QPushButton("STOP (Esc)", this);
        stop->setFocusPolicy(Qt::NoFocus);
        connect(stop, &QPushButton::clicked, this, [this] {
            if (cw_) cw_->stopKeying();
        });
        h->addWidget(stop);
        lay->addLayout(h);
    }

    // ---- log table ------------------------------------------------------
    table_ = new QTableWidget(this);
    table_->setColumnCount(10);
    table_->setHorizontalHeaderLabels({"UTC", "Call", "kHz", "Snt", "SNr",
                                       "Rcv", "RNr", "Ex1", "Ex2", "Pt"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(table_, 1);

    status_ = new QLabel(this);
    lay->addWidget(status_);

    auto* wipeSc = new QShortcut(QKeySequence("Ctrl+W"), this);
    connect(wipeSc, &QShortcut::activated, this, [this] { wipe(); });

    // Ctrl+Q must do NOTHING here — Q is QRZ, not quit. An explicit
    // no-op shortcut shadows any ambient QuitRole binding.
    auto* noQuit = new QShortcut(QKeySequence("Ctrl+Q"), this);
    connect(noQuit, &QShortcut::activated, this, [] {});

    applyFkeyLabels();
    entryBox_->setEnabled(false);
    resize(980, 620);
}

void ContestWindow::refreshResumeList() {
    resumePick_->clear();
    for (const ContestRow& c : db_->contests()) {
        const QList<ContestQso> q = db_->qsos(c.id);
        resumePick_->addItem(
            QString("%1  (%2 QSOs)").arg(c.title).arg(q.size()), c.id);
    }
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

bool ContestWindow::openContestId(qint64 id) {
    openContest(id);
    return contestId_ == id;
}

void ContestWindow::openContest(qint64 id) {
    row_ = db_->contest(id);
    def_ = contestDef(row_.defId);
    if (row_.id < 0 || !def_) {
        status_->setText("contest not found or its definition is missing");
        return;
    }
    contestId_ = id;
    ctx_ = ContestContext();
    ctx_.myCall =
        QSettings().value("station/callsign", "N8EM").toString().toUpper();
    CtyInfo me;
    if (cty_ && cty_->info(normalizeForCty(ctx_.myCall), me)) {
        ctx_.myCont = me.cont;
        ctx_.myCountry = me.country;
        ctx_.myCq = me.cq;
    }
    title_->setText(row_.title);
    setWindowTitle("Contest — " + row_.title + " — " + ctx_.myCall);
    setupStrip_->setVisible(false);
    entryBox_->setEnabled(true);
    rebuildEntryFields();
    applyFkeyLabels();
    refreshAll();
    trace(QString("OPEN %1 id=%2").arg(row_.defId).arg(id));
    call_->setFocus();
}

void ContestWindow::rebuildEntryFields() {
    edits_.clear();
    rstS_ = nullptr;
    sentNr_ = nullptr;
    QLayout* l = fieldsBox_->layout();
    while (QLayoutItem* it = l->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    auto addField = [&](const QString& label, int widthCh,
                        const QString& preset) {
        auto* box = new QVBoxLayout;
        box->addWidget(new QLabel(label, fieldsBox_));
        auto* e = new QLineEdit(preset, fieldsBox_);
        e->setMaxLength(qMax(widthCh + 4, 6));
        e->setFixedWidth(24 + widthCh * 11);
        connect(e, &QLineEdit::returnPressed, this, [this] { tryLog(); });
        box->addWidget(e);
        static_cast<QHBoxLayout*>(l)->addLayout(box);
        return e;
    };
    if (def_->hasRst) rstS_ = addField("SNT", 4, "599");
    if (def_->sentSerial) {
        auto* box = new QVBoxLayout;
        box->addWidget(new QLabel("SENT NR", fieldsBox_));
        sentNr_ = new QLabel(fieldsBox_);
        QFont f = sentNr_->font();
        f.setBold(true);
        sentNr_->setFont(f);
        box->addWidget(sentNr_);
        static_cast<QHBoxLayout*>(l)->addLayout(box);
    }
    for (const ExchFieldDef& fd : def_->fields) {
        QLineEdit* e = addField(fd.label, fd.widthCh, fd.preset);
        e->setObjectName(QString("exchEdit%1").arg(edits_.size()));
        edits_ << qMakePair(fd.col, e);
    }
}

QString ContestWindow::currentBand() const {
    // Offline (no CAT) falls back to 20 m so bench testing can log at
    // all; on the air the rig feed overwrites this within a second.
    return rigHz_ > 0 ? LogbookIndex::bandForHz(rigHz_)
                      : QStringLiteral("20M");
}

void ContestWindow::setRig(qint64 hz, const QString& adifMode) {
    rigHz_ = hz;
    rigMode_ = adifMode.isEmpty() ? QStringLiteral("CW") : adifMode;
    onCallEdited();                  // band change can flip the dupe verdict
}

void ContestWindow::onCallEdited() {
    // Uppercase in place without disturbing the cursor.
    const int pos = call_->cursorPosition();
    const QString up = call_->text().toUpper();
    if (up != call_->text()) {
        const QSignalBlocker b(call_);
        call_->setText(up);
        call_->setCursorPosition(pos);
    }
    if (contestId_ < 0) return;
    const QString c = up.trimmed();
    if (c.isEmpty()) {
        dupe_->clear();
        info_->clear();
        return;
    }
    dupe_->setText(def_ && isDupe(*def_, values_, c, currentBand(), "CW")
                       ? QString("DUPE — worked on %1").arg(currentBand())
                       : QString());
    CtyInfo ci;
    if (cty_ && cty_->info(normalizeForCty(c), ci))
        info_->setText(QString("%1 · %2 · CQ %3")
                           .arg(ci.country, ci.cont)
                           .arg(ci.cq));
    else
        info_->setText("—");
}

void ContestWindow::tryLog() {
    if (contestId_ < 0 || !def_) return;
    const QString c = call_->text().trimmed().toUpper();
    if (!loggableCall(c)) {
        status_->setText("call needs 3+ characters with a letter and a "
                         "digit — not logged");
        call_->setFocus();
        return;
    }
    for (int i = 0; i < def_->fields.size(); ++i) {
        if (!def_->fields[i].required) continue;
        if (edits_[i].second->text().trimmed().isEmpty()) {
            status_->setText(def_->fields[i].label
                             + " is required — not logged");
            edits_[i].second->setFocus();
            return;
        }
    }
    ContestQso q;
    q.contestId = contestId_;
    q.tsUtc = QDateTime::currentDateTimeUtc();
    q.freqHz = rigHz_ > 0 ? rigHz_ : 14030000;
    q.v.call = c;
    q.v.band = currentBand();
    q.v.mode = rigMode_ == QLatin1String("SSB") ? "SSB" : "CW";
    q.v.rstS = def_->hasRst && rstS_ ? rstS_->text().trimmed() : QString();
    q.v.serialS = def_->sentSerial ? row_.nextSerial : 0;
    q.runSp = runMode_ ? "R" : "S";
    for (const auto& [col, edit] : edits_) {
        const QString t = edit->text().trimmed();
        switch (col) {
            case ExchCol::RstR: q.v.rstR = t; break;
            case ExchCol::SerialR: q.v.serialR = t; break;
            case ExchCol::Exch1: q.v.exch1 = t; break;
            case ExchCol::Exch2: q.v.exch2 = t; break;
        }
    }
    CtyInfo ci;
    const bool ok = cty_ && cty_->info(normalizeForCty(c), ci);
    q.points = def_->points ? def_->points(q.v, ci, ok, ctx_) : 0;
    if (db_->addQso(q) < 0) {
        status_->setText("DATABASE ERROR — the QSO did not save");
        trace("ADD FAILED " + c);
        return;
    }
    if (def_->sentSerial) {
        row_.nextSerial++;
        db_->setNextSerial(contestId_, row_.nextSerial);
    }
    trace(QString("QSO %1 %2 %3 ser %4")
              .arg(c, q.v.band, q.v.serialR)
              .arg(q.v.serialS));
    // The field wipe IS the "it logged" signal — silent, per the
    // operator's spec. Presets refill, serial advances, focus home.
    wipe();
    refreshAll();                    // serial display catches the bump
    status_->setText(QString("logged %1 · %2 QSOs").arg(c).arg(
        values_.size()));
}

void ContestWindow::wipe() {
    call_->clear();
    if (rstS_) rstS_->setText("599");
    for (int i = 0; i < edits_.size() && i < def_->fields.size(); ++i)
        edits_[i].second->setText(def_->fields[i].preset);
    dupe_->clear();
    info_->clear();
    call_->setFocus();
}

void ContestWindow::keyFkey(int idx0) {
    if (!def_) return;
    const QHash<int, QString>& set =
        (!runMode_ && !def_->fkeySp.isEmpty()) ? def_->fkeySp
                                               : def_->fkeyRun;
    const QString spec = set.value(idx0 + 1);
    if (spec.isEmpty()) return;
    const QString text =
        expandMacro(fkeyText(spec), *def_, ctx_, call_->text(),
                    row_.sentExch, row_.nextSerial);
    if (text.isEmpty()) return;
    if (!cw_) {
        status_->setText("no keyer available (CW window backend missing)");
        return;
    }
    cw_->keyExternal(text);
    status_->setText("→ " + text);
}

void ContestWindow::applyFkeyLabels() {
    const QHash<int, QString> empty;
    const QHash<int, QString>& set =
        def_ ? ((!runMode_ && !def_->fkeySp.isEmpty()) ? def_->fkeySp
                                                       : def_->fkeyRun)
             : empty;
    for (int i = 0; i < 12; ++i) {
        const QString spec = set.value(i + 1);
        fk_[i]->setText(QString("F%1\n%2").arg(i + 1).arg(
            spec.isEmpty() ? QStringLiteral("—") : fkeyLabel(spec)));
        fk_[i]->setEnabled(!spec.isEmpty());
        fk_[i]->setToolTip(fkeyText(spec));
    }
}

void ContestWindow::refreshAll() {
    qsos_ = db_->qsos(contestId_);
    values_.clear();
    for (const ContestQso& q : qsos_) values_ << q.v;
    if (sentNr_)
        sentNr_->setText(formatSerial(row_.nextSerial, def_->cutNumbers,
                                      def_->serialPad));
    // newest first in the table
    table_->setRowCount(int(qsos_.size()));
    for (int i = 0; i < qsos_.size(); ++i) {
        const ContestQso& q = qsos_[qsos_.size() - 1 - i];
        const auto put = [&](int col, const QString& t) {
            table_->setItem(i, col, new QTableWidgetItem(t));
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
        put(9, QString::number(q.points));
    }
    refreshScore();
    refreshResumeList();
}

void ContestWindow::refreshScore() {
    const ScoreBreakdown sb = computeScore(*def_, values_, cty_, ctx_);
    score_->setText(QString("QSOs %1 · Pts %2 · Mults %3 (wt %4) · "
                            "Score %5")
                        .arg(sb.qsos)
                        .arg(sb.points)
                        .arg(sb.mults)
                        .arg(sb.weightedMults)
                        .arg(QLocale::c().toString(qlonglong(sb.total))));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int in10 = 0, in60 = 0;
    for (const ContestQso& q : qsos_) {
        const qint64 s = q.tsUtc.secsTo(now);
        if (s <= 600) ++in10;
        if (s <= 3600) ++in60;
    }
    rate_->setText(QString("rate %1/h (10 m) · %2/h")
                       .arg(in10 * 6)
                       .arg(in60));
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
    const QString text =
        Cabrillo::build(*def_, row_, qsos_, st, cty_, ctx_);
    QString err;
    if (!Cabrillo::selfCheck(text, *def_, row_, qsos_, &err)) {
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
        QMessageBox::critical(this, "Cabrillo",
                              "Cannot write " + path);
        return;
    }
    f.write(text.toUtf8());
    f.close();
    trace(QString("CABRILLO %1 (%2 QSOs, verified)")
              .arg(path)
              .arg(qsos_.size()));
    status_->setText(QString("Cabrillo written and verified — %1 QSOs → %2")
                         .arg(qsos_.size())
                         .arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ContestWindow::setSpeed(int wpm) {
    wpm_->setValue(wpm);             // handler forwards to the keyer
}

void ContestWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {   // Esc = stop keying, NEVER close
        if (cw_) cw_->stopKeying();
        status_->setText("keying stopped");
        return;
    }
    if (e->key() == Qt::Key_PageUp) {
        setSpeed(wpm_->value() + 1);
        return;
    }
    if (e->key() == Qt::Key_PageDown) {
        setSpeed(wpm_->value() - 1);
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        tryLog();
        return;
    }
    QDialog::keyPressEvent(e);
}

void ContestWindow::closeEvent(QCloseEvent* e) {
    QSettings().setValue("contest/geom", saveGeometry());
    QDialog::closeEvent(e);
}

void ContestWindow::trace(const QString& line) {
    // Contest software runs where no terminal is watching; every
    // consequential action leaves a line on disk.
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
