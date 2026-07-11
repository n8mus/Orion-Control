// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/FrequencyDisplay.h"

#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr uint64_t kMinHz = 100000;      // Orion RX range sanity
constexpr uint64_t kMaxHz = 60000000;
constexpr int kCellW  = 19;              // per-glyph cell; dots use a half cell
constexpr int kDotW   = 9;
constexpr int kMarginX = 6;
// Cell layout: [10M][1M] . [100k][10k][1k] . [100][10][1]
constexpr uint64_t kPlace[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};

// x offset of digit cell i (0..7), accounting for the two dot separators.
int cellX(int i) {
    int x = kMarginX + i * kCellW;
    if (i >= 2) x += kDotW;              // dot after MHz
    if (i >= 5) x += kDotW;              // dot after kHz
    return x;
}
} // namespace

FrequencyDisplay::FrequencyDisplay(const QString& caption, const QColor& accent,
                                   QWidget* parent)
    : QWidget(parent), caption_(caption), accent_(accent) {
    digitTop_ = caption_.isEmpty() ? 0 : 14;
    setFixedSize(cellX(7) + kCellW + kMarginX, digitTop_ + 34);
    setCursor(Qt::PointingHandCursor);

    edit_ = new QLineEdit(this);
    edit_->setGeometry(QRect(2, digitTop_ + 2, width() - 4, 28));
    edit_->setAlignment(Qt::AlignCenter);
    edit_->setStyleSheet("QLineEdit { background: #1c2430; color: #e8f0f8;"
                         " border: 1px solid #3f7cb4; font-size: 16px; }");
    edit_->hide();
    connect(edit_, &QLineEdit::returnPressed, this, &FrequencyDisplay::finishEdit);
    connect(edit_, &QLineEdit::editingFinished, this, [this] { edit_->hide(); });
}

void FrequencyDisplay::setAccent(const QColor& c) {
    if (c == accent_) return;
    accent_ = c;
    update();
}

void FrequencyDisplay::setFrequency(uint64_t hz) {
    if (edit_->isVisible()) return;      // don't repaint under a type-in
    if (hz == hz_) return;
    hz_ = hz;
    update();
}

void FrequencyDisplay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));

    if (!caption_.isEmpty()) {
        QFont cf = p.font();
        cf.setPixelSize(10);
        cf.setBold(true);
        p.setFont(cf);
        p.setPen(QColor(143, 163, 184));
        p.drawText(QRect(kMarginX, 0, width() - kMarginX, digitTop_),
                   Qt::AlignLeft | Qt::AlignVCenter, caption_);
    }

    // KE9NS look: MHz + kHz digits big in the VFO's accent color, the Hz
    // group smaller and near-white, leading zeros not drawn at all (their
    // cells stay live as click/wheel targets).
    QFont big("DejaVu Sans");
    big.setPixelSize(27);
    big.setBold(true);
    QFont small = big;
    small.setPixelSize(19);

    const QRect row(0, digitTop_, width(), height() - digitTop_);
    bool leading = true;
    for (int i = 0; i < 8; ++i) {
        const int d = static_cast<int>((hz_ / kPlace[i]) % 10);
        if (d != 0 || i >= 1) leading = false;      // only the 10 MHz digit hides
        if (leading) continue;
        p.setFont(i < 5 ? big : small);
        p.setPen(i < 5 ? accent_ : QColor(222, 230, 238));
        p.drawText(QRect(cellX(i), row.y(), kCellW, row.height()), Qt::AlignCenter,
                   QString::number(d));
    }
    p.setFont(big);
    p.setPen(QColor(accent_.red(), accent_.green(), accent_.blue(), 150));
    p.drawText(QRect(cellX(1) + kCellW, row.y(), kDotW, row.height()),
               Qt::AlignCenter, ".");
    p.drawText(QRect(cellX(4) + kCellW, row.y(), kDotW, row.height()),
               Qt::AlignCenter, ".");
}

int FrequencyDisplay::digitAt(int x) const {
    for (int i = 0; i < 8; ++i)
        if (x >= cellX(i) && x < cellX(i) + kCellW) return i;
    return -1;
}

void FrequencyDisplay::bump(int digit, int direction) {
    if (digit < 0) return;
    const int64_t step = static_cast<int64_t>(kPlace[digit]) * direction;
    const int64_t next = static_cast<int64_t>(hz_) + step;
    hz_ = static_cast<uint64_t>(std::clamp<int64_t>(next, kMinHz, kMaxHz));
    update();
    emit frequencyEdited(hz_);
}

void FrequencyDisplay::mousePressEvent(QMouseEvent* e) {
    // Top half of a digit increments that decade, bottom half decrements.
    const int mid = digitTop_ + (height() - digitTop_) / 2;
    bump(digitAt(e->pos().x()), e->pos().y() < mid ? +1 : -1);
}

void FrequencyDisplay::mouseDoubleClickEvent(QMouseEvent*) { beginEdit(); }

void FrequencyDisplay::wheelEvent(QWheelEvent* e) {
    const int steps = e->angleDelta().y() / 120;
    if (steps) bump(digitAt(static_cast<int>(e->position().x())), steps > 0 ? +1 : -1);
    e->accept();
}

void FrequencyDisplay::beginEdit() {
    edit_->setText(QString::number(hz_ / 1e6, 'f', 6));
    edit_->selectAll();
    edit_->show();
    edit_->setFocus();
}

void FrequencyDisplay::finishEdit() {
    bool ok = false;
    const double v = edit_->text().trimmed().toDouble(&ok);
    edit_->hide();
    if (!ok || v <= 0) return;
    // Accept MHz ("14.2"), kHz ("14200") or Hz ("14200000") by magnitude.
    double hz = v;
    if (v < 100.0)          hz = v * 1e6;
    else if (v < 100000.0)  hz = v * 1e3;
    hz_ = static_cast<uint64_t>(std::clamp(hz, double(kMinHz), double(kMaxHz)));
    update();
    emit frequencyEdited(hz_);
}

} // namespace ttc
