// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/QtcDialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "contest/ContestEngine.h"

namespace ttc {

QtcDialog::QtcDialog(ContestDb* db,
                     std::function<void(const QString&)> keyFn,
                     std::function<void()> stopFn, QWidget* parent)
    : QDialog(parent), db_(db), keyFn_(std::move(keyFn)),
      stopFn_(std::move(stopFn)) {
    setWindowTitle("QTC — WAE");
    auto* lay = new QVBoxLayout(this);

    // Send | Receive — shown for WAE RTTY only.
    modeRow_ = new QWidget(this);
    {
        auto* mr = new QHBoxLayout(modeRow_);
        mr->setContentsMargins(0, 0, 0, 0);
        sendTab_ = new QPushButton("Send", modeRow_);
        rcvTab_ = new QPushButton("Receive", modeRow_);
        for (QPushButton* b : {sendTab_, rcvTab_}) {
            b->setCheckable(true);
            b->setStyleSheet("QPushButton:checked { background:#2b5c8a;"
                             " color:#ffffff; }");
            b->setMinimumWidth(80);
            mr->addWidget(b);
        }
        sendTab_->setChecked(true);
        mr->addStretch(1);
        connect(sendTab_, &QPushButton::clicked, this, [this] { showPage(false); });
        connect(rcvTab_, &QPushButton::clicked, this, [this] { showPage(true); });
    }
    modeRow_->setVisible(false);
    lay->addWidget(modeRow_);

    auto* top = new QHBoxLayout;
    toLbl_ = new QLabel("To:", this);
    top->addWidget(toLbl_);
    to_ = new QLineEdit(this);
    to_->setMaxLength(14);
    QFont bf = to_->font();
    bf.setPointSize(bf.pointSize() + 3);
    to_->setFont(bf);
    connect(to_, &QLineEdit::textEdited, this, [this] {
        manualTo_ = true;              // the operator took the wheel
        const int pos = to_->cursorPosition();
        const QSignalBlocker b(to_);
        to_->setText(to_->text().toUpper());
        to_->setCursorPosition(pos);
        refreshCounters();
    });
    top->addWidget(to_);
    sentTo_ = new QLabel(this);
    top->addWidget(sentTo_);
    top->addStretch(1);
    blockLbl_ = new QLabel(this);
    top->addWidget(blockLbl_);
    lay->addLayout(top);

    pages_ = new QStackedWidget(this);
    auto* sendPage = new QWidget(pages_);
    auto* outer = lay;
    lay = new QVBoxLayout(sendPage);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* mid = new QHBoxLayout;
    auto* loadBtn = new QPushButton("Load", this);
    connect(loadBtn, &QPushButton::clicked, this, [this] { load(); });
    mid->addWidget(loadBtn);
    mid->addWidget(new QLabel(
        "oldest unreported QSOs · min(10, 10−sent, eligible)", this));
    mid->addStretch(1);
    auto* hdrBtn = new QPushButton("Send header", this);
    connect(hdrBtn, &QPushButton::clicked, this, [this] { sendHeader(); });
    mid->addWidget(hdrBtn);
    lay->addLayout(mid);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"#", "Time", "Call", "NR", ""});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lay->addWidget(table_, 1);

    auto* bot = new QHBoxLayout;
    auto* allBtn = new QPushButton("Send all", this);
    connect(allBtn, &QPushButton::clicked, this, [this] { sendAll(); });
    bot->addWidget(allBtn);
    bot->addWidget(new QLabel("Esc stops the keyer instantly", this));
    bot->addStretch(1);
    auto* cancelBtn = new QPushButton("Cancel block", this);
    connect(cancelBtn, &QPushButton::clicked, this,
            [this] { cancelBlock(); });
    bot->addWidget(cancelBtn);
    confirmBtn_ = new QPushButton("Confirm && log block", this);
    connect(confirmBtn_, &QPushButton::clicked, this,
            [this] { confirmBlock(); });
    bot->addWidget(confirmBtn_);
    lay->addLayout(bot);

    pages_->addWidget(sendPage);

    // ---- Receive page (RTTY) ----
    auto* rcvPage = new QWidget(pages_);
    {
        auto* rl = new QVBoxLayout(rcvPage);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* r1 = new QHBoxLayout;
        r1->addWidget(new QLabel("His series:", rcvPage));
        series_ = new QLineEdit(rcvPage);
        series_->setPlaceholderText("12/5");
        series_->setMaximumWidth(80);
        connect(series_, &QLineEdit::textEdited, this, [this] { rcvTouched_ = true; });
        r1->addWidget(series_);
        const auto keyBtn = [&](const QString& label, std::function<QString()> text) {
            auto* b = new QPushButton(label, rcvPage);
            connect(b, &QPushButton::clicked, this, [this, text] {
                if (keyFn_) keyFn_(text());
            });
            r1->addWidget(b);
        };
        keyBtn("QRV", [] { return QStringLiteral("QRV QRV"); });
        keyBtn("AGN", [] { return QStringLiteral("AGN AGN"); });
        keyBtn("QSL", [this] {
            const QString sr = series_->text().trimmed();
            return sr.isEmpty() ? QStringLiteral("QSL TU")
                                : QString("QSL %1 TU").arg(sr);
        });
        r1->addStretch(1);
        rcvCount_ = new QLabel(rcvPage);
        r1->addWidget(rcvCount_);
        rl->addLayout(r1);
        rcvTable_ = new QTableWidget(10, 3, rcvPage);
        rcvTable_->setHorizontalHeaderLabels({"Time", "Call", "NR"});
        rcvTable_->horizontalHeader()->setStretchLastSection(true);
        connect(rcvTable_, &QTableWidget::cellChanged, this, [this](int, int) {
            if (!filling_) rcvTouched_ = true;     // his hands: hands off
        });
        rl->addWidget(rcvTable_, 1);
        auto* r2 = new QHBoxLayout;
        auto* clr = new QPushButton("Clear", rcvPage);
        connect(clr, &QPushButton::clicked, this, [this] { clearReceive(); });
        r2->addWidget(clr);
        r2->addWidget(new QLabel("fills from the print · edit any cell · "
                                 "Clear starts over", rcvPage));
        r2->addStretch(1);
        auto* save = new QPushButton("Save && log block", rcvPage);
        connect(save, &QPushButton::clicked, this, [this] { saveReceived(); });
        r2->addWidget(save);
        rl->addLayout(r2);
    }
    pages_->addWidget(rcvPage);
    lay = outer;
    lay->addWidget(pages_, 1);

    status_ = new QLabel(
        "Confirm is all-or-nothing · reported QSOs lock forever", this);
    lay->addWidget(status_);

    for (QPushButton* b : findChildren<QPushButton*>()) {
        b->setAutoDefault(false);      // Enter must never "click" a send
        b->setDefault(false);
        b->setFocusPolicy(Qt::NoFocus);
    }
    resize(560, 540);
}

