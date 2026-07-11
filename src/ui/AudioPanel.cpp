// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/AudioPanel.h"

#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QToolButton>
#include <QTimer>

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

// Sized ~150% of the console's other popups: this one gets worked bare-handed
// mid-QSO (volume rides, ear flips), so bigger targets win over compactness.
AudioPanel::AudioPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 14px; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }"
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        // Routing cells: A green, B blue, A+B teal — the console's VFO colors.
        "QToolButton[accent=\"a\"]:checked { background: #1f7a45; border-color: #3ecf7a; color: #eafff2; }"
        "QToolButton[accent=\"b\"]:checked { background: #2f6d9e; border-color: #5db2f0; color: #eaf6ff; }"
        "QToolButton[accent=\"ab\"]:checked { background: #2f7d7d; border-color: #4ad0d0; color: #eafcfc; }"
        "QToolButton[accent=\"mute\"]:checked { background: #8a2727; border-color: #e05d5d; color: #ffecec; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(9);

    // --- volumes -----------------------------------------------------------
    static const char* volName[2] = {"VOL A", "VOL B"};
    for (int i = 0; i < 2; ++i) {
        g->addWidget(caption(volName[i], this), i, 0);
        vol_[i] = new QSlider(Qt::Horizontal, this);
        vol_[i]->setRange(0, 100);
        vol_[i]->setFixedWidth(210);
        volVal_[i] = new QLabel("0", this);
        volVal_[i]->setFixedWidth(38);
        mute_[i] = new QToolButton(this);
        mute_[i]->setText("MUTE");
        mute_[i]->setCheckable(true);
        mute_[i]->setProperty("accent", "mute");
        mute_[i]->setFixedSize(58, 25);
        mute_[i]->setFocusPolicy(Qt::NoFocus);
        g->addWidget(vol_[i],    i, 1, 1, 2);
        g->addWidget(volVal_[i], i, 3);
        g->addWidget(mute_[i],   i, 4);

        volTx_[i] = new QTimer(this);
        volTx_[i]->setSingleShot(true);
        volTx_[i]->setInterval(60);
        const Rx rx = i == 0 ? Rx::Main : Rx::Sub;
        connect(volTx_[i], &QTimer::timeout, this,
                [this, i, rx] { emit volumeEdited(rx, pendVol_[i]); });
        connect(vol_[i], &QSlider::valueChanged, this, [this, i](int v) {
            volVal_[i]->setText(QString::number(v));
            if (updating_) return;
            pendVol_[i] = v;
            volTx_[i]->start();                     // coalesce the stream
        });
        connect(mute_[i], &QToolButton::toggled, this, [this, rx](bool on) {
            if (!updating_) emit muteToggled(rx, on);
        });
    }

    // --- output routing ------------------------------------------------------
    g->addWidget(caption("A", this),   2, 1, Qt::AlignHCenter);
    g->addWidget(caption("B", this),   2, 2, Qt::AlignHCenter);
    g->addWidget(caption("A+B", this), 2, 3, Qt::AlignHCenter);
    static const char* rowName[3] = {"PH L", "PH R", "SPKR"};
    static const char* rowWhat[3] = {"left headphone", "right headphone", "speaker"};
    static const char* colAccent[3] = {"a", "b", "ab"};
    static const char* colWhat[3] = {"VFO A (main RX)", "VFO B (sub RX)", "both receivers"};
    for (int r = 0; r < 3; ++r) {
        g->addWidget(caption(rowName[r], this), r + 3, 0);
        for (int c = 0; c < 3; ++c) {
            route_[r][c] = cell(QString("%1 hears %2").arg(rowWhat[r]).arg(colWhat[c]));
            route_[r][c]->setProperty("accent", colAccent[c]);
            g->addWidget(route_[r][c], r + 3, c + 1);
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

void AudioPanel::showVolume(Rx rx, int pct) {
    const int i = rx == Rx::Main ? 0 : 1;
    updating_ = true;
    vol_[i]->setValue(pct);
    updating_ = false;
}

void AudioPanel::showMute(Rx rx, bool on) {
    const int i = rx == Rx::Main ? 0 : 1;
    updating_ = true;
    mute_[i]->setChecked(on);
    updating_ = false;
}

bool AudioPanel::isMuted(Rx rx) const {
    return mute_[rx == Rx::Main ? 0 : 1]->isChecked();
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
