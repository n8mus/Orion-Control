// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/ClipDeck.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <csignal>

namespace ttc {

ClipDeck::ClipDeck(QObject* parent) : QObject(parent) {
    proc_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        state_ = State::Idle;
        emit finished();
        // A stop() lands as SIGINT (clean exit) or, if pw-cat ignored it, a
        // signal death (CrashExit) — neither is a failure worth reporting.
        // failed() goes out after finished() so its message isn't overwritten
        // by generic idle-state handling.
        if (!stopping_ && st == QProcess::NormalExit && code != 0)
            emit failed(QString::fromUtf8(proc_.readAll()).trimmed());
    });
    connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {        // no finished() from QProcess
            state_ = State::Idle;
            emit finished();
            emit failed("parecord/paplay not found (pulseaudio-utils missing?)");
        }
    });
}

ClipDeck::~ClipDeck() {
    if (state_ != State::Idle) {                   // finalize an in-flight WAV
        stop();
        proc_.waitForFinished(1000);
    }
}

// Pulse-layer tools (parecord/paplay -> pipewire-pulse), not the native
// pw-cat pair: live-found that the native client path can wedge silently
// after a pipewire restart (stream RUNNING, sink monitor carrying audio,
// speakers silent) while the pulse layer — the road every desktop app
// takes — keeps working. s16le forced so normalizeWav's 16-bit PCM
// expectation holds; parecord finalizes a valid WAV on SIGINT (verified).
// pw-record, not parecord: parecord writes in 65536-frame (~1.37 s) blocks
// and drops the unfinished one on SIGINT, so every take lost up to 1.4 s
// off its END (measured 2026-10-14: 3 s -> 1.37 s, 6 s -> 4.10 s; pw-record
// 2.73 / 5.99 s). A voice-keyer message shorter than that saved empty.
bool ClipDeck::record(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    QStringList args{"--format=s16", "--rate=48000", "--channels=1"};
    if (!targetNode.isEmpty()) args << "--target" << targetNode;
    args << wavPath;
    return launch("pw-record", args, State::Recording);
}

bool ClipDeck::play(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    QStringList args;
    if (!targetNode.isEmpty()) args << "-d" << targetNode;
    args << wavPath;
    return launch("paplay", args, State::Playing);
}

void ClipDeck::stop() {
    if (state_ == State::Idle) return;
    stopping_ = true;
    if (proc_.processId() > 0)
        ::kill(static_cast<pid_t>(proc_.processId()), SIGINT);
}

bool ClipDeck::launch(const QString& exe, const QStringList& args, State s) {
    stopping_ = false;
    proc_.start(exe, args);
    if (!proc_.waitForStarted(2000)) return false; // errorOccurred already fired
    state_ = s;
    return true;
}

static QString findNode(const char* kind, const QString& match) {
    QProcess p;
    p.start("pactl", {"list", "short", kind});
    if (!p.waitForFinished(3000)) return {};
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString& line : out.split('\n')) {
        const QStringList f = line.split('\t');
        if (f.size() >= 2 && f[1].contains(match, Qt::CaseInsensitive)
            && !f[1].endsWith(".monitor"))
            return f[1];
    }
    return {};
}

QString ClipDeck::findSink(const QString& match)   { return findNode("sinks", match); }
QString ClipDeck::findSource(const QString& match) { return findNode("sources", match); }

QString ClipDeck::findSourceExcluding(const QString& avoid) {
    QProcess p;
    p.start("pactl", {"list", "short", "sources"});
    if (!p.waitForFinished(3000)) return {};
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    QStringList candidates;
    for (const QString& line : out.split('\n')) {
        const QStringList f = line.split('\t');
        if (f.size() < 2 || f[1].endsWith(".monitor")) continue;
        if (!avoid.isEmpty() && f[1].contains(avoid, Qt::CaseInsensitive)) continue;
        candidates << f[1];
    }
    for (const QString& c : candidates)            // a USB mic beats a probably-
        if (c.contains("usb", Qt::CaseInsensitive)) return c;   // empty line-in
    return candidates.isEmpty() ? QString() : candidates.first();
}

QVector<ClipDeck::Source> ClipDeck::sources() {
    QProcess p;
    p.start("pactl", {"--format=json", "list", "sources"});
    if (!p.waitForFinished(3000)) return {};
    QVector<Source> out;
    for (const QJsonValue& v :
         QJsonDocument::fromJson(p.readAllStandardOutput()).array()) {
        const QJsonObject o = v.toObject();
        const QString node = o.value("name").toString();
        if (node.isEmpty() || node.endsWith(".monitor")) continue;
        const QString label = o.value("description").toString();
        out.push_back({node, label.isEmpty() ? node : label});
    }
    return out;
}

QString ClipDeck::radioAudioMatch() {
    return QSettings().value("dvr/radioAudioMatch",
        // The SignaLink's PCM2906C by its full vendor string. Bare
        // "USB_AUDIO_CODEC" also matched (case-blind) the Alesis mixer's
        // PCM2900 "Burr-Brown_from_TI_USB_Audio_CODEC" — first-listed won,
        // so a re-plug could send VK TX audio into the mixer (live-found
        // 2026-10-14).
        "BurrBrown_from_Texas_Instruments_USB_AUDIO_CODEC"
        ).toString();
}

QString ClipDeck::friendlyLabel(const Source& src) {
    const QString own =
        QSettings().value("audio/names/" + src.node).toString().trimmed();
    if (!own.isEmpty()) return QString("%1  (%2)").arg(own, src.label);
    const QString match = radioAudioMatch();
    if (!match.isEmpty() && src.node.contains(match, Qt::CaseInsensitive))
        return src.label + "  — SignaLink (radio audio)";
    if (src.node.contains(".pci-"))
        return src.label + "  — motherboard jack";
    return src.label;
}