void QtcDialog::setBothWays(bool on, const QString& mode) {
    bothWays_ = on;
    mode_ = mode.isEmpty() ? QStringLiteral("CW") : mode;
    modeRow_->setVisible(on);
    if (!on) showPage(false);
}

int QtcDialog::usedWith(const QString& call) const {
    if (contestId_ < 0) return 0;
    return db_->qtcSentTo(contestId_, call)
         + (bothWays_ ? db_->qtcRcvdFrom(contestId_, call) : 0);
}

void QtcDialog::showPage(bool receive) {
    if (receive && !bothWays_) receive = false;
    sendTab_->setChecked(!receive);
    rcvTab_->setChecked(receive);
    pages_->setCurrentIndex(receive ? 1 : 0);
    toLbl_->setText(receive ? "From:" : "To:");
    if (receive) clearReceive();
    refreshCounters();
}

void QtcDialog::appendRx(const QString& text) {
    rxBuf_ += text;
    if (rxBuf_.size() > 4000) rxBuf_ = rxBuf_.right(3000);
    if (isVisible() && pages_->currentIndex() == 1 && !rcvTouched_) fillFromRx();
}

void QtcDialog::fillFromRx() {
    const QtcParsed p = parseQtcText(rxBuf_);
    filling_ = true;
    if (p.block > 0)
        series_->setText(QString("%1/%2").arg(p.block).arg(p.count));
    for (int i = 0; i < 10; ++i) {
        const bool have = i < p.lines.size();
        const QString v[3] = {have ? p.lines[i].qTime : QString(),
                              have ? p.lines[i].qCall : QString(),
                              have ? p.lines[i].qSerial : QString()};
        for (int col = 0; col < 3; ++col) {
            QTableWidgetItem* it = rcvTable_->item(i, col);
            if (!it) rcvTable_->setItem(i, col, it = new QTableWidgetItem);
            if (it->text() != v[col]) it->setText(v[col]);
        }
    }
    filling_ = false;
    rcvCount_->setText(p.count > 0
                           ? QString("%1 of %2 copied").arg(p.lines.size()).arg(p.count)
                           : QString("%1 copied").arg(p.lines.size()));
}

