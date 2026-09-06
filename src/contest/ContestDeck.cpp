// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestDeck.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QStandardPaths>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>

#include "contest/QtcDialog.h"
#include "cw/CwWindow.h"
#include "log/QrzLookup.h"
#include "net/RotorLink.h"
#include "util/Bearing.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

namespace {
QString fkeyLabel(const QString& spec) {
    const int bar = spec.indexOf('|');
    return bar < 0 ? spec : spec.left(bar);
}
QString fkeyText(const QString& spec) {
    const int bar = spec.indexOf('|');
    return bar < 0 ? QString() : spec.mid(bar + 1);
}
const char* kGlowStyle =
    "QPushButton { background:#173423; border:1px solid #3fb46a;"
    " color:#a9f0c6; }";
} // namespace

ContestDeck::ContestDeck(ContestDb* db, const CtyLookup* cty, CwWindow* cw,
                         RotorLink* rotor, QrzLookup* qrz, QWidget* parent)
    : QWidget(parent), db_(db), cty_(cty), cw_(cw), rotor_(rotor),
      qrz_(qrz) {
    buildUi();
    connect(db_, &ContestDb::changed, this, [this] {
        if (contestId_ >= 0) refreshAll();
    });
    // A QRZ grid sharpens the heading when it lands; a failure leaves
    // the (labeled) centroid and a trace line, never a stall.
    if (qrz_)
        connect(qrz_, &QrzLookup::result, this,
                [this](const QString& call, bool ok, const QString&,
                       const QString&, const QString& grid,
                       const QString& err) {
                    const QString c = call.trimmed().toUpper();
                    if (ok && !grid.trimmed().isEmpty())
                        qrzGrid_.insert(c, grid.trimmed());
                    else if (!ok)
                        trace("QRZ " + c + " failed: " + err);
                    if (c == call_->text().trimmed()) updateHeading();
                });
    qrzTimer_.setSingleShot(true);
    qrzTimer_.setInterval(700);          // fire once the typing settles
    connect(&qrzTimer_, &QTimer::timeout, this,
            [this] { requestQrz(call_->text().trimmed()); });
    connect(&clockTimer_, &QTimer::timeout, this, [this] {
        clock_->setText(
            QDateTime::currentDateTimeUtc().toString("HH:mm:ss'z'"));
    });
    clockTimer_.start(1000);
    const QString myGrid =
        QSettings().value("station/grid", "EN83al").toString();
    CtyLookup::gridToLatLon(myGrid, myLat_, myLon_);
    // Come back up in whatever contest was live last session.
    const qint64 last = QSettings().value("contest/currentId", -1).toLongLong();
    if (last > 0) openContest(last);
    // Floated panes come back floated, where they were parked.
    for (const QString& key : {QStringLiteral("read"),
                               QStringLiteral("type"),
                               QStringLiteral("log")})
        if (QSettings().value("contest/float/" + key + "/on", false)
                .toBool())
            QTimer::singleShot(0, this, [this, key] { floatPane(key); });
}