QString ClipDeck::sourceLabel(const QString& node) {
    for (const Source& src : sources())
        if (src.node == node) return friendlyLabel(src);
    return node;
}

// Minimal RIFF walker: find the fmt/data chunks of the PCM WAVs pw-record
// writes, scale the samples, rewrite the file.
namespace {
// Locate the 16-bit PCM data chunk of a WAV held in memory.
bool findPcm16(const QByteArray& b, qsizetype& dataOff, qsizetype& dataLen,
               int& rate, int& channels) {
    if (b.size() < 44 || !b.startsWith("RIFF") || b.mid(8, 4) != "WAVE")
        return false;
    const auto u16 = [&](qsizetype o) {
        return quint16(quint8(b[o])) | quint16(quint8(b[o + 1])) << 8;
    };
    const auto u32 = [&](qsizetype o) {
        return quint32(u16(o)) | quint32(u16(o + 2)) << 16;
    };
    dataOff = -1;
    dataLen = 0;
    bool pcm16 = false;
    for (qsizetype o = 12; o + 8 <= b.size();) {
        const QByteArray id = b.mid(o, 4);
        const qsizetype len = u32(o + 4);
        if (id == "fmt " && len >= 16) {
            pcm16 = u16(o + 8) == 1 && u16(o + 22) == 16;  // PCM, 16-bit
            channels = std::max(1, int(u16(o + 10)));
            rate = int(u32(o + 12));
        }
        if (id == "data") {
            dataOff = o + 8;
            // pw-record leaves the size field huge until it finalizes; the
            // file length is the truth either way.
            dataLen = std::min<qsizetype>(len, b.size() - dataOff);
            break;
        }
        o += 8 + len + (len & 1);                  // chunks are word-aligned
    }
    return pcm16 && dataOff >= 0;
}
}  // namespace

bool ClipDeck::wavStats(const QString& wavPath, double& seconds, double& peak) {
    seconds = peak = 0.0;
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = file.readAll();
    qsizetype off = 0, len = 0;
    int rate = 48000, ch = 1;
    if (!findPcm16(b, off, len, rate, ch)) return false;
    const auto* s = reinterpret_cast<const qint16*>(b.constData() + off);
    const qsizetype n = len / 2;
    int pk = 0;
    for (qsizetype i = 0; i < n; ++i) pk = std::max(pk, std::abs(int(s[i])));
    seconds = rate > 0 ? double(n) / ch / rate : 0.0;
    peak = pk / 32768.0;
    return true;
}

bool ClipDeck::readWavMono(const QString& wavPath, std::vector<float>& out,
                           int& rate) {
    out.clear();
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = file.readAll();
    qsizetype off = 0, len = 0;
    int ch = 1;
    rate = 48000;
    if (!findPcm16(b, off, len, rate, ch) || rate <= 0) return false;
    const auto* s = reinterpret_cast<const qint16*>(b.constData() + off);
    const qsizetype frames = len / 2 / ch;
    out.resize(static_cast<size_t>(frames));
    for (qsizetype f = 0; f < frames; ++f) {
        int acc = 0;
        for (int c = 0; c < ch; ++c) acc += s[f * ch + c];
        out[static_cast<size_t>(f)] = float(acc) / float(ch) / 32768.0f;
    }
    return true;
}

bool ClipDeck::writeWavMono(const QString& wavPath,
                            const std::vector<float>& samples, int rate) {
    QFile file(wavPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const quint32 dataBytes = quint32(samples.size() * 2);
    QByteArray h;
    const auto put16 = [&h](quint16 v) { h.append(char(v & 0xff)); h.append(char(v >> 8)); };
    const auto put32 = [&put16](quint32 v) { put16(quint16(v & 0xffff)); put16(quint16(v >> 16)); };
    h.append("RIFF");
    put32(36 + dataBytes);
    h.append("WAVEfmt ");
    put32(16);
    put16(1);                                      // PCM
    put16(1);                                      // mono
    put32(quint32(rate));
    put32(quint32(rate) * 2);                      // byte rate
    put16(2);                                      // block align
    put16(16);                                     // bits
    h.append("data");
    put32(dataBytes);
    QByteArray pcm(qsizetype(dataBytes), Qt::Uninitialized);
    auto* d = reinterpret_cast<qint16*>(pcm.data());
    for (size_t i = 0; i < samples.size(); ++i)
        d[i] = qint16(std::clamp(int(std::lround(samples[i] * 32767.0f)),
                                 -32768, 32767));
    return file.write(h) == h.size() && file.write(pcm) == pcm.size();
}

bool ClipDeck::normalizeWav(const QString& wavPath, double targetPeak,
                            double maxGain) {
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadWrite)) return false;
    QByteArray b = file.readAll();
    qsizetype dataOff = 0, dataLen = 0;
    int rate = 0, ch = 0;
    if (!findPcm16(b, dataOff, dataLen, rate, ch) || dataLen < 2) return false;
    auto* s = reinterpret_cast<qint16*>(b.data() + dataOff);
    const qsizetype n = dataLen / 2;
    int peak = 0;
    for (qsizetype i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(s[i])));
    if (peak == 0) return false;                   // dead silence: leave it
    const double gain = std::min(targetPeak * 32767.0 / peak, maxGain);
    if (gain <= 1.0) return true;                  // already hot enough
    for (qsizetype i = 0; i < n; ++i)
        s[i] = qint16(std::clamp(int(std::lround(s[i] * gain)), -32768, 32767));
    file.seek(0);
    file.write(b);
    return true;
}

} // namespace ttc
