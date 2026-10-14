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
    return {"Flat", "SSB voice", "Wide SSB", "DX punch (2.4 kHz)",
            "ESSB smooth", "Shure 444"};
}

// SSB voice: the usual broadcast-voice moves for a dynamic mic on SSB —
// rumble/proximity gone below 100 Hz, a little mud out around 300 Hz,
// presence up around 2.5 kHz. Wide SSB keeps more bottom (50 Hz HPF,
// gentle low shelf) for the Orion's 3.9 kHz TX filter.
VoiceEqSettings VoiceEqSettings::preset(Preset p) {
    VoiceEqSettings s;
    if (p == Preset::DxPunch) {
        // Heil DX target, fitted inside a 300-2700 Hz (2.4 kHz) TX filter:
        // within ~1-2 dB of it across the passband, steep outside so the
        // ALC/processor never spend drive on audio the filter removes.
        // Pair with TX BW 2400 on the TX bar.
        s.enabled = true;
        s.bands[0] = band(230.f, 0, 0.707f, true, FT::HighPass, 24);
        s.bands[1] = band(120.f, 0, 0.707f, false, FT::LowShelf);
        s.bands[2] = band(450.f, -2.f, 1.0f, true, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1600.f, 2.f, 1.0f, true, FT::Peak);
        s.bands[5] = band(2600.f, 6.f, 1.3f, true, FT::Peak);
        s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
        s.bands[7] = band(2900.f, 0, 0.707f, true, FT::LowPass, 24);
        return s;
    }
    if (p == Preset::Shure444) {
        // Least-squares fit to the Shure 444 target over 50-4000 Hz (the
        // Orion's widest TX passband): rms 0.75 dB, worst 2 dB. The 444's
        // thin bottom is mostly the -6 dB shelf; its rising presence is a
        // broad 1.8 kHz lift plus a narrower 2.8 kHz peak.
        s.enabled = true;
        s.bands[0] = band(90.f, 0, 0.707f, true, FT::HighPass, 12);
        s.bands[1] = band(240.f, -6.f, 0.707f, true, FT::LowShelf);
        s.bands[2] = band(300.f, 0, 1.0f, false, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1800.f, 2.f, 0.8f, true, FT::Peak);
        s.bands[5] = band(2800.f, 3.f, 1.8f, true, FT::Peak);
        s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
        s.bands[7] = band(3800.f, 0, 0.707f, false, FT::LowPass, 24);
        return s;
    }
    if (p == Preset::EssbSmooth) {
        // Full, smooth bottom (flat to 60 Hz, gentle shelf) with the mud
        // trimmed around 300 Hz and a broad +3..4.5 dB lift from 2 to
        // 3.8 kHz — for the Orion's 50-3900 Hz TX filter.
        s.enabled = true;
        s.bands[0] = band(40.f, 0, 0.707f, true, FT::HighPass, 12);
        s.bands[1] = band(110.f, 2.f, 0.6f, true, FT::LowShelf);
        s.bands[2] = band(300.f, -2.5f, 0.8f, true, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1800.f, 1.5f, 0.7f, true, FT::Peak);
        s.bands[5] = band(3000.f, 3.f, 0.9f, true, FT::Peak);
        s.bands[6] = band(3400.f, 1.5f, 0.707f, true, FT::HighShelf);
        s.bands[7] = band(3800.f, 0, 0.707f, false, FT::LowPass, 24);
        return s;
    }
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

namespace {
QJsonObject userProfiles() {
    return QJsonDocument::fromJson(
               QSettings().value("vk/eqProfiles").toByteArray()).object();
}
void storeUserProfiles(const QJsonObject& o) {
    QSettings().setValue("vk/eqProfiles",
                         QJsonDocument(o).toJson(QJsonDocument::Compact));
}
}  // namespace

QStringList VoiceEqSettings::userProfileNames() {
    QStringList names = userProfiles().keys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool VoiceEqSettings::loadUserProfile(const QString& name, VoiceEqSettings& out) {
    const QJsonObject all = userProfiles();
    if (!all.contains(name)) return false;
    out = fromJson(all.value(name).toObject());
    return true;
}

void VoiceEqSettings::saveUserProfile(const QString& name,
                                      const VoiceEqSettings& s) {
    QJsonObject all = userProfiles();
    all[name] = s.toJson();
    storeUserProfiles(all);
}

void VoiceEqSettings::deleteUserProfile(const QString& name) {
    QJsonObject all = userProfiles();
    all.remove(name);
    storeUserProfiles(all);
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

float VoiceEqTarget::dbAt(float hz) const {
    if (pts.empty()) return 0.0f;
    if (hz <= pts.front().hz) return pts.front().db;
    if (hz >= pts.back().hz) return pts.back().db;
    for (size_t i = 1; i < pts.size(); ++i)
        if (hz <= pts[i].hz) {
            const float t = std::log(hz / pts[i - 1].hz)
                          / std::log(pts[i].hz / pts[i - 1].hz);
            return pts[i - 1].db + t * (pts[i].db - pts[i - 1].db);
        }
    return pts.back().db;
}

const std::vector<VoiceEqTarget>& voiceEqTargets() {
    static const std::vector<VoiceEqTarget> t = {
        {"None", {}},
        {"AT&T 1959", {{50, -20}, {100, -12}, {200, -6}, {300, -2}, {500, 0},
                       {1000, 0}, {1500, 1}, {2000, 3}, {2500, 5}, {3000, 4},
                       {3400, 0}, {4000, -6}, {5000, -12}}},
        {"Heil DX", {{50, -25}, {100, -18}, {200, -10}, {300, -4}, {500, -1},
                     {1000, 0}, {1500, 2}, {2000, 4}, {2500, 6}, {2700, 6},
                     {3000, 5}, {3400, -2}, {4000, -10}, {5000, -18}}},
        {"Astatic D-104", {{50, -32}, {100, -22}, {200, -14}, {300, -8},
                           {500, -4}, {1000, 0}, {1500, 2}, {2000, 5},
                           {2500, 8}, {3000, 10}, {3500, 7}, {4000, 2},
                           {5000, -10}, {7000, -22}}},
        {"Shure 444", {{50, -15}, {100, -10}, {200, -4}, {300, -1}, {500, 0},
                       {1000, 1}, {1500, 2}, {2000, 3}, {2500, 4}, {3000, 4},
                       {3500, 3}, {4000, 1}, {5000, -3}, {7000, -10}}},
        {"Heil HC-5", {{50, -28}, {100, -18}, {200, -8}, {300, -3}, {500, 0},
                       {1000, 0}, {1500, 2}, {2000, 4}, {2500, 6}, {3000, 5},
                       {3500, 1}, {4000, -5}, {5000, -15}}},
    };
    return t;
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