void ContestDeck::buildUi() {
    setFixedHeight(288);   // room for the F-key block's symmetric gaps
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);
    // The deck lives where the waterfall was and must never make the
    // main window wider than the toolbar already does — losing the
    // maximize button is the width-budget failure. A plain QLabel
    // forces its full text width into the layout minimum, so the long
    // pane titles and the verbose score were dragging the deck's floor
    // up on the operator's font. Every label here is marked shrinkable
    // at the end of buildUi(); the hard floor is then just the F-key
    // row, which fits.

    // ---- CW READ (left; phone contests hide it) -------------------------
    {
        readPane_ = new QWidget(this);
        auto* box = new QVBoxLayout(readPane_);
        box->setContentsMargins(0, 0, 0, 0);
        auto* trow = new QHBoxLayout;
        auto* t = new QLabel(
            "CW READ — double-click a call to grab it · right-click: size",
            this);
        t->setStyleSheet("color:#8798a8; font-size:10px;");
        trow->addWidget(t);
        trow->addStretch(1);
        auto* clr = new QPushButton("clear", this);
        clr->setFlat(true);
        clr->setFocusPolicy(Qt::NoFocus);
        clr->setStyleSheet("color:#8798a8; font-size:10px; border:0;");
        clr->setCursor(Qt::PointingHandCursor);
        trow->addWidget(clr);
        box->addLayout(trow);
        read_ = new QPlainTextEdit(this);
        read_->setReadOnly(true);
        read_->setMaximumBlockCount(200);
        const auto applyReadFont = [this](int pt) {
            QFont mf("DejaVu Sans Mono");
            mf.setPointSize(std::clamp(pt, 8, 24));
            read_->setFont(mf);
            QSettings().setValue("contest/readPt", std::clamp(pt, 8, 24));
        };
        applyReadFont(QSettings().value("contest/readPt", 12).toInt());
        connect(clr, &QPushButton::clicked, read_,
                &QPlainTextEdit::clear);
        // Right-click sizes the copy — chords stay banned in contest
        // mode, and the mouse is allowed for setup gestures.
        read_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(read_, &QPlainTextEdit::customContextMenuRequested, this,
                [this, applyReadFont](const QPoint& p) {
                    QMenu m(read_);
                    m.addAction("Clear", read_, &QPlainTextEdit::clear);
                    m.addAction("Text bigger", this, [applyReadFont] {
                        applyReadFont(
                            QSettings().value("contest/readPt", 12).toInt()
                            + 1);
                    });
                    m.addAction("Text smaller", this, [applyReadFont] {
                        applyReadFont(
                            QSettings().value("contest/readPt", 12).toInt()
                            - 1);
                    });
                    m.exec(read_->mapToGlobal(p));
                });
        read_->viewport()->installEventFilter(this);
        box->addWidget(read_, 1);
        lay->addWidget(readPane_, 11);
        addPopout("read", "CW READ", readPane_, trow,
                  [this](QWidget* w) {
                      static_cast<QHBoxLayout*>(layout())
                          ->insertWidget(0, w, 11);
                  });
    }

    // ---- center: SCP + entry -------------------------------------------
    {
        auto* mid = new QVBoxLayout;
        mid->setSpacing(3);

        // Top line: contest name (bold) + the mode buttons. The score
        // lives on its OWN line below (operator's layout: the stats
        // never belong between the name and RUN, crowding the width).
        auto* hdr = new QHBoxLayout;
        title_ = new QLabel("no contest open", this);
        QFont bf = title_->font();
        bf.setBold(true);
        title_->setFont(bf);
        hdr->addWidget(title_);
        hdr->addStretch(1);
        clock_ = new QLabel("--:--:--z", this);
        hdr->addWidget(clock_);
        runBtn_ = new QPushButton("RUN", this);
        spBtn_ = new QPushButton("S&&P", this);
        for (QPushButton* b : {runBtn_, spBtn_}) {
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            hdr->addWidget(b);
        }
        runBtn_->setChecked(true);
        connect(runBtn_, &QPushButton::clicked, this, [this] {
            runMode_ = true;
            runBtn_->setChecked(true);
            spBtn_->setChecked(false);
            applyFkeyLabels();
            updateEsmHint();
        });
        connect(spBtn_, &QPushButton::clicked, this, [this] {
            runMode_ = false;
            runBtn_->setChecked(false);
            spBtn_->setChecked(true);
            if (autoBtn_->isChecked()) autoBtn_->setChecked(false);
            applyFkeyLabels();
            updateEsmHint();
        });
        // ESM on/off, finally on a button (live-found on AA Phone: with
        // old VK test recordings in the slots, Enter transmitted them —
        // the operator wanted Enter to just LOG). Off = plain
        // Enter-logs-when-complete, nothing keys or plays by itself.
        esmBtn_ = new QPushButton("ESM", this);
        esmBtn_->setCheckable(true);
        esmBtn_->setFocusPolicy(Qt::NoFocus);
        esmOn_ = QSettings().value("contest/esm", true).toBool();
        esmBtn_->setChecked(esmOn_);
        esmBtn_->setToolTip(
            "Enter Sends Message: Enter keys the next CW message or "
            "plays the next voice slot for you\n(answer, then TU+log)."
            "\nOFF: Enter only logs when the exchange is complete — "
            "nothing transmits by itself.");
        connect(esmBtn_, &QPushButton::toggled, this, [this](bool on) {
            esmOn_ = on;
            QSettings().setValue("contest/esm", on);
            updateEsmHint();
        });
        hdr->addWidget(esmBtn_);
        // AUTO CQ: F1 re-fires every N seconds. Typing a call PAUSES it
        // (never CQ over a station answering you); logging — or wiping,
        // which is abandoning — RESUMES it. Esc or the button stops it.
        autoBtn_ = new QPushButton("AUTO", this);
        autoBtn_->setCheckable(true);
        autoBtn_->setFocusPolicy(Qt::NoFocus);
        autoBtn_->setToolTip(
            "Repeat CQ (F1) every N seconds while the call box is empty.\n"
            "Typing pauses it; logging or F12 resumes it; Esc stops it.");
        hdr->addWidget(autoBtn_);
        autoSecs_ = new QSpinBox(this);
        autoSecs_->setRange(3, 120);
        autoSecs_->setSuffix(" s");
        autoSecs_->setValue(
            QSettings().value("contest/autoCqSecs", 15).toInt());
        autoSecs_->setFocusPolicy(Qt::NoFocus);
        connect(autoSecs_, &QSpinBox::valueChanged, this, [this](int v) {
            QSettings().setValue("contest/autoCqSecs", v);
            autoCqTimer_.setInterval(v * 1000);
        });
        hdr->addWidget(autoSecs_);
        // The mode buttons wear the F-key green when ON — "is it on?"
        // must be answerable from across the shack.
        for (QPushButton* b : {runBtn_, spBtn_, esmBtn_, autoBtn_})
            b->setStyleSheet(
                "QPushButton:checked { background:#173423;"
                " border:1px solid #3fb46a; color:#a9f0c6;"
                " font-weight:bold; }");
        autoCqTimer_.setInterval(autoSecs_->value() * 1000);
        connect(&autoCqTimer_, &QTimer::timeout, this, [this] {
            if (!autoBtn_->isChecked() || autoPaused_) return;
            if (!call_->text().trimmed().isEmpty()) return;
            keyFkey(0);
        });
        connect(autoBtn_, &QPushButton::toggled, this, [this](bool on) {
            autoPaused_ = false;
            if (on) {
                runMode_ = true;         // CQing IS running
                runBtn_->setChecked(true);
                spBtn_->setChecked(false);
                applyFkeyLabels();
                keyFkey(0);              // first CQ now
                autoCqTimer_.start();
            } else {
                autoCqTimer_.stop();
            }
            updateEsmHint();
        });
        wpm_ = new QSpinBox(this);
        wpm_->setObjectName("wpmSpin");
        wpm_->setRange(5, 60);
        wpm_->setSuffix(" wpm");
        wpm_->setValue(cw_ ? cw_->speedWpm()
                           : QSettings().value("cw/wpm", 30).toInt());
        wpm_->setFocusPolicy(Qt::NoFocus);
        wpm_->setToolTip("PgUp/PgDn from anywhere in the deck");
        connect(wpm_, &QSpinBox::valueChanged, this, [this](int v) {
            if (cw_) cw_->setSpeedWpm(v);
        });
        hdr->addWidget(wpm_);
        mid->addLayout(hdr);

        // Score / rate on its own line under the contest name.
        score_ = new QLabel(this);
        score_->setStyleSheet("color:#8798a8;");
        mid->addWidget(score_);

        // SUPER CHECK: one match line, DIRECTLY above the call box (the
        // sketch's placement) — populates as the partial grows.
        scpRow_ = new QWidget(this);
        auto* sl = new QHBoxLayout(scpRow_);
        sl->setContentsMargins(2, 0, 2, 0);
        sl->setSpacing(10);
        scpRow_->setFixedHeight(20);
        mid->addWidget(scpRow_);

        // entry row
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* cbox = new QVBoxLayout;
        auto* crow = new QHBoxLayout;
        auto* clbl = new QLabel("CALL", this);
        clbl->setStyleSheet("color:#8798a8; font-size:10px;");
        crow->addWidget(clbl);
        frameLbl_ = new QLabel(this);    // knob-tune call frame
        frameLbl_->setStyleSheet("font-size:10px; font-weight:bold;");
        frameLbl_->setToolTip("Spot under the dial — Space grabs it");
        crow->addWidget(frameLbl_);
        crow->addStretch(1);
        cbox->addLayout(crow);
        call_ = new QLineEdit(this);
        call_->setObjectName("entryCall");
        QFont cf = call_->font();
        cf.setPointSize(cf.pointSize() + 3);   // big but condensed
        call_->setFont(cf);
        call_->setMaxLength(14);
        call_->setMinimumWidth(120);
        call_->setMaximumWidth(150);
        call_->installEventFilter(this);
        connect(call_, &QLineEdit::textEdited, this, [this] {
            callFromSpot_ = false;   // typing reclaims ←/→ for the cursor
            confirmPending_.clear(); // an edit restarts the double-check
            if (anchorHz_ == 0 && !call_->text().isEmpty())
                anchorHz_ = rigHz_;  // heard HERE — the park remembers
            if (autoBtn_->isChecked() && !call_->text().isEmpty())
                autoPaused_ = true;  // never CQ over an answering station
            onCallEdited();
        });
        connect(call_, &QLineEdit::returnPressed, this,
                [this] { enterPressed(); });
        cbox->addWidget(call_);
        row->addLayout(cbox);
        fieldsBox_ = new QWidget(this);
        new QHBoxLayout(fieldsBox_);
        fieldsBox_->layout()->setContentsMargins(0, 0, 0, 0);
        row->addWidget(fieldsBox_);
        row->addStretch(1);
        // heading + rotor, the sketch's spot for them
        auto* hbox = new QVBoxLayout;
        hdgSrcLbl_ = new QLabel("HDG", this);
        hdgSrcLbl_->setStyleSheet("color:#8798a8; font-size:10px;");
        auto* hlbl = hdgSrcLbl_;
        hdgLbl_ = new QLabel("—", this);
        QFont hf = hdgLbl_->font();
        hf.setPointSize(hf.pointSize() + 3);
        hdgLbl_->setFont(hf);
        auto* hrow = new QHBoxLayout;
        hrow->addWidget(hdgLbl_);
        auto* spB = new QPushButton("SP", this);
        auto* lpB = new QPushButton("LP", this);
        for (QPushButton* b : {spB, lpB}) {
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedWidth(34);
            hrow->addWidget(b);
        }
        connect(spB, &QPushButton::clicked, this, [this] {
            if (rotor_ && hdg_ >= 0) rotor_->turnTo(hdg_);
        });
        connect(lpB, &QPushButton::clicked, this, [this] {
            if (rotor_ && hdg_ >= 0)
                rotor_->turnTo((hdg_ + 180) % 360);
        });
        hbox->addWidget(hlbl);
        hbox->addLayout(hrow);
        row->addLayout(hbox);
        mid->addLayout(row);

        // status line: dupe/mult · country · ESM hint
        auto* st = new QHBoxLayout;
        dupe_ = new QLabel(this);
        dupe_->setStyleSheet("font-weight:bold;");
        st->addWidget(dupe_);
        info_ = new QLabel(this);
        info_->setStyleSheet("color:#8798a8;");
        st->addWidget(info_);
        st->addStretch(1);
        hint_ = new QLabel(this);
        hint_->setStyleSheet("color:#7ee2a8;");
        st->addWidget(hint_);
        mid->addLayout(st);

        // F keys — two even rows of six (F1-F6 / F7-F12), each button
        // sized like the mode buttons (not stretched), CENTERED, with
        // equal breathing space above / between / below so the block is
        // symmetric within the deck.
        auto* fr1 = new QHBoxLayout;
        auto* fr2 = new QHBoxLayout;
        fr1->setSpacing(6);
        fr2->setSpacing(6);
        fr1->addStretch(1);
        fr2->addStretch(1);
        for (int i = 0; i < 12; ++i) {
            fk_[i] = new QPushButton(this);
            fk_[i]->setFocusPolicy(Qt::NoFocus);
            fk_[i]->setFixedSize(58, 34);
            connect(fk_[i], &QPushButton::clicked, this,
                    [this, i] { keyFkey(i); });
            fk_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(fk_[i], &QPushButton::customContextMenuRequested,
                    this, [this, i](const QPoint&) { editFkey(i); });
            (i < 6 ? fr1 : fr2)->addWidget(fk_[i]);
        }
        fr1->addStretch(1);
        fr2->addStretch(1);
        mid->addStretch(1);      // three equal gaps: above, between,
        mid->addLayout(fr1);     // and below the two F-key rows
        mid->addStretch(1);
        mid->addLayout(fr2);
        mid->addStretch(1);
        lay->addLayout(mid, 22);
    }

    // ---- CW TYPE (right; the box itself hides on phone) -----------------
    {
        auto* box = new QVBoxLayout;
        typeTop_ = new QWidget(this);
        auto* tt = new QVBoxLayout(typeTop_);
        tt->setContentsMargins(0, 0, 0, 0);
        auto* thdr = new QHBoxLayout;
        auto* t = new QLabel("CW TYPE — Enter sends", this);
        t->setStyleSheet("color:#8798a8; font-size:10px;");
        thdr->addWidget(t);
        thdr->addStretch(1);
        tt->addLayout(thdr);
        type_ = new QLineEdit(this);
        QFont mf("DejaVu Sans Mono");
        type_->setFont(mf);
        connect(type_, &QLineEdit::returnPressed, this, [this] {
            const QString t2 = type_->text().trimmed();
            if (t2.isEmpty()) return;
            keyText(t2);
            sent_->setText("sent: " + t2);
            type_->clear();
        });
        sent_ = new QLabel(this);
        sent_->setStyleSheet("color:#8798a8; font-size:10px;");
        tt->addWidget(type_);
        tt->addWidget(sent_);
        box->addWidget(typeTop_);
        addPopout("type", "CW TYPE", typeTop_, thdr,
                  [box](QWidget* w) { box->insertWidget(0, w); });
        // LAST QSOs, always in view — logging must be VISIBLE (six
        // Enters once "logged" into an empty database with nobody the
        // wiser). Newest on top; the full grid stays in the manager.
        logPane_ = new QWidget(this);
        auto* lv = new QVBoxLayout(logPane_);
        lv->setContentsMargins(0, 0, 0, 0);
        auto* lhdr = new QHBoxLayout;
        auto* llbl = new QLabel("LAST QSOs", this);
        llbl->setStyleSheet("color:#8798a8; font-size:10px;");
        lhdr->addWidget(llbl);
        lhdr->addStretch(1);
        lv->addLayout(lhdr);
        lastLog_ = new QTableWidget(this);
        lastLog_->setObjectName("lastLog");
        lastLog_->setColumnCount(4);
        lastLog_->setHorizontalHeaderLabels({"UTC", "CALL", "EXCH", "P"});
        lastLog_->verticalHeader()->setVisible(false);
        lastLog_->horizontalHeader()->setStretchLastSection(true);
        lastLog_->setColumnWidth(0, 44);
        lastLog_->setColumnWidth(1, 84);
        lastLog_->setColumnWidth(2, 70);
        lastLog_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        lastLog_->setFocusPolicy(Qt::NoFocus);
        lastLog_->setSelectionMode(QAbstractItemView::NoSelection);
        {
            QFont lf = lastLog_->font();
            lf.setPointSize(lf.pointSize() - 1);
            lastLog_->setFont(lf);
        }
        lv->addWidget(lastLog_, 1);
        box->addWidget(logPane_, 1);
        addPopout("log", "Last QSOs", logPane_, lhdr,
                  [box](QWidget* w) { box->insertWidget(1, w, 1); });
        auto* br = new QHBoxLayout;
        qtcBtn_ = new QPushButton("QTC", this);
        qtcBtn_->setFocusPolicy(Qt::NoFocus);
        qtcBtn_->setVisible(false);          // WAE-family defs only
        qtcBtn_->setToolTip("Send QTC traffic (WAE) — the To box follows "
                            "the call you're working");
        connect(qtcBtn_, &QPushButton::clicked, this, [this] {
            if (contestId_ < 0) return;
            if (!qtc_)
                qtc_ = new QtcDialog(
                    db_, [this](const QString& t) { keyText(t); },
                    [this] { if (cw_) cw_->stopKeying(); }, this);
            qtc_->setRigFreq(rigHz_);
            qtc_->openFor(contestId_);
            qtc_->followCall(call_->text());
        });
        br->addWidget(qtcBtn_);
        auto* spotBtn = new QPushButton("Spot", this);
        spotBtn->setFocusPolicy(Qt::NoFocus);
        spotBtn->setToolTip("Send the entered call as a DX spot to the "
                            "cluster, at the dial frequency");
        connect(spotBtn, &QPushButton::clicked, this, [this] {
            const QString c = call_->text().trimmed().toUpper();
            if (loggableCall(c))
                emit spotDxRequested(c, rigHz_);
            else
                status_->setText("type the call before spotting it");
        });
        br->addWidget(spotBtn);
        auto* mgr = new QPushButton("Contest log…", this);
        mgr->setFocusPolicy(Qt::NoFocus);
        connect(mgr, &QPushButton::clicked, this,
                [this] { emit openManagerRequested(); });
        br->addWidget(mgr);
        status_ = new QLabel(this);
        status_->setStyleSheet("color:#8798a8; font-size:10px;");
        br->addWidget(status_, 1);
        box->addLayout(br);
        lay->addLayout(box, 11);
    }

    // Chord-free keys, active while the deck is visible in the active
    // window, wherever focus sits (F1 must key from the panadapter too).
    const auto sc = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, fn);
        shortcuts_ << s;
        return s;
    };
    for (int i = 0; i < 12; ++i)
        sc(QKeySequence(Qt::Key_F1 + i), [this, i] { keyFkey(i); });
    sc(QKeySequence(Qt::Key_Escape), [this] { stopEverything(); });
    sc(QKeySequence(Qt::Key_PageUp),
       [this] { wpm_->setValue(wpm_->value() + 1); });
    sc(QKeySequence(Qt::Key_PageDown),
       [this] { wpm_->setValue(wpm_->value() - 1); });
    for (QShortcut* s : shortcuts_) s->setEnabled(false);

    // Make every descriptive label width-shrinkable: a QLabel otherwise
    // forces its full text width into the layout minimum. The entry
    // boxes, F-keys and spinboxes keep their real minimums (those are
    // the legitimate floor); only the prose labels stop dragging width.
    // Maximum: the text width is a CEILING (won't grab extra space and
    // misalign the row) and minimumWidth 0 drops the layout floor, so a
    // long label clips instead of forcing the deck wider.
    for (QLabel* l : findChildren<QLabel*>()) {
        auto sp = l->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Maximum);
        l->setSizePolicy(sp);
        l->setMinimumWidth(0);
    }
    setEnabled(false);
}

