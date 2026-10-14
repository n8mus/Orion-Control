// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/LiveVoice.h"

#include <QProcess>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 480;                        // 10 ms through the chain
}

LiveVoice::LiveVoice(QObject* parent) : QObject(parent) {}

LiveVoice::~LiveVoice() { stop(); }

bool LiveVoice::running() const {
    return rec_ && rec_->state() != QProcess::NotRunning
        && play_ && play_->state() != QProcess::NotRunning;
}

bool LiveVoice::start(const QString& sourceNode, const QString& sinkNode,
                      const VoiceEqSettings& s) {
    stop();
    stopping_ = false;
    carry_.clear();
    chain_.prepare(kRate, s, s.comp.makeupDb);
    play_ = new QProcess(this);
    play_->start("pacat", {"--playback", "--raw", "--format=s16le",
                           "--rate=48000", "--channels=1",
                           "--latency-msec=30", "--device=" + sinkNode});
    rec_ = new QProcess(this);
    connect(rec_, &QProcess::readyReadStandardOutput, this, &LiveVoice::onCaptured);
    const auto died = [this](const char* who) {
        return [this, who](int code, QProcess::ExitStatus) {
            if (stopping_) return;
            emit failed(QString("%1 ended (%2)").arg(who).arg(code));
        };
    };
    connect(rec_, &QProcess::finished, this, died("mic capture"));
    connect(play_, &QProcess::finished, this, died("SignaLink playback"));
    rec_->start("parec", {"--raw", "--format=s16le", "--rate=48000",
                          "--channels=1", "--latency-msec=20",
                          "--device=" + sourceNode});
    if (!play_->waitForStarted(2000) || !rec_->waitForStarted(2000)) {
        stop();
        emit failed("parec/pacat would not start");
        return false;
    }
    return true;
}

void LiveVoice::stop() {
    stopping_ = true;
    for (QProcess** p : {&rec_, &play_}) {
        if (!*p) continue;
        (*p)->disconnect(this);
        (*p)->kill();
        (*p)->waitForFinished(500);
        (*p)->deleteLater();
        *p = nullptr;
    }
    carry_.clear();
}

void LiveVoice::setSettings(const VoiceEqSettings& s) {
    chain_.configure(s, s.comp.makeupDb);
}

void LiveVoice::onCaptured() {
    if (!rec_ || !play_) return;
    carry_ += rec_->readAllStandardOutput();
    const int frames = int(carry_.size() / 2);
    if (frames < kBlock) return;
    const int take = frames / kBlock * kBlock;     // whole blocks only
    buf_.resize(size_t(take));
    const auto* in = reinterpret_cast<const qint16*>(carry_.constData());
    float pk = 0.f;
    for (int i = 0; i < take; ++i) {
        buf_[size_t(i)] = in[i] / 32768.0f;
        pk = std::max(pk, std::fabs(buf_[size_t(i)]));
    }
    carry_.remove(0, qsizetype(take) * 2);
    inPeakDb_ = 20.f * std::log10(std::max(pk, 1e-6f));
    for (int o = 0; o < take; o += kBlock) chain_.process(buf_.data() + o, kBlock);
    QByteArray out(qsizetype(take) * 2, Qt::Uninitialized);
    auto* d = reinterpret_cast<qint16*>(out.data());
    for (int i = 0; i < take; ++i)
        d[i] = muted_ ? qint16(0)
                      : qint16(std::clamp(int(std::lround(buf_[size_t(i)] * 32767.f)),
                                          -32768, 32767));
    play_->write(out);
}

} // namespace ttc
