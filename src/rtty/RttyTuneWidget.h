// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFont>
#include <QPainter>
#include <QWidget>
#include <algorithm>

namespace ttc {

// Mark / space as two bars with the keying contrast between them: both
// tall and alternating = on frequency; one tall = off by a shift; both
// short = nothing there. Green while the squelch is open.
class RttyTuneWidget : public QWidget {
public:
    explicit RttyTuneWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(60, 56);
        setToolTip("Mark (left) and space (right) tone levels. Click the signal on\n"
                   "the waterfall; the AFC pulls in the last +-200 Hz. Both bars tall\n"
                   "and swapping as the station keys = tuned; green = the squelch is\n"
                   "open and text prints. Below: signal over noise, keying contrast,\n"
                   "and the mark tone with the AFC's trim.");
    }
    void set(float mark, float space, float quality, float snrDb, bool open,
             float afcHz, int markHz) {
        m_ = mark; s_ = space; q_ = quality; snr_ = snrDb; open_ = open;
        afc_ = afcHz; markHz_ = markHz;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0e141b"));
        const int w = width(), h = height() - 24;
        const int bw = std::max(6, (w - 18) / 2);
        const QColor on = open_ ? QColor("#82de8c") : QColor("#6aa5d8");
        const auto bar = [&](int x, float v) {
            const int bh = int(std::clamp(v, 0.0f, 1.0f) * (h - 4));
            p.fillRect(x, 2 + (h - 4 - bh), bw, bh, on);
            p.setPen(QColor("#2a3644"));
            p.drawRect(x, 2, bw, h - 4);
        };
        bar(6, m_);
        bar(12 + bw, s_);
        p.setPen(QColor("#8798a8"));
        QFont f = p.font();
        f.setPointSize(8);
        p.setFont(f);
        p.drawText(QRect(0, h, w, 12), Qt::AlignCenter,
                   QString("%1 dB · %2").arg(int(snr_)).arg(q_, 0, 'f', 2));
        // Where the pair sits: the nominal mark and the AFC's trim on it.
        p.drawText(QRect(0, h + 12, w, 12), Qt::AlignCenter,
                   QString("%1%2%3").arg(markHz_)
                       .arg(afc_ >= 0 ? "+" : "").arg(int(afc_)));
    }
private:
    float m_ = 0, s_ = 0, q_ = 0, snr_ = 0, afc_ = 0;
    int markHz_ = 2125;
    bool open_ = false;
};

}  // namespace ttc