void ContestDeck::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(true);
    for (auto it = panes_.begin(); it != panes_.end(); ++it)
        if (it->fly) it->fly->setVisible(it->allowed);
    if (contestId_ >= 0) call_->setFocus();
}

void ContestDeck::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(false);
    for (auto it = panes_.begin(); it != panes_.end(); ++it)
        if (it->fly) it->fly->hide();   // contest mode off = floats too
}

// ---- pop-out panes -------------------------------------------------------

void ContestDeck::addPopout(const QString& key, const QString& title,
                            QWidget* pane, QHBoxLayout* headerRow,
                            std::function<void(QWidget*)> reinsert) {
    PaneSlot s;
    s.pane = pane;
    s.title = title;
    s.reinsert = std::move(reinsert);
    panes_.insert(key, s);
    auto* pop = new QPushButton("⧉", pane);
    pop->setFlat(true);
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setCursor(Qt::PointingHandCursor);
    pop->setStyleSheet("color:#8798a8; font-size:11px; border:0;");
    pop->setToolTip("Pop out as its own window — size it and park it "
                    "anywhere; closing it returns it to the deck");
    connect(pop, &QPushButton::clicked, this,
            [this, key] { floatPane(key); });
    headerRow->addWidget(pop);
}

void ContestDeck::floatPane(const QString& key) {
    PaneSlot& s = panes_[key];
    if (!s.pane || !s.allowed) return;
    if (s.fly) {
        s.fly->show();
        s.fly->raise();
        return;
    }
    auto* d = new QDialog(window());
    d->setWindowTitle(s.title + " — contest");
    auto* v = new QVBoxLayout(d);
    v->setContentsMargins(6, 6, 6, 6);
    v->addWidget(s.pane);            // reparents out of the deck
    const QByteArray g =
        QSettings().value("contest/float/" + key + "/geom").toByteArray();
    if (!g.isEmpty()) d->restoreGeometry(g);
    else d->resize(430, 300);
    d->installEventFilter(this);     // Close = come home
    s.fly = d;
    QSettings().setValue("contest/float/" + key + "/on", true);
    d->show();
}

