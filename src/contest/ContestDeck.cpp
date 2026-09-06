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
}

void ContestDeck::buildUi() {
    setFixedHeight(270);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);

    // ---- CW READ (left) -------------------------------------------------
    {
        auto* box = new QVBoxLayout;
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
        lay->addLayout(box, 11);
    }

    // ---- center: SCP + entry -------------------------------------------
    {
        auto* mid = new QVBoxLayout;
        mid->setSpacing(3);

        // header: title · score · clock · run/s&p · wpm
        auto* hdr = new QHBoxLayout;
        title_ = new QLabel("no contest open", this);
        QFont bf = title_->font();
        bf.setBold(true);
        title_->setFont(bf);
        hdr->addWidget(title_);
        score_ = new QLabel(this);
        hdr->addWidget(score_);
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

        // SUPER CHECK: one match line, DIRECTLY above the call box (the
        // sketch's placement) — populates as the partial grows.
        scpRow_ = new QWidget(this);
        auto* sl = new QHBoxLayout(scpRow_);
        sl->setContentsMargins(2, 0, 2, 0);
        sl->setSpacing(10);
        scpRow_->setFixedHeight(24);
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
        cf.setPointSize(cf.pointSize() + 5);
        call_->setFont(cf);
        call_->setMaxLength(14);
        call_->setMinimumWidth(160);
        call_->installEventFilter(this);
        connect(call_, &QLineEdit::textEdited, this, [this] {
            callFromSpot_ = false;   // typing reclaims ←/→ for the cursor
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

        // F keys
        auto* fr = new QHBoxLayout;
        fr->setSpacing(3);
        for (int i = 0; i < 12; ++i) {
            fk_[i] = new QPushButton(this);
            fk_[i]->setFocusPolicy(Qt::NoFocus);
            fk_[i]->setMinimumWidth(46);
            fk_[i]->setFixedHeight(34);
            connect(fk_[i], &QPushButton::clicked, this,
                    [this, i] { keyFkey(i); });
            fk_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(fk_[i], &QPushButton::customContextMenuRequested,
                    this, [this, i](const QPoint&) { editFkey(i); });
            fr->addWidget(fk_[i], 1);
        }
        auto* stop = new QPushButton("STOP\nEsc", this);
        stop->setFocusPolicy(Qt::NoFocus);
        stop->setFixedHeight(34);
        connect(stop, &QPushButton::clicked, this, [this] {
            if (cw_) cw_->stopKeying();
            if (stopVoice_) stopVoice_();
        });
        fr->addWidget(stop);
        mid->addLayout(fr);
        lay->addLayout(mid, 22);
    }

    // ---- CW TYPE (right) ------------------------------------------------
    {
        auto* box = new QVBoxLayout;
        auto* t = new QLabel("CW TYPE — Enter sends", this);
        t->setStyleSheet("color:#8798a8; font-size:10px;");
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
        box->addWidget(t);
        box->addWidget(type_);
        box->addWidget(sent_);
        box->addStretch(1);
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
    sc(QKeySequence(Qt::Key_Escape), [this] {
        if (cw_) cw_->stopKeying();
        if (stopVoice_) stopVoice_();
        if (autoBtn_ && autoBtn_->isChecked())
            autoBtn_->setChecked(false);   // Esc kills the robot too
        status_->setText("keying stopped");
    });
    sc(QKeySequence(Qt::Key_PageUp),
       [this] { wpm_->setValue(wpm_->value() + 1); });
    sc(QKeySequence(Qt::Key_PageDown),
       [this] { wpm_->setValue(wpm_->value() - 1); });
    for (QShortcut* s : shortcuts_) s->setEnabled(false);
    setEnabled(false);
}

void ContestDeck::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(true);
    if (contestId_ >= 0) call_->setFocus();
}

void ContestDeck::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(false);
}

bool ContestDeck::openContestId(qint64 id) {
    openContest(id);
    return contestId_ == id;
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
    if (def_->hasRst) rstS_ = addField("SNT", 4, "599");
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
        QLineEdit* e = addField(fd.label, fd.widthCh, fd.preset);
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

void ContestDeck::setRig(qint64 hz, const QString& adifMode) {
    const bool bandMoved =
        hz > 0 && LogbookIndex::bandForHz(hz) != currentBand();
    const qint64 prev = prevHz_;
    prevHz_ = hz;
    rigHz_ = hz;
    rigMode_ = adifMode.isEmpty() ? QStringLiteral("CW") : adifMode;
    if (qtc_) qtc_->setRigFreq(hz);
    // The park: a TYPED (never walk-landed), unworked call abandoned by
    // turning the knob gets remembered as a local spot at the frequency
    // it was heard on. callFromSpot_ landings are excluded or every
    // arrow hop would park its own passenger.
    if (contestId_ >= 0 && prev > 0 && hz > 0 && qAbs(hz - prev) > 1000) {
        const QString c = call_->text().trimmed();
        if (!callFromSpot_ && loggableCall(c)
            && classifySpot(c, prev) != 'W') {
            emit callParked(c, prev);
            trace(QString("PARK %1 @ %2").arg(c).arg(prev));
            wipe();
        }
    }
    if (bandMoved) onCallEdited();       // dupe verdict can flip
}

void ContestDeck::setNearbySpot(const QString& call, char cls) {
    const QString c = call.trimmed().toUpper();
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

void ContestDeck::prefillCall(const QString& call) {
    if (contestId_ < 0) return;
    call_->setText(call.trimmed().toUpper());
    myCallSent_ = exchSent_ = false;
    onCallEdited();
    callFromSpot_ = true;            // ←/→ keep walking from here
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
    execPlan(esmPlan(in), false);
    updateEsmHint();
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
                if (!updateOnly && !edits_.isEmpty())
                    edits_[0].second->setFocus();
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
    trace(QString("DECK QSO %1 %2 ser %3")
              .arg(c, q.v.band)
              .arg(q.v.serialS));
    wipe();                          // the silent "it logged" signal
    refreshAll();
    status_->setText(QString("logged %1").arg(c));
}

void ContestDeck::wipe() {
    call_->clear();
    if (rstS_) rstS_->setText("599");
    if (def_)
        for (int i = 0; i < edits_.size() && i < def_->fields.size(); ++i)
            edits_[i].second->setText(def_->fields[i].preset);
    dupe_->clear();
    info_->clear();
    updateHeading();                     // call box empty -> "—"
    myCallSent_ = exchSent_ = false;
    refreshScp();
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

void ContestDeck::setVoiceKeyer(std::function<void(int)> play,
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
        if (!playVk_) {
            status_->setText("voice keyer not wired");
            return;
        }
        playVk_(slot - 1);           // DVR slots are 0-based
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
    if (cty_ && cty_->info(normalizeForCty(c), ci))
        info_->setText(QString("%1 · %2 · CQ %3")
                           .arg(ci.country, ci.cont)
                           .arg(ci.cq));
    else
        info_->setText("—");
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

bool ContestDeck::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
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
                prefillCall(frameCall_);
                return true;
            }
            historyPrefill();
            if (!edits_.isEmpty()) edits_[0].second->setFocus();
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
