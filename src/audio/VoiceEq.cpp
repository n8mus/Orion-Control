// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/VoiceEq.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/ClipDeck.h"

namespace ttc {

namespace {
using FT = ClientEq::FilterType;

constexpr FT kSlotTypes[VoiceEqSettings::kBands] = {
    FT::HighPass, FT::LowShelf, FT::Peak, FT::Peak,
    FT::Peak,     FT::Peak,     FT::HighShelf, FT::LowPass,
};

ClientEq::BandParams band(float hz, float gainDb, float q, bool on,
                          FT type, int slope = 12) {
    ClientEq::BandParams b;
    b.freqHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    b.type = type;
    b.enabled = on;
    b.slopeDbPerOct = slope;
    return b;
}

// Normalizing up after an EQ cut is capped: a take that was mostly rumble
// must not have its hiss raised 30 dB because the high-pass removed the
// rumble. 4x = +12 dB.
constexpr double kMaxMakeupGain = 4.0;
}  // namespace

ClientEq::FilterType VoiceEqSettings::slotType(int i) {
    return kSlotTypes[std::clamp(i, 0, kBands - 1)];
}

QString VoiceEqSettings::slotName(int i) {
    static const char* names[kBands] = {"High-pass", "Low shelf", "Peak 1",
                                        "Peak 2",    "Peak 3",    "Peak 4",
                                        "High shelf", "Low-pass"};
    return QString::fromLatin1(names[std::clamp(i, 0, kBands - 1)]);
}

QStringList VoiceEqSettings::presetNames() {
    return {"Flat", "SSB voice", "Wide SSB"};
}

// SSB voice: the usual broadcast-voice moves for a dynamic mic on SSB —
// rumble/proximity gone below 100 Hz, a little mud out around 300 Hz,
// presence up around 2.5 kHz. Wide SSB keeps more bottom (50 Hz HPF,
// gentle low shelf) for the Orion's 3.9 kHz TX filter.
VoiceEqSettings VoiceEqSettings::preset(Preset p) {
    VoiceEqSettings s;
    s.enabled = p != Preset::Flat;
    const bool wide = p == Preset::WideSsb;
    const bool on = p != Preset::Flat;
    s.bands[0] = band(wide ? 50.f : 100.f, 0, 0.707f, on, FT::HighPass, 24);
    s.bands[1] = band(120.f, wide ? 2.f : 0.f, 0.707f, wide, FT::LowShelf);
    s.bands[2] = band(wide ? 350.f : 300.f, wide ? -2.f : -3.f, 1.0f, on, FT::Peak);
    s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
    s.bands[4] = band(1600.f, 0, 1.0f, false, FT::Peak);
    s.bands[5] = band(wide ? 2800.f : 2500.f, wide ? 2.f : 3.f, wide ? 0.9f : 1.0f,
                      on, FT::Peak);
    s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
    s.bands[7] = band(wide ? 3800.f : 3000.f, 0, 0.707f, false, FT::LowPass, 24);
    return s;
}

QJsonObject VoiceEqSettings::toJson() const {
    QJsonArray arr;
    for (const auto& b : bands)
        arr.append(QJsonObject{{"on", b.enabled}, {"hz", b.freqHz},
                               {"db", b.gainDb}, {"q", b.q},
                               {"slope", b.slopeDbPerOct}});
    return QJsonObject{{"enabled", enabled}, {"bands", arr}};
}

VoiceEqSettings VoiceEqSettings::fromJson(const QJsonObject& o) {
    VoiceEqSettings s = preset(Preset::Flat);
    s.enabled = o.value("enabled").toBool();
    const QJsonArray arr = o.value("bands").toArray();
    for (int i = 0; i < kBands && i < arr.size(); ++i) {
        const QJsonObject b = arr.at(i).toObject();
        auto& d = s.bands[size_t(i)];
        d.type = slotType(i);
        d.enabled = b.value("on").toBool();
        d.freqHz = std::clamp(float(b.value("hz").toDouble(d.freqHz)), 20.f, 8000.f);
        d.gainDb = std::clamp(float(b.value("db").toDouble(0)), -24.f, 24.f);
        d.q = std::clamp(float(b.value("q").toDouble(d.q)), 0.1f, 18.f);
        d.slopeDbPerOct = std::clamp(b.value("slope").toInt(12), 12, 48) / 12 * 12;
    }
    return s;
}

VoiceEqSettings VoiceEqSettings::load() {
    const QByteArray raw = QSettings().value("vk/eq").toByteArray();
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        // Never configured: the voice preset's curve, but OFF — the
        // operator's existing messages stay exactly as recorded until he
        // turns it on.
        VoiceEqSettings s = preset(Preset::SsbVoice);
        s.enabled = false;
        return s;
    }
    return fromJson(doc.object());
}

void VoiceEqSettings::save() const {
    QSettings().setValue("vk/eq", QJsonDocument(toJson())
                                      .toJson(QJsonDocument::Compact));
}

float VoiceEqSettings::responseDb(float hz) const {
    if (!enabled) return 0.0f;
    float db = 0.0f;
    for (const auto& b : bands)
        db += ClientEq::bandMagnitudeDb(b, hz, 48000.0);
    return db;
}

bool renderVoiceEq(const QString& inWav, const QString& outWav,
                   const VoiceEqSettings& s, double targetPeak,
                   QString* error) {
    std::vector<float> x;
    int rate = 48000;
    if (!ClipDeck::readWavMono(inWav, x, rate) || x.empty()) {
        if (error) *error = "cannot read " + inWav;
        return false;
    }
    if (s.enabled) {
        ClientEq eq;
        eq.prepare(rate);
        eq.setEnabled(true);
        eq.setActiveBandCount(VoiceEqSettings::kBands);
        for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
            ClientEq::BandParams b = s.bands[size_t(i)];
            b.type = VoiceEqSettings::slotType(i);
            eq.setBand(i, b);
        }
        eq.snapToTargets();                        // whole message on the curve
        constexpr int kBlock = 1024;
        for (size_t o = 0; o < x.size(); o += kBlock)
            eq.process(x.data() + o,
                       int(std::min<size_t>(kBlock, x.size() - o)), 1);
    }
    float peak = 0.0f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    if (peak > 0.0f) {
        const double gain = std::min(targetPeak / peak, kMaxMakeupGain);
        for (float& v : x) v = float(v * gain);
    }
    const QString tmp = outWav + ".tmp";
    if (!ClipDeck::writeWavMono(tmp, x, rate)) {
        if (error) *error = "cannot write " + tmp;
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(outWav);
    if (!QFile::rename(tmp, outWav)) {
        if (error) *error = "cannot replace " + outWav;
        return false;
    }
    return true;
}

} // namespace ttc