void ContestDeck::unfloatPane(const QString& key, bool saveGeom) {
    PaneSlot& s = panes_[key];
    if (!s.fly) return;
    if (saveGeom)
        QSettings().setValue("contest/float/" + key + "/geom",
                             s.fly->saveGeometry());
    QSettings().setValue("contest/float/" + key + "/on", false);
    s.reinsert(s.pane);              // back into its deck slot
    s.pane->setVisible(s.allowed);
    s.fly->deleteLater();
    s.fly = nullptr;
}

void ContestDeck::applyPaneVisibility() {
    // A phone contest has no use for CW panes — the entry gets the room.
    const bool phone =
        def_ && def_->modeCategory == QLatin1String("SSB");
    const auto vis = [this](const QString& key, bool on) {
        if (!panes_.contains(key)) return;
        PaneSlot& s = panes_[key];
        s.allowed = on;
        s.pane->setVisible(on);
        if (s.fly) s.fly->setVisible(on && isVisible());
    };
    vis("read", !phone);
    vis("type", !phone);
    vis("log", true);
}

bool ContestDeck::openContestId(qint64 id) {
    openContest(id);
    return contestId_ == id;
}

void ContestDeck::closeContest() {
    contestId_ = -1;
    def_ = nullptr;
    QSettings().remove("contest/currentId");
    title_->setText("no contest open");
    score_->clear();
    call_->clear();
    if (lastLog_) lastLog_->setRowCount(0);
    setEnabled(false);
}

void ContestDeck::openContest(qint64 id) {
    if (id == contestId_ && def_) {    // already live — just refresh
        refreshAll();
        return;
    }
    row_ = db_->contest(id);
    def_ = contestDef(row_.defId);
    if (row_.id < 0 || !def_) {
        title_->setText("no contest open — Contest log… starts one");
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
    setEnabled(true);
    myCallSent_ = exchSent_ = false;
    if (qtcBtn_) qtcBtn_->setVisible(def_->hasQtc);
    applyPaneVisibility();   // phone drops the CW panes, floats included
    rebuildEntryFields();
    applyFkeyLabels();
    refreshAll();
    trace(QString("DECK OPEN %1 id=%2").arg(row_.defId).arg(id));
    if (isVisible()) call_->setFocus();
}

void ContestDeck::rebuildEntryFields() {
    edits_.clear();
    rstS_ = nullptr;
    sentNr_ = nullptr;
    // The fields sit in nested label+edit columns, and a QLayoutItem
    // does NOT own child widgets — draining the layout alone left the
    // old edits alive (two sets of "exchEdit0", one orphaned; the
    // harness typed into the ghost). Delete the widgets first.
    for (QWidget* w : fieldsBox_->findChildren<QWidget*>(
             QString(), Qt::FindDirectChildrenOnly))
        delete w;
    QLayout* l = fieldsBox_->layout();
    while (QLayoutItem* it = l->takeAt(0)) delete it;
    auto addField = [&](const QString& label, int widthCh,
                        const QString& preset) {
        auto* box = new QVBoxLayout;
        auto* lb = new QLabel(label, fieldsBox_);
        lb->setStyleSheet("color:#8798a8; font-size:10px;");
        box->addWidget(lb);
        auto* e = new QLineEdit(preset, fieldsBox_);
        e->setMaxLength(qMax(widthCh + 4, 6));
        e->setFixedWidth(22 + widthCh * 10);
        e->installEventFilter(this);     // ↑/↓ speed from here too
        connect(e, &QLineEdit::returnPressed, this,
                [this] { enterPressed(); });
        connect(e, &QLineEdit::textEdited, this,
                [this] { updateEsmHint(); });
        box->addWidget(e);
        static_cast<QHBoxLayout*>(l)->addLayout(box);
        return e;
    };
    if (def_->hasRst) {
        rstS_ = addField("SNT", 4, rstPreset());
        rstS_->setObjectName("sntEdit");
    }
    if (def_->sentSerial) {
        auto* box = new QVBoxLayout;
        auto* lb = new QLabel("SENT NR", fieldsBox_);
        lb->setStyleSheet("color:#8798a8; font-size:10px;");
        box->addWidget(lb);
        sentNr_ = new QLabel(fieldsBox_);
        QFont f = sentNr_->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 3);
        sentNr_->setFont(f);
        box->addWidget(sentNr_);
        static_cast<QHBoxLayout*>(l)->addLayout(box);
    }
    for (const ExchFieldDef& fd : def_->fields) {
        QLineEdit* e = addField(fd.label, fd.widthCh,
                                fd.col == ExchCol::RstR ? rstPreset()
                                                        : fd.preset);
        e->setObjectName(QString("exchEdit%1").arg(edits_.size()));
        edits_ << qMakePair(fd.col, e);
    }
}

QString ContestDeck::currentBand() const {
    return rigHz_ > 0 ? LogbookIndex::bandForHz(rigHz_)
                      : QStringLiteral("20M");
}

