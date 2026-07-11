// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/AudioPanel.h"

#include <QGridLayout>
#include <QLabel>
#include <QToolButton>

namespace ttc {

namespace {
QLabel* caption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 13px; font-weight: bold;");
    return l;
}
constexpr char kColLetter[3] = {'M', 'S', 'B'};    // A / B / A+B columns
} // namespace

QToolButton* AudioPanel::cell(const QString& tip) {
    auto* b = new QToolButton(this);
    b->setCheckable(true);
    b->setFixedSize(44, 25);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    return b;
}

// Sized ~150% of the console's other popups: worked bare-handed mid-QSO.
AudioPanel::AudioPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 14px; }"
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        // Routing cells: A green, B blue, A+B teal — the console's VFO colors.
        "QToolButton[accent=\"a\"]:checked { background: #1f7a45; border-color: #3ecf7a; color: #eafff2; }"
        "QToolButton[accent=\"b\"]:checked { background: #2f6d9e; border-color: #5db2f0; color: #eaf6ff; }"
        "QToolButton[accent=\"ab\"]:checked { background: #2f7d7d; border-color: #4ad0d0; color: #eafcfc; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(9);

    g->addWidget(caption("A", this),   0, 1, Qt::AlignHCenter);
    g->addWidget(caption("B", this),   0, 2, Qt::AlignHCenter);
    g->addWidget(caption("A+B", this), 0, 3, Qt::AlignHCenter);
    static const char* rowName[3] = {"PH L", "PH R", "SPKR"};
    static const char* rowWhat[3] = {"left headphone", "right headphone", "speaker"};
    static const char* colAccent[3] = {"a", "b", "ab"};
    static const char* colWhat[3] = {"VFO A (main RX)", "VFO B (sub RX)", "both receivers"};
    for (int r = 0; r < 3; ++r) {
        g->addWidget(caption(rowName[r], this), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            route_[r][c] = cell(QString("%1 hears %2").arg(rowWhat[r]).arg(colWhat[c]));
            route_[r][c]->setProperty("accent", colAccent[c]);
            g->addWidget(route_[r][c], r + 1, c + 1);
            connect(route_[r][c], &QToolButton::clicked, this, [this, r, c] {
                for (int k = 0; k < 3; ++k)         // one source per output
                    route_[r][k]->setChecked(k == c);
                emitRouting();
            });
        }
    }

    showRouting('B', 'B', 'B');                     // sane default until ?UC answers
}

void AudioPanel::emitRouting() {
    if (updating_) return;
    char out[3] = {'B', 'B', 'B'};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (route_[r][c]->isChecked()) out[r] = kColLetter[c];
    emit routingEdited(out[0], out[1], out[2]);
}

void AudioPanel::showRouting(char left, char right, char speaker) {
    const char in[3] = {left, right, speaker};
    updating_ = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            route_[r][c]->setChecked(in[r] == kColLetter[c]);
    updating_ = false;
}

} // namespace ttc