void QtcDialog::clearReceive() {
    rxBuf_.clear();
    rcvTouched_ = false;
    series_->clear();
    fillFromRx();                          // empties the grid
    status_->setText("ready to copy — he sends, the grid fills; QSL when it is right");
}

void QtcDialog::saveReceived() {
    if (contestId_ < 0) return;
    const QString from = to_->text().trimmed().toUpper();
    if (!loggableCall(from)) {
        status_->setText("whose QTCs? type (or let the deck fill) his call first");
        return;
    }
    if (refuse_) {
        const QString why = refuse_(from);
        if (!why.isEmpty()) { status_->setText(why); return; }
    }
    QList<ContestDb::QtcRcvdLine> lines;
    for (int i = 0; i < 10; ++i) {
        const auto cell = [&](int col) {
            const QTableWidgetItem* it = rcvTable_->item(i, col);
            return it ? it->text().trimmed().toUpper() : QString();
        };
        const QString tm = cell(0), call = cell(1), nr = cell(2);
        if (tm.isEmpty() && call.isEmpty() && nr.isEmpty()) continue;
        if (tm.size() != 4 || call.isEmpty() || nr.isEmpty()) {
            status_->setText(QString("line %1 is incomplete — fix it or empty "
                                     "it (AGN asks for a repeat)").arg(i + 1));
            return;
        }
        lines.push_back({tm, call, nr});
    }
    if (lines.isEmpty()) {
        status_->setText("nothing copied yet");
        return;
    }
    const int block = series_->text().section('/', 0, 0).trimmed().toInt();
    if (block <= 0) {
        status_->setText("his series number is missing (the 12 of QTC 12/5)");
        return;
    }
    const int used = usedWith(from);
    if (used + lines.size() > 10) {
        status_->setText(QString("%1 already has %2 of 10 with you — only %3 "
                                 "more may be logged")
                             .arg(from).arg(used).arg(qMax(0, 10 - used)));
        return;
    }
    if (!db_->addQtcRcvdBlock(contestId_, from, block, lines, rigHz_, mode_)) {
        status_->setText("DATABASE ERROR — block NOT logged");
        return;
    }
    const int n = int(lines.size());
    clearReceive();
    status_->setText(QString("QTC %1/%2 from %3 logged — %2 points")
                         .arg(block).arg(n).arg(from));
    refreshCounters();
}

void QtcDialog::openFor(qint64 contestId) {
    contestId_ = contestId;
    manualTo_ = false;
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    showPage(false);
    refreshCounters();
    show();
    raise();
    activateWindow();
}

void QtcDialog::followCall(const QString& call) {
    if (manualTo_ || !isVisible()) return;
    const QSignalBlocker b(to_);
    to_->setText(call.trimmed().toUpper());
    refreshCounters();
}

void QtcDialog::refreshCounters() {
    if (contestId_ < 0) return;
    const QString to = to_->text().trimmed();
    sentTo_->setText(to.isEmpty()
                         ? QString()
                         : QString(bothWays_ ? "with %1: %2 of 10 (sent + received)"
                                             : "sent to %1: %2 of 10")
                               .arg(to)
                               .arg(usedWith(to)));
    blockLbl_->setText(
        QString("next block %1").arg(db_->nextQtcBlock(contestId_)));
}