QString ContestDeck::modeNow() const {
    // Mixed-mode contests dupe per band+mode; follow the rig.
    return rigMode_ == QLatin1String("SSB") ? QStringLiteral("SSB")
                                            : QStringLiteral("CW");
}

QString ContestDeck::rstPreset() const {
    // Phone reports are two digits (live-found: AA Phone showed a 599
    // SNT). SSB defs say 59 outright; MIXED follows the rig.
    if (!def_) return QStringLiteral("599");
    if (def_->modeCategory == QLatin1String("SSB"))
        return QStringLiteral("59");
    if (def_->modeCategory == QLatin1String("MIXED")
        && modeNow() == QLatin1String("SSB"))
        return QStringLiteral("59");
    return QStringLiteral("599");
}

void ContestDeck::setRig(qint64 hz, const QString& adifMode) {
    const bool bandMoved =
        hz > 0 && LogbookIndex::bandForHz(hz) != currentBand();
    rigHz_ = hz;
    rigMode_ = adifMode.isEmpty() ? QStringLiteral("CW") : adifMode;
    if (qtc_) qtc_->setRigFreq(hz);
    // Abandon-on-QSY, judged against where the call was ACQUIRED — not
    // the previous poll tick, which is what let a Space-grabbed call
    // survive the roll-away (live-found with K6TK). A typed call parks
    // as a local spot; a grabbed/landed one just clears, because its
    // spot is already on the map. Walk hops are safe: every landing
    // re-anchors before the tune reports back.
    if (contestId_ >= 0 && hz > 0 && anchorHz_ > 0
        && qAbs(hz - anchorHz_) > 1000) {
        const QString c = call_->text().trimmed();
        if (c.isEmpty()) {
            anchorHz_ = 0;
        } else {
            if (!callFromSpot_ && loggableCall(c)
                && classifySpot(c, anchorHz_) != 'W') {
                emit callParked(c, anchorHz_);
                trace(QString("PARK %1 @ %2").arg(c).arg(anchorHz_));
            } else {
                status_->setText(c + " left behind");
            }
            wipe();                      // resets the anchor too
        }
    }
    if (bandMoved) onCallEdited();       // dupe verdict can flip
    // MIXED contests: flipping the rig between CW and phone swaps the
    // UNTOUCHED report presets (a typed real report is never clobbered).
    if (rstS_ && def_ && def_->modeCategory == QLatin1String("MIXED")) {
        const QString want = rstPreset();
        const auto swap = [&want](QLineEdit* e) {
            if (e && (e->text() == QLatin1String("599")
                      || e->text() == QLatin1String("59")))
                e->setText(want);
        };
        swap(rstS_);
        for (const auto& [col, edit] : edits_)
            if (col == ExchCol::RstR) swap(edit);
    }
}

void ContestDeck::setNearbySpot(const QString& call, char cls, qint64 hz) {
    const QString c = call.trimmed().toUpper();
    frameHz_ = hz;
    if (c == frameCall_) return;
    frameCall_ = c;
    if (c.isEmpty() || c == call_->text().trimmed()) {
        frameLbl_->clear();
        return;
    }
    const char* color = cls == 'M' ? "#ff5252"
                      : cls == 'W' ? "#6c7a88"
                      : cls == 'Z' ? "#4a4a4a"
                                   : "#5db2f0";
    frameLbl_->setStyleSheet(QString("font-size:10px; font-weight:bold;"
                                     " color:%1;").arg(color));
    frameLbl_->setText("▸ " + c + "  (Space)");
}

void ContestDeck::prefillCall(const QString& call, qint64 hz) {
    if (contestId_ < 0) return;
    call_->setText(call.trimmed().toUpper());
    myCallSent_ = exchSent_ = false;
    onCallEdited();
    callFromSpot_ = true;            // ←/→ keep walking from here
    anchorHz_ = hz > 0 ? hz : rigHz_;
    requestQrz(call_->text().trimmed());  // a spot call is complete: ask now
    call_->setFocus();
}

void ContestDeck::requestQrz(const QString& call) {
    if (!qrz_ || !loggableCall(call) || qrzAsked_.contains(call)) return;
    qrzAsked_.insert(call);              // once per call, misses included
    qrz_->lookup(call);
}

void ContestDeck::appendRead(const QString& text) {
    if (!isVisible()) return;
    QTextCursor c = read_->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertText(text);
    read_->setTextCursor(c);
}

char ContestDeck::classifySpot(const QString& call, qint64 hz) const {
    if (contestId_ < 0 || !def_) return 0;
    const QString c = call.trimmed().toUpper();
    const QString band =
        hz > 0 ? LogbookIndex::bandForHz(hz) : currentBand();
    if (isDupe(*def_, values_, c, band, modeNow())) return 'W';
    CQsoValues probe;
    probe.call = c;
    probe.band = band;
    probe.mode = modeNow();
    CtyInfo ci;
    const bool ok = cty_ && cty_->info(normalizeForCty(c), ci);
    const int pts = def_->points ? def_->points(probe, ci, ok, ctx_) : 0;
    if (pts <= 0) return 'Z';
    if (def_->mults)
        for (const QString& k : def_->mults(probe, ci, ok, ctx_))
            if (!k.isEmpty() && !sb_.multKeys.contains(k)) return 'M';
    return 'N';
}

// ---- ESM -----------------------------------------------------------------

void ContestDeck::enterPressed() {
    if (contestId_ < 0 || !def_) return;
    EsmInput in;
    // ESM drives BOTH modes when the (now clearly lit) button is on —
    // the operator's revised ruling once the toggle was understood:
    // Enter keys CW or plays the VK slots alike; the button is the off
    // switch, not a hidden mode rule.
    in.esmOn = esmOn_;
    in.run = runMode_;
    const QString c = call_->text().trimmed();
    in.callEmpty = c.isEmpty();
    in.callLoggable = loggableCall(c);
    in.exchComplete = true;
    for (int i = 0; i < def_->fields.size(); ++i)
        if (def_->fields[i].required
            && edits_[i].second->text().trimmed().isEmpty())
            in.exchComplete = false;
    in.myCallSent = myCallSent_;
    in.exchSent = exchSent_;
    QList<EsmAct> plan = esmPlan(in);
    // N1MM's "won't take a bad call easily": a call that resolves to NO
    // country needs Enter TWICE — the first press refuses and says so,
    // an unedited second press logs it on the operator's authority
    // (special-event calls exist; silence about J12MED does not).
    if (plan.contains(EsmAct::Log)) {
        const QString c = call_->text().trimmed();
        CtyInfo ci;
        const bool resolves = cty_ && cty_->info(normalizeForCty(c), ci);
        if (!resolves && confirmPending_ != c) {
            confirmPending_ = c;
            flashRefusal(call_,
                         c + " maps to NO country — press Enter again "
                             "to log it anyway");
            return;
        }
    }
    if (plan.isEmpty() && !in.callEmpty) {
        // Enter has nothing to do — SHOUT why: red-flash the field that
        // needs copy. The quiet version was pressed six times over an
        // age that had landed in the wrong box.
        for (int i = 0; i < def_->fields.size(); ++i)
            if (def_->fields[i].required
                && edits_[i].second->text().trimmed().isEmpty()) {
                flashRefusal(edits_[i].second,
                             def_->fields[i].label
                                 + " missing — Enter logs once the "
                                   "exchange is complete");
                return;
            }
        if (!in.callLoggable)
            flashRefusal(call_, "call needs 3+ chars with a letter and "
                                "a digit");
    }
    execPlan(plan, false);
    updateEsmHint();
}

