// SPDX-License-Identifier: GPL-3.0-or-later
// Copy Assist:
//   cmake --build build --target asrtest && ./build/asrtest
// - callsign extraction from whisper-style text (one word, letter runs,
//   hyphens, phonetics, trailing number words, non-calls)
// - with the base.en model present: G8SEZ's test recording (rade_c's
//   input_sample.wav) through the live path — 48 kHz feed with hiss
//   around it, the VAD, whisper — makes one utterance with "test" in it,
//   and with G8SEZ as a band-map hint the call copies as G8SEZ.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "asr/AsrEngine.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static QString calls(const QString& t) {
    QStringList l;
    for (const CallHit& h : extractCalls(t)) l << h.call;
    return l.join(',');
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    struct Case { const char* text; const char* want; };
    const Case cases[] = {
        {"G8SEZ test. Testing G8SEZ test.", "G8SEZ,G8SEZ"},
        {"this is Kilo One Alpha Bravo Charlie, you're five nine", "K1ABC"},
        {"Kilo One Alpha Bravo Charlie five nine", "K1ABC"},
        {"G-8-S-E-Z test", "G8SEZ"},
        {"CQ CQ de N8EM N 8 E M", "N8EM,N8EM"},
        {"VK5DGR portable, W1AW/P and 9A1A", "VK5DGR,W1AW/P,9A1A"},
        {"Victor Kilo Five Delta Golf Romeo", "VK5DGR"},
        {"1, 2, 3, 4, 5. Five nine nine. QSL 73", ""},
        {"I have a new antenna at 40 feet", ""},
    };
    for (const Case& c : cases) {
        const QString got = calls(QString::fromUtf8(c.text));
        std::printf("    \"%s\" -> [%s]\n", c.text, qPrintable(got));
        check(got == QLatin1String(c.want), "  extracted as expected");
    }

    const QString model = AsrEngine::defaultModelPath();
#ifdef RADE_WAV_DIR
    const char* wavPath = RADE_WAV_DIR "/input_sample.wav";
#else
    const char* wavPath = "input_sample.wav";
#endif
    std::ifstream f(wavPath, std::ios::binary);
    if (!QFileInfo::exists(model) || !f) {
        std::printf("  SKIP whisper checks (model %s, wav %s)\n", qPrintable(model), wavPath);
    } else {
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<float> pcm16;
        for (size_t p = 12; p + 8 <= b.size();) {
            uint32_t len;
            std::memcpy(&len, &b[p + 4], 4);
            if (std::memcmp(&b[p], "data", 4) == 0) {
                for (size_t i = 0; i + 1 < len && p + 9 + i < b.size(); i += 2) {
                    int16_t v;
                    std::memcpy(&v, &b[p + 8 + i], 2);
                    pcm16.push_back(v / 32768.f);
                }
                break;
            }
            p += 8 + len;
        }
        AsrEngine eng;
        QString err;
        check(eng.start(model, 8, &err), "whisper loads base.en");
        // Direct: with and without the band-map hint.
        const AsrEngine::Result plain = eng.transcribe(pcm16, {});
        const AsrEngine::Result hinted = eng.transcribe(pcm16, {"G8SEZ", "N8EM"});
        std::printf("    no hint (%.1f s): %s  [conf %.2f]\n", plain.decodeMs / 1000.0,
                    qPrintable(plain.text), plain.confidence);
        std::printf("    hinted  (%.1f s): %s  [conf %.2f]\n", hinted.decodeMs / 1000.0,
                    qPrintable(hinted.text), hinted.confidence);
        check(plain.text.contains("test", Qt::CaseInsensitive), "speech transcribes");
        check(plain.decodeMs < 1000.0 * pcm16.size() / 16000, "faster than real time");
        bool g8 = false;
        for (const CallHit& h : hinted.calls) g8 |= h.call == "G8SEZ";
        check(g8, "band-map hint copies the call as G8SEZ");

        // Live path: 48 kHz in 10 ms blocks, hiss before and after.
        QVector<AsrEngine::Result> got;
        QObject::connect(&eng, &AsrEngine::transcribed,
                         [&](const AsrEngine::Result& r) { got.push_back(r); });
        eng.setHints({"G8SEZ"}, 14236000);
        std::vector<float> feed;
        unsigned seed = 3;
        const auto hiss = [&] { seed = seed * 1103515245u + 12345u; return 0.003f * (((seed >> 9) & 0xffff) / 32768.f - 1.f); };
        for (int i = 0; i < 48000 * 2; ++i) feed.push_back(hiss());
        for (size_t i = 0; i + 1 < pcm16.size(); ++i)
            for (int k = 0; k < 3; ++k)
                feed.push_back(0.5f * (pcm16[i] + (pcm16[i + 1] - pcm16[i]) * k / 3.f) + hiss());
        for (int i = 0; i < 48000 * 2; ++i) feed.push_back(hiss());
        for (size_t o = 0; o + 480 <= feed.size(); o += 480) eng.feed48k(&feed[o], 480, false);
        QElapsedTimer t;
        t.start();
        while ((got.isEmpty() || eng.queuedSeconds() > 0) && t.elapsed() < 30000)
            app.processEvents(QEventLoop::AllEvents, 50);
        t.restart();
        while (t.elapsed() < 6000) app.processEvents(QEventLoop::AllEvents, 50);   // stragglers
        std::printf("    live path: %d utterance(s)\n", int(got.size()));
        for (const auto& r : got)
            std::printf("      %.1f s: %s [conf %.2f]\n", r.seconds, qPrintable(r.text), r.confidence);
        check(!got.isEmpty() && got[0].text.contains("test", Qt::CaseInsensitive),
              "VAD closes the over and whisper copies it");
        check(got.size() == 1 && got[0].seconds > 7 && got[0].seconds < 14,
              "one utterance spanning the speech, not the hiss");
        eng.stop();
    }
    if (fails == 0) std::printf("asrtest: all checks passed\n");
    return fails ? 1 : 0;
}