void QtcDialog::load() {
    if (contestId_ < 0) return;
    const QString to = to_->text().trimmed().toUpper();
    if (!loggableCall(to)) {
        status_->setText("type (or let the deck fill) the receiving "
                         "station first");
        return;
    }
    if (refuse_) {
        const QString why = refuse_(to);
        if (!why.isEmpty()) {
            status_->setText(why);
            return;
        }
    }
    const QList<ContestQso> qsos = db_->qsos(contestId_);
    const QList<qint64> ids =
        allocateQtc(qsos, db_->qtcReportedQsoIds(contestId_), to,
                    usedWith(to));
    alloc_.clear();
    for (qint64 id : ids)
        for (const ContestQso& q : qsos)
            if (q.id == id) { alloc_ << q; break; }
    block_ = db_->nextQtcBlock(contestId_);
    table_->setRowCount(int(alloc_.size()));
    for (int i = 0; i < alloc_.size(); ++i) {
        const ContestQso& q = alloc_[i];
        table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        table_->setItem(i, 1, new QTableWidgetItem(
            q.tsUtc.toUTC().toString("HHmm")));
        table_->setItem(i, 2, new QTableWidgetItem(q.v.call));
        table_->setItem(i, 3, new QTableWidgetItem(q.v.serialR));
        auto* send = new QPushButton("Send", table_);
        send->setFocusPolicy(Qt::NoFocus);
        connect(send, &QPushButton::clicked, this,
                [this, i] { sendRow(i); });
        table_->setCellWidget(i, 4, send);
    }
    confirmBtn_->setEnabled(!alloc_.isEmpty());
    status_->setText(alloc_.isEmpty()
                         ? QString("nothing to send to %1 — limit reached "
                                   "or no eligible QSOs").arg(to)
                         : QString("QTC %1/%2 loaded — Send all, then "
                                   "Confirm when he acknowledges")
                               .arg(block_)
                               .arg(alloc_.size()));
}

QString QtcDialog::rowText(int i) const {
    const ContestQso& q = alloc_[i];
    return q.tsUtc.toUTC().toString("HHmm") + ' ' + q.v.call + ' '
        + q.v.serialR;
}

void QtcDialog::sendHeader() {
    if (alloc_.isEmpty() || !keyFn_) return;
    const QString h = QString("QTC %1/%2").arg(block_).arg(alloc_.size());
    keyFn_(bothWays_ ? h + ' ' + h : h);   // a teleprinter gets it twice
}

void QtcDialog::sendRow(int i) {
    if (i < 0 || i >= alloc_.size() || !keyFn_) return;
    keyFn_(rowText(i));
    table_->selectRow(i);
}

void QtcDialog::sendAll() {
    if (alloc_.isEmpty() || !keyFn_) return;
    sendHeader();
    for (int i = 0; i < alloc_.size(); ++i) keyFn_(rowText(i));
    status_->setText("block keyed — Confirm when he acknowledges, "
                     "Cancel if it fell apart");
}

void QtcDialog::confirmBlock() {
    if (alloc_.isEmpty()) return;
    QList<qint64> ids;
    for (const ContestQso& q : alloc_) ids << q.id;
    if (!db_->addQtcBlock(contestId_, to_->text(), block_, ids, rigHz_,
                          mode_)) {
        // Silence here once cost a contest evening of doubt — say it.
        status_->setText("DATABASE ERROR — block NOT logged, QSOs still "
                         "available");
        return;
    }
    status_->setText(QString("QTC %1/%2 to %3 logged — those QSOs are "
                             "spent")
                         .arg(block_)
                         .arg(alloc_.size())
                         .arg(to_->text().trimmed()));
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    refreshCounters();
}

void QtcDialog::cancelBlock() {
    alloc_.clear();
    table_->setRowCount(0);
    confirmBtn_->setEnabled(false);
    status_->setText("block discarded — nothing spent");
}

void QtcDialog::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {  // stop keying, never close
        if (stopFn_) stopFn_();
        status_->setText("keying stopped");
        return;
    }
    QDialog::keyPressEvent(e);
}

} // namespace ttc