void ContestDeck::focusExchange() {
    for (int i = 0; i < def_->fields.size() && i < edits_.size(); ++i)
        if (def_->fields[i].required
            && edits_[i].second->text().trimmed().isEmpty()) {
            edits_[i].second->setFocus();
            return;
        }
    for (int i = 0; i < def_->fields.size() && i < edits_.size(); ++i)
        if (def_->fields[i].col != ExchCol::RstR) {
            edits_[i].second->setFocus();
            return;
        }
    if (!edits_.isEmpty()) edits_[0].second->setFocus();
}

void ContestDeck::flashRefusal(QLineEdit* field, const QString& msg) {
    status_->setText("⚠ " + msg);
    trace("ENTER refused: " + msg);
    if (!field) return;
    field->setFocus();
    field->setStyleSheet(
        "QLineEdit { border: 2px solid #e05d5d; }");
    QTimer::singleShot(1400, field,
                       [field] { field->setStyleSheet(QString()); });
}

void ContestDeck::execPlan(const QList<EsmAct>& plan, bool updateOnly) {
    for (int i = 0; i < 12; ++i)
        if (fk_[i]) fk_[i]->setStyleSheet(QString());
    const auto glow = [this](int key) {
        if (fk_[key - 1]) fk_[key - 1]->setStyleSheet(kGlowStyle);
    };
    for (const EsmAct a : plan) {
        switch (a) {
            case EsmAct::KeyCq:
                glow(1);
                if (!updateOnly) keyFkey(0);
                break;
            case EsmAct::KeyHisCall:
                glow(2);
                if (!updateOnly) keyFkey(1);
                break;
            case EsmAct::KeyExch:
                glow(3);
                if (!updateOnly) keyFkey(2);
                break;
            case EsmAct::KeyMyCall:
                glow(5);
                if (!updateOnly) keyFkey(4);
                break;
            case EsmAct::KeyTu:
                glow(4);
                if (!updateOnly) keyFkey(3);
                break;
            case EsmAct::Log:
                if (!updateOnly) logNow();
                break;
            case EsmAct::FocusExch:
                if (!updateOnly) focusExchange();
                break;
        }
    }
}

void ContestDeck::updateEsmHint() {
    if (contestId_ < 0 || !def_) return;
    EsmInput in;
    in.esmOn = esmOn_;
    in.run = runMode_;
    const QString c = call_->text().trimmed();
    in.callEmpty = c.isEmpty();
    in.callLoggable = loggableCall(c);
    in.exchComplete = true;
    for (int i = 0; i < def_->fields.size(); ++i)
        if (def_->fields[i].required
            && edits_[i].second->text().trimmed().isEmpty())
            in.exchComplete = false;
    in.myCallSent = myCallSent_;
    in.exchSent = exchSent_;
    const QList<EsmAct> plan = esmPlan(in);
    execPlan(plan, true);
    QStringList words;
    for (const EsmAct a : plan) {
        switch (a) {
            case EsmAct::KeyCq: words << "CQ"; break;
            case EsmAct::KeyHisCall: words << "his call"; break;
            case EsmAct::KeyExch: words << "exchange"; break;
            case EsmAct::KeyMyCall: words << "my call"; break;
            case EsmAct::KeyTu: words << "TU"; break;
            case EsmAct::Log: words << "LOG"; break;
            case EsmAct::FocusExch: break;
        }
    }
    hint_->setText(words.isEmpty() ? QString()
                                   : "Enter → " + words.join(" + "));
}

void ContestDeck::logNow() {
    const QString c = call_->text().trimmed().toUpper();
    if (!loggableCall(c)) return;
    ContestQso q;
    q.contestId = contestId_;
    q.tsUtc = QDateTime::currentDateTimeUtc();
    q.freqHz = rigHz_ > 0 ? rigHz_ : 14030000;
    q.v.call = c;
    q.v.band = currentBand();
    q.v.mode = modeNow();
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
            case ExchCol::Exch3: q.v.exch3 = t; break;
        }
    }
    CtyInfo ci;
    const bool ok = cty_ && cty_->info(normalizeForCty(c), ci);
    q.points = def_->points ? def_->points(q.v, ci, ok, ctx_) : 0;
    if (db_->addQso(q) < 0) {
        status_->setText("DATABASE ERROR — QSO NOT SAVED");
        trace("DECK ADD FAILED " + c);
        return;
    }
    if (def_->sentSerial) {
        row_.nextSerial++;
        db_->setNextSerial(contestId_, row_.nextSerial);
    }
    confirmPending_.clear();
    trace(QString("DECK QSO %1 %2 ser %3")
              .arg(c, q.v.band)
              .arg(q.v.serialS));
    wipe();                          // the silent "it logged" signal
    refreshAll();
    status_->setText(QString("logged %1").arg(c));
}

void ContestDeck::wipe() {
    call_->clear();
    if (rstS_) rstS_->setText(rstPreset());
    if (def_)
        for (int i = 0; i < edits_.size() && i < def_->fields.size(); ++i)
            edits_[i].second->setText(
                def_->fields[i].col == ExchCol::RstR
                    ? rstPreset()
                    : def_->fields[i].preset);
    dupe_->clear();
    info_->clear();
    updateHeading();                     // call box empty -> "—"
    myCallSent_ = exchSent_ = false;
    refreshScp();
    anchorHz_ = 0;                       // nothing in the box to abandon
    // A wipe means logged or abandoned — either way, back to CQing.
    if (autoBtn_ && autoBtn_->isChecked()) {
        autoPaused_ = false;
        autoCqTimer_.start();
    }
    call_->setFocus();
    updateEsmHint();
}

QString ContestDeck::fkeySpec(int key) const {
    if (!def_) return QString();
    const QString ov =
        QSettings()
            .value(QString("contest/macros/%1/%2/F%3")
                       .arg(def_->id, runMode_ ? "R" : "S")
                       .arg(key))
            .toString();
    if (!ov.isEmpty()) return ov == QLatin1String("-") ? QString() : ov;
    const QHash<int, QString>& set =
        (!runMode_ && !def_->fkeySp.isEmpty()) ? def_->fkeySp
                                               : def_->fkeyRun;
    return set.value(key);
}

void ContestDeck::setVoiceKeyer(std::function<bool(int)> play,
                                std::function<void()> stop) {
    playVk_ = std::move(play);
    stopVoice_ = std::move(stop);
}

void ContestDeck::nudgeSpeed(int delta) {
    // Up = faster, down = slower — CW only; a phone contest has no
    // keying speed and a surprise wpm change would outlive the weekend.
    if (modeNow() != QLatin1String("CW")) return;
    wpm_->setValue(wpm_->value() + delta);   // handler drives the keyer
}

void ContestDeck::keyFkey(int idx0) {
    if (contestId_ < 0 || !def_) return;
    if (idx0 == 11) {                // F12 is WIPE, always
        wipe();
        return;
    }
    const QString spec = fkeySpec(idx0 + 1);
    if (spec.isEmpty()) return;
    const QString raw = fkeyText(spec);
    if (const int slot = vkSlot(raw)) {
        // Phone: the key plays a recorded message instead of keying CW.
        // A slot with no recording keys NOTHING and — critically —
        // advances no ESM beat: a silent failure that still marched the
        // state machine left Enter meaning nothing (live-found).
        if (!playVk_ || !playVk_(slot - 1)) {
            status_->setText(
                QString("VK%1 has no recording — right-click it on the "
                        "TX bar, or turn ESM off").arg(slot));
            return;
        }
        status_->setText(QString("▶ VK%1").arg(slot));
    } else {
        const QString text =
            expandMacro(raw, *def_, ctx_, call_->text(),
                        row_.sentExch, row_.nextSerial);
        if (text.isEmpty()) return;
        keyText(text);
        status_->setText("→ " + text);
    }
    // Hand-keyed F3/F5 advance the same ESM beats Enter would — a
    // played exchange counts exactly like a keyed one.
    if (idx0 == 2) exchSent_ = true;
    if (idx0 == 4) myCallSent_ = true;
    // Every CQ — timer-fired or hand-pressed — restarts the cadence, so
    // the rhythm is start-to-start and a manual F1 never double-fires.
    if (idx0 == 0 && autoBtn_ && autoBtn_->isChecked())
        autoCqTimer_.start();
    updateEsmHint();
}

void ContestDeck::keyText(const QString& text) {
    if (!cw_) {
        status_->setText("no keyer backend");
        return;
    }
    cw_->keyExternal(text);
}

void ContestDeck::editFkey(int idx0) {
    if (!def_) return;
    if (idx0 == 11) {
        status_->setText("F12 is reserved: WIPE");
        return;
    }
    QDialog d(this);
    d.setWindowTitle(QString("F%1 — %2 (%3)")
                         .arg(idx0 + 1)
                         .arg(def_->title, runMode_ ? "Run" : "S&P"));
    auto* form = new QFormLayout(&d);
    const QString spec = fkeySpec(idx0 + 1);
    auto* label = new QLineEdit(fkeyLabel(spec), &d);
    auto* text = new QLineEdit(fkeyText(spec), &d);
    text->setMinimumWidth(320);
    form->addRow("Label", label);
    form->addRow("Sends", text);
    form->addRow(new QLabel(
        "Tokens: {MYCALL} {HISCALL} {SNT} {SENTNR} {EXCH}", &d));
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::RestoreDefaults
            | QDialogButtonBox::Cancel,
        &d);
    form->addRow(bb);
    const QString key = QString("contest/macros/%1/%2/F%3")
                            .arg(def_->id, runMode_ ? "R" : "S")
                            .arg(idx0 + 1);
    connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        const QString l = label->text().trimmed();
        const QString t = text->text().trimmed();
        // "-" marks an explicitly emptied key (distinct from "use the
        // contest default").
        QSettings().setValue(key,
                             t.isEmpty() && l.isEmpty() ? "-"
                                                        : l + "|" + t);
        d.accept();
    });
    connect(bb->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked, &d, [&] {
        QSettings().remove(key);
        d.accept();
    });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    d.exec();
    applyFkeyLabels();
}

void ContestDeck::applyFkeyLabels() {
    for (int i = 0; i < 12; ++i) {
        if (i == 11) {
            fk_[i]->setText("F12\nWIPE");
            fk_[i]->setEnabled(contestId_ >= 0);
            fk_[i]->setToolTip("Clear the entry (reserved key)");
            continue;
        }
        const QString spec = fkeySpec(i + 1);
        fk_[i]->setText(QString("F%1\n%2").arg(i + 1).arg(
            spec.isEmpty() ? QStringLiteral("—") : fkeyLabel(spec)));
        fk_[i]->setEnabled(!spec.isEmpty());
        fk_[i]->setToolTip(spec.isEmpty()
                               ? QStringLiteral("right-click to assign")
                               : fkeyText(spec));
    }
}

// ---- call box helpers ----------------------------------------------------

void ContestDeck::onCallEdited() {
    const int pos = call_->cursorPosition();
    const QString up = call_->text().toUpper();
    if (up != call_->text()) {
        const QSignalBlocker b(call_);
        call_->setText(up);
        call_->setCursorPosition(pos);
    }
    myCallSent_ = exchSent_ = false;   // new station, new beats
    if (contestId_ < 0) return;
    const QString c = up.trimmed();
    if (qtc_) qtc_->followCall(c);     // QTC To box rides the entry
    refreshScp();
    if (c.isEmpty()) {
        dupe_->clear();
        info_->clear();
        updateHeading();
        updateEsmHint();
        return;
    }
    const char cls = classifySpot(c);
    if (cls == 'W') {
        dupe_->setStyleSheet("color:#e0b050; font-weight:bold;");
        dupe_->setText(QString("DUPE — worked on %1").arg(currentBand()));
    } else if (cls == 'M') {
        dupe_->setStyleSheet("color:#ff5252; font-weight:bold;");
        dupe_->setText("MULT");
    } else if (cls == 'Z') {
        dupe_->setStyleSheet("color:#6c7a88; font-weight:bold;");
        dupe_->setText("0 pts");
    } else {
        dupe_->clear();
    }
    CtyInfo ci;
    if (cty_ && cty_->info(normalizeForCty(c), ci)) {
        info_->setStyleSheet("color:#8798a8;");
        info_->setText(QString("%1 · %2 · CQ %3")
                           .arg(ci.country, ci.cont)
                           .arg(ci.cq));
    } else if (loggableCall(c)) {
        // The loudest bust alarm there is: a call that maps to NO
        // country. J12MED wore a quiet "—" while the real JI2MED sat
        // one keystroke away.
        info_->setStyleSheet("color:#e0b050; font-weight:bold;");
        info_->setText("⚠ no country — check the call");
    } else {
        info_->setStyleSheet("color:#8798a8;");
        info_->setText("—");
    }
    updateHeading();
    if (loggableCall(c)) qrzTimer_.start();  // ask QRZ once typing settles
    updateEsmHint();
}

void ContestDeck::updateHeading() {
    const QString c = call_->text().trimmed();
    hdg_ = -1;
    if (c.isEmpty() || contestId_ < 0) {
        hdgLbl_->setText("—");
        hdgSrcLbl_->setText("HDG");
        return;
    }
    double lat = 0, lon = 0, glat = 0, glon = 0;
    const char* src = nullptr;
    const QString qg = qrzGrid_.value(c);
    if (!qg.isEmpty() && CtyLookup::gridToLatLon(qg, glat, glon)) {
        lat = glat;
        lon = glon;
        src = "QRZ";                     // his real QTH
    } else {
        const HistoryRow h = db_->historyFor(c);
        if (!h.grid.isEmpty()
            && CtyLookup::gridToLatLon(h.grid, glat, glon)) {
            lat = glat;
            lon = glon;
            src = "hist";                // call-history grid
        } else {
            CtyInfo ci;
            if (cty_ && cty_->info(normalizeForCty(c), ci)) {
                lat = ci.lat;
                lon = ci.lon;
                src = "cty ctr";         // entity centre — every US call
            }                            // bears 228° from here; say so
        }
    }
    if (!src) {
        hdgLbl_->setText("—");
        hdgSrcLbl_->setText("HDG");
        return;
    }
    hdg_ = int(bearing::initialDeg(myLat_, myLon_, lat, lon) + 0.5) % 360;
    hdgLbl_->setText(QString("%1°").arg(hdg_));
    hdgSrcLbl_->setText(QString("HDG · %1").arg(src));
}

void ContestDeck::historyPrefill() {
    if (contestId_ < 0 || !def_) return;
    const HistoryRow h = db_->historyFor(call_->text());
    if (h.call.isEmpty()) return;
    for (int i = 0; i < def_->fields.size(); ++i) {
        if (!edits_[i].second->text().trimmed().isEmpty()) continue;
        const QString& src = def_->fields[i].historyCol;
        QString v;
        if (src == QLatin1String("name")) v = h.name;
        else if (src == QLatin1String("exch1")) v = h.exch1;
        else if (src == QLatin1String("sect")) v = h.sect;
        else if (src == QLatin1String("state")) v = h.state;
        else if (src == QLatin1String("ck")) v = h.ck;
        else if (src == QLatin1String("grid")) v = h.grid;
        if (!v.isEmpty()) edits_[i].second->setText(v.toUpper());
    }
    updateEsmHint();
}

void ContestDeck::refreshScp() {
    QLayout* l = scpRow_->layout();
    while (QLayoutItem* it = l->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QString p = call_->text().trimmed().toUpper();
    if (p.size() < 2 || contestId_ < 0) return;
    // master.scp + everything already in this contest's log.
    QSet<QString> pool = scp_;
    for (const CQsoValues& v : values_) pool.insert(v.call);
    const QStringList m = scpMatches(p, pool, 12);
    auto* h = static_cast<QHBoxLayout*>(l);
    for (const QString& c : m) {
        auto* b = new QPushButton(c, scpRow_);
        b->setFlat(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        const char cls = classifySpot(c);
        const char* color = cls == 'W' ? "#6c7a88"
                          : cls == 'M' ? "#ff5252"
                          : cls == 'Z' ? "#4a4a4a"
                                       : "#5db2f0";
        b->setStyleSheet(QString("QPushButton { border:0; font-family:"
                                 "'DejaVu Sans Mono'; color:%1; }")
                             .arg(color));
        if (cls == 'W') b->setToolTip("worked this band");
        connect(b, &QPushButton::clicked, this,
                [this, c] { prefillCall(c); });
        h->addWidget(b);
    }
    if (pool.contains(p)) {
        auto* ok = new QLabel("✓", scpRow_);
        ok->setStyleSheet("color:#3fb46a; font-weight:bold;");
        ok->setToolTip("exact match in master.scp / log");
        h->addWidget(ok);
    }
    h->addStretch(1);
}

// ---- refresh -------------------------------------------------------------

void ContestDeck::refreshAll() {
    qsos_ = db_->qsos(contestId_);
    values_.clear();
    for (const ContestQso& q : qsos_) values_ << q.v;
    row_ = db_->contest(contestId_);   // serial may have moved elsewhere
    const int qtcN = def_->hasQtc ? db_->qtcCount(contestId_) : 0;
    sb_ = computeScore(*def_, values_, cty_, ctx_, qtcN);
    if (sentNr_)
        sentNr_->setText(formatSerial(row_.nextSerial, def_->cutNumbers,
                                      def_->serialPad));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int in10 = 0, pts60 = 0;
    for (const ContestQso& q : qsos_) {
        const qint64 secs = q.tsUtc.secsTo(now);
        if (secs <= 600) ++in10;
        if (secs <= 3600) pts60 += q.points;
    }
    // Near-term meters, N1MM Info-window style: the last-10-QSO pace
    // (measured to NOW, so it honestly decays while you idle) beside
    // the 10-minute rate and the points actually banked this hour.
    int last10 = 0;
    if (qsos_.size() >= 10) {
        const qint64 span =
            qsos_[qsos_.size() - 10].tsUtc.secsTo(now);
        if (span > 0) last10 = int(10 * 3600 / span);
    }
    QString s =
        QString("· %1 Q · %2 pts · %3 mult · %4 · 10q %5/h · 10m %6/h"
                " · %7 pt/h")
            .arg(sb_.qsos)
            .arg(sb_.points)
            .arg(sb_.weightedMults)
            .arg(QLocale::c().toString(qlonglong(sb_.total)))
            .arg(last10)
            .arg(in10 * 6)
            .arg(pts60);
    // LAST QSOs strip: newest first, enough rows to trust the log.
    if (lastLog_) {
        const int n = qMin(qsizetype(12), qsos_.size());
        lastLog_->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            const ContestQso& q = qsos_[qsos_.size() - 1 - i];
            QStringList ex;
            if (q.v.serialR.isEmpty() == false) ex << q.v.serialR;
            for (const QString& e : {q.v.exch1, q.v.exch2, q.v.exch3})
                if (!e.isEmpty()) ex << e;
            const auto put = [&](int col, const QString& t) {
                auto* it = new QTableWidgetItem(t);
                it->setFlags(Qt::ItemIsEnabled);
                lastLog_->setItem(i, col, it);
            };
            put(0, q.tsUtc.toString("HHmm"));
            put(1, q.v.call);
            put(2, ex.join(' '));
            put(3, QString::number(q.points));
        }
    }
    if (def_->hasQtc) {
        // WAE's rest rule: a break only counts after 60 min with no QSO
        // AND no QTC — the clock shows which side of the line you're on.
        QList<QDateTime> ev;
        for (const ContestQso& q : qsos_) ev << q.tsUtc;
        for (const ContestDb::QtcRow& r : db_->qtcRows(contestId_))
            ev << r.tsUtc;
        const int op = opTimeSecs(ev);
        s += QString(" · QTC %1 · op %2:%3")
                 .arg(qtcN)
                 .arg(op / 3600)
                 .arg((op % 3600) / 60, 2, 10, QChar('0'));
    }
    score_->setText(s);
}

// ---- key routing ---------------------------------------------------------

void ContestDeck::stopEverything() {
    if (cw_) cw_->stopKeying();      // dump the WinKeyer buffer NOW
    if (stopVoice_) stopVoice_();
    if (autoBtn_ && autoBtn_->isChecked())
        autoBtn_->setChecked(false); // Esc also kills the auto-CQ robot
    status_->setText("stopped");
}

bool ContestDeck::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::Close) {
        // A closing float returns its pane to the deck.
        for (auto it = panes_.begin(); it != panes_.end(); ++it)
            if (it->fly == obj) {
                QSettings().setValue("contest/float/" + it.key() + "/geom",
                                     it->fly->saveGeometry());
                unfloatPane(it.key(), false);
                return true;
            }
    }
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        // Esc stops keying from ANY entry field — a QLineEdit doesn't
        // consume it, but the window shortcut lost to focus, so catch
        // it here where it can't be missed (the auto-CQ-won't-quit bug).
        if (ke->key() == Qt::Key_Escape) {
            stopEverything();
            return true;
        }
        // ↑/↓ = keying speed from ANY entry field — the hands never
        // leave the keyboard mid-run (up faster, down slower).
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            nudgeSpeed(ke->key() == Qt::Key_Up ? +1 : -1);
            return true;
        }
    }
    if (obj == call_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Space) {
            // Empty box + a spot under the dial: Space grabs the frame.
            // Otherwise: history prefill + jump to the exchange, the
            // operator's Not1MM muscle memory.
            if (call_->text().trimmed().isEmpty()
                && !frameCall_.isEmpty()) {
                prefillCall(frameCall_, frameHz_);
                return true;
            }
            historyPrefill();
            focusExchange();     // skip preset RST — land on real copy
            return true;
        }
        if ((ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
            && (call_->text().isEmpty() || callFromSpot_)) {
            // The arrows walk the spots while the box is empty OR still
            // holding an untouched walk/click landing — that's what
            // lets ←/→ ROLL through the band. The first typed character
            // hands the arrows back to the text cursor.
            emit walkSpots(ke->key() == Qt::Key_Right ? +1 : -1);
            return true;
        }
    }
    if (obj == read_->viewport()
        && ev->type() == QEvent::MouseButtonDblClick) {
        QTextCursor c = read_->cursorForPosition(
            static_cast<QMouseEvent*>(ev)->pos());
        c.select(QTextCursor::WordUnderCursor);
        // The reader splits on non-alnum, so W1AW/4 selects in pieces;
        // widen across '/' the way the CW window's pane does.
        QString tok = c.selectedText().trimmed().toUpper();
        if (loggableCall(tok)) prefillCall(tok);
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

void ContestDeck::trace(const QString& line) {
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
